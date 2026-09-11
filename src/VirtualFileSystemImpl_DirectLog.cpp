#include "VirtualFileSystemImpl_DirectLog.h"
#include "VirtualFileSystemImpl.h"
#include "DirectLogDecoder.h"
#include "DNGDecoder.h"
#include "CalibrationData.h"
#include "Utils.h"
#include "GainMapBake.h"
#include "LRUCache.h"
#include "Types.h"

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
#include <string_view>
#include <thread>
#include <tuple>
#include <QByteArray>

using motioncam::Timestamp;

namespace {

bool directLogDiagnosticsEnabled() {
    static const bool enabled = [] {
        const char* value = std::getenv("MOTIONCAM_DIRECTLOG_DIAGNOSTICS");
        return value && value[0] != '\0' && std::string(value) != "0";
    }();
    return enabled;
}

double elapsedMilliseconds(const std::chrono::steady_clock::time_point start) {
    return std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - start).count();
}

std::pair<uint32_t, uint32_t> legacyGainMapCoordinateExtent(
        uint32_t inputWidth, uint32_t inputHeight,
        uint32_t mapRight, uint32_t mapBottom) {
    // Camera Native sidecars written before coordinateWidth/coordinateHeight
    // used one of these full-sensor coordinate spaces. Pick the smallest one
    // that contains both the encoded video and the gain-map bounds.
    constexpr std::array<std::pair<uint32_t, uint32_t>, 5> sensorExtents{{
        {2048, 1536},
        {4096, 3072},
        {4608, 3456},
        {8192, 6144},
        {9248, 6944},
    }};
    const uint32_t requiredWidth = std::max(inputWidth, mapRight);
    const uint32_t requiredHeight = std::max(inputHeight, mapBottom);
    for (const auto& extent : sensorExtents)
        if (extent.first >= requiredWidth && extent.second >= requiredHeight)
            return extent;
    throw std::runtime_error(
        "DirectLog gain-map coordinates exceed the largest legacy sensor extent");
}

template<typename Function>
void parallelRows(int rows, Function&& function) {
    if (rows < 256) {
        function(0, rows);
        return;
    }
    const unsigned int workers = std::min(4u,
        std::max(1u, std::thread::hardware_concurrency()));
    std::vector<std::thread> threads;
    threads.reserve(workers - 1);
    for (unsigned int worker = 1; worker < workers; ++worker) {
        const int begin = rows * static_cast<int>(worker) / static_cast<int>(workers);
        const int end = rows * static_cast<int>(worker + 1) / static_cast<int>(workers);
        threads.emplace_back([&, begin, end] { function(begin, end); });
    }
    function(0, rows / static_cast<int>(workers));
    for (auto& thread : threads) thread.join();
}

std::vector<uint8_t> packSamples(
    const std::vector<uint16_t>& samples,
    size_t samplesPerRow,
    unsigned int bitsPerSample) {
    if (samplesPerRow == 0 || samples.size() % samplesPerRow != 0)
        throw std::invalid_argument("Invalid dimensions for sample packing");
    const size_t rows = samples.size() / samplesPerRow;
    const size_t rowBytes =
        (samplesPerRow * static_cast<size_t>(bitsPerSample) + 7) / 8;
    std::vector<uint8_t> output(rows * rowBytes, 0);

    // Eight-bit LOG output is by far the most common preview format. Its
    // samples are already byte aligned, so avoid the generic per-bit writer
    // (roughly 200 million loop iterations for one 4K RGB frame).
    if (bitsPerSample == 8) {
        std::transform(samples.begin(), samples.end(), output.begin(),
                       [](uint16_t sample) { return static_cast<uint8_t>(sample); });
        return output;
    }

    const uint16_t mask = bitsPerSample == 16
        ? 0xffffu
        : static_cast<uint16_t>((1u << bitsPerSample) - 1u);

    for (size_t row = 0; row < rows; ++row) {
        size_t bitOffset = row * rowBytes * 8;
        for (size_t column = 0; column < samplesPerRow; ++column) {
            const uint16_t sample = samples[row * samplesPerRow + column] & mask;
            for (int bit = static_cast<int>(bitsPerSample) - 1; bit >= 0; --bit) {
                output[bitOffset / 8] |= static_cast<uint8_t>(
                    ((sample >> bit) & 1u) << (7 - bitOffset % 8));
                ++bitOffset;
            }
        }
    }
    return output;
}

