#include "VirtualFileSystemImpl_MCRAW.h"
#include "VirtualFileSystemImpl.h"
#include "CameraFrameMetadata.h"
#include "CameraMetadata.h"
#include "CalibrationData.h"
#include "Utils.h"
#include "AudioWriter.h"
#include "LRUCache.h"
#include "DNGDecoder.h"
#include "GainMapBake.h"
#include "DataLevels.h"

#include <motioncam/Decoder.hpp>

#include <boost/filesystem.hpp>
#include <boost/regex.hpp>
#include <boost/algorithm/string.hpp>

#include <BS_thread_pool.hpp>
#include <spdlog/spdlog.h>
#include <audiofile/AudioFile.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <mutex>
#include <tuple>

namespace {
struct CachedMcrawAnalysis {
    std::vector<motioncam::Timestamp> frames;
    double baselineExposure = 0.0;
    std::map<motioncam::Timestamp, float> smoothedExposure;
    std::map<motioncam::Timestamp, std::array<float, 3>> smoothedNeutral;
};
std::mutex mcrawAnalysisCacheMutex;
std::unordered_map<std::string, CachedMcrawAnalysis> mcrawAnalysisCache;
constexpr uint64_t mcrawAnalysisCacheMagic = 0x4d43464d43000001ULL;

std::string effectiveCfaArrangement(
        const motioncam::RenderSettings& settings,
        const std::optional<motioncam::CalibrationData>& calibration,
        std::string sensorArrangement) {
    auto validOverride = [](std::string phase) {
        boost::algorithm::to_lower(phase);
        return phase == "rggb" || phase == "bggr" ||
               phase == "grbg" || phase == "gbrg" ? phase : std::string{};
    };
    if (!settings.cfaPhase.empty() && settings.cfaPhase != "Don't override CFA") {
        if (auto phase = validOverride(settings.cfaPhase); !phase.empty()) return phase;
    } else if (calibration && !calibration->cfaPhase.empty()) {
        if (auto phase = validOverride(calibration->cfaPhase); !phase.empty()) return phase;
    }
    boost::algorithm::to_lower(sensorArrangement);
    return sensorArrangement;
}

void reorderNativeShadingMapToCfaPhases(
        motioncam::CameraFrameMetadata& metadata,
        std::string sensorArrangement) {
    if (metadata.lensShadingMap.size() != 4) return;
    boost::algorithm::to_lower(sensorArrangement);
    if (sensorArrangement != "rggb" && sensorArrangement != "bggr" &&
        sensorArrangement != "grbg" && sensorArrangement != "gbrg") return;
    const auto cfa = motioncam::cfaColorsFromPhase(sensorArrangement);

    // MotionCam stores Android LensShadingMap channels as R, G-even,
    // G-odd, B. The processing and DNG opcode paths use spatial CFA phases.
    const auto channels = metadata.lensShadingMap;
    for (size_t phase = 0; phase < cfa.size(); ++phase) {
        const size_t source = cfa[phase] == 0 ? 0
            : cfa[phase] == 2 ? 3
            : phase / 2 == 0 ? 1 : 2;
        metadata.lensShadingMap[phase] = channels[source];
    }
}

std::string mcrawAnalysisCacheKey(const std::string& path) {
    std::error_code error;
    const auto absolute = std::filesystem::absolute(path, error).lexically_normal();
    const auto size = std::filesystem::file_size(absolute, error);
    if (error) return absolute.string();
    const auto modified = std::filesystem::last_write_time(absolute, error);
    return "timestamp-repair-v1:" + absolute.string() + ":" + std::to_string(size) +
           (error ? "" : ":" + std::to_string(
               static_cast<long long>(modified.time_since_epoch().count())));
}

std::filesystem::path mcrawAnalysisCachePath(const std::string& key) {
    const char* cacheRoot = std::getenv("XDG_CACHE_HOME");
    std::filesystem::path root;
    if (cacheRoot && cacheRoot[0] != '\0') root = cacheRoot;
    else if (const char* home = std::getenv("HOME")) root = std::filesystem::path(home) / ".cache";
    else root = std::filesystem::temp_directory_path();
    return root / "motioncam-fuse" / "mcraw-analysis" /
           (std::to_string(std::hash<std::string>{}(key)) + ".bin");
}

template<typename T> bool readCacheValue(std::istream& input, T& value) {
    return static_cast<bool>(input.read(reinterpret_cast<char*>(&value), sizeof(value)));
}
template<typename T> void writeCacheValue(std::ostream& output, const T& value) {
    output.write(reinterpret_cast<const char*>(&value), sizeof(value));
}
bool readCacheString(std::istream& input, std::string& value) {
    uint32_t size = 0;
    if (!readCacheValue(input, size) || size > 16384) return false;
    value.resize(size);
    return size == 0 || static_cast<bool>(input.read(value.data(), size));
}
void writeCacheString(std::ostream& output, const std::string& value) {
    const uint32_t size = static_cast<uint32_t>(value.size());
    writeCacheValue(output, size);
    output.write(value.data(), size);
}
bool loadPersistentMcrawAnalysis(const std::string& key, CachedMcrawAnalysis& cached) {
    std::ifstream input(mcrawAnalysisCachePath(key), std::ios::binary);
    uint64_t magic = 0, frameCount = 0, exposureCount = 0, neutralCount = 0;
    std::string storedKey;
    if (!readCacheValue(input, magic) || magic != mcrawAnalysisCacheMagic ||
        !readCacheString(input, storedKey) || storedKey != key ||
        !readCacheValue(input, cached.baselineExposure) ||
        !readCacheValue(input, frameCount) || frameCount > 10000000) return false;
    cached.frames.resize(static_cast<size_t>(frameCount));
    for (auto& frame : cached.frames) if (!readCacheValue(input, frame)) return false;
    if (!readCacheValue(input, exposureCount) || exposureCount > frameCount) return false;
    for (uint64_t index = 0; index < exposureCount; ++index) {
        motioncam::Timestamp timestamp;
        float value;
        if (!readCacheValue(input, timestamp) || !readCacheValue(input, value)) return false;
        cached.smoothedExposure.emplace(timestamp, value);
    }
    if (!readCacheValue(input, neutralCount) || neutralCount > frameCount) return false;
    for (uint64_t index = 0; index < neutralCount; ++index) {
        motioncam::Timestamp timestamp;
        std::array<float, 3> value;
        if (!readCacheValue(input, timestamp) ||
            !input.read(reinterpret_cast<char*>(value.data()), sizeof(value))) return false;
        cached.smoothedNeutral.emplace(timestamp, value);
    }
    return !cached.frames.empty() && input.peek() == std::char_traits<char>::eof();
}
void savePersistentMcrawAnalysis(const std::string& key, const CachedMcrawAnalysis& cached) {
    const auto path = mcrawAnalysisCachePath(key);
    std::error_code error;
    std::filesystem::create_directories(path.parent_path(), error);
    if (error) return;
    const auto temporary = path.string() + ".tmp";
    std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
    if (!output) return;
    writeCacheValue(output, mcrawAnalysisCacheMagic);
    writeCacheString(output, key);
    writeCacheValue(output, cached.baselineExposure);
    writeCacheValue(output, static_cast<uint64_t>(cached.frames.size()));
    for (const auto frame : cached.frames) writeCacheValue(output, frame);
    writeCacheValue(output, static_cast<uint64_t>(cached.smoothedExposure.size()));
    for (const auto& [timestamp, value] : cached.smoothedExposure) {
        writeCacheValue(output, timestamp); writeCacheValue(output, value);
    }
    writeCacheValue(output, static_cast<uint64_t>(cached.smoothedNeutral.size()));
    for (const auto& [timestamp, value] : cached.smoothedNeutral) {
        writeCacheValue(output, timestamp);
        output.write(reinterpret_cast<const char*>(value.data()), sizeof(value));
    }
    output.close();
    if (!output) { std::filesystem::remove(temporary, error); return; }
    std::filesystem::rename(temporary, path, error);
    if (error) {
        std::filesystem::remove(path, error);
        error.clear();
        std::filesystem::rename(temporary, path, error);
    }
}

double elapsedMs(const std::chrono::steady_clock::time_point start) {
    return std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - start).count();
}
bool galleryDiagnosticsEnabled() {
    const char* value = std::getenv("MOTIONCAM_GALLERY_DIAGNOSTICS");
    return value && value[0] != '\0' && std::string(value) != "0";
}
}

