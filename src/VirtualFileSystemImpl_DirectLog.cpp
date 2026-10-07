#include "VirtualFileSystemImpl_DirectLog.h"
#include "VirtualFileSystemImpl.h"
#include "DirectLogDecoder.h"
#include "DNGDecoder.h"
#include "CalibrationData.h"
#include "DataLevels.h"
#include "Utils.h"
#include "GainMapBake.h"
#include "LRUCache.h"
#include "Types.h"
#include "AudioWriter.h"

#include <boost/filesystem.hpp>
#include <boost/algorithm/string.hpp>

#include <BS_thread_pool.hpp>
#include <spdlog/spdlog.h>
#include "tinydng/tiny_dng_writer.h"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <sstream>
#include <iomanip>
#include <cstring>
#include <fstream>
#include <mutex>
#include <QByteArray>
extern "C" {
#include <libswresample/swresample.h>
}

using motioncam::Timestamp;

namespace {

bool isIdentityMatrix(const std::array<float, 9>& matrix) {
    for (size_t i = 0; i < matrix.size(); ++i) {
        const float expected = (i % 4) == 0 ? 1.0f : 0.0f;
        if (std::abs(matrix[i] - expected) > 1.0e-6f) return false;
    }
    return true;
}

motioncam::ResolvedDataLevels directLogDataLevels(
        const motioncam::RenderSettings& settings, float defaultWhite = 65535.0f) {
    const std::array<float, 4> black{0, 0, 0, 0};
    return motioncam::resolveDataLevels(
        settings.levels, defaultWhite, black, defaultWhite, black, 3);
}

int directLogBaseLogBits(const motioncam::RenderSettings& settings) {
    const auto levels = directLogDataLevels(settings);
    const int effectiveBits = motioncam::utils::evenBitsNeeded(static_cast<uint16_t>(
        std::clamp(std::lround(levels.white), 1l, 65535l)));
    return effectiveBits <= 10 ? effectiveBits : 12;
}

int directLogLogBits(motioncam::LogTransformMode mode, int baseBits = 12) {
    int reduction = 0;
    if (mode == motioncam::LogTransformMode::ReduceBy2Bit) reduction = 2;
    else if (mode == motioncam::LogTransformMode::ReduceBy4Bit) reduction = 4;
    else if (mode == motioncam::LogTransformMode::ReduceBy6Bit) reduction = 6;
    else if (mode == motioncam::LogTransformMode::ReduceBy8Bit) reduction = 8;
    return std::max(1, baseBits - reduction);
}

bool directLogAppliesLogTransform(const motioncam::RenderSettings& settings) {
    if (!(settings.options & motioncam::RENDER_OPT_LOG_TRANSFORM) ||
        settings.logTransform == motioncam::LogTransformMode::Disabled)
        return false;
    if (settings.options & motioncam::RENDER_OPT_APPLY_VIGNETTE_CORRECTION)
        return true;
    const auto levels = directLogDataLevels(settings);
    const auto white = static_cast<uint16_t>(std::clamp(
        std::lround(levels.white), 1l, 65535l));
    return directLogLogBits(settings.logTransform, directLogBaseLogBits(settings)) <
        motioncam::utils::bitsNeeded(white);
}

uint32_t directLogQuantizationWhite(const motioncam::RenderSettings& settings) {
    const auto levels = directLogDataLevels(settings);
    const auto white = static_cast<uint16_t>(std::clamp(
        std::lround(levels.white), 1l, 65535l));
    return motioncam::utils::bitsNeeded(white) <= 10 ? white : 4095;
}

bool directLogDiagnosticsEnabled() {
    static const bool enabled = [] {
        const char* value = std::getenv("MOTIONCAM_DIRECTLOG_DIAGNOSTICS");
        return value && value[0] != '\0' && std::string(value) != "0";
    }();
    return enabled;
}

std::array<uint8_t, 4> directLogCfaPhase(
        const motioncam::RenderSettings& settings,
        const std::optional<motioncam::CalibrationData>& calibration) {
    std::string phase = "bggr";
    if (calibration && !calibration->cfaPhase.empty())
        phase = calibration->cfaPhase;
    else if (!settings.cfaPhase.empty() &&
             settings.cfaPhase != "Don't override CFA")
        phase = settings.cfaPhase;
    std::transform(phase.begin(), phase.end(), phase.begin(), ::tolower);
    return motioncam::cfaColorsFromPhase(phase);
}

double elapsedMilliseconds(const std::chrono::steady_clock::time_point start) {
    return std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - start).count();
}

std::shared_ptr<std::vector<uint8_t>> decodeAudioWav(
        const std::string& path, double fps) {
    AVFormatContext* rawFormat = nullptr;
    if (avformat_open_input(&rawFormat, path.c_str(), nullptr, nullptr) < 0)
        throw std::runtime_error("Could not open DirectLog audio source");
    std::unique_ptr<AVFormatContext, void(*)(AVFormatContext*)> format(
        rawFormat, [](AVFormatContext* value) { avformat_close_input(&value); });
    if (avformat_find_stream_info(format.get(), nullptr) < 0)
        throw std::runtime_error("Could not inspect DirectLog audio source");
    const int streamIndex = av_find_best_stream(
        format.get(), AVMEDIA_TYPE_AUDIO, -1, -1, nullptr, 0);
    if (streamIndex < 0) return {};
    const auto* parameters = format->streams[streamIndex]->codecpar;
    const AVCodec* codec = avcodec_find_decoder(parameters->codec_id);
    if (!codec) throw std::runtime_error("Unsupported DirectLog audio codec");
    std::unique_ptr<AVCodecContext, void(*)(AVCodecContext*)> decoder(
        avcodec_alloc_context3(codec),
        [](AVCodecContext* value) { avcodec_free_context(&value); });
    if (!decoder || avcodec_parameters_to_context(decoder.get(), parameters) < 0 ||
        avcodec_open2(decoder.get(), codec, nullptr) < 0)
        throw std::runtime_error("Could not initialize DirectLog audio decoder");
    if (decoder->sample_rate <= 0 || decoder->ch_layout.nb_channels <= 0)
        throw std::runtime_error("Invalid DirectLog audio format");
    SwrContext* rawResampler = nullptr;
    if (swr_alloc_set_opts2(&rawResampler, &decoder->ch_layout,
            AV_SAMPLE_FMT_S16, decoder->sample_rate, &decoder->ch_layout,
            decoder->sample_fmt, decoder->sample_rate, 0, nullptr) < 0 ||
        !rawResampler)
        throw std::runtime_error("Could not configure DirectLog audio conversion");
    std::unique_ptr<SwrContext, void(*)(SwrContext*)> resampler(
        rawResampler, [](SwrContext* value) { swr_free(&value); });
    if (swr_init(resampler.get()) < 0)
        throw std::runtime_error("Could not initialize DirectLog audio conversion");
    std::unique_ptr<AVPacket, void(*)(AVPacket*)> packet(
        av_packet_alloc(), [](AVPacket* value) { av_packet_free(&value); });
    std::unique_ptr<AVFrame, void(*)(AVFrame*)> frame(
        av_frame_alloc(), [](AVFrame* value) { av_frame_free(&value); });
    if (!packet || !frame) throw std::bad_alloc();
    const auto fpsRational = av_d2q(fps > 0.0 ? fps : 24.0, 100000);
    auto wav = std::make_shared<std::vector<uint8_t>>();
    {
        motioncam::AudioWriter writer(*wav, decoder->ch_layout.nb_channels,
            decoder->sample_rate, fpsRational.num, fpsRational.den, 16);
        const auto drain = [&] {
            while (true) {
                const int received = avcodec_receive_frame(decoder.get(), frame.get());
                if (received == AVERROR(EAGAIN) || received == AVERROR_EOF) break;
                if (received < 0) throw std::runtime_error("Could not decode DirectLog audio");
                const int capacity = swr_get_out_samples(resampler.get(), frame->nb_samples);
                std::vector<int16_t> samples(static_cast<size_t>(capacity) *
                    decoder->ch_layout.nb_channels);
                uint8_t* output[] = {reinterpret_cast<uint8_t*>(samples.data())};
                const int count = swr_convert(resampler.get(), output, capacity,
                    const_cast<const uint8_t**>(frame->extended_data), frame->nb_samples);
                if (count < 0) throw std::runtime_error("Could not convert DirectLog audio");
                if (count > 0) writer.write(samples, count);
                av_frame_unref(frame.get());
            }
        };
        while (av_read_frame(format.get(), packet.get()) >= 0) {
            if (packet->stream_index == streamIndex) {
                const int sent = avcodec_send_packet(decoder.get(), packet.get());
                if (sent < 0) throw std::runtime_error("Could not submit DirectLog audio packet");
                drain();
            }
            av_packet_unref(packet.get());
        }
        if (avcodec_send_packet(decoder.get(), nullptr) < 0)
            throw std::runtime_error("Could not finish DirectLog audio decode");
        drain();
        while (true) {
            const int capacity = swr_get_out_samples(resampler.get(), 0);
            if (capacity <= 0) break;
            std::vector<int16_t> samples(static_cast<size_t>(capacity) *
                decoder->ch_layout.nb_channels);
            uint8_t* output[] = {reinterpret_cast<uint8_t*>(samples.data())};
            const int count = swr_convert(resampler.get(), output, capacity, nullptr, 0);
            if (count <= 0) break;
            writer.write(samples, count);
        }
    }
    return wav;
}

} // namespace