const std::array<uint16_t, 65536>& log60EncodeLut(unsigned int bits) {
    static std::array<std::array<uint16_t, 65536>, 17> luts{};
    static std::array<std::once_flag, 17> initialized;
    if (bits == 0 || bits >= luts.size())
        throw std::invalid_argument("Invalid LOG60 output bit depth");
    std::call_once(initialized[bits], [bits] {
        const float whiteLevel = static_cast<float>((1u << bits) - 1u);
        const float denominator = std::log2(61.0f);
        for (size_t value = 0; value < luts[bits].size(); ++value) {
            const float normalized = static_cast<float>(value) / 65535.0f;
            const float encoded = std::log2(1.0f + 60.0f * normalized) / denominator;
            luts[bits][value] = static_cast<uint16_t>(
                std::clamp(std::round(encoded * whiteLevel), 0.0f, whiteLevel));
        }
    });
    return luts[bits];
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
            spdlog::info("Loaded calibration for DirectLog: {}", calibPath.string());
        }
    }
    
    // Initialize DirectLogDecoder
    try {
        mDecoder = std::make_unique<DirectLogDecoder>(mSrcPath);
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
        // Uncompressed DNG size is independent of sample values. Avoid decoding
        // and color-converting frame zero whenever the mount settings change.
        std::vector<uint16_t> sampleRgbData(
            static_cast<size_t>(mWidth) * mHeight * 3, 0);
        const auto sampleGainMaps = prepareSidecarGainMaps(0);
        const auto sampleMetadata = frameMetadata(0);
        std::vector<uint8_t> sampleDngData;
        if (convertRGBToDNG(sampleRgbData, sampleDngData, 0, frames[0].timestamp,
                            false, 0.0f, {1.0f, 1.0f, 1.0f}, sampleMetadata.iso,
                            sampleMetadata.shutterSpeed, sampleMetadata.baselineExposure,
                            sampleMetadata.asShotNeutral,
                            sampleGainMaps.opcodeList2, sampleGainMaps.opcodeList3)) {
            if (!DNGDecoder::setTimingMetadata(sampleDngData, mFps, 0))
                throw std::runtime_error("Could not size DirectLog DNG timing metadata");
            mTypicalDngSize = sampleDngData.size();
            firstDngSize = mTypicalDngSize;
            spdlog::info("DirectLog DNG size determined from sample: {} bytes ({:.2f} MB)",
                        mTypicalDngSize, mTypicalDngSize / (1024.0 * 1024.0));

            // Draft sequences intentionally expose frame 000000 at native
            // resolution. Calculate its distinct size so the mounted file is
            // not truncated to the proxy frame size.
            if (vfs::getScaleFromOptions(mConfig.options, mConfig.draftScale) > 1) {
                std::vector<uint8_t> firstDngData;
                if (!convertRGBToDNG(sampleRgbData, firstDngData, 0, frames[0].timestamp,
                                     false, 0.0f, {1.0f, 1.0f, 1.0f}, sampleMetadata.iso,
                                     sampleMetadata.shutterSpeed, sampleMetadata.baselineExposure,
                                     sampleMetadata.asShotNeutral,
                                     sampleGainMaps.opcodeList2, sampleGainMaps.opcodeList3,
                                     mWidth, mHeight) ||
                    !DNGDecoder::setTimingMetadata(firstDngData, mFps, 0))
                    throw std::runtime_error("Could not size native DirectLog metadata frame");
                firstDngSize = firstDngData.size();
            }
        } else {
            // Fallback to calculated estimate if conversion fails
            const auto& videoInfo = mDecoder->getVideoInfo();
            mTypicalDngSize = static_cast<size_t>(videoInfo.width) * videoInfo.height * 3 * 2 + (1024 * 1024);
            spdlog::warn("Failed to generate sample DNG, using estimated size: {} bytes", mTypicalDngSize);
        }
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
        const auto& metadata = metadataFrames[i];
        vfs::ExposureSample sample;
        sample.timestamp = sourceFrames[i].timestamp;
        sample.iso = metadata.value("iso", 0.0);
        sample.exposureSeconds = metadata.value("shutterSpeedSeconds", 0.0);
        sample.baselineExposure = metadata.value("baselineExposure", 0.0);
        if (metadata.contains("asShotNeutral") && metadata["asShotNeutral"].is_array() &&
            metadata["asShotNeutral"].size() >= 3) {
            for (size_t c = 0; c < 3; ++c)
                sample.asShotNeutral[c] = metadata["asShotNeutral"][c].get<float>();
        }
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
    if (frameNumber < 0 || !mSidecarMetadata.contains("dynamic") ||
        !mSidecarMetadata["dynamic"].contains("frames") ||
        static_cast<size_t>(frameNumber) >= mSidecarMetadata["dynamic"]["frames"].size())
        return result;
    const auto& metadata = mSidecarMetadata["dynamic"]["frames"][frameNumber];
    result.iso = metadata.value("iso", 0.0);
    result.shutterSpeed = metadata.value("shutterSpeedSeconds", 0.0);
    result.baselineExposure = metadata.value("baselineExposure", 0.0);
    if (metadata.contains("asShotNeutral") && metadata["asShotNeutral"].is_array() &&
        metadata["asShotNeutral"].size() >= 3) {
        result.asShotNeutral = std::array<float, 3>{
            metadata["asShotNeutral"][0].get<float>(),
            metadata["asShotNeutral"][1].get<float>(),
            metadata["asShotNeutral"][2].get<float>()};
    }
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
    auto gainMaps = loadSidecarGainMaps(frameNumber, "gainMaps");
    auto deferredGainMaps = loadSidecarGainMaps(frameNumber, "deferredGainMaps");
    std::string cfaPhase = "bggr";
    if (mCalibration && !mCalibration->cfaPhase.empty())
        cfaPhase = mCalibration->cfaPhase;
    else if (!mConfig.cfaPhase.empty() && mConfig.cfaPhase != "Don't override CFA")
        cfaPhase = mConfig.cfaPhase;
    std::transform(cfaPhase.begin(), cfaPhase.end(), cfaPhase.begin(), ::tolower);
    prepared.cfa = cfaColorsFromPhase(cfaPhase);
    if (mConfig.options & RENDER_OPT_OPTIMIZE_GAIN_MAPS) {
        const auto adjustment = optimizeGainMapLayers<GainMap>(
            std::array{&gainMaps, &deferredGainMaps}, prepared.cfa);
        prepared.exposureOffset = static_cast<float>(adjustment.exposureOffset);
        prepared.neutralScale = adjustment.neutralScale;
    }
    prepared.bakeList2 = gainMaps;
    prepared.bakeList3 = deferredGainMaps;
    if ((mConfig.options & RENDER_OPT_APPLY_VIGNETTE_CORRECTION) &&
        (mConfig.options & RENDER_OPT_VIGNETTE_ONLY_COLOR) &&
        !reduceGainMapStackToColor(prepared.bakeList2))
        throw std::runtime_error("DirectLog gain-map planes have mismatched dimensions");
    if (mConfig.options & RENDER_OPT_APPLY_VIGNETTE_CORRECTION) {
        std::vector<std::vector<GainMap>*> layers{&prepared.bakeList2};
        if (!(mConfig.options & RENDER_OPT_VIGNETTE_ONLY_COLOR))
            layers.push_back(&prepared.bakeList3);
        transformGainMapLayersForBake<GainMap>(layers,
            mConfig.options & RENDER_OPT_NORMALIZE_SHADING_MAP,
            mConfig.options & RENDER_OPT_DEBUG_SHADING_MAP);
    }
    auto classify = [&](const std::vector<GainMap>& maps) {
        if (maps.empty()) return;
        if (maps.size() == 4 || (maps.size() == 1 && maps.front().channels == 4))
            prepared.opcodeList2.insert(prepared.opcodeList2.end(), maps.begin(), maps.end());
        else if (maps.size() == 1 && maps.front().channels == 1)
            prepared.opcodeList3.push_back(maps.front());
        else
            throw std::runtime_error("Unsupported DirectLog gain-map layout");
    };
    if (!(mConfig.options & RENDER_OPT_APPLY_VIGNETTE_CORRECTION)) {
        if (mConfig.options & RENDER_OPT_VIGNETTE_ONLY_COLOR) {
            const bool supported = gainMaps.empty() ||
                (gainMaps.size() == 1 &&
                 (gainMaps.front().channels == 1 || gainMaps.front().channels == 4)) ||
                (gainMaps.size() == 4 &&
                 std::all_of(gainMaps.begin(), gainMaps.end(), [](const GainMap& map) {
                     return map.channels == 1;
                 }));
            if (!supported || !reduceGainMapStackToColor(gainMaps))
                throw std::runtime_error("Unsupported DirectLog color gain-map layout");
            if (!(gainMaps.size() == 1 && gainMaps.front().channels == 1))
                prepared.opcodeList2 = std::move(gainMaps);
            // Single-plane and deferred maps contain only luminance, which is
            // intentionally discarded when correction is reduced to color.
        } else {
            classify(gainMaps);
            classify(deferredGainMaps);
        }
    } else if (mConfig.options & RENDER_OPT_VIGNETTE_ONLY_COLOR) {
        if (deferredGainMaps.size() == 1 && deferredGainMaps.front().channels == 1)
            prepared.opcodeList3 = deferredGainMaps;
        else if (!deferredGainMaps.empty())
            throw std::runtime_error("Unsupported DirectLog deferred gain-map layout");
        else if (gainMaps.size() == 1 && gainMaps.front().channels == 1)
            prepared.opcodeList3 = gainMaps;
    }
    if (!canonicalizeCfaGainMaps(prepared.opcodeList2))
        throw std::runtime_error("Unsupported DirectLog OpcodeList2 gain-map layout");
    return prepared;
}

