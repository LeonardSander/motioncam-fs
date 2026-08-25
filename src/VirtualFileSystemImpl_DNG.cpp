#include "VirtualFileSystemImpl_DNG.h"
#include "VirtualFileSystemImpl.h"
#include "DNGDecoder.h"
#include "CalibrationData.h"
#include "Utils.h"
#include "LRUCache.h"
#include "Types.h"

#include <boost/filesystem.hpp>
#include <boost/algorithm/string.hpp>

#include <BS_thread_pool.hpp>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>

using motioncam::Timestamp;

namespace motioncam {

VirtualFileSystemImpl_DNG::VirtualFileSystemImpl_DNG(
        BS::thread_pool& ioThreadPool,
        BS::thread_pool& processingThreadPool,
        LRUCache& lruCache,
        const RenderSettings& config,
        const std::string& file,
        const std::string& baseName) :
        mCache(lruCache),
        mIoThreadPool(ioThreadPool),
        mProcessingThreadPool(processingThreadPool),
        mSrcPath(file),
        mBaseName(baseName),
        mTypicalDngSize(0),
        mFps(0),
        mTotalFrames(0),
        mDroppedFrames(0),
        mDuplicatedFrames(0),
        mWidth(0),
        mHeight(0),
        mConfig(config) {
    
    // Load calibration JSON if it exists (for DNG folder)
    const auto calibPath = vfs::sidecarPath(mSrcPath);
    vfs::loadSidecar(calibPath, mSidecarMetadata, mCalibration);
    if (boost::filesystem::exists(calibPath)) {
        if (mCalibration.has_value()) {
            spdlog::info("Loaded calibration for DNG sequence: {}", calibPath.string());
        }
    }
    
    // Initialize DNGDecoder
    try {
        mDecoder = std::make_unique<DNGDecoder>(mSrcPath);
        const auto& sequenceInfo = mDecoder->getSequenceInfo();
        
        mWidth = sequenceInfo.width;
        mHeight = sequenceInfo.height;
        mFps = static_cast<float>(sequenceInfo.fps);
        mTotalFrames = static_cast<int>(sequenceInfo.totalFrames);
        mDroppedFrames = 0;
        mDuplicatedFrames = 0;
        mHasCfa = mDecoder->getCFAMetadata(0, mCfaSize, mCfaPhase);
        if (mCalibration && mCalibration->hasCfaSize && mCalibration->cfaSize > 0) {
            mCfaSize = mCalibration->cfaSize;
            mHasCfa = mCfaSize >= 2;
        }
        
        // Calculate frame rate statistics
        calculateFrameRateStats();

        const auto& frames = mDecoder->getFrames();
        for (size_t i = 0; i < frames.size(); ++i)
            mFrameIndexByTimestamp[frames[i].timestamp] = i;
        std::vector<vfs::ExposureSample> exposureSamples;
        std::vector<DNGFrameMetadata> metadata(frames.size());
        exposureSamples.reserve(frames.size());
        size_t missingExposureFrames = 0;
        for (size_t i = 0; i < frames.size(); ++i) {
            if (!mDecoder->getFrameMetadata(static_cast<int>(i), metadata[i]))
                throw std::runtime_error("Could not read DNG frame metadata: " + frames[i].filePath);
            if (metadata[i].hasExposure) {
                exposureSamples.push_back({frames[i].timestamp, metadata[i].iso,
                    metadata[i].exposureTime, metadata[i].baselineExposure,
                    metadata[i].asShotNeutral});
                mExposureTimes[frames[i].timestamp] = metadata[i].exposureTime;
                mIsoValues[frames[i].timestamp] = metadata[i].iso;
            } else {
                ++missingExposureFrames;
            }
            mHasBaselineExposure[frames[i].timestamp] = metadata[i].hasBaselineExposure;
            mHasAsShotNeutral[frames[i].timestamp] = metadata[i].hasAsShotNeutral;
        }
        if (!metadata.empty()) {
            if (metadata[0].whiteLevelCount > 0)
                mSourceWhiteLevel = metadata[0].whiteLevel[0];
            if (metadata[0].blackLevelCount > 0)
                mSourceBlackLevel = metadata[0].blackLevel;
        }
        if (!frames.empty()) {
            GainMap sourceGainMap;
            mSourceHasGainMap = mDecoder->getGainMap(0, sourceGainMap);
            const auto sidecarMaps = vfs::loadSidecarGainMaps(
                mSidecarMetadata, 0, "gainMaps");
            if (!sidecarMaps.empty()) mSourceHasGainMap = true;
        }
        // Exposure normalization/smoothing is meaningful only when every
        // frame has both ISO and ExposureTime. Missing optional exposure tags
        // must not prevent an otherwise valid DNG sequence from mounting.
        if (missingExposureFrames == 0) {
            const auto analysis = vfs::analyzeExposureMetadata(
                exposureSamples, mFrameRateInfo.medianFrameRate);
            mNormalizedExposureOffsets = analysis.normalizedBaseline;
            mSmoothedExposureOffsets = analysis.smoothedBaseline;
            mSmoothedAsShotNeutrals = analysis.smoothedNeutral;
        } else {
            spdlog::warn("DNG sequence has {} frame(s) without ISO or ExposureTime; "
                         "exposure normalization and smoothing are disabled",
                         missingExposureFrames);
        }
        
        spdlog::info("DNG sequence loaded: {}x{} @ {:.2f}fps (avg: {:.2f}, med: {:.2f}), {} frames",
                     mWidth, mHeight, mFps, mFrameRateInfo.averageFrameRate,
                     mFrameRateInfo.medianFrameRate, mTotalFrames);
    }
    catch (const std::exception& e) {
        spdlog::error("Failed to initialize DNGDecoder: {}", e.what());
        throw;
    }
    
    this->init();
}

VirtualFileSystemImpl_DNG::~VirtualFileSystemImpl_DNG() {
    spdlog::info("Destroying VirtualFileSystemImpl_DNG({})", mSrcPath);
}

void VirtualFileSystemImpl_DNG::init() {
    spdlog::debug("VirtualFileSystemImpl_DNG::init(options={})", optionsToString(mConfig.options));
    
    mFiles.clear();
    mDroppedFrames = 0;
    mDuplicatedFrames = 0;
    const bool applyCFRConversion = mConfig.options & RENDER_OPT_FRAMERATE_CONVERSION;
    mFps = vfs::determineCFRTarget(
        mFrameRateInfo, mConfig.cfrTarget, applyCFRConversion);

    vfs::appendDesktopIni(mFiles);

    const auto& frames = mDecoder->getFrames();
    if (frames.empty()) {
        mTypicalDngSize = 0;
        return;
    }
    // Mounted output is uncompressed and has a fixed layout, so one frame is
    // representative of every frame at the same resolution.
    mTypicalDngSize = transformFrame(0, 0, false).size();
    const bool draft = vfs::getScaleFromOptions(
        mConfig.options, mConfig.draftScale) > 1;
    const size_t firstDngSize = draft
        ? transformFrame(0, 0, false, true).size() : mTypicalDngSize;

    std::vector<Entry> sourceEntries;
    sourceEntries.reserve(frames.size());
    for (size_t i = 0; i < frames.size(); ++i) {
        Entry dngEntry;
        dngEntry.type = EntryType::FILE_ENTRY;
        dngEntry.pathParts = {};
        dngEntry.name = vfs::constructFrameFilename(mBaseName, static_cast<int>(i), 6, "dng");
        dngEntry.userData = frames[i].timestamp;
        dngEntry.duplicateFrame = frames[i].duplicateFrame;
        dngEntry.syntheticFrame = frames[i].syntheticFrame;
        dngEntry.size = mTypicalDngSize;
        sourceEntries.push_back(dngEntry);
    }

    std::vector<Timestamp> timestamps;
    timestamps.reserve(frames.size());
    for (const auto& frame : frames) timestamps.push_back(frame.timestamp);
    auto mapped = vfs::mapFramesToCfr(sourceEntries, timestamps, mBaseName, mFps,
        applyCFRConversion, mDroppedFrames, mDuplicatedFrames);
    if (!mapped.empty()) mapped.front().size = firstDngSize;
    mFiles.insert(mFiles.end(), std::make_move_iterator(mapped.begin()),
                  std::make_move_iterator(mapped.end()));

}

std::vector<Entry> VirtualFileSystemImpl_DNG::listFiles(const std::string& filter) const {
    std::lock_guard<std::mutex> lock(mMutex);
    
    return vfs::filterEntries(mFiles, filter);
}

std::optional<Entry> VirtualFileSystemImpl_DNG::findEntry(const std::string& fullPath) const {
    std::lock_guard<std::mutex> lock(mMutex);
    
    return vfs::findEntry(mFiles, fullPath);
}

int VirtualFileSystemImpl_DNG::readFile(
    const Entry& entry,
    const size_t pos,
    const size_t len,
    void* dst,
    std::function<void(size_t, int)> result,
    bool async) {
    
    return vfs::readMountedEntry(entry, pos, len, dst, result, async,
        mProcessingThreadPool, [this, entry] { return materializeFile(entry, false); }, {},
        vfs::outputFrameNumber(entry));
}

std::shared_ptr<std::vector<char>> VirtualFileSystemImpl_DNG::materializeFile(
    const Entry& entry, bool jpegCompression) {
    std::shared_lock renderLock(mRenderMutex);
    return vfs::materializeCached(mCache, entry, jpegCompression, [&] {
        const auto timestamp = std::get<Timestamp>(entry.userData);
        const auto& frames = mDecoder->getFrames();
        const auto frameIt = mFrameIndexByTimestamp.find(timestamp);
        if (frameIt == mFrameIndexByTimestamp.end())
            throw std::runtime_error("DNG source frame not found");
        const bool converted = mConfig.options & RENDER_OPT_FRAMERATE_CONVERSION;
        const Timestamp outputTimestamp = vfs::outputTimestamp(
            entry, timestamp, frames.front().timestamp, mFps, converted);
        const bool nativeResolution = vfs::outputFrameNumber(entry) == 0 &&
            vfs::getScaleFromOptions(mConfig.options, mConfig.draftScale) > 1;
        auto bytes = transformFrame(
            frameIt->second, outputTimestamp, jpegCompression, nativeResolution);
        auto output = std::make_shared<std::vector<char>>(bytes.begin(), bytes.end());
        return output;
    });
}

bool VirtualFileSystemImpl_DNG::generateThumbnail(
        const std::string& outputPath, int width, int height) {
    try {
        DNGDecoder decoder(mSrcPath);
        std::vector<uint8_t> source;
        if (!decoder.extractFrame(0, source)) return false;
        const std::vector<char> bytes(source.begin(), source.end());
        return utils::generateJpegThumbnailFromDng(
            bytes, outputPath, width, height);
    } catch (const std::exception& error) {
        spdlog::warn("Could not generate DNG thumbnail: {}", error.what());
        return false;
    }
}

std::vector<uint8_t> VirtualFileSystemImpl_DNG::transformFrame(
        size_t frameIndex, Timestamp outputTimestamp, bool jpegCompression,
        bool nativeResolution) {
    const auto& frames = mDecoder->getFrames();
    if (frameIndex >= frames.size()) throw std::out_of_range("DNG source frame index");
    const auto& frame = frames[frameIndex];
    const Timestamp timestamp = frame.timestamp;
    std::vector<uint8_t> bytes;
    if (!mDecoder->extractFrame(static_cast<int>(frameIndex), bytes))
        throw std::runtime_error("Could not read source DNG");
    if (!DNGDecoder::ensureUncompressed(bytes))
        throw std::runtime_error("Could not decode source DNG to an uncompressed DNG");
    if (mSidecarMetadata.contains("dynamic") &&
        mSidecarMetadata["dynamic"].contains("frames") &&
        frameIndex < mSidecarMetadata["dynamic"]["frames"].size()) {
        const auto& overrideFrame = mSidecarMetadata["dynamic"]["frames"][frameIndex];
        if (overrideFrame.contains("gainMaps") &&
            !DNGDecoder::replaceGainMaps(bytes, 2,
                vfs::loadSidecarGainMaps(mSidecarMetadata, frameIndex, "gainMaps")))
            throw std::runtime_error("Could not apply DNG OpcodeList2 gain-map override");
        if (overrideFrame.contains("deferredGainMaps") &&
            !DNGDecoder::replaceGainMaps(bytes, 3,
                vfs::loadSidecarGainMaps(mSidecarMetadata, frameIndex, "deferredGainMaps")))
            throw std::runtime_error("Could not apply DNG OpcodeList3 gain-map override");
    }
    const std::optional<bool> gainMapOrderOverride =
        mCalibration && mCalibration->hasNeedGainMapOrderFixed
            ? std::optional<bool>(mCalibration->needGainMapOrderFixed)
            : std::nullopt;
    if (!DNGDecoder::repairGainMapCfaPhase(bytes, gainMapOrderOverride))
        throw std::runtime_error("Could not reconcile DNG gain maps with its CFA phase");
    if (mCalibration && mCalibration->hasFullSensorResolution &&
        !DNGDecoder::cropGainMapsToFullSensor(
            bytes, mCalibration->fullSensorResolution[0],
            mCalibration->fullSensorResolution[1]))
        throw std::runtime_error("Could not crop full-sensor DNG gain maps");
    if (!DNGDecoder::overrideDataLevels(bytes, mConfig.levels))
        throw std::runtime_error("Could not override source DNG data levels");
    if (const auto exposure = mExposureTimes.find(timestamp);
        exposure != mExposureTimes.end())
        DNGDecoder::repairExposureTime(bytes, exposure->second);

    const bool normalize = mConfig.options & RENDER_OPT_NORMALIZE_EXPOSURE;
    const bool smoothExposure = mConfig.options & RENDER_OPT_SMOOTH_EXPOSURE;
    const bool smoothWhiteBalance = mConfig.options & RENDER_OPT_SMOOTH_WHITE_BALANCE;
    const auto baselineIt = smoothExposure
        ? mSmoothedExposureOffsets.find(timestamp)
        : mNormalizedExposureOffsets.find(timestamp);
    const auto neutralIt = mSmoothedAsShotNeutrals.find(timestamp);
    const bool updateExposure = (normalize || smoothExposure) &&
        baselineIt != (smoothExposure ? mSmoothedExposureOffsets.end()
                                     : mNormalizedExposureOffsets.end());
    const bool updateWhiteBalance = smoothWhiteBalance &&
        neutralIt != mSmoothedAsShotNeutrals.end();
    if (updateExposure || updateWhiteBalance) {
        double baseline = updateExposure
            ? baselineIt->second + vfs::configuredExposureOffset(mConfig) : 0.0;
        const double* baselinePtr = updateExposure ? &baseline : nullptr;
        const std::array<float, 3>* neutralPtr = updateWhiteBalance
            ? &neutralIt->second : nullptr;
        if (!DNGDecoder::updateMetadata(bytes, baselinePtr, neutralPtr))
            throw std::runtime_error("Could not update DNG exposure/white-balance tags");
    }

    std::vector<GainMap> effectiveGainMaps;
    const bool hasGainMap = DNGDecoder::getGainMaps(bytes, 2, effectiveGainMaps) &&
                            !effectiveGainMaps.empty();
    const bool bakeGainMap = (mConfig.options & RENDER_OPT_APPLY_VIGNETTE_CORRECTION) &&
                             hasGainMap;
    if (hasGainMap && !bakeGainMap &&
        (mConfig.options & RENDER_OPT_VIGNETTE_ONLY_COLOR) &&
        !DNGDecoder::transformGainMaps(bytes, false, true, false))
        throw std::runtime_error("Unsupported DNG gain-map color transform: " + frame.filePath);
    if (hasGainMap && (mConfig.options & RENDER_OPT_OPTIMIZE_GAIN_MAPS) &&
        !DNGDecoder::transformGainMaps(bytes, false, false, true))
        throw std::runtime_error("Unsupported DNG gain-map optimization: " + frame.filePath);
    if (bakeGainMap && !DNGDecoder::bakeGainMaps(
            bytes, mConfig.options & RENDER_OPT_NORMALIZE_SHADING_MAP,
            mConfig.options & RENDER_OPT_VIGNETTE_ONLY_COLOR,
            mConfig.options & RENDER_OPT_OPTIMIZE_GAIN_MAPS,
            mConfig.options & RENDER_OPT_DEBUG_SHADING_MAP))
        throw std::runtime_error("Unsupported DNG layout for vignette baking: " + frame.filePath);
    if (hasGainMap && !bakeGainMap && !DNGDecoder::canonicalizeGainMapOpcodes(bytes))
        throw std::runtime_error("Could not canonicalize DNG gain maps: " + frame.filePath);

    const int outputScale = nativeResolution ? 1 :
        vfs::getScaleFromOptions(mConfig.options, mConfig.draftScale);
    if ((mCfaSize > 2 || (mHasCfa && mConfig.cameraNativeStaging) || outputScale > 1 ||
         (mConfig.options & RENDER_OPT_REMOSAIC_TO_BAYER)) &&
        !DNGDecoder::processHigherCFA(
            bytes, mCfaSize, mCfaPhase, mConfig.quadBayerOption,
            mConfig.options & RENDER_OPT_REMOSAIC_TO_BAYER,
            outputScale,
            mConfig.options & RENDER_OPT_HIGHER_CFA_HQ))
        throw std::runtime_error("Unsupported DNG layout for higher CFA processing: " + frame.filePath);

    if (mConfig.options & RENDER_OPT_BAKE_ISO) {
        const auto iso = mIsoValues.find(timestamp);
        if (iso != mIsoValues.end() && !DNGDecoder::bakeIsoOverlay(bytes, iso->second))
            throw std::runtime_error("Unsupported DNG layout for ISO overlay: " + frame.filePath);
    }
    if ((mConfig.options & RENDER_OPT_LOG_TRANSFORM) &&
        !(mConfig.options & RENDER_OPT_DEBUG_SHADING_MAP) &&
        (mConfig.logTransform != LogTransformMode::KeepInput || bakeGainMap) &&
        !DNGDecoder::applyLogTransform(bytes, mConfig.logTransform))
        throw std::runtime_error("Could not apply DNG log transform: " + frame.filePath);
    if (!DNGDecoder::setTimingMetadata(bytes, mFps, outputTimestamp))
        throw std::runtime_error("Could not update DNG timing metadata");
    if (!mConfig.cameraNativeStaging && !DNGDecoder::packUncompressedToWhiteLevel(bytes))
        throw std::runtime_error("Could not pack uncompressed DNG to its sensor bit depth");
    if (jpegCompression) {
        const bool compressed = isLossyJpegDct(mConfig.jxlDistance)
            ? DNGDecoder::compressLossyJPEG(bytes)
            : mConfig.jxlDistance < 0.0f
                ? DNGDecoder::compressLosslessJPEG(bytes)
                : DNGDecoder::compressJPEGXL(bytes, mConfig.jxlDistance);
        if (!compressed) throw std::runtime_error("Could not compress finalized DNG");
    }
    return bytes;
}

bool VirtualFileSystemImpl_DNG::sourceImagePayloadsEqual(const Entry& left,
                                                         const Entry& right) {
    std::shared_lock renderLock(mRenderMutex);
    const auto leftTimestamp = std::get<Timestamp>(left.userData);
    const auto rightTimestamp = std::get<Timestamp>(right.userData);
    if (leftTimestamp == rightTimestamp) return true;
    const auto& frames = mDecoder->getFrames();
    auto findFrame = [&](Timestamp timestamp) {
        return std::find_if(frames.begin(), frames.end(), [timestamp](const auto& frame) {
            return frame.timestamp == timestamp;
        });
    };
    const auto a = findFrame(leftTimestamp), b = findFrame(rightTimestamp);
    if (a == frames.end() || b == frames.end()) return false;
    auto fingerprint = [&](decltype(a) frame) -> std::optional<uint64_t> {
        const size_t index = static_cast<size_t>(std::distance(frames.begin(), frame));
        {
            std::lock_guard<std::mutex> lock(mPayloadHashMutex);
            if (const auto cached = mPayloadHashes.find(index);
                cached != mPayloadHashes.end())
                return cached->second;
        }
        std::vector<uint8_t> bytes;
        if (!mDecoder->extractFrame(static_cast<int>(index), bytes))
            return std::nullopt;
        uint64_t hash = 0;
        if (!DNGDecoder::imagePayloadHash(bytes, hash)) return std::nullopt;
        std::lock_guard<std::mutex> lock(mPayloadHashMutex);
        return mPayloadHashes.emplace(index, hash).first->second;
    };
    const auto leftHash = fingerprint(a);
    const auto rightHash = fingerprint(b);
    if (!leftHash || !rightHash || *leftHash != *rightHash) return false;
    std::vector<uint8_t> leftBytes, rightBytes;
    if (!mDecoder->extractFrame(static_cast<int>(std::distance(frames.begin(), a)), leftBytes) ||
        !mDecoder->extractFrame(static_cast<int>(std::distance(frames.begin(), b)), rightBytes))
        return false;
    return DNGDecoder::imagePayloadsEqual(leftBytes, rightBytes);
}

void VirtualFileSystemImpl_DNG::updateOptions(const RenderSettings& config) {
    std::unique_lock renderLock(mRenderMutex);
    std::lock_guard<std::mutex> lock(mMutex);
    mCache.clear();
    mPayloadHashes.clear();
    mConfig = config;

    const auto calibPath = vfs::sidecarPath(mSrcPath);
    vfs::loadSidecar(calibPath, mSidecarMetadata, mCalibration);
    if (boost::filesystem::exists(calibPath)) {
        if (mCalibration)
            spdlog::info("Reloaded calibration for DNG sequence: {}", calibPath.string());
    }
    mHasCfa = mDecoder->getCFAMetadata(0, mCfaSize, mCfaPhase);
    if (mCalibration && mCalibration->hasCfaSize && mCalibration->cfaSize > 0) {
        mCfaSize = mCalibration->cfaSize;
        mHasCfa = mCfaSize >= 2;
    }

    init();
}

FileInfo VirtualFileSystemImpl_DNG::getFileInfo() const {
    FileInfo info;
    info.frameRateInfo = mFrameRateInfo;
    info.fps = mFps;
    info.totalFrames = mTotalFrames;
    info.droppedFrames = mDroppedFrames;
    info.duplicatedFrames = mDuplicatedFrames;
    info.width = mWidth;
    info.height = mHeight;
    
    // DNG sequences are pass-through, so we show source format
    info.dataType = vfs::getDisplayDataType(!mHasCfa, mHasCfa ? mCfaSize : 0) + " (DNG)";
    const bool applyLogCurve = (mConfig.options & RENDER_OPT_LOG_TRANSFORM) &&
        mConfig.logTransform != LogTransformMode::Disabled &&
        !(mConfig.options & RENDER_OPT_DEBUG_SHADING_MAP);
    info.levelsInfo = vfs::getDisplayDataLevels(
        mSourceWhiteLevel, mSourceBlackLevel,
        mSourceWhiteLevel, mSourceBlackLevel,
        mConfig.levels,
        applyLogCurve ? logTransformModeToString(mConfig.logTransform) : std::string(),
        mSourceHasGainMap && (mConfig.options & RENDER_OPT_APPLY_VIGNETTE_CORRECTION),
        mConfig.options & RENDER_OPT_NORMALIZE_SHADING_MAP);
    
    // Calculate runtime from frame count and fps
    const int outputFrames = mTotalFrames - mDroppedFrames + mDuplicatedFrames;
    info.runtimeSeconds = (mFps > 0) ? (static_cast<float>(outputFrames) / mFps) : 0.0f;
    const auto& frames = mDecoder->getFrames();
    auto presentationTimestamps = std::make_shared<std::vector<std::int64_t>>();
    presentationTimestamps->reserve(frames.size());
    for (const auto& frame : frames)
        presentationTimestamps->push_back(frame.timestamp);
    info.presentationTimestamps = std::move(presentationTimestamps);
    info.timingTimeBaseNum = 1;
    info.timingTimeBaseDen = 1000000000;
    info.timingUsesCfrMapping = mConfig.options & RENDER_OPT_FRAMERATE_CONVERSION;

    return info;
}

void VirtualFileSystemImpl_DNG::calculateFrameRateStats() {
    const auto& frames = mDecoder->getFrames();
    
    if (frames.size() < 2) {
        mFrameRateInfo.medianFrameRate = mFps;
        mFrameRateInfo.averageFrameRate = mFps;
        return;
    }
    
    // Convert frame timestamps to vector of Timestamp
    std::vector<Timestamp> timestamps;
    timestamps.reserve(frames.size());
    for (const auto& frame : frames) {
        timestamps.push_back(frame.timestamp);
    }
    
    mFrameRateInfo = vfs::calculateFrameRate(timestamps);
    
    spdlog::debug("DNG sequence frame rate stats: avg={:.2f}fps, median={:.2f}fps",
                  mFrameRateInfo.averageFrameRate, mFrameRateInfo.medianFrameRate);
}

} // namespace motioncam