namespace motioncam {



VirtualFileSystemImpl_DirectLog::VirtualFileSystemImpl_DirectLog(
        BS::thread_pool&,
        BS::thread_pool& processingThreadPool,
        LRUCache& lruCache,
        const RenderSettings& config,
        const std::string& file,
        const std::string& baseName) :
        MountedDngSource(lruCache, processingThreadPool),
        mSrcPath(file),
        mBaseName(baseName),
        mTypicalDngSize(0),
        mFps(0),
        mFrameRateInfo(),
        mTotalFrames(0),
        mDroppedFrames(0),
        mDuplicatedFrames(0),
        mWidth(0),
        mHeight(0),
        mConfig(config),
        mIsHLG(false) {
    
    // Load calibration JSON if it exists
    const auto calibPath = vfs::sidecarPath(mSrcPath);
    if (boost::filesystem::exists(calibPath)) {
        vfs::loadSidecar(calibPath, mSidecarMetadata, mCalibration);
        if (mCalibration.has_value()) {
            if (mCalibration->hasLevels) mConfig.levels = mCalibration->levels;
            if (mCalibration->hasCenterCrop) {
                mConfig.cropTarget = std::to_string(mCalibration->centerCrop[0]) + "x" +
                                     std::to_string(mCalibration->centerCrop[1]);
                mConfig.options |= RENDER_OPT_CROPPING;
            }
            spdlog::info("Loaded calibration for DirectLog: {}", calibPath.string());
        }
    }
    mManualVignetteSidecars = vfs::loadManualVignetteSidecars(
        mSrcPath, &mSidecarMetadata, &calibPath);
    mGyroflowLensProfile = vfs::loadGyroflowLensProfile(
        vfs::referencedSidecarPath(vfs::gyroflowSidecarPath(mSrcPath),
            mSidecarMetadata, calibPath, "gyroflow"));

    // Initialize DirectLogDecoder
    try {
        mDecoder = std::make_unique<DirectLogDecoder>(mSrcPath);
        if (mSidecarMetadata.contains("dynamic") &&
            mSidecarMetadata["dynamic"].contains("frames")) {
            const auto& metadataFrames = mSidecarMetadata["dynamic"]["frames"];
            std::vector<Timestamp> exactTimestamps;
            exactTimestamps.reserve(metadataFrames.size());
            for (const auto& metadata : metadataFrames) {
                if (!metadata.contains("timestampNs")) { exactTimestamps.clear(); break; }
                exactTimestamps.push_back(metadata["timestampNs"].get<Timestamp>());
            }
            mDecoder->overrideTimestamps(exactTimestamps);
        }
        if (mCalibration && mCalibration->hasDataLevels) {
            if (mCalibration->dataLevels == "Full")
                mDecoder->setFullRangeOverride(true);
            else if (mCalibration->dataLevels == "Limited")
                mDecoder->setFullRangeOverride(false);
        }
        const auto& videoInfo = mDecoder->getVideoInfo();
        
        mWidth = videoInfo.width;
        mHeight = videoInfo.height;
        mTotalFrames = static_cast<int>(videoInfo.totalFrames);
        mPixelFormat = videoInfo.pixelFormat;
        mIsHLG = videoInfo.isHLG;
        mDroppedFrames = 0;
        mDuplicatedFrames = 0;
        const auto& frames = mDecoder->getFrames();
        std::vector<Timestamp> sourceTimestamps;
        sourceTimestamps.reserve(frames.size());
        for (const auto& frame : frames) sourceTimestamps.push_back(frame.timestamp);
        mFrameIndexByTimestamp = vfs::indexTimestamps(sourceTimestamps);
        
        // Calculate frame rate statistics from actual frame timestamps
        calculateFrameRateStats();
        analyzeSidecarExposure();
        if (!mConfig.streamingPreview) {
            try {
                mAudioWav = decodeAudioWav(mSrcPath, mFps);
            } catch (const std::exception& error) {
                spdlog::warn("Could not project DirectLog audio for {}: {}",
                             mSrcPath, error.what());
            }
        }
        
        spdlog::info("DirectLog video loaded: {}x{} @ {:.2f}fps (avg: {:.2f}, med: {:.2f}), {} frames, format: {}, HLG: {}", 
                     mWidth, mHeight, mFps, mFrameRateInfo.averageFrameRate, mFrameRateInfo.medianFrameRate, mTotalFrames, mPixelFormat, mIsHLG);
    }
    catch (const std::exception& e) {
        spdlog::error("Failed to initialize DirectLogDecoder: {}", e.what());
        throw;
    }
    
    this->init();
}

VirtualFileSystemImpl_DirectLog::~VirtualFileSystemImpl_DirectLog() {
    spdlog::info("Destroying VirtualFileSystemImpl_DirectLog({})", mSrcPath);
}

void VirtualFileSystemImpl_DirectLog::init() {
    spdlog::debug("VirtualFileSystemImpl_DirectLog::init(options={})", optionsToString(mConfig.options));
    
    mFiles.clear();

    vfs::appendDesktopIni(mFiles);
    if (mAudioWav && !mAudioWav->empty()) {
        Entry audioEntry;
        audioEntry.type = EntryType::FILE_ENTRY;
        audioEntry.name = "audio.wav";
        audioEntry.size = mAudioWav->size();
        mFiles.push_back(std::move(audioEntry));
    }

    const auto& frames = mDecoder->getFrames();
    if (frames.empty()) {
        return;
    }
    
    // Determine target FPS based on CFR conversion options
    bool applyCFRConversion = mConfig.options & RENDER_OPT_FRAMERATE_CONVERSION;
    mFps = vfs::determineCFRTarget(mFrameRateInfo, mConfig.cfrTarget, applyCFRConversion);
    
    spdlog::info("DirectLog target FPS: {:.2f} (CFR conversion: {})", mFps, applyCFRConversion);
    
    size_t firstDngSize = 0;

    // Generate one sample DNG to determine actual file size (mimics MCRAW approach)
    if (mConfig.streamingPreview) {
        // Callback-only previews neither pad nor expose projected file sizes.
        // Avoid allocating a full-resolution RGB image and encoding one or two
        // synthetic sizing DNGs before the real preview frame is decoded.
        mTypicalDngSize = 1;
        firstDngSize = 1;
    } else if (!frames.empty()) {
        // FUSE only needs a safe projected size for directory metadata; reads
        // stop at the materialized buffer's actual EOF. Avoid constructing and
        // gain-baking a 75 MB sample DNG solely to obtain this projection.
        uint32_t width = static_cast<uint32_t>(mWidth);
        uint32_t height = static_cast<uint32_t>(mHeight);
        if (mCalibration && mCalibration->hasLeftTopCropStride) {
            width = std::min(width, static_cast<uint32_t>(
                std::max(1, mCalibration->leftTopCropStride[0])));
            height = std::min(height, static_cast<uint32_t>(
                std::max(1, mCalibration->leftTopCropStride[1])));
        }
        if (mConfig.options & RENDER_OPT_CROPPING) {
            uint32_t cropWidth = 0, cropHeight = 0, stride = 0;
            utils::parseCropTarget(mConfig.cropTarget, cropWidth, cropHeight, stride);
            if (cropWidth && cropHeight) {
                width = std::min(width, cropWidth);
                height = std::min(height, cropHeight);
            }
        }
        constexpr size_t transformedMetadataAllowance = 256 * 1024;
        size_t measuredMetadataBytes =
            vfs::projectedSidecarMetadataSize(mSidecarMetadata) +
            vfs::projectedDcpMetadataSize(mManualVignetteSidecars);
        size_t largestManualSidecarBytes = 0;
        for (const auto& candidate : mManualVignetteSidecars.candidates) {
            size_t candidateBytes = 0;
            for (const auto& entry : candidate.metadata)
                candidateBytes += entry.value.size();
            candidateBytes += vfs::projectedGainMapMetadataSize(
                candidate.image.opcodeList2);
            candidateBytes += vfs::projectedGainMapMetadataSize(
                candidate.image.opcodeList3);
            for (const auto& opcodeList : candidate.nonGainMapOpcodes)
                candidateBytes += opcodeList.size();
            largestManualSidecarBytes = std::max(
                largestManualSidecarBytes, candidateBytes);
        }
        measuredMetadataBytes += largestManualSidecarBytes;
        const uint32_t projectedChannels =
            (mConfig.options & RENDER_OPT_REMOSAIC_TO_BAYER) ? 1u : 3u;
        const auto levels = directLogDataLevels(mConfig);
        const bool appliesLog = directLogAppliesLogTransform(mConfig);
        const double sizingWhite = appliesLog
            ? static_cast<double>(directLogQuantizationWhite(mConfig))
            : levels.white;
        const std::array<double, 4> sizingBlack{
            levels.black[0], levels.black[1], levels.black[2], levels.black[3]};
        const auto outputLevels = utils::planDngOutputLevels(
            sizingWhite, sizingBlack,
            mConfig.options & RENDER_OPT_APPLY_VIGNETTE_CORRECTION,
            mConfig.options & RENDER_OPT_NORMALIZE_SHADING_MAP,
            mConfig.options & RENDER_OPT_DEBUG_SHADING_MAP,
            appliesLog ? mConfig.logTransform : LogTransformMode::Disabled);
        const uint32_t projectedBits = utils::dngPackedBits(
            static_cast<uint16_t>(std::clamp(
                std::lround(outputLevels.white), 1l, 65535l)),
            projectedChannels == 3, mConfig.cameraNativeStaging);
        const auto projectedSize = [&](uint32_t projectedWidth,
                                       uint32_t projectedHeight) {
            return vfs::projectedDngSize(
                projectedWidth, projectedHeight, projectedChannels, projectedBits,
                measuredMetadataBytes, transformedMetadataAllowance);
        };
        const uint32_t proxyScale = static_cast<uint32_t>(std::max(
            1, vfs::getScaleFromOptions(mConfig.options, mConfig.draftScale)));
        mTypicalDngSize = projectedSize(
            (width + proxyScale - 1) / proxyScale,
            (height + proxyScale - 1) / proxyScale);
        firstDngSize = proxyScale > 1
            ? projectedSize(width, height) : mTypicalDngSize;
        spdlog::info("DirectLog projected DNG size: {} bytes ({:.2f} MB)",
                     mTypicalDngSize, mTypicalDngSize / (1024.0 * 1024.0));
    }
    std::vector<Entry> sourceEntries;
    std::vector<Timestamp> timestamps;
    sourceEntries.reserve(frames.size());
    timestamps.reserve(frames.size());
    auto restoreFrameFlags = [&](Entry& entry, size_t sourceIndex) {
        if (!mSidecarMetadata.contains("dynamic") ||
            !mSidecarMetadata["dynamic"].contains("frames") ||
            sourceIndex >= mSidecarMetadata["dynamic"]["frames"].size()) return;
        const auto& metadata = mSidecarMetadata["dynamic"]["frames"][sourceIndex];
        entry.duplicateFrame = metadata.value("duplicateFrame", false);
        entry.syntheticFrame = metadata.value("syntheticFrame", false);
    };
    
    for (size_t i = 0; i < frames.size(); ++i) {
        Entry entry;
        entry.type = EntryType::FILE_ENTRY;
        entry.size = mTypicalDngSize;
        entry.userData = frames[i].timestamp;
        restoreFrameFlags(entry, i);
        sourceEntries.push_back(entry);
        timestamps.push_back(frames[i].timestamp);
    }
    auto mapped = vfs::mapFramesToCfr(sourceEntries, timestamps, mBaseName + "-", mFps,
        applyCFRConversion, mDroppedFrames, mDuplicatedFrames);
    if (!mapped.empty() && firstDngSize > 0)
        mapped.front().size = firstDngSize;
    mFiles.insert(mFiles.end(), std::make_move_iterator(mapped.begin()),
                  std::make_move_iterator(mapped.end()));
    
    spdlog::info("DirectLog generated {} DNG entries (dropped: {}, duplicated: {})", 
                 mFiles.size(), mDroppedFrames, mDuplicatedFrames);
}

void VirtualFileSystemImpl_DirectLog::analyzeSidecarExposure() {
    mNormalizedExposureOffsets.clear();
    mSmoothedExposureOffsets.clear();
    mSmoothedAsShotNeutrals.clear();
    if (!mSidecarMetadata.contains("dynamic") ||
        !mSidecarMetadata["dynamic"].contains("frames")) return;
    const auto& metadataFrames = mSidecarMetadata["dynamic"]["frames"];
    const auto& sourceFrames = mDecoder->getFrames();
    std::vector<vfs::ExposureSample> samples;
    samples.reserve(std::min(metadataFrames.size(), sourceFrames.size()));
    for (size_t i = 0; i < metadataFrames.size() && i < sourceFrames.size(); ++i) {
        const auto metadata = frameMetadata(static_cast<int>(i));
        vfs::ExposureSample sample;
        sample.timestamp = sourceFrames[i].timestamp;
        sample.iso = metadata.iso;
        sample.exposureSeconds = metadata.shutterSpeed;
        sample.baselineExposure = metadata.baselineExposure;
        if (metadata.asShotNeutral) sample.asShotNeutral = *metadata.asShotNeutral;
        samples.push_back(sample);
    }
    const auto analysis = vfs::analyzeExposureMetadata(
        samples, mFrameRateInfo.medianFrameRate);
    mNormalizedExposureOffsets = analysis.normalizedBaseline;
    mSmoothedExposureOffsets = analysis.smoothedBaseline;
    mSmoothedAsShotNeutrals = analysis.smoothedNeutral;
}

VirtualFileSystemImpl_DirectLog::FrameMetadata
VirtualFileSystemImpl_DirectLog::frameMetadata(int frameNumber) const {
    FrameMetadata result;
    const nlohmann::json* dynamic = nullptr;
    if (frameNumber >= 0 && mSidecarMetadata.contains("dynamic") &&
        mSidecarMetadata["dynamic"].contains("frames") &&
        static_cast<size_t>(frameNumber) < mSidecarMetadata["dynamic"]["frames"].size())
        dynamic = &mSidecarMetadata["dynamic"]["frames"][frameNumber];
    auto field = [&](const char* name) -> const nlohmann::json* {
        if (dynamic && dynamic->contains(name)) return &dynamic->at(name);
        if (mSidecarMetadata.contains(name)) return &mSidecarMetadata.at(name);
        return nullptr;
    };
    if (const auto* value = field("iso")) result.iso = value->get<double>();
    if (const auto* value = field("shutterSpeedSeconds")) result.shutterSpeed = value->get<double>();
    if (const auto* value = field("baselineExposure")) result.baselineExposure = value->get<double>();
    if (const auto* value = field("asShotNeutral"); value && value->is_array() && value->size() >= 3) {
        result.asShotNeutral = std::array<float, 3>{
            (*value)[0].get<float>(), (*value)[1].get<float>(), (*value)[2].get<float>()};
    }
    if (const auto* value = field("tiffOrientation"))
        result.tiffOrientation = value->get<uint16_t>();
    auto matrix = [&](const char* name) -> std::optional<std::array<float, 9>> {
        const auto* value = field(name);
        if (!value || !value->is_array() || value->size() != 9) return std::nullopt;
        std::array<float, 9> out{};
        for (size_t i = 0; i < out.size(); ++i) out[i] = (*value)[i].get<float>();
        return out;
    };
    result.colorMatrix1 = matrix("colorMatrix1");
    result.colorMatrix2 = matrix("colorMatrix2");
    result.forwardMatrix1 = matrix("forwardMatrix1");
    result.forwardMatrix2 = matrix("forwardMatrix2");
    result.cameraCalibration1 = matrix("cameraCalibration1");
    result.cameraCalibration2 = matrix("cameraCalibration2");
    if (const auto* value = field("calibrationIlluminant1"))
        result.calibrationIlluminant1 = value->get<uint16_t>();
    if (const auto* value = field("calibrationIlluminant2"))
        result.calibrationIlluminant2 = value->get<uint16_t>();
    return result;
}

std::vector<GainMap> VirtualFileSystemImpl_DirectLog::loadSidecarGainMaps(
        int frameNumber, const char* field) const {
    return frameNumber < 0 ? std::vector<GainMap>{} :
        vfs::loadSidecarGainMaps(mSidecarMetadata, static_cast<size_t>(frameNumber), field);
}

VirtualFileSystemImpl_DirectLog::PreparedSidecarGainMaps
VirtualFileSystemImpl_DirectLog::prepareSidecarGainMaps(int frameNumber) const {
    PreparedSidecarGainMaps prepared;
    if (mConfig.vignetteCorrection == VignetteCorrectionMode::Exclude)
        return prepared;
    const auto gainMaps = loadSidecarGainMaps(frameNumber, "gainMaps");
    const auto deferredGainMaps = loadSidecarGainMaps(frameNumber, "deferredGainMaps");
    auto classify = [&](const std::vector<GainMap>& maps) {
        if (maps.empty()) return;
        if (maps.size() == 4 || (maps.size() == 1 && maps.front().channels == 4))
            prepared.opcodeList2.insert(prepared.opcodeList2.end(), maps.begin(), maps.end());
        else if (maps.size() == 1 && maps.front().channels == 1)
            prepared.opcodeList3.push_back(maps.front());
        else
            throw std::runtime_error("Unsupported DirectLog gain-map layout");
    };
    classify(gainMaps);
    classify(deferredGainMaps);
    if (!canonicalizeCfaGainMaps(prepared.opcodeList2))
        throw std::runtime_error("Unsupported DirectLog OpcodeList2 gain-map layout");

    // Some DirectLog sidecars retain full-sensor gain-map coordinates even
    // though the encoded video already contains only the map's active window.
    // Rebase that exact-size window onto the video instead of applying the map
    // only to an incorrectly offset center rectangle.
    auto rebaseEncodedWindow = [&](std::vector<GainMap>& maps) {
        for (auto& map : maps) {
            const uint32_t coordinateWidth = map.coordinateWidth
                ? map.coordinateWidth : map.right;
            const uint32_t coordinateHeight = map.coordinateHeight
                ? map.coordinateHeight : map.bottom;
            if (map.right - map.left != static_cast<uint32_t>(mWidth) ||
                map.bottom - map.top != static_cast<uint32_t>(mHeight) ||
                (coordinateWidth == static_cast<uint32_t>(mWidth) &&
                 coordinateHeight == static_cast<uint32_t>(mHeight)))
                continue;
            const uint32_t encodedLeft = map.left;
            const uint32_t encodedTop = map.top;
            map.originH = (map.originH * coordinateWidth - map.left) / mWidth;
            map.originV = (map.originV * coordinateHeight - map.top) / mHeight;
            map.spacingH *= static_cast<double>(coordinateWidth) / mWidth;
            map.spacingV *= static_cast<double>(coordinateHeight) / mHeight;
            map.left = 0; map.top = 0;
            map.right = mWidth; map.bottom = mHeight;
            map.coordinateWidth = mWidth;
            map.coordinateHeight = mHeight;
            if (directLogDiagnosticsEnabled())
                spdlog::info(
                    "DirectLog diagnostic: rebased gain map sensor={}x{} offset={},{} video={}x{}",
                    coordinateWidth, coordinateHeight, encodedLeft, encodedTop, mWidth, mHeight);
        }
    };
    rebaseEncodedWindow(prepared.opcodeList2);
    rebaseEncodedWindow(prepared.opcodeList3);

    // JSON gain maps finalized for the requested output crop need to pass
    // through the shared pipeline in the uncropped DirectLog staging DNG.
    // Lift them into staging coordinates here; cropImage() will then bring
    // them back to their original geometry together with the pixels. This
    // also preserves the distinct Top/Left values of the four CFA phases.
    uint32_t cropWidth = static_cast<uint32_t>(mWidth);
    uint32_t cropHeight = static_cast<uint32_t>(mHeight);
    bool hasCropWindow = false;
    if (mCalibration && mCalibration->hasLeftTopCropStride &&
        mCalibration->leftTopCropStride[0] > 0 &&
        mCalibration->leftTopCropStride[1] > 0 &&
        mCalibration->leftTopCropStride[0] <= mWidth &&
        mCalibration->leftTopCropStride[1] <= mHeight) {
        cropWidth = std::min(cropWidth,
            static_cast<uint32_t>(mCalibration->leftTopCropStride[0]));
        cropHeight = std::min(cropHeight,
            static_cast<uint32_t>(mCalibration->leftTopCropStride[1]));
        hasCropWindow = true;
    }
    if (mConfig.options & RENDER_OPT_CROPPING) {
        uint32_t centeredWidth = 0, centeredHeight = 0, ignoredStride = 0;
        utils::parseCropTarget(
            mConfig.cropTarget, centeredWidth, centeredHeight, ignoredStride);
        if (centeredWidth && centeredHeight &&
            centeredWidth <= static_cast<uint32_t>(mWidth) &&
            centeredHeight <= static_cast<uint32_t>(mHeight)) {
            cropWidth = std::min(cropWidth, centeredWidth);
            cropHeight = std::min(cropHeight, centeredHeight);
            hasCropWindow = true;
        }
    }
    if (hasCropWindow) {
        // Both crop declarations constrain the same centered gain-map window.
        // Lift the smaller window into the original image coordinate space.
        const uint32_t cropLeft = (static_cast<uint32_t>(mWidth) - cropWidth) / 2;
        const uint32_t cropTop = (static_cast<uint32_t>(mHeight) - cropHeight) / 2;
        auto liftToStaging = [&](std::vector<GainMap>& maps) {
            for (auto& map : maps) {
                const uint32_t coordinateWidth = map.coordinateWidth
                    ? map.coordinateWidth : map.right;
                const uint32_t coordinateHeight = map.coordinateHeight
                    ? map.coordinateHeight : map.bottom;
                if (coordinateWidth != cropWidth || coordinateHeight != cropHeight ||
                    map.right > cropWidth || map.bottom > cropHeight)
                    continue;
                map.left += cropLeft; map.right += cropLeft;
                map.top += cropTop; map.bottom += cropTop;
                map.originH = (map.originH * cropWidth + cropLeft) / mWidth;
                map.originV = (map.originV * cropHeight + cropTop) / mHeight;
                map.spacingH *= static_cast<double>(cropWidth) / mWidth;
                map.spacingV *= static_cast<double>(cropHeight) / mHeight;
                map.coordinateWidth = mWidth;
                map.coordinateHeight = mHeight;
            }
        };
        liftToStaging(prepared.opcodeList2);
        liftToStaging(prepared.opcodeList3);
    }
    return prepared;
}

bool VirtualFileSystemImpl_DirectLog::convertRGBToDNG(
    std::vector<uint16_t> rgbData,
    std::vector<uint8_t>& dngData,
    int frameNumber,
    Timestamp timestamp,
    double iso,
    double shutterSpeed, double baselineExposure,
    const std::optional<std::array<float, 3>>& asShotNeutral,
    uint16_t tiffOrientation,
    const FrameMetadata* sourceMetadata,
    const std::vector<GainMap>& opcodeList2Maps,
    const std::vector<GainMap>& opcodeList3Maps,
    int decodedWidth, int decodedHeight) {

    try {
        const bool diagnostics = directLogDiagnosticsEnabled();
        auto diagnosticStage = std::chrono::steady_clock::now();
        const auto& videoInfo = mDecoder->getVideoInfo();
        int width = decodedWidth > 0 ? decodedWidth : videoInfo.width;
        int height = decodedHeight > 0 ? decodedHeight : videoInfo.height;
        
        // This adapter emits one canonical, uncompressed 16-bit linear RGB
        // DNG. Source-independent processing happens after construction.
        std::vector<uint16_t> processedRgbData = std::move(rgbData);

        const auto outputLevels = directLogDataLevels(mConfig);
        std::vector<uint16_t> imageSamples = std::move(processedRgbData);
        constexpr int samplesPerPixel = 3;
        constexpr int photometric = tinydngwriter::PHOTOMETRIC_LINEARRAW;

        if (diagnostics) {
            spdlog::info("DirectLog diagnostic: frame={} process_ms={:.3f} samples={} channels={}",
                         frameNumber, elapsedMilliseconds(diagnosticStage),
                         imageSamples.size(),
                         samplesPerPixel);
            diagnosticStage = std::chrono::steady_clock::now();
        }

        const uint8_t* imageData = reinterpret_cast<const uint8_t*>(imageSamples.data());
        const size_t imageDataSize = imageSamples.size() * sizeof(uint16_t);
        if (diagnostics) {
            spdlog::info("DirectLog diagnostic: frame={} pack_ms={:.3f} image_bytes={}",
                         frameNumber, elapsedMilliseconds(diagnosticStage), imageDataSize);
        }
        
        // Create DNG image
        tinydngwriter::DNGImage dng;
        
        // Set basic image properties
        dng.SetBigEndian(false);
        dng.SetImageWidth(width);
        dng.SetImageLength(height);
        dng.SetSamplesPerPixel(samplesPerPixel);
        dng.SetRowsPerStrip(height);
        
        unsigned short bitsPerSample[3] = {16, 16, 16};
        dng.SetBitsPerSample(samplesPerPixel, bitsPerSample);
        
        // Photometric interpretation
        dng.SetPhotometric(photometric);
        dng.SetPlanarConfig(1); // Chunky
        dng.SetCompression(tinydngwriter::COMPRESSION_NONE);
        
        unsigned short sampleFormat[3] = {1, 1, 1}; // Unsigned integer
        dng.SetSampleFormat(samplesPerPixel, sampleFormat);
        
        // Set DNG version
        dng.SetDNGVersion(1, 4, 0, 0);
        const bool hasStageOpcodes = !opcodeList2Maps.empty() || !opcodeList3Maps.empty();
        dng.SetDNGBackwardVersion(1, hasStageOpcodes ? 3 : 4, 0, 0);
        
        // Set camera/software metadata
        const auto identity = vfs::resolveCameraIdentity(
            mConfig.cameraModel, "");
        if (!identity.uniqueModel.empty()) dng.SetUniqueCameraModel(identity.uniqueModel);
        if (!identity.make.empty()) dng.SetMake(identity.make);
        if (!identity.model.empty()) dng.SetCameraModelName(identity.model);
        dng.SetSoftware("MotionCam DirectLog Decoder");
        if (iso > 0.0) dng.SetIso(static_cast<unsigned int>(std::lround(iso)));
        if (shutterSpeed > 0.0) dng.SetExposureTime(shutterSpeed);
        
        // Set image description with frame info
        std::ostringstream desc;
        desc << "Frame " << frameNumber << " from DirectLog video";
        if (mIsHLG) {
            desc << " (HLG to Linear)";
        }
        dng.SetImageDescription(desc.str());
        
        // Set resolution
        dng.SetXResolution(72.0f);
        dng.SetYResolution(72.0f);
        dng.SetResolutionUnit(2); // inches
        const int orientation = mCalibration && mCalibration->hasOrientation
            ? mCalibration->orientation
            : mConfig.orientation >= 0 ? mConfig.orientation
                                       : videoInfo.orientation;
        if (!(mCalibration && mCalibration->hasOrientation) && tiffOrientation >= 1 &&
            tiffOrientation <= 8) dng.SetOrientation(tiffOrientation);
        else if (orientation == 90) dng.SetOrientation(6);
        else if (orientation == 180) dng.SetOrientation(3);
        else if (orientation == 270) dng.SetOrientation(8);
        else dng.SetOrientation(1);
        if (mFps > 0.0f) {
            dng.SetFrameRate(mFps);
        }

        // Source baseline only. Configured display compensation and gain-map
        // optimization are appended later as shared metadata operations.
        dng.SetBaselineExposure(static_cast<float>(baselineExposure));
        
        // Level overrides describe the already-linear 16-bit domain. They only
        // change metadata; decoded samples remain untouched.
        // Set white/black levels and linearization table
        dng.SetWhiteLevel(static_cast<unsigned int>(std::clamp(
            std::lround(outputLevels.white), 0l, 65535l)));
        unsigned short blackLevel[4]{};
        for (size_t channel = 0; channel < 3; ++channel)
            blackLevel[channel] = static_cast<unsigned short>(std::clamp(
                std::lround(outputLevels.black[channel]), 0l, 65535l));
        dng.SetBlackLevel(3, blackLevel);
        
        diagnosticStage = std::chrono::steady_clock::now();
        if (!dng.SetImageData(imageData, imageDataSize)) {
            throw std::runtime_error("Failed to attach DirectLog image data");
        }
        if (diagnostics)
            spdlog::info("DirectLog diagnostic: frame={} attach_image_ms={:.3f}",
                         frameNumber, elapsedMilliseconds(diagnosticStage));
        
        // Apply calibration if available
        if (mCalibration.has_value()) {
            const auto color1 = mCalibration->hasColorMatrix1
                ? std::optional{mCalibration->colorMatrix1}
                : sourceMetadata ? sourceMetadata->colorMatrix1 : std::nullopt;
            const auto color2 = mCalibration->hasColorMatrix2
                ? std::optional{mCalibration->colorMatrix2}
                : sourceMetadata ? sourceMetadata->colorMatrix2 : std::nullopt;
            const auto forward1 = mCalibration->hasForwardMatrix1
                ? std::optional{mCalibration->forwardMatrix1}
                : sourceMetadata ? sourceMetadata->forwardMatrix1 : std::nullopt;
            const auto forward2 = mCalibration->hasForwardMatrix2
                ? std::optional{mCalibration->forwardMatrix2}
                : sourceMetadata ? sourceMetadata->forwardMatrix2 : std::nullopt;
            const auto camera1 = mCalibration->hasCameraCalibration1
                ? std::optional{mCalibration->cameraCalibration1}
                : sourceMetadata ? sourceMetadata->cameraCalibration1 : std::nullopt;
            const auto camera2 = mCalibration->hasCameraCalibration2
                ? std::optional{mCalibration->cameraCalibration2}
                : sourceMetadata ? sourceMetadata->cameraCalibration2 : std::nullopt;
            const bool includeCamera1 = camera1 &&
                (!isIdentityMatrix(*camera1) || (camera2 && !isIdentityMatrix(*camera2)));
            const bool includeCamera2 = camera2 &&
                (!isIdentityMatrix(*camera2) || (camera1 && !isIdentityMatrix(*camera1)));
            const bool hasMatrix1 = (color1 && !isIdentityMatrix(*color1)) ||
                (forward1 && !isIdentityMatrix(*forward1)) || includeCamera1;
            const bool hasMatrix2 = (color2 && !isIdentityMatrix(*color2)) ||
                (forward2 && !isIdentityMatrix(*forward2)) || includeCamera2;
            if (hasMatrix1) {
                dng.SetCalibrationIlluminant1(mCalibration->hasCalibrationIlluminant1
                    ? mCalibration->calibrationIlluminant1
                    : sourceMetadata && sourceMetadata->calibrationIlluminant1
                    ? sourceMetadata->calibrationIlluminant1
                    : mSidecarMetadata.value("calibrationIlluminant1", 21));
            }
            if (hasMatrix2) {
                dng.SetCalibrationIlluminant2(mCalibration->hasCalibrationIlluminant2
                    ? mCalibration->calibrationIlluminant2
                    : sourceMetadata && sourceMetadata->calibrationIlluminant2
                    ? sourceMetadata->calibrationIlluminant2
                    : mSidecarMetadata.value("calibrationIlluminant2", 17));
            }
            if (color1 && !isIdentityMatrix(*color1)) dng.SetColorMatrix1(3, color1->data());
            if (color2 && !isIdentityMatrix(*color2)) dng.SetColorMatrix2(3, color2->data());
            if (forward1 && !isIdentityMatrix(*forward1)) dng.SetForwardMatrix1(3, forward1->data());
            if (forward2 && !isIdentityMatrix(*forward2)) dng.SetForwardMatrix2(3, forward2->data());
            if (includeCamera1) dng.SetCameraCalibration1(3, camera1->data());
            if (includeCamera2) dng.SetCameraCalibration2(3, camera2->data());
        }
        auto outputNeutral = mCalibration && mCalibration->hasAsShotNeutral
            ? std::optional{mCalibration->asShotNeutral} : asShotNeutral;
        if (outputNeutral)
            dng.SetAsShotNeutral(3, outputNeutral->data());
        auto makeOpcodeList = [&](const std::vector<GainMap>& maps, bool cfaPhases) {
            tinydngwriter::OpcodeList result;
            if (maps.empty()) return result;
            for (const auto& map : maps) {
                if (!map.coordinateWidth || !map.coordinateHeight ||
                    map.left >= map.right || map.top >= map.bottom)
                    throw std::runtime_error("Invalid DirectLog opcode coordinate geometry");
                tinydngwriter::GainMapParams params{};
                params.top = map.top; params.left = map.left;
                params.bottom = std::min<uint32_t>(height, map.bottom);
                params.right = std::min<uint32_t>(width, map.right);
                params.plane = map.plane; params.planes = map.planes;
                params.row_pitch = map.rowPitch; params.col_pitch = map.colPitch;
                params.map_points_v = map.height; params.map_points_h = map.width;
                params.map_spacing_v = map.spacingV * map.coordinateHeight / height;
                params.map_spacing_h = map.spacingH * map.coordinateWidth / width;
                params.map_origin_v = map.originV * map.coordinateHeight / height;
                params.map_origin_h = map.originH * map.coordinateWidth / width;
                params.map_planes = map.channels;
                if (!cfaPhases && map.channels == 1) {
                    params.top = 0;
                    params.left = 0;
                    params.bottom = height;
                    params.right = width;
                    params.plane = 0;
                    params.planes = 3;
                    params.row_pitch = 1;
                    params.col_pitch = 1;
                }
                const size_t planeSize = static_cast<size_t>(map.width) * map.height;
                if (!map.channels || map.data.size() != planeSize * map.channels)
                    throw std::runtime_error("Invalid DirectLog opcode gain-map payload");
                if (cfaPhases && map.channels == 3) {
                    // The early proxy decode has already collapsed CFA phase
                    // maps to RGB. Keep the three channels in one RGB opcode;
                    // splitting them into scalar opcodes would apply all three
                    // gains to every channel during the shared bake.
                    params.plane = 0;
                    params.planes = 3;
                    params.gain_data = map.data;
                    result.AddGainMap(params);
                    continue;
                }
                if (cfaPhases && map.channels > 1 && map.channels != 4)
                    throw std::runtime_error("Unsupported DirectLog CFA gain-map channel count");
                // Sidecars are point-major; the shared opcode helper accepts
                // plane-major input and emits one opcode per channel.
                params.gain_data.resize(map.data.size());
                for (size_t point = 0; point < planeSize; ++point)
                    for (uint32_t channel = 0; channel < map.channels; ++channel)
                        params.gain_data[static_cast<size_t>(channel) * planeSize + point] =
                            map.data[point * map.channels + channel];
                utils::addSinglePlaneGainMaps(
                    result, params, cfaPhases && map.channels > 1);
            }
            return result;
        };
        const auto opcodeList2 = makeOpcodeList(opcodeList2Maps, true);
        const auto opcodeList3 = makeOpcodeList(opcodeList3Maps, false);
        if (!opcodeList2.IsEmpty()) dng.SetOpcodeList2(opcodeList2);
        if (!opcodeList3.IsEmpty()) dng.SetOpcodeList3(opcodeList3);
        // Use the same vector-backed writer as MCRAW. Avoid constructing an
        // intermediate string and copying the complete RGB DNG afterward.
        dngData.clear();
        dngData.reserve(imageDataSize + 512 * 1024);
        utils::vector_ostream stream(dngData);
        tinydngwriter::DNGWriter writer(false); // little-endian
        writer.AddImage(&dng);
        
        std::string err;
        diagnosticStage = std::chrono::steady_clock::now();
        if (!writer.WriteToFile(stream, &err)) {
            spdlog::error("Failed to write DNG for frame {}: {}", frameNumber, err);
            return false;
        }
        if (diagnostics)
            spdlog::info("DirectLog diagnostic: frame={} writer_ms={:.3f}",
                         frameNumber, elapsedMilliseconds(diagnosticStage));

        // Apply metadata directly to the vector-backed DNG.
        diagnosticStage = std::chrono::steady_clock::now();
        if (!vfs::applyManualOpcodeSidecar(dngData, mManualVignetteSidecars))
            throw std::runtime_error("Could not apply DirectLog opcode sidecar");
        if (mConfig.vignetteCorrection != VignetteCorrectionMode::Exclude &&
            !vfs::applyManualVignetteSidecar(dngData, mManualVignetteSidecars))
            throw std::runtime_error("Could not apply manual DirectLog vignette sidecar");
        if (!vfs::applyManualDngMetadata(
                dngData, mManualVignetteSidecars,
                mCalibration ? &*mCalibration : nullptr))
            throw std::runtime_error("Could not apply manual DirectLog metadata sidecar");
        if (mConfig.vignetteCorrection == VignetteCorrectionMode::Exclude &&
            (!DNGDecoder::replaceGainMaps(dngData, 2, {}) ||
             !DNGDecoder::replaceGainMaps(dngData, 3, {})))
            throw std::runtime_error("Could not exclude DirectLog gain maps");
        if (!mConfig.cameraNativeStaging &&
            mConfig.vignetteCorrection == VignetteCorrectionMode::Resample) {
            const auto manualResolution =
                vfs::manualVignetteSensorResolution(mManualVignetteSidecars);
            const auto sensorResolution = mCalibration && mCalibration->hasFullSensorResolution
                ? mCalibration->fullSensorResolution : manualResolution;
            if (sensorResolution[0] > 0 && sensorResolution[1] > 0 &&
                !DNGDecoder::cropGainMapsToFullSensor(
                    dngData, sensorResolution[0], sensorResolution[1]))
                throw std::runtime_error("Could not resample DirectLog gain maps for crop");
        }
        if (diagnostics)
            spdlog::info("DirectLog diagnostic: frame={} output_copy_ms={:.3f}",
                         frameNumber, elapsedMilliseconds(diagnosticStage));
        
        return true;
    }
    catch (const std::exception& e) {
        spdlog::error("Exception in convertRGBToDNG for frame {}: {}", frameNumber, e.what());
        return false;
    }
}

VirtualFileSystemImpl_DirectLog::ProcessedFrame
VirtualFileSystemImpl_DirectLog::processFrame(const Entry& entry, int previewScale) {
    const bool profile = std::getenv("MOTIONCAM_GALLERY_PERF_PROFILE");
    const auto processStarted = std::chrono::steady_clock::now();
    ProcessedFrame result;
    result.timestamp = std::get<Timestamp>(entry.userData);
    const auto frameIt = mFrameIndexByTimestamp.find(result.timestamp);
    if (frameIt == mFrameIndexByTimestamp.end())
        throw std::runtime_error("DirectLog source frame not found");
    result.frameNumber = static_cast<int>(frameIt->second);

    result.gainMaps = prepareSidecarGainMaps(result.frameNumber);
    const auto sidecarsFinished = std::chrono::steady_clock::now();
    if (mConfig.vignetteCorrection != VignetteCorrectionMode::Exclude) {
        DNGImageLayout manualTarget;
        manualTarget.width = static_cast<uint32_t>(mWidth);
        manualTarget.height = static_cast<uint32_t>(mHeight);
        manualTarget.bitsPerSample = 16;
        manualTarget.samplesPerPixel = 3;
        manualTarget.pixels = DNGPixelLayout::LinearRGB;
        manualTarget.cfaRepeatSize = 2;
        manualTarget.cfaPhase = directLogCfaPhase(mConfig, mCalibration);
        std::vector<GainMap> manualOpcode2, manualOpcode3;
        if (!vfs::manualVignetteSidecarGainMaps(
                mManualVignetteSidecars, manualTarget,
                manualOpcode2, manualOpcode3))
            throw std::runtime_error("Could not prepare manual DirectLog vignette sidecar");
        if (mManualVignetteSidecars.useDcpGainmap) {
            result.gainMaps.opcodeList2.clear();
            result.gainMaps.opcodeList3.clear();
        }
        if (!manualOpcode2.empty())
            result.gainMaps.opcodeList2 = std::move(manualOpcode2);
        if (!manualOpcode3.empty())
            result.gainMaps.opcodeList3 = std::move(manualOpcode3);
    }
    // Collapse complete CFA-phase maps while their source phase offsets are
    // intact. RGB maps and scalar luminance maps can then be rebased to the
    // proxy geometry without changing which color each map affects.
    std::vector<GainMap> rgbMaps;
    if (previewScale > 1)
        rgbMaps = collapseCfaGainMapsForRgb(
            result.gainMaps.opcodeList2, directLogCfaPhase(mConfig, mCalibration));
    const bool rgbMapsSafe = std::all_of(rgbMaps.begin(), rgbMaps.end(),
        [](const GainMap& map) {
            return validGainMap(map) && map.channels == 3 &&
                map.rowPitch == 1 && map.colPitch == 1;
        });
    const bool luminanceMapsSafe = std::all_of(
        result.gainMaps.opcodeList3.begin(), result.gainMaps.opcodeList3.end(),
        [](const GainMap& map) {
            return validGainMap(map) && map.channels == 1 &&
                map.rowPitch == 1 && map.colPitch == 1;
        });
    const bool scaleInFfmpeg = previewScale > 1 && rgbMapsSafe && luminanceMapsSafe;
    if (scaleInFfmpeg)
        result.gainMaps.opcodeList2 = std::move(rgbMaps);
    result.width = scaleInFfmpeg ? mWidth / previewScale : 0;
    result.height = scaleInFfmpeg ? mHeight / previewScale : 0;
    // Interpolated chroma reconstruction softens colour detail before RGB is
    // sampled back into a Bayer mosaic. Preserve the decoded chroma samples
    // for remosaiced DNG output; RGB output and previews retain smoothing.
    const bool smoothChroma =
        !(mConfig.options & RENDER_OPT_REMOSAIC_TO_BAYER) || mConfig.streamingPreview;
    if (!mDecoder->extractFrame(result.frameNumber, result.rgb, result.width,
                                result.height, false, smoothChroma))
        throw std::runtime_error("Could not decode DirectLog frame");
    const auto extracted = std::chrono::steady_clock::now();
    if (result.width <= 0) result.width = mWidth;
    if (result.height <= 0) result.height = mHeight;

    int sourceLeft = 0, sourceTop = 0;
    int sourceWidth = mWidth, sourceHeight = mHeight;
    auto cropRgb = [&](int targetWidth, int targetHeight, bool centered) {
        if (targetWidth <= 0 || targetHeight <= 0 || targetWidth > result.width ||
            targetHeight > result.height ||
            (targetWidth == result.width && targetHeight == result.height)) return;
        const int left = centered ? (result.width - targetWidth) / 2 : 0;
        const int top = centered ? (result.height - targetHeight) / 2 : 0;
        const double sourcePerPixelX = static_cast<double>(sourceWidth) / result.width;
        const double sourcePerPixelY = static_cast<double>(sourceHeight) / result.height;
        std::vector<uint16_t> cropped(static_cast<size_t>(targetWidth) * targetHeight * 3);
        for (int y = 0; y < targetHeight; ++y)
            std::copy_n(result.rgb.begin() +
                            (static_cast<size_t>(y + top) * result.width + left) * 3,
                        static_cast<size_t>(targetWidth) * 3,
                        cropped.begin() + static_cast<size_t>(y) * targetWidth * 3);
        result.rgb = std::move(cropped);
        sourceLeft += static_cast<int>(std::lround(left * sourcePerPixelX));
        sourceTop += static_cast<int>(std::lround(top * sourcePerPixelY));
        sourceWidth = static_cast<int>(std::lround(targetWidth * sourcePerPixelX));
        sourceHeight = static_cast<int>(std::lround(targetHeight * sourcePerPixelY));
        result.width = targetWidth;
        result.height = targetHeight;
    };
    if (mCalibration && mCalibration->hasLeftTopCropStride)
        cropRgb(mCalibration->leftTopCropStride[0],
                mCalibration->leftTopCropStride[1], false);
    auto remap = [&](std::vector<GainMap>& maps) {
        for (auto& map : maps) {
            const double coordinateWidth = map.coordinateWidth
                ? map.coordinateWidth : mWidth;
            const double coordinateHeight = map.coordinateHeight
                ? map.coordinateHeight : mHeight;
            const double left = sourceLeft * coordinateWidth / mWidth;
            const double top = sourceTop * coordinateHeight / mHeight;
            const double extentWidth = sourceWidth * coordinateWidth / mWidth;
            const double extentHeight = sourceHeight * coordinateHeight / mHeight;
            if (!(extentWidth > 0.0) || !(extentHeight > 0.0)) continue;
            map.originH = (map.originH * coordinateWidth - left) / extentWidth;
            map.originV = (map.originV * coordinateHeight - top) / extentHeight;
            map.spacingH *= coordinateWidth / extentWidth;
            map.spacingV *= coordinateHeight / extentHeight;
            auto x = [&](uint32_t value) {
                return static_cast<uint32_t>(std::clamp(
                    (static_cast<double>(value) - left) * result.width / extentWidth,
                    0.0, static_cast<double>(result.width)));
            };
            auto y = [&](uint32_t value) {
                return static_cast<uint32_t>(std::clamp(
                    (static_cast<double>(value) - top) * result.height / extentHeight,
                    0.0, static_cast<double>(result.height)));
            };
            map.left = x(map.left); map.right = x(map.right);
            map.top = y(map.top); map.bottom = y(map.bottom);
            map.coordinateWidth = result.width;
            map.coordinateHeight = result.height;
        }
    };
    remap(result.gainMaps.opcodeList2);
    remap(result.gainMaps.opcodeList3);

    result.metadata = frameMetadata(result.frameNumber);
    const bool normalizeExposure = mConfig.options & RENDER_OPT_NORMALIZE_EXPOSURE;
    const bool smoothExposure = mConfig.options & RENDER_OPT_SMOOTH_EXPOSURE;
    if (normalizeExposure || smoothExposure) {
        const auto& offsets = smoothExposure
            ? mSmoothedExposureOffsets : mNormalizedExposureOffsets;
        if (offsets.count(result.timestamp))
            result.metadata.baselineExposure = offsets.at(result.timestamp);
    }
    if ((mConfig.options & RENDER_OPT_SMOOTH_WHITE_BALANCE) &&
        mSmoothedAsShotNeutrals.count(result.timestamp))
        result.metadata.asShotNeutral = mSmoothedAsShotNeutrals.at(result.timestamp);
    if (profile)
        spdlog::info("GALLERY_PERF event=directlog_process_stage sidecars_ms={:.3f} extract_ms={:.3f} remaining_ms={:.3f}",
            std::chrono::duration<double, std::milli>(sidecarsFinished-processStarted).count(),
            std::chrono::duration<double, std::milli>(extracted-sidecarsFinished).count(),
            std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now()-extracted).count());
    return result;
}