namespace motioncam {

VirtualFileSystemImpl_MCRAW::VirtualFileSystemImpl_MCRAW(
        BS::thread_pool&,
        BS::thread_pool& processingThreadPool,
        LRUCache& lruCache,
        const RenderSettings& settings,
        const std::string& file,
        const std::string& baseName) :
        MountedDngSource(lruCache, processingThreadPool),
        mSrcPath(file),
        mBaseName(baseName),
        mSettings(settings) {
    const auto initializationStarted = std::chrono::steady_clock::now();
    mSettings.draftScale =
        vfs::getScaleFromOptions(mSettings.options, mSettings.draftScale);
    
    // Load calibration JSON if it exists
    const auto calibPath = vfs::sidecarPath(mSrcPath);
    if (boost::filesystem::exists(calibPath)) {
        vfs::loadSidecar(calibPath, mSidecarMetadata, mCalibration);
        if (mCalibration.has_value()) {
            spdlog::info("Loaded calibration for MCRAW: {}", calibPath.string());
        }
    }
    
    const auto cacheKey = mcrawAnalysisCacheKey(mSrcPath);
    bool reusedAnalysis = false;
    {
        std::lock_guard lock(mcrawAnalysisCacheMutex);
        if (const auto cached = mcrawAnalysisCache.find(cacheKey);
            cached != mcrawAnalysisCache.end()) {
            mSourceFrames = cached->second.frames;
            mBaselineExpValue = cached->second.baselineExposure;
            mSmoothedExposureOffsets = cached->second.smoothedExposure;
            mSmoothedAsShotNeutrals = cached->second.smoothedNeutral;
            reusedAnalysis = true;
        } else {
            CachedMcrawAnalysis persistent;
            if (loadPersistentMcrawAnalysis(cacheKey, persistent)) {
                mSourceFrames = persistent.frames;
                mBaselineExpValue = persistent.baselineExposure;
                mSmoothedExposureOffsets = persistent.smoothedExposure;
                mSmoothedAsShotNeutrals = persistent.smoothedNeutral;
                mcrawAnalysisCache.emplace(cacheKey, std::move(persistent));
                reusedAnalysis = true;
            }
        }
    }
    if (!reusedAnalysis) {
        Decoder decoder(mSrcPath);
        auto frames = decoder.getFrames();
        std::sort(frames.begin(), frames.end());
        if(frames.empty()) return;
        mSourceFrames = frames;
        mBaselineExpValue = std::numeric_limits<double>::max();
        nlohmann::json metadata;
        std::vector<vfs::ExposureSample> exposureSamples;
        exposureSamples.reserve(frames.size());
        for(const auto& frame : frames) {
            decoder.loadFrameMetadata(frame, metadata);
            const auto cameraFrameMetadata = CameraFrameMetadata::limitedParse(metadata);
            mBaselineExpValue = std::min(mBaselineExpValue,
                cameraFrameMetadata.iso * cameraFrameMetadata.exposureTime);
            vfs::ExposureSample sample;
            sample.timestamp = frame;
            sample.iso = cameraFrameMetadata.iso;
            sample.exposureSeconds = cameraFrameMetadata.exposureTime;
            for (size_t channel = 0; channel < 3; ++channel) {
                const double neutral = metadata.contains("asShotNeutral") &&
                        metadata["asShotNeutral"].is_array() &&
                        metadata["asShotNeutral"].size() > channel
                    ? metadata["asShotNeutral"][channel].get<double>() : 1.0;
                sample.asShotNeutral[channel] = static_cast<float>(neutral);
            }
            exposureSamples.push_back(sample);
        }
        const double frameRate = frames.size() > 1
            ? vfs::calculateFrameRate(frames).medianFrameRate : 30.0;
        const auto analysis = vfs::analyzeExposureMetadata(exposureSamples, frameRate);
        mSmoothedExposureOffsets = analysis.smoothedBaseline;
        mSmoothedAsShotNeutrals = analysis.smoothedNeutral;
        std::lock_guard lock(mcrawAnalysisCacheMutex);
        CachedMcrawAnalysis cached{mSourceFrames, mBaselineExpValue,
            mSmoothedExposureOffsets, mSmoothedAsShotNeutrals};
        mcrawAnalysisCache[cacheKey] = cached;
        savePersistentMcrawAnalysis(cacheKey, cached);
    }
    mFrameIndexByTimestamp = vfs::indexTimestamps(mSourceFrames);
    if (galleryDiagnosticsEnabled())
        spdlog::info("GALLERY_PERF event=mcraw_analysis source={} reused={} frames={} latency_ms={:.3f}",
                     mSrcPath, reusedAnalysis, mSourceFrames.size(),
                     elapsedMs(initializationStarted));
    init();
    if (galleryDiagnosticsEnabled())
        spdlog::info("GALLERY_PERF event=mcraw_vfs_init source={} reused_analysis={} total_ms={:.3f}",
                     mSrcPath, reusedAnalysis, elapsedMs(initializationStarted));
}

VirtualFileSystemImpl_MCRAW::~VirtualFileSystemImpl_MCRAW() {
    spdlog::info("Destroying VirtualFileSystemImpl_MCRAW({})", mSrcPath);
}

void VirtualFileSystemImpl_MCRAW::init() {
    Decoder decoder(mSrcPath);
    const auto& frames = mSourceFrames;

    if(frames.empty())
        return;

    spdlog::debug("VirtualFileSystemImpl_MCRAW::init(options={})", optionsToString(mSettings.options));

    // Clear everything
    mFiles.clear();

    mFrameRateInfo = vfs::calculateFrameRate(frames);

    bool applyCFRConversion = mSettings.options & RENDER_OPT_FRAMERATE_CONVERSION;
    mFps = vfs::determineCFRTarget(mFrameRateInfo, mSettings.cfrTarget, applyCFRConversion);

    // Calculate typical DNG size that we can use for all files
    nlohmann::json metadata;

    uint32_t cropWidth = 0, cropHeight = 0, strideOverride = 0;
    if (mSettings.options & RENDER_OPT_CROPPING)
        utils::parseCropTarget(mSettings.cropTarget, cropWidth, cropHeight, strideOverride);
    decoder.loadFrameMetadata(frames[0], metadata);

    auto cameraConfig = CameraConfiguration::parse(decoder.getContainerMetadata());
    auto cameraFrameMetadata = CameraFrameMetadata::parse(metadata);
    reorderNativeShadingMapToCfaPhases(
        cameraFrameMetadata, effectiveCfaArrangement(
            mSettings, mCalibration, cameraConfig.sensorArrangement));
    utils::overrideLensShadingMap(cameraFrameMetadata,
        vfs::loadSidecarGainMaps(mSidecarMetadata, 0, "gainMaps"));

    // Match DNG-sequence sizing by accounting for the metadata that will be
    // serialized alongside the pixels. JSON sidecars may store compressed gain
    // maps, whereas DNG opcodes contain their expanded float samples, so include
    // both the serialized documents and the expanded native/sidecar map data.
    constexpr size_t transformedMetadataAllowance = 256 * 1024;
    auto staticSidecarMetadata = mSidecarMetadata;
    if (staticSidecarMetadata.is_object())
        staticSidecarMetadata.erase("dynamic");
    size_t serializedMetadataBytes = metadata.dump().size() +
        staticSidecarMetadata.dump().size();
    if (cameraFrameMetadata.lensShadingMapWidth > 0 &&
        cameraFrameMetadata.lensShadingMapHeight > 0) {
        const size_t gainMapPlanes = std::min<size_t>(
            4, cameraFrameMetadata.lensShadingMap.size());
        const size_t gainMapSamples =
            static_cast<size_t>(cameraFrameMetadata.lensShadingMapWidth) *
            static_cast<size_t>(cameraFrameMetadata.lensShadingMapHeight) *
            gainMapPlanes;
        serializedMetadataBytes += gainMapSamples * sizeof(float);
    }
    if (mSidecarMetadata.contains("dynamic") &&
        mSidecarMetadata["dynamic"].contains("frames")) {
        const auto& dynamicFrames = mSidecarMetadata["dynamic"]["frames"];
        size_t largestDynamicFrameBytes = 0;
        for (size_t frame = 0; frame < dynamicFrames.size(); ++frame) {
            size_t dynamicFrameBytes = dynamicFrames[frame].dump().size();
            for (const char* field : {"gainMaps", "deferredGainMaps"}) {
                const auto maps = vfs::loadSidecarGainMaps(
                    mSidecarMetadata, frame, field);
                for (const auto& map : maps)
                    dynamicFrameBytes += map.data.size() * sizeof(float);
            }
            largestDynamicFrameBytes = std::max(
                largestDynamicFrameBytes, dynamicFrameBytes);
        }
        serializedMetadataBytes += largestDynamicFrameBytes;
    }

    // Mounted-file sizes only need a safe upper bound. Rendering frame zero to
    // measure it made higher-CFA imports perform a full demosaic before the
    // user had requested any frame. Use the same conservative uncompressed
    // geometry estimate as DNG sequences and render lazily on first read.
    auto estimatedDngSize = [&](bool nativeResolution) {
        RenderSettings plannedSettings = mSettings;
        if (nativeResolution)
            plannedSettings.options = static_cast<FileRenderOptions>(
                plannedSettings.options & ~RENDER_OPT_DRAFT);
        const auto plan = utils::planDngFrameProcessing(
            plannedSettings, cameraFrameMetadata, mCalibration);
        uint32_t width = cropWidth > 0 ? cropWidth
            : static_cast<uint32_t>(cameraFrameMetadata.width);
        uint32_t height = cropHeight > 0 ? cropHeight
            : static_cast<uint32_t>(cameraFrameMetadata.height);
        const int cfaSize = plan.cfaRepeatSize;
        const bool binning = cfaSize > 2 && plan.explicitBinning;
        if (binning) {
            const uint32_t factor = mSettings.quadBayerOption == QuadBayerMode::Bin8x8To4x4 &&
                                    cfaSize == 8 ? 2u : static_cast<uint32_t>(cfaSize / 2);
            width = std::max<uint32_t>(1, width / factor);
            height = std::max<uint32_t>(1, height / factor);
        }
        const int scale = plan.draftScale;
        if (scale > 1) {
            width = std::max<uint32_t>(1, width / static_cast<uint32_t>(scale));
            height = std::max<uint32_t>(1, height / static_cast<uint32_t>(scale));
        }
        const uint32_t channels = plan.demosaic && !plan.remosaic ? 3u : 1u;
        const auto levels = resolveDataLevels(
            mSettings.levels, cameraFrameMetadata.dynamicWhiteLevel,
            cameraFrameMetadata.dynamicBlackLevel, cameraConfig.whiteLevel,
            cameraConfig.blackLevel);
        double storedWhite = std::clamp<double>(levels.white, 1.0, 65535.0);
        std::array<double, 4> storedBlack{};
        std::copy(levels.black.begin(), levels.black.end(), storedBlack.begin());
        if (cfaSize > 2 && scale > 1 && plan.hqProxy) {
            const double area = std::pow(std::max(1, cfaSize / 2), 2);
            storedWhite *= area;
            for (double& black : storedBlack) black *= area;
            while (storedWhite > 65535.0) {
                storedWhite *= 0.5;
                for (double& black : storedBlack) black *= 0.5;
            }
        }
        const bool vignetteBake =
            mSettings.options & RENDER_OPT_APPLY_VIGNETTE_CORRECTION;
        const auto outputLevels = utils::planDngOutputLevels(
            storedWhite, storedBlack, vignetteBake,
            mSettings.options & RENDER_OPT_NORMALIZE_SHADING_MAP,
            mSettings.options & RENDER_OPT_DEBUG_SHADING_MAP, plan.logTransform);
        storedWhite = outputLevels.white;
        const uint32_t storedBits = utils::dngPackedBits(
            static_cast<uint16_t>(storedWhite), channels == 3,
            mSettings.cameraNativeStaging);
        const size_t rowBytes =
            (static_cast<size_t>(width) * channels * storedBits + 7) / 8;
        return rowBytes * height +
               serializedMetadataBytes + transformedMetadataAllowance;
    };
    mTypicalDngSize = mSettings.streamingPreview ? 1 : estimatedDngSize(false);
    const bool nativeFirstFrame = !mSettings.streamingPreview &&
        vfs::getScaleFromOptions(mSettings.options, mSettings.draftScale) > 1;
    const size_t firstDngSize = mSettings.streamingPreview
        ? 1 : estimatedDngSize(nativeFirstFrame);

    // Generate file entries
    mFiles.reserve(frames.size()*2);

    vfs::appendDesktopIni(mFiles);

    // Generate and add audio (TODO: We're loading all the audio into memory)
    Entry audioEntry;

    std::vector<AudioChunk> audioChunks;
    if (!mSettings.streamingPreview)
        decoder.loadAudio(audioChunks);

    if(!audioChunks.empty()) {
        auto fpsFraction = utils::toFraction(mFps);
        AudioSampleFormat audioFormat = audioChunks[0].format;
        int bitDepth = audioFormat == AudioSampleFormat::Float32 ? 32 : 16;
        AudioWriter audioWriter(
            mAudioFile,
            decoder.numAudioChannels(),
            decoder.audioSampleRateHz(),
            fpsFraction.first,
            fpsFraction.second,
            bitDepth);

        // Sync the audio to the video
        vfs::syncAudio(
            frames[0],
            audioChunks,
            decoder.audioSampleRateHz(),
            decoder.numAudioChannels());

        // Calculate total audio duration
        for(auto& x : audioChunks) {
            int numFrames = x.sampleCount() / decoder.numAudioChannels();
            if (audioFormat == AudioSampleFormat::Float32) {
                audioWriter.write(x.float32Data, numFrames);
            } else {
                audioWriter.write(x.int16Data, numFrames);
            }
        }
    }

    if(!mAudioFile.empty()) {
        audioEntry.type = EntryType::FILE_ENTRY;
        audioEntry.size = mAudioFile.size();
        audioEntry.name = "audio.wav";

        mFiles.emplace_back(audioEntry);
    }

    std::vector<Entry> sourceEntries;
    sourceEntries.reserve(frames.size());
    for (const auto timestamp : frames) {
        Entry entry;
        entry.type = EntryType::FILE_ENTRY;
        entry.size = mTypicalDngSize;
        entry.userData = timestamp;
        sourceEntries.push_back(entry);
    }
    int duplicatedFrames = 0, droppedFrames = 0;
    auto mapped = vfs::mapFramesToCfr(sourceEntries, frames, mBaseName + "-", mFps,
        applyCFRConversion, droppedFrames, duplicatedFrames);
    if (!mapped.empty()) mapped.front().size = firstDngSize;
    mFiles.insert(mFiles.end(), std::make_move_iterator(mapped.begin()),
                  std::make_move_iterator(mapped.end()));

    // Store frame information
    const int outputWidth = cropWidth > 0 && cropWidth <= static_cast<uint32_t>(cameraFrameMetadata.width)
        ? static_cast<int>(cropWidth) : cameraFrameMetadata.width;
    const int outputHeight = cropHeight > 0 && cropHeight <= static_cast<uint32_t>(cameraFrameMetadata.height)
        ? static_cast<int>(cropHeight) : cameraFrameMetadata.height;
    mFileInfo = vfs::makeFileInfo(
        mFrameRateInfo, mFps, static_cast<int>(frames.size()), droppedFrames,
        duplicatedFrames, outputWidth, outputHeight);
    const auto sidecar = vfs::sidecarPath(mSrcPath);
    mFileInfo.sidecarState = !boost::filesystem::exists(sidecar)
        ? 0 : (mCalibration.has_value() ? 1 : 2);
    if (mCalibration && mCalibration->hasIgnoreForwardMat) {
        mFileInfo.hasIgnoreForwardMatOverride = true;
        mFileInfo.ignoreForwardMatOverride = mCalibration->ignoreForwardMat;
    }
    switch (cameraFrameMetadata.orientation) {
    case ScreenOrientation::PORTRAIT: mFileInfo.orientation = 90; break;
    case ScreenOrientation::REVERSE_PORTRAIT: mFileInfo.orientation = 270; break;
    case ScreenOrientation::REVERSE_LANDSCAPE: mFileInfo.orientation = 180; break;
    case ScreenOrientation::LANDSCAPE: mFileInfo.orientation = 0; break;
    default: break;
    }
    if (mCalibration && mCalibration->hasOrientation)
        mFileInfo.orientation = mCalibration->orientation;
    auto duplicateMask = std::make_shared<std::vector<bool>>();
    duplicateMask->reserve(mapped.size());
    for (const auto& entry : mFiles)
        if (boost::filesystem::path(entry.name).extension() == ".dng" ||
            boost::filesystem::path(entry.name).extension() == ".DNG")
            duplicateMask->push_back(entry.duplicateFrame);
    mFileInfo.duplicateFrameMask = std::move(duplicateMask);
    vfs::buildGalleryFrameMap(frames, mFiles,
        mFileInfo.sourceFrameToOutput, mFileInfo.sourceFrameDuplicated);
    int displayCfaSize = cameraFrameMetadata.cfaSize;
    if (mCalibration && mCalibration->hasCfaSize && mCalibration->cfaSize > 0)
        displayCfaSize = mCalibration->cfaSize;
    mFileInfo.dataType = vfs::getDisplayDataType(false, displayCfaSize);
    mFileInfo.levelsInfo = vfs::getDisplayDataLevels(
        cameraFrameMetadata.dynamicWhiteLevel, cameraFrameMetadata.dynamicBlackLevel,
        cameraConfig.whiteLevel, cameraConfig.blackLevel,
        mSettings.levels, logTransformModeToString(mSettings.logTransform),
        mSettings.options & RENDER_OPT_APPLY_VIGNETTE_CORRECTION,
        mSettings.options & RENDER_OPT_NORMALIZE_SHADING_MAP);
    // Video timestamps/frame mapping define clip duration. Audio capture can
    // start late or end early and must not shorten or extend the image stream.
    if (applyCFRConversion) {
        mFileInfo.runtimeSeconds = mFps > 0.0f
            ? static_cast<float>(mapped.size()) / mFps : 0.0f;
    } else if (frames.size() > 1) {
        const double spanSeconds = static_cast<double>(frames.back() - frames.front()) / 1e9;
        const double finalFrameSeconds = mFrameRateInfo.medianFrameRate > 0.0f
            ? 1.0 / mFrameRateInfo.medianFrameRate : 0.0;
        mFileInfo.runtimeSeconds = static_cast<float>(
            std::max(0.0, spanSeconds + finalFrameSeconds));
    } else {
        mFileInfo.runtimeSeconds = mFps > 0.0f ? 1.0f / mFps : 0.0f;
    }
    if (!mAudioFile.empty())
        mFileInfo.audioWav = std::make_shared<const std::vector<uint8_t>>(mAudioFile);
    mFileInfo.presentationTimestamps =
        std::make_shared<const std::vector<std::int64_t>>(frames.begin(), frames.end());
    mFileInfo.timingTimeBaseNum = 1;
    mFileInfo.timingTimeBaseDen = 1000000000;
    mFileInfo.timingUsesCfrMapping = applyCFRConversion;
}

std::shared_ptr<std::vector<char>> VirtualFileSystemImpl_MCRAW::materializeFile(
    const Entry& entry, bool jpegCompression) {
    std::shared_lock renderLock(mRenderMutex);
    if (boost::ends_with(entry.name, "wav"))
        return std::make_shared<std::vector<char>>(mAudioFile.begin(), mAudioFile.end());

    return vfs::materializeCached(mCache, entry, jpegCompression, [&] {
        thread_local std::map<std::string, std::unique_ptr<Decoder>> decoders;
        auto& decoder = decoders[mSrcPath];
        if (!decoder)
            decoder = std::make_unique<Decoder>(mSrcPath);
        const auto timestamp = std::get<Timestamp>(entry.userData);
        const auto frameIt = mFrameIndexByTimestamp.find(timestamp);
        if (frameIt == mFrameIndexByTimestamp.end())
            throw std::runtime_error("MCRAW source frame not found");

        std::vector<uint8_t> frameData;
        nlohmann::json metadata;
        uint32_t cropWidth = 0, cropHeight = 0, strideOverride = 0;
        if (mSettings.options & RENDER_OPT_CROPPING)
            utils::parseCropTarget(mSettings.cropTarget, cropWidth, cropHeight, strideOverride);
        decoder->loadFrame(timestamp, frameData, metadata, static_cast<int>(strideOverride));
        const int outputFrameNumber = vfs::outputFrameNumber(entry);
        RenderSettings frameSettings = mSettings;
        if (!mSettings.streamingPreview && outputFrameNumber == 0 &&
            vfs::getScaleFromOptions(mSettings.options, mSettings.draftScale) > 1)
            frameSettings.options = static_cast<FileRenderOptions>(
                frameSettings.options & ~RENDER_OPT_DRAFT);
        std::optional<float> exposureOverride;
        if (mSettings.options & RENDER_OPT_SMOOTH_EXPOSURE)
            exposureOverride = mSmoothedExposureOffsets.at(timestamp);
        std::optional<std::array<float, 3>> neutralOverride;
        if (mSettings.options & RENDER_OPT_SMOOTH_WHITE_BALANCE)
            neutralOverride = mSmoothedAsShotNeutrals.at(timestamp);
        auto frameMetadata = CameraFrameMetadata::parse(metadata);
        auto cameraConfig = CameraConfiguration::parse(decoder->getContainerMetadata());
        reorderNativeShadingMapToCfaPhases(
            frameMetadata, effectiveCfaArrangement(
                frameSettings, mCalibration, cameraConfig.sensorArrangement));
        utils::overrideLensShadingMap(frameMetadata,
            vfs::loadSidecarGainMaps(mSidecarMetadata,
                frameIt->second, "gainMaps"));
        auto output = utils::generateDng(
            frameData,
            frameMetadata,
            cameraConfig,
            mFps,
            outputFrameNumber,
            mBaselineExpValue,
            frameSettings,
            mCalibration,
            jpegCompression,
            exposureOverride,
            neutralOverride);
        if (!output)
            throw std::runtime_error("DNG generation returned no data");
        const bool converted = mSettings.options & RENDER_OPT_FRAMERATE_CONVERSION;
        const Timestamp outputTimestamp = vfs::outputTimestamp(
            entry, timestamp, mSourceFrames.front(), mFps, converted);
        std::vector<uint8_t> timed(output->begin(), output->end());
        applySidecarGainMapOpcodes(
            timed, frameIt->second);
        if (!DNGDecoder::setTimingMetadata(timed, mFps, outputTimestamp))
            throw std::runtime_error("Could not write DNG timing metadata");
        if (!jpegCompression && !mSettings.streamingPreview) {
            if (timed.size() > entry.size)
                throw std::runtime_error(
                    "Generated MCRAW DNG exceeds advertised mounted size");
            timed.resize(entry.size, 0);
        }
        output = std::make_shared<std::vector<char>>(timed.begin(), timed.end());
        return output;
    });
}

bool VirtualFileSystemImpl_MCRAW::materializePreviewFrame(
        const Entry& entry, PreviewFrame& preview) {
    std::shared_lock renderLock(mRenderMutex);
    thread_local std::map<std::string, std::unique_ptr<Decoder>> decoders;
    auto& decoder = decoders[mSrcPath];
    if (!decoder) decoder = std::make_unique<Decoder>(mSrcPath);
    const auto timestamp = std::get<Timestamp>(entry.userData);
    const auto frameIt = mFrameIndexByTimestamp.find(timestamp);
    if (frameIt == mFrameIndexByTimestamp.end()) return false;

    std::vector<uint8_t> frameData;
    nlohmann::json metadata;
    uint32_t cropWidth = 0, cropHeight = 0, strideOverride = 0;
    if (mSettings.options & RENDER_OPT_CROPPING)
        utils::parseCropTarget(mSettings.cropTarget, cropWidth, cropHeight, strideOverride);
    decoder->loadFrame(timestamp, frameData, metadata, static_cast<int>(strideOverride));
    auto frameMetadata = CameraFrameMetadata::parse(metadata);
    auto cameraConfig = CameraConfiguration::parse(decoder->getContainerMetadata());
    reorderNativeShadingMapToCfaPhases(
        frameMetadata, effectiveCfaArrangement(mSettings, mCalibration,
                                                cameraConfig.sensorArrangement));
    utils::overrideLensShadingMap(frameMetadata,
        vfs::loadSidecarGainMaps(mSidecarMetadata, frameIt->second, "gainMaps"));
    std::optional<float> exposureOverride;
    if (mSettings.options & RENDER_OPT_SMOOTH_EXPOSURE)
        exposureOverride = mSmoothedExposureOffsets.at(timestamp);
    std::optional<std::array<float, 3>> neutralOverride;
    if (mSettings.options & RENDER_OPT_SMOOTH_WHITE_BALANCE)
        neutralOverride = mSmoothedAsShotNeutrals.at(timestamp);
    utils::generateDng(frameData, frameMetadata, cameraConfig, mFps,
        vfs::outputFrameNumber(entry), mBaselineExpValue, mSettings, mCalibration,
        false, exposureOverride, neutralOverride, &preview);
    preview.timestamp = vfs::outputTimestamp(
        entry, timestamp, mSourceFrames.front(), mFps,
        mSettings.options & RENDER_OPT_FRAMERATE_CONVERSION);
    return !preview.rgb.empty();
}

void VirtualFileSystemImpl_MCRAW::applySidecarGainMapOpcodes(
        std::vector<uint8_t>& dng, size_t frameIndex) const {
    if (!mSidecarMetadata.contains("dynamic") ||
        !mSidecarMetadata["dynamic"].contains("frames") ||
        frameIndex >= mSidecarMetadata["dynamic"]["frames"].size()) return;
    const auto& frame = mSidecarMetadata["dynamic"]["frames"][frameIndex];
    const bool bake = mSettings.options & RENDER_OPT_APPLY_VIGNETTE_CORRECTION;
    // OpcodeList2 was already built from the sidecar-overridden frame metadata
    // by generateDng(). Only the independently stored deferred layer still
    // needs to be attached here. Replacing and transforming list 2 again would
    // run color reduction/optimization twice.
    vfs::replaceSidecarGainMapOpcodes(
        dng, mSidecarMetadata, frameIndex, false,
        !bake && !(mSettings.options & RENDER_OPT_VIGNETTE_ONLY_COLOR));
    if (bake && (mSettings.options & RENDER_OPT_VIGNETTE_ONLY_COLOR)) {
        const char* field = frame.contains("deferredGainMaps")
            ? "deferredGainMaps" : "gainMaps";
        if (frame.contains(field)) {
            const auto maps = vfs::loadSidecarGainMaps(mSidecarMetadata, frameIndex, field);
            if (maps.size() == 1 && maps.front().channels == 1 &&
                !DNGDecoder::replaceGainMaps(dng, 3, maps))
                throw std::runtime_error("Could not apply MCRAW deferred gain-map override");
        }
    }
    if (!DNGDecoder::canonicalizeGainMapOpcodes(dng))
        throw std::runtime_error("Could not canonicalize MCRAW sidecar gain maps");
}

int VirtualFileSystemImpl_MCRAW::readPriority(const Entry& entry) const {
    return boost::ends_with(entry.name, ".dng") ? vfs::outputFrameNumber(entry) : 0;
}

std::function<std::shared_ptr<std::vector<char>>()>
VirtualFileSystemImpl_MCRAW::staticMaterializer(const Entry& entry) {
    if (!boost::ends_with(entry.name, ".wav")) return {};
    return [this, entry] { return materializeFile(entry, false); };
}

void VirtualFileSystemImpl_MCRAW::updateOptions(const RenderSettings& settings) {
    std::unique_lock renderLock(mRenderMutex);
    std::lock_guard<std::mutex> lock(mMutex);
    mSettings = settings;
    mSettings.draftScale =
        vfs::getScaleFromOptions(mSettings.options, mSettings.draftScale);
    mCache.clear();
    vfs::loadSidecar(vfs::sidecarPath(mSrcPath), mSidecarMetadata, mCalibration, true);
    init();
}

FileInfo VirtualFileSystemImpl_MCRAW::getFileInfo() const {
    return mFileInfo;
}

} // namespace motioncam