void VirtualFileSystemImpl_DirectLog::applySidecarGainMaps(
        std::vector<uint16_t>& rgbData, int frameNumber,
        const PreparedSidecarGainMaps& prepared,
        int imageWidth, int imageHeight) const {
    const bool bakeCorrection =
        (mConfig.options & RENDER_OPT_APPLY_VIGNETTE_CORRECTION) != 0;
    const bool optimize = (mConfig.options & RENDER_OPT_OPTIMIZE_GAIN_MAPS) != 0;
    if ((!bakeCorrection && !optimize) || !mSidecarMetadata.contains("dynamic")) return;
    if (imageWidth <= 0) imageWidth = mWidth;
    if (imageHeight <= 0) imageHeight = mHeight;
    if (rgbData.size() < static_cast<size_t>(imageWidth) * imageHeight * 3)
        throw std::runtime_error("DirectLog gain-map image dimensions do not match pixels");
    const auto& dynamic = mSidecarMetadata["dynamic"];
    if (!dynamic.contains("frames") || frameNumber < 0 ||
        static_cast<size_t>(frameNumber) >= dynamic["frames"].size()) return;

    const auto& frame = dynamic["frames"][frameNumber];
    const auto& cfa = prepared.cfa;
    if (bakeCorrection && (mConfig.options & RENDER_OPT_DEBUG_SHADING_MAP))
        std::fill(rgbData.begin(), rgbData.end(), std::numeric_limits<uint16_t>::max());

    const auto& gainMaps = prepared.bakeList2;
    const auto& deferredGainMaps = prepared.bakeList3;
    if (optimize) {
        if (directLogDiagnosticsEnabled())
            spdlog::info(
                "DirectLog diagnostic: gain-map optimization exposure_offset={:.6f}",
                prepared.exposureOffset);
    }
    std::vector<float> combinedGain(rgbData.size(), 1.0f);
    auto apply = [&](const char* field, const std::vector<GainMap>& loadedMaps) {
        if (!frame.contains(field) || !frame[field].is_array()) return;
        if (loadedMaps.empty()) return;
        if (!bakeCorrection ||
            ((mConfig.options & RENDER_OPT_VIGNETTE_ONLY_COLOR) &&
             std::string_view(field) == "deferredGainMaps"))
            return;
        if (frame[field].size() != loadedMaps.size())
            throw std::runtime_error("DirectLog gain-map reference count mismatch");
        for (const auto& preparedMap : loadedMaps) {
            if (!validGainMap(preparedMap))
                throw std::runtime_error("Invalid DirectLog gain-map dimensions");
            const uint32_t channels = preparedMap.channels;
            const uint32_t top = preparedMap.top;
            const uint32_t left = preparedMap.left;
            uint32_t coordinateWidth = preparedMap.coordinateWidth;
            uint32_t coordinateHeight = preparedMap.coordinateHeight;
            if (!coordinateWidth || !coordinateHeight)
                std::tie(coordinateWidth, coordinateHeight) = legacyGainMapCoordinateExtent(
                    static_cast<uint32_t>(mWidth), static_cast<uint32_t>(mHeight),
                    preparedMap.right, preparedMap.bottom);
            if (!coordinateWidth || !coordinateHeight)
                throw std::runtime_error("Invalid DirectLog gain-map coordinate extent");
            // Match the CFA opcode's row/column selection. Pitch-one maps apply
            // to every CFA color (for example deferred luminance); pitch-two
            // maps normally select one or two phases through top/left.
            const auto affectedColors = gainMapAffectedColors(preparedMap, cfa);
            // A single-channel map is the deferred luminance remainder from
            // an earlier color-only bake. Reduce-to-color must leave it
            // deferred; it contains no channel-relative correction to apply.
            if ((mConfig.options & RENDER_OPT_VIGNETTE_ONLY_COLOR) && channels == 1 &&
                affectedColors[0] && affectedColors[1] && affectedColors[2])
                continue;

            const double sourcePerPixelX = static_cast<double>(mWidth) / imageWidth;
            const double sourcePerPixelY = static_cast<double>(mHeight) / imageHeight;
            const bool commonSinglePlane = channels == 1 &&
                affectedColors[0] && affectedColors[1] && affectedColors[2];
            const bool remosaicOutput =
                (mConfig.options & RENDER_OPT_REMOSAIC_TO_BAYER) != 0;
            parallelRows(imageHeight, [&](int beginY, int endY) {
                for (int y = beginY; y < endY; ++y) {
                    const double sensorY = top + (y + 0.5) * sourcePerPixelY - 0.5;
                    for (int x = 0; x < imageWidth; ++x) {
                        const size_t pixel = (static_cast<size_t>(y) * imageWidth + x) * 3;
                        auto interpolatedGain = [&](uint32_t color) {
                            const double sensorX = left + (x + 0.5) * sourcePerPixelX - 0.5;
                            return sampleGainMapColorNormalized(
                                preparedMap, sensorX / coordinateWidth,
                                sensorY / coordinateHeight, color, cfa);
                        };
                        if (commonSinglePlane) {
                            const float gain = interpolatedGain(0);
                            const uint32_t firstColor = remosaicOutput
                                ? cfa[((y & 1) << 1) | (x & 1)] : 0;
                            const uint32_t endColor = remosaicOutput ? firstColor + 1 : 3;
                            for (uint32_t color = firstColor; color < endColor; ++color)
                                combinedGain[pixel + color] *= gain;
                        } else {
                            const uint32_t firstColor = remosaicOutput
                                ? cfa[((y & 1) << 1) | (x & 1)] : 0;
                            const uint32_t endColor = remosaicOutput ? firstColor + 1 : 3;
                            for (uint32_t color = firstColor; color < endColor; ++color) {
                                if (channels == 1 && !affectedColors[color]) continue;
                                combinedGain[pixel + color] *= interpolatedGain(color);
                            }
                        }
                    }
                }
            });
        }
    };
    apply("gainMaps", gainMaps);
    apply("deferredGainMaps", deferredGainMaps);
    if (bakeCorrection) {
        for (size_t sample = 0; sample < rgbData.size(); ++sample)
            rgbData[sample] = bakeLinearGainSample(
                rgbData[sample], combinedGain[sample],
                0.0, 65535.0, 0.0, 65535.0);
    }
}