std::function<std::shared_ptr<std::vector<uint8_t>>()>
VirtualFileSystemImpl_DirectLog::staticMaterializer(const Entry& entry) {
    if (entry.name != "audio.wav") return {};
    return [this, entry] { return materializeFile(entry, false); };
}

std::shared_ptr<std::vector<uint8_t>> VirtualFileSystemImpl_DirectLog::materializeFile(
    const Entry& entry, bool jpegCompression) {
    if (entry.name == "audio.wav") return mAudioWav;
    std::shared_lock renderLock(mRenderMutex);
    return vfs::materializeCached(mCache, entry, jpegCompression, [&] {
        const auto materializeStart = std::chrono::steady_clock::now();
        const auto& frames = mDecoder->getFrames();
        const int configuredScale = std::max(1,
            vfs::getScaleFromOptions(mConfig.options, mConfig.draftScale));
        const bool directProxy = !jpegCompression && !mConfig.streamingPreview &&
            vfs::outputFrameNumber(entry) != 0 && configuredScale > 1 &&
            !(mConfig.options & (RENDER_OPT_CROPPING |
                                 RENDER_OPT_HIGHER_CFA_HQ)) &&
            !(mCalibration && mCalibration->hasLeftTopCropStride) &&
            mWidth / configuredScale > 0 && mHeight / configuredScale > 0;
        const auto queueStarted = std::chrono::steady_clock::now();
        struct DngWriterSlot {
            VirtualFileSystemImpl_DirectLog& owner;
            explicit DngWriterSlot(VirtualFileSystemImpl_DirectLog& value) : owner(value) {
                std::unique_lock lock(owner.mDngWriterMutex);
                owner.mDngWriterAvailable.wait(lock, [&] {
                    return owner.mActiveDngWriters < 4;
                });
                ++owner.mActiveDngWriters;
            }
            ~DngWriterSlot() {
                {
                    std::lock_guard lock(owner.mDngWriterMutex);
                    --owner.mActiveDngWriters;
                }
                owner.mDngWriterAvailable.notify_one();
            }
        } writerSlot(*this);
        const auto acquiredAt = std::chrono::steady_clock::now();
        auto processed = processFrame(entry, directProxy ? configuredScale : 1);
        const auto decodedAt = std::chrono::steady_clock::now();
        const bool scaledInFfmpeg = directProxy &&
            processed.width == mWidth / configuredScale &&
            processed.height == mHeight / configuredScale;
        const auto timestamp = processed.timestamp;
        const int frameNumber = processed.frameNumber;
        const bool diagnostics = directLogDiagnosticsEnabled();
        auto stageStart = std::chrono::steady_clock::now();
        if (diagnostics)
            spdlog::info("DirectLog diagnostic: frame={} materialize begin name={}",
                         frameNumber, entry.name);

        const auto renderPlan = vfs::planDngRender(
            entry, timestamp, frames.front().timestamp, mConfig, mFps,
            jpegCompression);
        const int outputFrameNumber = renderPlan.outputFrameNumber;
        if (diagnostics) {
            spdlog::info("DirectLog diagnostic: frame={} metadata_ms={:.3f}; DNG construction begin",
                         frameNumber, elapsedMilliseconds(stageStart));
            stageStart = std::chrono::steady_clock::now();
        }
        std::vector<uint8_t> dngData;
        if (!convertRGBToDNG(std::move(processed.rgb), dngData, outputFrameNumber,
                             timestamp,
                             processed.metadata.iso, processed.metadata.shutterSpeed,
                             processed.metadata.baselineExposure,
                             processed.metadata.asShotNeutral,
                             processed.metadata.tiffOrientation,
                             &processed.metadata,
                             processed.gainMaps.opcodeList2,
                             processed.gainMaps.opcodeList3,
                             processed.width, processed.height))
            throw std::runtime_error("Could not generate DirectLog DNG");
        const auto generatedAt = std::chrono::steady_clock::now();
        vfs::DngPixelPipelineOptions pixels;
        pixels.hasCfa = false;
        pixels.cfaPhase = directLogCfaPhase(mConfig, mCalibration);
        pixels.outputScale = renderPlan.scale;
        pixels.preScaledProxy = scaledInFfmpeg;
        pixels.inputQuantizationWhite = directLogQuantizationWhite(mConfig);
        pixels.linearInputBitDepth = utils::evenBitsNeeded(static_cast<uint16_t>(
            std::clamp(std::lround(directLogDataLevels(mConfig).white), 1l, 65535l)));
        pixels.sourceName = "DirectLog";
        vfs::processDngPixels(dngData, mConfig, pixels);
        const auto processedAt = std::chrono::steady_clock::now();
        vfs::DngFinalizeOptions finalize;
        finalize.frameRate = mFps;
        finalize.timestamp = renderPlan.outputTimestamp;
        if (mConfig.options & RENDER_OPT_BAKE_ISO)
            finalize.isoOverlay = processed.metadata.iso;
        finalize.packToWhiteLevel = !mConfig.cameraNativeStaging;
        finalize.compression = jpegCompression;
        finalize.gyroflowLensProfile = mGyroflowLensProfile && !mConfig.cameraNativeStaging
            ? &*mGyroflowLensProfile : nullptr;
        finalize.sourceName = "DirectLog";
        vfs::finalizeDng(dngData, mConfig, finalize);
        const auto finalizedAt = std::chrono::steady_clock::now();
        if (!jpegCompression && !mConfig.streamingPreview) {
            if (dngData.size() > entry.size)
                throw std::runtime_error(
                    "Generated DirectLog DNG exceeds advertised mounted size");
            dngData.resize(entry.size, 0);
        }
        auto output = std::make_shared<std::vector<uint8_t>>(std::move(dngData));
        if (std::getenv("MOTIONCAM_GALLERY_PERF_PROFILE")) {
            spdlog::info("GALLERY_PERF event=directlog_mounted_proxy frame={} ffmpeg_proxy={} width={} height={}",
                         frameNumber, scaledInFfmpeg, processed.width,
                         processed.height);
            spdlog::info("GALLERY_PERF event=mounted_frame_stage source=DirectLog source_path={} frame={} queue_ms={:.3f} load_ms={:.3f} generate_ms={:.3f} process_ms={:.3f} finalize_ms={:.3f} total_ms={:.3f} bytes={}",
                         mSrcPath, frameNumber,
                         std::chrono::duration<double, std::milli>(acquiredAt-queueStarted).count(),
                         std::chrono::duration<double, std::milli>(decodedAt-acquiredAt).count(),
                         std::chrono::duration<double, std::milli>(generatedAt-decodedAt).count(),
                         std::chrono::duration<double, std::milli>(processedAt-generatedAt).count(),
                         std::chrono::duration<double, std::milli>(finalizedAt-processedAt).count(),
                         elapsedMilliseconds(materializeStart), output->size());
        }
        if (diagnostics)
            spdlog::info("DirectLog diagnostic: frame={} queue_ms={:.3f} load_ms={:.3f} generate_ms={:.3f} process_ms={:.3f} finalize_ms={:.3f} dng_ms={:.3f} total_ms={:.3f} output_bytes={}",
                         frameNumber,
                         std::chrono::duration<double, std::milli>(acquiredAt-queueStarted).count(),
                         std::chrono::duration<double, std::milli>(decodedAt-acquiredAt).count(),
                         std::chrono::duration<double, std::milli>(generatedAt-decodedAt).count(),
                         std::chrono::duration<double, std::milli>(processedAt-generatedAt).count(),
                         std::chrono::duration<double, std::milli>(finalizedAt-processedAt).count(),
                         elapsedMilliseconds(stageStart),
                         elapsedMilliseconds(materializeStart), output->size());
        return output;
    });
}

bool VirtualFileSystemImpl_DirectLog::materializePreviewFrame(
        const Entry& entry, PreviewFrame& preview, bool retainSourceSamples) {
    std::shared_lock renderLock(mRenderMutex);
    try {
        const auto processStarted = std::chrono::steady_clock::now();
        const int configuredScale = std::max(1,
            vfs::getScaleFromOptions(mConfig.options, mConfig.draftScale));
        const bool directProxy = configuredScale > 1 &&
            !(mConfig.options & (RENDER_OPT_CROPPING |
                                 RENDER_OPT_REMOSAIC_TO_BAYER |
                                 RENDER_OPT_HIGHER_CFA_HQ)) &&
            !(mCalibration && mCalibration->hasLeftTopCropStride) &&
            mWidth / configuredScale > 0 && mHeight / configuredScale > 0;
        auto processed = processFrame(entry, directProxy ? configuredScale : 1);
        const auto processFinished = std::chrono::steady_clock::now();
        DecodedDNGImage image;
        image.samples = std::move(processed.rgb);
        image.layout.width = static_cast<uint32_t>(processed.width);
        image.layout.height = static_cast<uint32_t>(processed.height);
        image.layout.bitsPerSample = 16;
        image.layout.samplesPerPixel = 3;
        image.layout.pixels = DNGPixelLayout::LinearRGB;
        image.layout.cfaRepeatSize = 2;
        image.layout.cfaPhase = directLogCfaPhase(mConfig, mCalibration);
        image.opcodeList2 = std::move(processed.gainMaps.opcodeList2);
        image.opcodeList3 = std::move(processed.gainMaps.opcodeList3);

        const auto levels = directLogDataLevels(mConfig);
        image.metadata.blackLevel = levels.black;
        image.metadata.blackLevelCount = 3;
        image.metadata.whiteLevel.fill(levels.white);
        image.metadata.whiteLevelCount = 3;
        image.metadata.inputBitDepth = utils::evenBitsNeeded(static_cast<uint16_t>(
            std::clamp(std::lround(levels.white), 1l, 65535l)));
        image.metadata.iso = processed.metadata.iso;
        image.metadata.exposureTime = processed.metadata.shutterSpeed;
        image.metadata.hasExposure = processed.metadata.shutterSpeed > 0.0;
        image.metadata.baselineExposure = processed.metadata.baselineExposure;
        image.metadata.hasBaselineExposure = true;
        vfs::applyPreviewExposureOffset(image.metadata, mConfig);
        if (mCalibration && mCalibration->hasAsShotNeutral) {
            image.metadata.asShotNeutral = mCalibration->asShotNeutral;
            image.metadata.hasAsShotNeutral = true;
        } else if (processed.metadata.asShotNeutral) {
            image.metadata.asShotNeutral = *processed.metadata.asShotNeutral;
            image.metadata.hasAsShotNeutral = true;
        }
        auto copyMatrix = [](const auto& source, auto& destination, bool& present) {
            if (source) { destination = *source; present = true; }
        };
        copyMatrix(processed.metadata.colorMatrix1, image.metadata.colorMatrix1,
                   image.metadata.hasColorMatrix1);
        copyMatrix(processed.metadata.colorMatrix2, image.metadata.colorMatrix2,
                   image.metadata.hasColorMatrix2);
        copyMatrix(processed.metadata.forwardMatrix1, image.metadata.forwardMatrix1,
                   image.metadata.hasForwardMatrix1);
        copyMatrix(processed.metadata.forwardMatrix2, image.metadata.forwardMatrix2,
                   image.metadata.hasForwardMatrix2);
        copyMatrix(processed.metadata.cameraCalibration1,
                   image.metadata.cameraCalibration1,
                   image.metadata.hasCameraCalibration1);
        copyMatrix(processed.metadata.cameraCalibration2,
                   image.metadata.cameraCalibration2,
                   image.metadata.hasCameraCalibration2);
        image.metadata.calibrationIlluminant1 =
            processed.metadata.calibrationIlluminant1;
        image.metadata.calibrationIlluminant2 =
            processed.metadata.calibrationIlluminant2;
        vfs::mergeManualDngMetadata(
            image.metadata, mManualVignetteSidecars,
            mCalibration ? &*mCalibration : nullptr);
        if (!image.metadata.calibrationIlluminant1)
            image.metadata.calibrationIlluminant1 = 21;
        if (!image.metadata.calibrationIlluminant2)
            image.metadata.calibrationIlluminant2 = 17;
        if ((mConfig.options & RENDER_OPT_BAKE_ISO) && image.metadata.iso > 0.0)
            utils::bakeIsoOverlay(image.samples.data(), image.layout.width,
                image.layout.height, 3, image.metadata.iso,
                static_cast<uint16_t>(std::clamp(std::lround(levels.black[0]), 0l, 65535l)),
                static_cast<uint16_t>(std::clamp(std::lround(levels.white), 0l, 65535l)));
        image.timestamp = vfs::outputTimestamp(
            entry, processed.timestamp, mDecoder->getFrames().front().timestamp,
            mFps, mConfig.options & RENDER_OPT_FRAMERATE_CONVERSION);
        const auto previewStarted = std::chrono::steady_clock::now();
        auto previewSettings = mConfig;
        const bool scaledInFfmpeg = directProxy &&
            processed.width == mWidth / configuredScale &&
            processed.height == mHeight / configuredScale;
        if (scaledInFfmpeg)
            previewSettings.options = static_cast<FileRenderOptions>(
                previewSettings.options & ~RENDER_OPT_DRAFT);
        const bool decoded = DNGDecoder::decodePreview(
            std::move(image), previewSettings, preview, true, retainSourceSamples);
        if (std::getenv("MOTIONCAM_GALLERY_PERF_PROFILE"))
            spdlog::info("GALLERY_PERF event=directlog_preview_stage source={} ffmpeg_proxy={} width={} height={} process_ms={:.3f} metadata_ms={:.3f} preview_ms={:.3f}",
                mSrcPath, scaledInFfmpeg, processed.width, processed.height,
                std::chrono::duration<double, std::milli>(processFinished-processStarted).count(),
                std::chrono::duration<double, std::milli>(previewStarted-processFinished).count(),
                std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now()-previewStarted).count());
        return decoded;
    } catch (const std::exception&) { return false; }
}