bool VirtualFileSystemImpl_DirectLog::convertRGBToDNG(
    std::vector<uint16_t> rgbData,
    std::vector<uint8_t>& dngData, 
    int frameNumber, 
    Timestamp timestamp,
    bool jpegCompression, float gainMapExposureOffset,
    const std::array<float, 3>& gainMapNeutralScale, double iso,
    double shutterSpeed, double baselineExposure,
    const std::optional<std::array<float, 3>>& asShotNeutral,
    const std::vector<GainMap>& opcodeList2Maps,
    const std::vector<GainMap>& opcodeList3Maps,
    int decodedWidth, int decodedHeight, bool inputLogEncoded) {

    try {
        const bool diagnostics = directLogDiagnosticsEnabled();
        auto diagnosticStage = std::chrono::steady_clock::now();
        const auto& videoInfo = mDecoder->getVideoInfo();
        int width = decodedWidth > 0 ? decodedWidth : videoInfo.width;
        int height = decodedHeight > 0 ? decodedHeight : videoInfo.height;
        
        // Determine if we should apply log curve and bit reduction
        const bool applyLogCurve =
            (mConfig.options & RENDER_OPT_LOG_TRANSFORM) &&
            mConfig.logTransform != LogTransformMode::Disabled;
        int bitReduction = 0;
        
        if (applyLogCurve) {
            // Parse bit reduction from logTransform option
            if (mConfig.logTransform == LogTransformMode::ReduceBy2Bit) {
                bitReduction = 2;
            } else if (mConfig.logTransform == LogTransformMode::ReduceBy4Bit) {
                bitReduction = 4;
            } else if (mConfig.logTransform == LogTransformMode::ReduceBy6Bit) {
                bitReduction = 6;
            } else if (mConfig.logTransform == LogTransformMode::ReduceBy8Bit) {
                bitReduction = 8;
            } else if (mConfig.logTransform == LogTransformMode::KeepInput) {
                bitReduction = 0;
            }
        }
        
        // Process RGB data: apply log curve to reduce to 12-bit, then apply additional bit reduction
        // The frame materializer transfers ownership here, avoiding an
        // initial full-resolution RGB copy in the playback path.
        std::vector<uint16_t> processedRgbData = std::move(rgbData);
        float dstWhiteLevel = 65535.0f;
        int encodeBits = 16;
        const bool lossyJpegDct = jpegCompression && isLossyJpegDct(mConfig.jxlDistance);
        const bool shouldRemosaic =
            (mConfig.options & RENDER_OPT_REMOSAIC_TO_BAYER) != 0;
        std::string cfaPhase = "bggr";
        if (mCalibration.has_value() && !mCalibration->cfaPhase.empty())
            cfaPhase = mCalibration->cfaPhase;
        else if (!mConfig.cfaPhase.empty() && mConfig.cfaPhase != "Don't override CFA")
            cfaPhase = mConfig.cfaPhase;
        std::transform(cfaPhase.begin(), cfaPhase.end(), cfaPhase.begin(), ::tolower);
        const auto remosaicChannels = cfaColorsFromPhase(cfaPhase);
        std::vector<uint8_t> directlyPackedSamples;

        // Spatial reduction must happen while the decoded RGB values are
        // still linear. Averaging LOG60 values would darken mixed blocks.
        const int proxyScale = vfs::getScaleFromOptions(mConfig.options, mConfig.draftScale);
        if (proxyScale > 1 && decodedWidth <= 0) {
            std::vector<uint16_t> reduced;
            uint32_t reducedWidth = 0, reducedHeight = 0;
            utils::reduceRGB(processedRgbData, reduced,
                             static_cast<uint32_t>(width), static_cast<uint32_t>(height),
                             static_cast<uint32_t>(proxyScale),
                             mConfig.options & RENDER_OPT_HIGHER_CFA_HQ,
                             reducedWidth, reducedHeight);
            if (reduced.empty())
                throw std::runtime_error("Proxy scale is too large for the DirectLog image");
            processedRgbData = std::move(reduced);
            width = static_cast<int>(reducedWidth);
            height = static_cast<int>(reducedHeight);
        }
        
        if (applyLogCurve) {
            // First reduce to 12-bit using log curve
            int useBits = 12;
            dstWhiteLevel = std::pow(2.0f, useBits) - 1.0f; // 4095 for 12-bit
            
            // Apply additional bit reduction if specified
            if (bitReduction > 0) {
                useBits = std::max(1, useBits - bitReduction);
                dstWhiteLevel = std::pow(2.0f, useBits) - 1.0f;
            }

            encodeBits = useBits;
            if (useBits == 8 && !jpegCompression) {
                // Quantize directly into the final byte buffer. Remosaic only
                // writes its selected CFA channel; RGB writes all channels.
                const auto* logLut = inputLogEncoded
                    ? nullptr : &log60EncodeLut(static_cast<unsigned int>(useBits));
                auto quantize = [&](uint16_t sample) {
                    return inputLogEncoded
                        ? static_cast<uint8_t>((static_cast<uint32_t>(sample) * 255u + 32767u) / 65535u)
                        : static_cast<uint8_t>((*logLut)[sample]);
                };
                if (shouldRemosaic) {
                    const size_t pixels = static_cast<size_t>(width) * height;
                    directlyPackedSamples.resize(pixels);
                    for (int y = 0; y < height; ++y)
                        for (int x = 0; x < width; ++x) {
                            const size_t pixel = static_cast<size_t>(y) * width + x;
                            directlyPackedSamples[pixel] = quantize(processedRgbData[
                                pixel * 3 + remosaicChannels[((y & 1) << 1) | (x & 1)]]);
                        }
                } else {
                    directlyPackedSamples.resize(processedRgbData.size());
                    std::transform(processedRgbData.begin(), processedRgbData.end(),
                                   directlyPackedSamples.begin(), quantize);
                }
                processedRgbData.clear();
            } else if (inputLogEncoded) {
                const uint32_t whiteLevel = (1u << useBits) - 1u;
                std::transform(processedRgbData.begin(), processedRgbData.end(),
                               processedRgbData.begin(), [whiteLevel](uint16_t sample) {
                    return static_cast<uint16_t>(
                        (static_cast<uint32_t>(sample) * whiteLevel + 32767u) / 65535u);
                });
            } else {
                const auto& logLut = log60EncodeLut(static_cast<unsigned int>(useBits));
                std::transform(processedRgbData.begin(), processedRgbData.end(),
                               processedRgbData.begin(),
                               [&logLut](uint16_t sample) { return logLut[sample]; });
            }
        }
        
        std::vector<uint16_t> imageSamples;
        int samplesPerPixel = 3;
        int photometric = tinydngwriter::PHOTOMETRIC_LINEARRAW;
        
        if (shouldRemosaic) {
            if (directlyPackedSamples.empty()) {
                // For non-8-bit output, compact RGB to Bayer in place.
                const size_t pixels = static_cast<size_t>(width) * height;
                for (int y = 0; y < height; ++y)
                    for (int x = 0; x < width; ++x) {
                        const size_t pixel = static_cast<size_t>(y) * width + x;
                        const int channel = remosaicChannels[((y & 1) << 1) | (x & 1)];
                        processedRgbData[pixel] = processedRgbData[pixel * 3 + channel];
                    }
                processedRgbData.resize(pixels);
                imageSamples = std::move(processedRgbData);
            }
            
            samplesPerPixel = 1; // Single channel for CFA
            photometric = 32803; // CFA (Color Filter Array)
        } else {
            // Pack RGB data to actual bit depth
            imageSamples = std::move(processedRgbData);
        }

        if (diagnostics) {
            spdlog::info("DirectLog diagnostic: frame={} process_ms={:.3f} samples={} channels={}",
                         frameNumber, elapsedMilliseconds(diagnosticStage),
                         directlyPackedSamples.empty() ? imageSamples.size()
                                                       : directlyPackedSamples.size(),
                         samplesPerPixel);
            diagnosticStage = std::chrono::steady_clock::now();
        }

        const bool writerCompression = jpegCompression && !lossyJpegDct;
        const bool jpegXlCompression = writerCompression && mConfig.jxlDistance >= 0.0f;
        std::vector<uint8_t> imageBytes;
        const uint8_t* imageData = nullptr;
        size_t imageDataSize = 0;
        if (!directlyPackedSamples.empty()) {
            imageBytes = std::move(directlyPackedSamples);
            imageData = imageBytes.data();
            imageDataSize = imageBytes.size();
        } else if (writerCompression || lossyJpegDct || encodeBits == 16) {
            // SetImageData consumes the input synchronously. Use the owned
            // sample storage directly instead of duplicating a 16-bit frame.
            imageData = reinterpret_cast<const uint8_t*>(imageSamples.data());
            imageDataSize = imageSamples.size() * sizeof(uint16_t);
        } else {
            imageBytes = packSamples(
                imageSamples,
                static_cast<size_t>(width) * samplesPerPixel,
                static_cast<unsigned int>(encodeBits));
            imageData = imageBytes.data();
            imageDataSize = imageBytes.size();
        }
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
        
        unsigned short bitsPerSample[3] = {
            static_cast<unsigned short>((jpegXlCompression || lossyJpegDct) ? 16 : encodeBits),
            static_cast<unsigned short>((jpegXlCompression || lossyJpegDct) ? 16 : encodeBits),
            static_cast<unsigned short>((jpegXlCompression || lossyJpegDct) ? 16 : encodeBits)
        };
        dng.SetBitsPerSample(samplesPerPixel, bitsPerSample);
        
        // Photometric interpretation
        dng.SetPhotometric(photometric);
        dng.SetPlanarConfig(1); // Chunky
        dng.SetCompression(writerCompression
            ? (mConfig.jxlDistance < 0.0f ? tinydngwriter::COMPRESSION_JPEG
                                         : tinydngwriter::COMPRESSION_JPEG_XL)
            : tinydngwriter::COMPRESSION_NONE);
        if (writerCompression && mConfig.jxlDistance >= 0.0f)
            dng.SetJXLDistance(mConfig.jxlDistance);
        
        unsigned short sampleFormat[3] = {1, 1, 1}; // Unsigned integer
        dng.SetSampleFormat(samplesPerPixel, sampleFormat);
        
        // Set CFA pattern if remosaicing
        if (shouldRemosaic) {
            auto cfaPattern = cfaColorsFromPhase(cfaPhase);
            dng.SetCFARepeatPatternDim(2, 2);
            dng.SetCFAPattern(4, cfaPattern.data());
            dng.SetCFALayout(1); // Rectangular (or square) layout
            dng.SetBlackLevelRepeatDim(2, 2);
            const unsigned int activeArea[4] = {
                0, 0, static_cast<unsigned int>(height),
                static_cast<unsigned int>(width)
            };
            dng.SetActiveArea(activeArea);
        }
        
        // Set DNG version
        dng.SetDNGVersion(1, jpegXlCompression ? 7 : 4, 0, 0);
        const bool hasStageOpcodes = !opcodeList2Maps.empty() || !opcodeList3Maps.empty();
        if (jpegXlCompression) {
            dng.SetDNGBackwardVersion(1, 7, 0, 0);
        } else if (shouldRemosaic) {
            dng.SetDNGBackwardVersion(1, hasStageOpcodes ? 3 : 1, 0, 0);
        } else {
            dng.SetDNGBackwardVersion(1, 4, 0, 0);
        }
        
        // Set camera/software metadata
        const auto identity = vfs::resolveCameraIdentity(
            mConfig.cameraModel, "DirectLog Video");
        dng.SetUniqueCameraModel(identity.uniqueModel);
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
        if (applyLogCurve) {
            desc << " (Log " << encodeBits << "-bit)";
        }
        if (shouldRemosaic) {
            desc << " (Remosaiced " << cfaPhase << ")";
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
        if (orientation == 90) dng.SetOrientation(6);
        else if (orientation == 180) dng.SetOrientation(3);
        else if (orientation == 270) dng.SetOrientation(8);
        else dng.SetOrientation(1);
        if (mFps > 0.0f) {
            dng.SetFrameRate(mFps);
        }

        // Set baseline exposure with the optional gain offset
        const float exposureOffset = vfs::configuredExposureOffset(mConfig);
        dng.SetBaselineExposure(static_cast<float>(baselineExposure) +
                                exposureOffset + gainMapExposureOffset);
        
        // Set white/black levels and linearization table
        if (applyLogCurve) {
            const auto linearizationTable = utils::makeLogLinearizationTable(
                static_cast<unsigned int>(dstWhiteLevel));
            if (linearizationTable.empty())
                throw std::runtime_error("Invalid DirectLog log white level");
            dng.SetLinearizationTable(static_cast<unsigned int>(linearizationTable.size()),
                                      linearizationTable.data());
            
            // Set black level to 0 and white level to 65534 (as per MCRAW implementation)
            unsigned short blackLevel[4] = {0, 0, 0, 0};
            dng.SetBlackLevel(shouldRemosaic ? 4 : 3, blackLevel);
            dng.SetWhiteLevel(65534);
        } else {
            // Without a linearization table, white level is the maximum value
            // representable by the stored sample bit depth.
            const unsigned int whiteLevel =
                (static_cast<unsigned int>(1) << encodeBits) - 1;
            dng.SetWhiteLevel(whiteLevel);
            unsigned short blackLevel[4] = {0, 0, 0, 0};
            dng.SetBlackLevel(shouldRemosaic ? 4 : 3, blackLevel);
        }
        
        diagnosticStage = std::chrono::steady_clock::now();
        if (!dng.SetImageData(imageData, imageDataSize)) {
            throw std::runtime_error("Failed to attach DirectLog image data");
        }
        if (diagnostics)
            spdlog::info("DirectLog diagnostic: frame={} attach_image_ms={:.3f}",
                         frameNumber, elapsedMilliseconds(diagnosticStage));
        
        // Apply calibration if available
        if (mCalibration.has_value()) {
            if (mCalibration->hasColorMatrix1 || mCalibration->hasForwardMatrix1) {
                dng.SetCalibrationIlluminant1(21); // D65
            }
            if (mCalibration->hasColorMatrix2 || mCalibration->hasForwardMatrix2) {
                dng.SetCalibrationIlluminant2(17); // Standard Light A
            }
            if (mCalibration->hasColorMatrix1) {
                dng.SetColorMatrix1(3, mCalibration->colorMatrix1.data());
            }
            if (mCalibration->hasColorMatrix2) {
                dng.SetColorMatrix2(3, mCalibration->colorMatrix2.data());
            }
            if (mCalibration->hasForwardMatrix1) {
                dng.SetForwardMatrix1(3, mCalibration->forwardMatrix1.data());
            }
            if (mCalibration->hasForwardMatrix2) {
                dng.SetForwardMatrix2(3, mCalibration->forwardMatrix2.data());
            }
        }
        auto outputNeutral = asShotNeutral;
        if (!outputNeutral && mCalibration && mCalibration->hasAsShotNeutral)
            outputNeutral = mCalibration->asShotNeutral;
        if (outputNeutral) {
            for (size_t color = 0; color < outputNeutral->size(); ++color)
                (*outputNeutral)[color] *= gainMapNeutralScale[color];
            dng.SetAsShotNeutral(3, outputNeutral->data());
        }
        auto makeOpcodeList = [&](const std::vector<GainMap>& maps, bool cfaPhases) {
            tinydngwriter::OpcodeList result;
            if (maps.empty()) return result;
            const uint32_t cropLeft = std::min_element(maps.begin(), maps.end(),
                [](const GainMap& a, const GainMap& b) { return a.left < b.left; })->left;
            const uint32_t cropTop = std::min_element(maps.begin(), maps.end(),
                [](const GainMap& a, const GainMap& b) { return a.top < b.top; })->top;
            for (const auto& map : maps) {
                if (!map.coordinateWidth || !map.coordinateHeight ||
                    map.left < cropLeft || map.top < cropTop ||
                    map.right <= cropLeft || map.bottom <= cropTop)
                    throw std::runtime_error("Invalid DirectLog opcode coordinate geometry");
                tinydngwriter::GainMapParams params{};
                params.top = map.top - cropTop; params.left = map.left - cropLeft;
                params.bottom = std::min<uint32_t>(mHeight, map.bottom - cropTop);
                params.right = std::min<uint32_t>(mWidth, map.right - cropLeft);
                params.plane = map.plane; params.planes = map.planes;
                params.row_pitch = map.rowPitch; params.col_pitch = map.colPitch;
                params.map_points_v = map.height; params.map_points_h = map.width;
                params.map_spacing_v = map.spacingV * map.coordinateHeight / mHeight;
                params.map_spacing_h = map.spacingH * map.coordinateWidth / mWidth;
                params.map_origin_v =
                    (map.originV * map.coordinateHeight - cropTop) / mHeight;
                params.map_origin_h =
                    (map.originH * map.coordinateWidth - cropLeft) / mWidth;
                params.map_planes = map.channels;
                if (!cfaPhases && map.channels == 1) {
                    params.top = 0;
                    params.left = 0;
                    params.bottom = mHeight;
                    params.right = mWidth;
                    params.plane = 0;
                    params.planes = 3;
                    params.row_pitch = 1;
                    params.col_pitch = 1;
                }
                const size_t planeSize = static_cast<size_t>(map.width) * map.height;
                if (!map.channels || map.data.size() != planeSize * map.channels)
                    throw std::runtime_error("Invalid DirectLog opcode gain-map payload");
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
        // Write DNG to memory stream
        std::ostringstream oss;
        tinydngwriter::DNGWriter writer(false); // little-endian
        writer.AddImage(&dng);
        
        std::string err;
        diagnosticStage = std::chrono::steady_clock::now();
        if (!writer.WriteToFile(oss, &err)) {
            spdlog::error("Failed to write DNG for frame {}: {}", frameNumber, err);
            return false;
        }
        if (diagnostics)
            spdlog::info("DirectLog diagnostic: frame={} writer_ms={:.3f}",
                         frameNumber, elapsedMilliseconds(diagnosticStage));

        // Copy to output vector
        diagnosticStage = std::chrono::steady_clock::now();
        std::string dngStr = std::move(oss).str();
        dngData.assign(dngStr.begin(), dngStr.end());
        if (lossyJpegDct && !DNGDecoder::compressLossyJPEG(dngData))
            throw std::runtime_error("Failed to enable lossy JPEG DCT compression");
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
VirtualFileSystemImpl_DirectLog::processFrame(const Entry& entry, bool dngOutput) {
    ProcessedFrame result;
    result.timestamp = std::get<Timestamp>(entry.userData);
    const auto frameIt = mFrameIndexByTimestamp.find(result.timestamp);
    if (frameIt == mFrameIndexByTimestamp.end())
        throw std::runtime_error("DirectLog source frame not found");
    result.frameNumber = static_cast<int>(frameIt->second);

    const int proxyScale = vfs::getScaleFromOptions(mConfig.options, mConfig.draftScale);
    const bool sequenceMetadataFrame = dngOutput && !mConfig.streamingPreview &&
        vfs::outputFrameNumber(entry) == 0;
    const bool directProxyDecode = proxyScale > 1 && !sequenceMetadataFrame &&
        !(mConfig.options & RENDER_OPT_HIGHER_CFA_HQ);
    result.width = sequenceMetadataFrame && proxyScale > 1
        ? mWidth : (directProxyDecode ? mWidth / proxyScale : 0);
    result.height = sequenceMetadataFrame && proxyScale > 1
        ? mHeight : (directProxyDecode ? mHeight / proxyScale : 0);

    result.gainMaps = prepareSidecarGainMaps(result.frameNumber);
    const bool bakesGainMaps = !result.gainMaps.bakeList2.empty() ||
                               !result.gainMaps.bakeList3.empty();
    result.inputLogEncoded = dngOutput && mDecoder->getVideoInfo().isLOG60 &&
        (mConfig.options & RENDER_OPT_LOG_TRANSFORM) &&
        mConfig.logTransform != LogTransformMode::Disabled && !bakesGainMaps;
    if (!mDecoder->extractFrame(result.frameNumber, result.rgb, result.width,
                                result.height, result.inputLogEncoded))
        throw std::runtime_error("Could not decode DirectLog frame");
    applySidecarGainMaps(result.rgb, result.frameNumber, result.gainMaps,
                         result.width, result.height);

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
    return result;
}

std::shared_ptr<std::vector<char>> VirtualFileSystemImpl_DirectLog::materializeFile(
    const Entry& entry, bool jpegCompression) {
    std::shared_lock renderLock(mRenderMutex);
    return vfs::materializeCached(mCache, entry, jpegCompression, [&] {
        const auto materializeStart = std::chrono::steady_clock::now();
        const auto& frames = mDecoder->getFrames();
        auto processed = processFrame(entry, true);
        const auto timestamp = processed.timestamp;
        const int frameNumber = processed.frameNumber;
        const bool diagnostics = directLogDiagnosticsEnabled();
        auto stageStart = std::chrono::steady_clock::now();
        if (diagnostics)
            spdlog::info("DirectLog diagnostic: frame={} materialize begin name={}",
                         frameNumber, entry.name);

        const int outputFrameNumber = vfs::outputFrameNumber(entry);
        if (diagnostics) {
            spdlog::info("DirectLog diagnostic: frame={} metadata_ms={:.3f}; DNG construction begin",
                         frameNumber, elapsedMilliseconds(stageStart));
            stageStart = std::chrono::steady_clock::now();
        }
        struct DngWriterSlot {
            VirtualFileSystemImpl_DirectLog& owner;
            explicit DngWriterSlot(VirtualFileSystemImpl_DirectLog& value) : owner(value) {
                std::unique_lock lock(owner.mDngWriterMutex);
                owner.mDngWriterAvailable.wait(lock, [&] {
                    return owner.mActiveDngWriters < 2;
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
        std::vector<uint8_t> dngData;
        if (!convertRGBToDNG(std::move(processed.rgb), dngData, outputFrameNumber,
                             timestamp, jpegCompression,
                             processed.gainMaps.exposureOffset,
                             processed.gainMaps.neutralScale,
                             processed.metadata.iso, processed.metadata.shutterSpeed,
                             processed.metadata.baselineExposure,
                             processed.metadata.asShotNeutral,
                             processed.gainMaps.opcodeList2,
                             processed.gainMaps.opcodeList3,
                             processed.width, processed.height,
                             processed.inputLogEncoded))
            throw std::runtime_error("Could not generate DirectLog DNG");
        const bool converted = mConfig.options & RENDER_OPT_FRAMERATE_CONVERSION;
        const Timestamp outputTimestamp = vfs::outputTimestamp(
            entry, timestamp, frames.front().timestamp, mFps, converted);
        if (!DNGDecoder::setTimingMetadata(dngData, mFps, outputTimestamp))
            throw std::runtime_error("Could not write DirectLog DNG timing metadata");
        auto output = std::make_shared<std::vector<char>>(dngData.begin(), dngData.end());
        if (diagnostics)
            spdlog::info("DirectLog diagnostic: frame={} dng_ms={:.3f} total_ms={:.3f} output_bytes={}",
                         frameNumber, elapsedMilliseconds(stageStart),
                         elapsedMilliseconds(materializeStart), output->size());
        return output;
    });
}

bool VirtualFileSystemImpl_DirectLog::materializePreviewFrame(
        const Entry& entry, PreviewFrame& preview) {
    std::shared_lock renderLock(mRenderMutex);
    ProcessedFrame processed;
    try { processed = processFrame(entry, false); }
    catch (const std::exception&) { return false; }
    auto& metadata = processed.metadata;
    auto& gainMaps = processed.gainMaps;
    preview.width = processed.width > 0 ? static_cast<uint32_t>(processed.width)
                                     : static_cast<uint32_t>(mWidth);
    preview.height = processed.height > 0 ? static_cast<uint32_t>(processed.height)
                                       : static_cast<uint32_t>(mHeight);
    preview.rgb.resize(processed.rgb.size() * sizeof(uint16_t));
    std::memcpy(preview.rgb.data(), processed.rgb.data(), preview.rgb.size());
    preview.timestamp = vfs::outputTimestamp(
        entry, processed.timestamp, mDecoder->getFrames().front().timestamp, mFps,
        mConfig.options & RENDER_OPT_FRAMERATE_CONVERSION);
    preview.metadata.iso = metadata.iso;
    preview.metadata.exposureTime = metadata.shutterSpeed;
    preview.metadata.baselineExposure = metadata.baselineExposure +
        vfs::configuredExposureOffset(mConfig) + gainMaps.exposureOffset;
    preview.metadata.hasExposure = metadata.shutterSpeed > 0.0;
    preview.metadata.hasBaselineExposure = true;
    auto neutral = metadata.asShotNeutral;
    if (!neutral && mCalibration && mCalibration->hasAsShotNeutral)
        neutral = mCalibration->asShotNeutral;
    if (neutral) {
        for (size_t channel = 0; channel < 3; ++channel)
            (*neutral)[channel] *= gainMaps.neutralScale[channel];
        preview.metadata.asShotNeutral = *neutral;
        preview.metadata.hasAsShotNeutral = true;
    }
    if (mCalibration) {
        preview.metadata.colorMatrix1 = mCalibration->colorMatrix1;
        preview.metadata.colorMatrix2 = mCalibration->colorMatrix2;
        preview.metadata.forwardMatrix1 = mCalibration->forwardMatrix1;
        preview.metadata.forwardMatrix2 = mCalibration->forwardMatrix2;
        preview.metadata.hasColorMatrix1 = mCalibration->hasColorMatrix1;
        preview.metadata.hasColorMatrix2 = mCalibration->hasColorMatrix2;
        preview.metadata.hasForwardMatrix1 = mCalibration->hasForwardMatrix1;
        preview.metadata.hasForwardMatrix2 = mCalibration->hasForwardMatrix2;
        if (mCalibration->hasColorMatrix1 || mCalibration->hasForwardMatrix1)
            preview.metadata.calibrationIlluminant1 = 21;
        if (mCalibration->hasColorMatrix2 || mCalibration->hasForwardMatrix2)
            preview.metadata.calibrationIlluminant2 = 17;
    }
    return preview.rgb.size() == static_cast<size_t>(preview.width) * preview.height * 6;
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
            spdlog::info("Reloaded calibration for DirectLog: {}", calibPath.string());
        }
    }
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
    FileInfo info = vfs::makeFileInfo(
        mFrameRateInfo, mFps, mTotalFrames, mDroppedFrames,
        mDuplicatedFrames, mWidth, mHeight);
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
    
    // This describes the input, not the selected DNG render operation. A
    // companion JSON may explicitly reinterpret the input CFA size.
    const int cfaSize = mCalibration && mCalibration->hasCfaSize && mCalibration->cfaSize > 0
        ? mCalibration->cfaSize : 0;
    info.dataType = vfs::getDisplayDataType(true, cfaSize);
    
    // Determine levels info
    const bool applyLogCurve =
        (mConfig.options & RENDER_OPT_LOG_TRANSFORM) &&
        mConfig.logTransform != LogTransformMode::Disabled;
    int srcBits = 16;
    int dstBits = 16;
    
    if (applyLogCurve) {
        // Strip " lq" suffix if present for comparison
        dstBits = 12; // Start with 12-bit log
        if (mConfig.logTransform == LogTransformMode::ReduceBy2Bit) {
            dstBits = 10;
        } else if (mConfig.logTransform == LogTransformMode::ReduceBy4Bit) {
            dstBits = 8;
        } else if (mConfig.logTransform == LogTransformMode::ReduceBy6Bit) {
            dstBits = 6;
        } else if (mConfig.logTransform == LogTransformMode::ReduceBy8Bit) {
            dstBits = 4;
        }
    }
    
    int srcWhiteLevel = (1 << srcBits) - 1;
    int dstWhiteLevel = (1 << dstBits) - 1;
    
    info.levelsInfo = std::to_string(srcWhiteLevel) + "/0 -> " + 
                      std::to_string(dstWhiteLevel) + "/0 " + std::to_string(dstBits) + "b" +
                      (applyLogCurve ? " log" : "");
    
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