void VirtualFileSystemImpl_DirectLog::updateOptions(const RenderSettings& config) {
    std::unique_lock renderLock(mRenderMutex);
    std::lock_guard<std::mutex> lock(mMutex);

    mCache.clear();
    mConfig = config;
    if (directLogDiagnosticsEnabled())
        spdlog::info("DirectLog diagnostic: settings updated options={} log={} draft_scale={}",
                     optionsToString(mConfig.options),
                     logTransformModeToString(mConfig.logTransform), mConfig.draftScale);
    
    // Reload calibration JSON if it exists
    const auto calibPath = vfs::sidecarPath(mSrcPath);
    mCalibration.reset();
    mSidecarMetadata = nlohmann::json();
    if (boost::filesystem::exists(calibPath)) {
        vfs::loadSidecar(calibPath, mSidecarMetadata, mCalibration, true);
        if (mCalibration.has_value()) {
            if (mCalibration->hasLevels) mConfig.levels = mCalibration->levels;
            if (mCalibration->hasCenterCrop) {
                mConfig.cropTarget = std::to_string(mCalibration->centerCrop[0]) + "x" +
                                     std::to_string(mCalibration->centerCrop[1]);
                mConfig.options |= RENDER_OPT_CROPPING;
            }
            spdlog::info("Reloaded calibration for DirectLog: {}", calibPath.string());
        }
    }
    mGyroflowLensProfile = vfs::loadGyroflowLensProfile(
        vfs::referencedSidecarPath(vfs::gyroflowSidecarPath(mSrcPath),
            mSidecarMetadata, calibPath, "gyroflow"), true);
    mManualVignetteSidecars = vfs::loadManualVignetteSidecars(
        mSrcPath, &mSidecarMetadata, &calibPath);
    analyzeSidecarExposure();
    mDecoder->setFullRangeOverride(std::nullopt);
    if (mCalibration && mCalibration->hasDataLevels) {
        if (mCalibration->dataLevels == "Full")
            mDecoder->setFullRangeOverride(true);
        else if (mCalibration->dataLevels == "Limited")
            mDecoder->setFullRangeOverride(false);
    }
    
    init();
}

FileInfo VirtualFileSystemImpl_DirectLog::getFileInfo() const {
    int outputWidth = mWidth;
    int outputHeight = mHeight;
    auto applyReportedCrop = [&](int width, int height) {
        if (width > 0 && height > 0 && width <= outputWidth && height <= outputHeight) {
            outputWidth = width;
            outputHeight = height;
        }
    };
    // Match processFrame(): the optional left/top source crop is applied
    // first, followed by the configured centered crop.
    if (mCalibration && mCalibration->hasLeftTopCropStride)
        applyReportedCrop(mCalibration->leftTopCropStride[0],
                          mCalibration->leftTopCropStride[1]);
    if (mConfig.options & RENDER_OPT_CROPPING) {
        uint32_t cropWidth = 0, cropHeight = 0, ignoredStride = 0;
        utils::parseCropTarget(mConfig.cropTarget, cropWidth, cropHeight, ignoredStride);
        applyReportedCrop(static_cast<int>(cropWidth), static_cast<int>(cropHeight));
    }
    FileInfo info = vfs::makeFileInfo(
        mFrameRateInfo, mFps, mTotalFrames, mDroppedFrames,
        mDuplicatedFrames, outputWidth, outputHeight);
    info.audioWav = mAudioWav;
    info.orientation = mDecoder->getVideoInfo().orientation;
    if (mCalibration && mCalibration->hasOrientation)
        info.orientation = mCalibration->orientation;
    auto duplicateMask = std::make_shared<std::vector<bool>>();
    for (const auto& entry : mFiles)
        if (boost::filesystem::path(entry.name).extension() == ".dng" ||
            boost::filesystem::path(entry.name).extension() == ".DNG")
            duplicateMask->push_back(entry.duplicateFrame);
    info.duplicateFrameMask = std::move(duplicateMask);
    std::vector<Timestamp> galleryTimestamps;
    galleryTimestamps.reserve(mDecoder->getFrames().size());
    for (const auto& frame : mDecoder->getFrames()) galleryTimestamps.push_back(frame.timestamp);
    vfs::buildGalleryFrameMap(galleryTimestamps, mFiles,
        info.sourceFrameToOutput, info.sourceFrameDuplicated);
    
    // DirectLog's source representation is the encoded video pixel format;
    // CFA overrides describe a later interpretation/rendering step.
    info.dataType = mPixelFormat;
    
    // Determine levels info
    const bool applyLogCurve = directLogAppliesLogTransform(mConfig);
    const std::array<float, 4> sourceBlack{0, 0, 0, 0};
    const auto displayLevels = resolveDataLevels(
        mConfig.levels, 65535.0f, sourceBlack, 65535.0f, sourceBlack, 3);
    int inputBits = utils::bitsNeeded(static_cast<uint16_t>(std::clamp(
        std::lround(displayLevels.white), 1l, 65535l)));
    if ((inputBits & 1) != 0 && inputBits < 16)
        ++inputBits;
    info.levelsInfo = std::to_string(static_cast<int>(displayLevels.white)) + "/" +
        std::to_string(static_cast<int>(displayLevels.black[0]));
    if (applyLogCurve) {
        const int outputBits = directLogLogBits(
            mConfig.logTransform, directLogBaseLogBits(mConfig));
        info.levelsInfo += " -> " +
            std::to_string((1 << outputBits) - 1) + "/0 " +
            std::to_string(outputBits) + "b log";
    } else {
        info.levelsInfo += " " + std::to_string(inputBits) + "b";
    }
    
    // Calculate runtime from video duration
    info.runtimeSeconds = (mFps > 0) ? (static_cast<float>(mTotalFrames) / mFps) : 0.0f;

    const auto& frames = mDecoder->getFrames();
    auto presentationTimestamps = std::make_shared<std::vector<std::int64_t>>();
    presentationTimestamps->reserve(frames.size());
    for (const auto& frame : frames)
        presentationTimestamps->push_back(frame.pts);
    info.presentationTimestamps = std::move(presentationTimestamps);
    if (!frames.empty() && frames.front().timeBase > 0.0) {
        const AVRational timeBase = av_d2q(frames.front().timeBase, INT_MAX);
        info.timingTimeBaseNum = timeBase.num;
        info.timingTimeBaseDen = timeBase.den;
    }
    info.timingUsesCfrMapping = mConfig.options & RENDER_OPT_FRAMERATE_CONVERSION;

    return info;
}

bool VirtualFileSystemImpl_DirectLog::isHLGVideo() const {
    return mIsHLG;
}

void VirtualFileSystemImpl_DirectLog::calculateFrameRateStats() {
    const auto& frames = mDecoder->getFrames();
    
    if (frames.size() < 2) {
        return;
    }
    
    // Convert frame timestamps to vector of Timestamp
    std::vector<Timestamp> timestamps;
    timestamps.reserve(frames.size());
    for (const auto& frame : frames) {
        timestamps.push_back(frame.timestamp);
    }
    
    mFrameRateInfo = vfs::calculateFrameRate(timestamps);
    
    spdlog::debug("DirectLog frame rate stats: avg={:.2f}fps, median={:.2f}fps", mFrameRateInfo.averageFrameRate, mFrameRateInfo.medianFrameRate);
}

} // namespace motioncam
