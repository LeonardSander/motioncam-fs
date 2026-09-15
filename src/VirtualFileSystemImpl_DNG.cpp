#include "VirtualFileSystemImpl_DNG.h"
#include "VirtualFileSystemImpl.h"
#include "DNGDecoder.h"
#include "CalibrationData.h"
#include "DataLevels.h"
#include "GainMapBake.h"
#include "Utils.h"
#include "LRUCache.h"
#include "Types.h"

#include <boost/filesystem.hpp>
#include <boost/algorithm/string.hpp>

#include <BS_thread_pool.hpp>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstring>
#include <thread>
#ifdef __linux__
#include <sys/resource.h>
#include <sys/syscall.h>
#include <unistd.h>
#endif

using motioncam::Timestamp;

namespace motioncam {

namespace {
bool sameRenderSettings(const RenderSettings& left, const RenderSettings& right) {
    return left.options == right.options &&
           left.draftScale == right.draftScale &&
           left.cfrTarget.mode == right.cfrTarget.mode &&
           left.cfrTarget.customValue == right.cfrTarget.customValue &&
           left.cropTarget == right.cropTarget &&
           left.cameraModel == right.cameraModel &&
           left.levels == right.levels &&
           left.logTransform == right.logTransform &&
           left.exposureCompensation == right.exposureCompensation &&
           left.badPixelTreatment == right.badPixelTreatment &&
           left.vignetteCorrection == right.vignetteCorrection &&
           left.quadBayerOption == right.quadBayerOption &&
           left.cfaPhase == right.cfaPhase &&
           left.jxlDistance == right.jxlDistance &&
           left.cameraNativeStaging == right.cameraNativeStaging &&
           left.streamingPreview == right.streamingPreview;
}
}

VirtualFileSystemImpl_DNG::VirtualFileSystemImpl_DNG(
        BS::thread_pool&,
        BS::thread_pool& processingThreadPool,
        LRUCache& lruCache,
        const RenderSettings& config,
        const std::string& file,
        const std::string& baseName) :
        MountedDngSource(lruCache, processingThreadPool),
        mSrcPath(file),
        mBaseName(baseName),
        mSidecarPath(config.sourceSidecarPath.empty()
            ? vfs::sidecarPath(file) : config.sourceSidecarPath),
        mGyroflowSidecarPath(config.sourceGyroflowSidecarPath.empty()
            ? vfs::gyroflowSidecarPath(file) : config.sourceGyroflowSidecarPath),
        mFps(0),
        mTotalFrames(0),
        mDroppedFrames(0),
        mDuplicatedFrames(0),
        mWidth(0),
        mHeight(0),
        mConfig(config) {
    
    // Load calibration JSON if it exists (for DNG folder)
    const auto& calibPath = mSidecarPath;
    vfs::loadSidecar(calibPath, mSidecarMetadata, mCalibration);
    mManualVignetteSidecars = vfs::loadManualVignetteSidecars(mSrcPath);
    mGyroflowLensProfile = vfs::loadGyroflowLensProfile(
        mGyroflowSidecarPath);
    if (mCalibration && mCalibration->hasLevels) mConfig.levels = mCalibration->levels;
    if (mCalibration && mCalibration->hasCenterCrop) {
        mConfig.cropTarget = std::to_string(mCalibration->centerCrop[0]) + "x" +
                             std::to_string(mCalibration->centerCrop[1]);
        mConfig.options |= RENDER_OPT_CROPPING;
    }
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
        mHasFrameNumberSequence = sequenceInfo.hasFrameNumberSequence;
        mTotalFrames = static_cast<int>(sequenceInfo.totalFrames);
        mDroppedFrames = 0;
        mDuplicatedFrames = 0;
        mHasCfa = mDecoder->getCFAMetadata(0, mCfaSize, mCfaPhase);
        if (mCalibration && mCalibration->hasCfaSize && mCalibration->cfaSize > 0) {
            mCfaSize = mCalibration->cfaSize;
            mHasCfa = mCfaSize >= 2;
        }
        if (mCalibration && !mCalibration->cfaPhase.empty()) {
            std::string phase = boost::algorithm::to_lower_copy(mCalibration->cfaPhase);
            if (phase == "rggb") mCfaPhase = {0, 1, 1, 2};
            else if (phase == "grbg") mCfaPhase = {1, 0, 2, 1};
            else if (phase == "gbrg") mCfaPhase = {1, 2, 0, 1};
            else if (phase == "bggr") mCfaPhase = {2, 1, 1, 0};
            else spdlog::warn("Ignoring invalid sidecar CFA phase '{}'", mCalibration->cfaPhase);
        }
        
        // Calculate frame rate statistics
        calculateFrameRateStats();

        const auto& frames = mDecoder->getFrames();
        std::vector<Timestamp> sourceTimestamps;
        sourceTimestamps.reserve(frames.size());
        for (const auto& frame : frames) sourceTimestamps.push_back(frame.timestamp);
        mFrameIndexByTimestamp = vfs::indexTimestamps(sourceTimestamps);
        std::vector<vfs::ExposureSample> exposureSamples;
        std::vector<DNGFrameMetadata> metadata(frames.size());
        mSourceMetadataSizes.resize(frames.size());
        mSourceWhiteLevels.resize(frames.size(), mSourceWhiteLevel);
        mSourceInputBitDepths.resize(frames.size(), mSourceInputBitDepth);
        exposureSamples.reserve(frames.size());
        size_t missingExposureFrames = 0;
        for (size_t i = 0; i < frames.size(); ++i) {
            if (!mDecoder->getFrameMetadata(static_cast<int>(i), metadata[i]))
                throw std::runtime_error("Could not read DNG frame metadata: " + frames[i].filePath);
            mSourceMetadataSizes[i] = metadata[i].metadataBytes;
            if (metadata[i].whiteLevelCount)
                mSourceWhiteLevels[i] = metadata[i].whiteLevel[0];
            if (metadata[i].inputBitDepth)
                mSourceInputBitDepths[i] = metadata[i].inputBitDepth;
            if (metadata[i].hasExposure) {
                exposureSamples.push_back({frames[i].timestamp, metadata[i].iso,
                    metadata[i].exposureTime, metadata[i].baselineExposure,
                    metadata[i].asShotNeutral});
                mExposureTimes[frames[i].timestamp] = metadata[i].exposureTime;
                mIsoValues[frames[i].timestamp] = metadata[i].iso;
            } else {
                ++missingExposureFrames;
            }
        }
        if (!metadata.empty()) {
            if (metadata[0].whiteLevelCount > 0)
                mSourceWhiteLevel = metadata[0].whiteLevel[0];
            if (metadata[0].blackLevelCount > 0)
                mSourceBlackLevel = metadata[0].blackLevel;
            if (metadata[0].inputBitDepth > 0)
                mSourceInputBitDepth = metadata[0].inputBitDepth;
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
    const bool applyCFRConversion = mHasFrameNumberSequence &&
        (mConfig.options & RENDER_OPT_FRAMERATE_CONVERSION);
    if (mHasFrameNumberSequence) {
        mFps = vfs::determineCFRTarget(
            mFrameRateInfo, mConfig.cfrTarget, applyCFRConversion);
    } else {
        // Independent stills form a 24 fps CFR clip by default, but an explicit
        // custom rate remains available to the user.
        mFps = mConfig.cfrTarget.mode == CFRMode::Custom
            ? mConfig.cfrTarget.customValue : 24.0f;
    }

    vfs::appendDesktopIni(mFiles);

    const auto& frames = mDecoder->getFrames();
    if (frames.empty()) {
        return;
    }
    // Only a proper numbered sequence keeps frame zero at native resolution as
    // its metadata frame. Independent DNGs always honor proxy/HQ settings.
    const bool hasNativeMetadataFrame = mHasFrameNumberSequence &&
        vfs::getScaleFromOptions(mConfig.options, mConfig.draftScale) > 1;
    auto estimatedSize = [&](size_t frameIndex, bool nativeResolution) {
        constexpr size_t transformedMetadataAllowance = 256 * 1024;
        const int scale = nativeResolution ? 1
            : vfs::getScaleFromOptions(mConfig.options, mConfig.draftScale);
        uint32_t width = frameIndex < frames.size() && frames[frameIndex].width > 0
            ? static_cast<uint32_t>(frames[frameIndex].width)
            : static_cast<uint32_t>(mWidth);
        uint32_t height = frameIndex < frames.size() && frames[frameIndex].height > 0
            ? static_cast<uint32_t>(frames[frameIndex].height)
            : static_cast<uint32_t>(mHeight);
        const bool binning = mHasCfa && mCfaSize > 2 &&
            (mConfig.quadBayerOption == QuadBayerMode::Binning ||
             mConfig.quadBayerOption == QuadBayerMode::Bin8x8To4x4);
        if (binning) {
            const uint32_t factor = mConfig.quadBayerOption == QuadBayerMode::Bin8x8To4x4 &&
                                    mCfaSize == 8
                ? 2u : static_cast<uint32_t>(mCfaSize / 2);
            width = std::max<uint32_t>(1, width / factor);
            height = std::max<uint32_t>(1, height / factor);
        }
        if (scale > 1) {
            width = std::max<uint32_t>(1, width / static_cast<uint32_t>(scale));
            height = std::max<uint32_t>(1, height / static_cast<uint32_t>(scale));
        }

        uint32_t channels = mHasCfa ? 1u : 3u;
        const bool remosaic = mConfig.options & RENDER_OPT_REMOSAIC_TO_BAYER;
        const bool hq = mConfig.options & RENDER_OPT_HIGHER_CFA_HQ;
        const bool demosaicMode = mConfig.quadBayerOption == QuadBayerMode::Demosaic ||
                                  mConfig.quadBayerOption == QuadBayerMode::DemosaicColor ||
                                  mConfig.quadBayerOption == QuadBayerMode::DemosaicOCL;
        if (!mHasCfa) channels = remosaic ? 1u : 3u;
        else if (scale > 1 && hq)
            channels = remosaic ? 1u : 3u;
        else if (scale == 1 && mCfaSize > 2 && demosaicMode)
            channels = remosaic ? 1u : 3u;

        uint32_t storedBits = frameIndex < mSourceInputBitDepths.size()
            ? mSourceInputBitDepths[frameIndex] : mSourceInputBitDepth;
        // DNG level overrides use this source frame's levels for both Dynamic
        // and Static; only an explicit numeric white changes the code range.
        const float sourceWhite = frameIndex < mSourceWhiteLevels.size()
            ? mSourceWhiteLevels[frameIndex] : mSourceWhiteLevel;
        const auto effectiveLevels = resolveDataLevels(
            mConfig.levels, sourceWhite, mSourceBlackLevel,
            sourceWhite, mSourceBlackLevel);
        const uint32_t white = static_cast<uint32_t>(
            std::clamp(effectiveLevels.white, 1.0f, 65535.0f));
        // Explicit level overrides define a new quantization range. Otherwise
        // retain the stored input depth reported by the DNG metadata.
        const auto levelSeparator = mConfig.levels.find('/');
        const std::string selectedWhite = levelSeparator == std::string::npos
            ? mConfig.levels : mConfig.levels.substr(0, levelSeparator);
        if (!selectedWhite.empty() && selectedWhite != "Dynamic" &&
            selectedWhite != "Static") {
            storedBits = 1;
            while (storedBits < 16 && ((uint32_t{1} << storedBits) - 1) < white)
                ++storedBits;
        }
        const bool vignetteBake =
            mConfig.options & RENDER_OPT_APPLY_VIGNETTE_CORRECTION;
        const bool logApplied = (mConfig.options & RENDER_OPT_LOG_TRANSFORM) &&
            !(mConfig.options & RENDER_OPT_DEBUG_SHADING_MAP) &&
            (mConfig.logTransform != LogTransformMode::KeepInput || vignetteBake);
        if (vignetteBake && !logApplied) {
            std::array<double, 4> sourceBlack{};
            std::copy(effectiveLevels.black.begin(), effectiveLevels.black.end(),
                      sourceBlack.begin());
            storedBits = planLinearGainBake(
                effectiveLevels.white, sourceBlack,
                mConfig.options & RENDER_OPT_NORMALIZE_SHADING_MAP).destinationBits;
        }
        if (logApplied) {
            if (mConfig.logTransform == LogTransformMode::ReduceBy2Bit) storedBits = std::max(1u, storedBits - 2);
            else if (mConfig.logTransform == LogTransformMode::ReduceBy4Bit) storedBits = std::max(1u, storedBits - 4);
            else if (mConfig.logTransform == LogTransformMode::ReduceBy6Bit) storedBits = std::max(1u, storedBits - 6);
            else if (mConfig.logTransform == LogTransformMode::ReduceBy8Bit) storedBits = std::max(1u, storedBits - 8);
        }
        const size_t rowBytes =
            (static_cast<size_t>(width) * channels * storedBits + 7) / 8;
        const size_t metadataBytes = frameIndex < mSourceMetadataSizes.size()
            ? mSourceMetadataSizes[frameIndex] : transformedMetadataAllowance;
        // The packed estimate is normally tight, but gain-map baking,
        // metadata repair, and writers that promote samples to 16-bit can
        // produce a larger strip than the selected storage bit depth implies.
        // Never advertise less than an uncompressed 16-bit payload plus room
        // for transformed metadata; a projected mount must not truncate it.
        const size_t uncompressedUpperBound =
            static_cast<size_t>(width) * height * channels * sizeof(uint16_t);
        return std::max(rowBytes * height,
                        uncompressedUpperBound) + metadataBytes +
               transformedMetadataAllowance;
    };

    std::vector<size_t> measuredDngSizes(frames.size());
    for (size_t i = 0; i < frames.size(); ++i)
        measuredDngSizes[i] = estimatedSize(i, false);
    const size_t firstDngSize = estimatedSize(0, hasNativeMetadataFrame);

    std::vector<Entry> sourceEntries;
    sourceEntries.reserve(frames.size());
    for (size_t i = 0; i < frames.size(); ++i) {
        Entry dngEntry;
        dngEntry.type = EntryType::FILE_ENTRY;
        dngEntry.pathParts = {};
        dngEntry.name = mHasFrameNumberSequence
            ? vfs::constructFrameFilename(mBaseName, static_cast<int>(i), 6, "dng")
            : boost::filesystem::path(frames[i].filePath).filename().string();
        dngEntry.userData = frames[i].timestamp;
        dngEntry.duplicateFrame = frames[i].duplicateFrame;
        dngEntry.syntheticFrame = frames[i].syntheticFrame;
        dngEntry.size = measuredDngSizes[i];
        sourceEntries.push_back(dngEntry);
    }

    std::vector<Timestamp> timestamps;
    timestamps.reserve(frames.size());
    for (const auto& frame : frames) timestamps.push_back(frame.timestamp);
    auto mapped = mHasFrameNumberSequence
        ? vfs::mapFramesToCfr(sourceEntries, timestamps, mBaseName, mFps,
              applyCFRConversion, mDroppedFrames, mDuplicatedFrames)
        : sourceEntries;
    if (!mapped.empty()) mapped.front().size = firstDngSize;
    mFiles.insert(mFiles.end(), std::make_move_iterator(mapped.begin()),
                  std::make_move_iterator(mapped.end()));

}

int VirtualFileSystemImpl_DNG::readPriority(const Entry& entry) const {
    return mHasFrameNumberSequence ? vfs::outputFrameNumber(entry) : 0;
}

std::shared_ptr<std::vector<char>> VirtualFileSystemImpl_DNG::materializeFile(
    const Entry& entry, bool jpegCompression) {
    std::shared_lock renderLock(mRenderMutex);
    return vfs::materializeCached(mCache, entry, jpegCompression, [&] {
        const auto materializeStarted = std::chrono::steady_clock::now();
        spdlog::info("DNG timing [{}]: materialize cache miss started (advertised {:.2f} MiB)",
                     entry.name, static_cast<double>(entry.size) / (1024.0 * 1024.0));
        const auto timestamp = std::get<Timestamp>(entry.userData);
        const auto& frames = mDecoder->getFrames();
        const auto frameIt = mFrameIndexByTimestamp.find(timestamp);
        if (frameIt == mFrameIndexByTimestamp.end())
            throw std::runtime_error("DNG source frame not found");
        const auto renderPlan = vfs::planDngRender(
            entry, timestamp, frames.front().timestamp, mConfig, mFps,
            jpegCompression, mHasFrameNumberSequence);
        DNGDecoder::beginForegroundWork();
        struct ForegroundGuard {
            ~ForegroundGuard() { DNGDecoder::endForegroundWork(); }
        } foregroundGuard;
        // Concurrent readers commonly probe every frame at once. Serializing
        // the heavyweight decode prevents several 108 MP working sets from
        // forcing the machine into swap; queued reads still keep thumbnails
        // paused through the foreground-work counter above.
        std::lock_guard<std::mutex> materializeLock(mMaterializeMutex);
        auto bytes = transformFrame(
            frameIt->second, renderPlan.outputTimestamp, jpegCompression,
            renderPlan.nativeMetadataFrame);
        const auto transformedAt = std::chrono::steady_clock::now();
        const size_t advertisedSize = entry.size;
        if (!jpegCompression && !mConfig.streamingPreview) {
            if (bytes.size() > advertisedSize)
                throw std::runtime_error(
                    "Transformed DNG size " + std::to_string(bytes.size()) +
                    " exceeds advertised mounted size " + std::to_string(advertisedSize));
            bytes.resize(advertisedSize, 0);
        }
        auto output = std::make_shared<std::vector<char>>(bytes.begin(), bytes.end());
        const auto completedAt = std::chrono::steady_clock::now();
        spdlog::info(
            "DNG timing [{}]: materialize complete {:.1f} ms "
            "(transform {:.1f} ms, pad/copy {:.1f} ms, output {:.2f} MiB)",
            entry.name,
            std::chrono::duration<double, std::milli>(completedAt - materializeStarted).count(),
            std::chrono::duration<double, std::milli>(transformedAt - materializeStarted).count(),
            std::chrono::duration<double, std::milli>(completedAt - transformedAt).count(),
            static_cast<double>(output->size()) / (1024.0 * 1024.0));
        return output;
    });
}

bool VirtualFileSystemImpl_DNG::materializePreviewFrame(
        const Entry& entry, PreviewFrame& preview) {
    std::shared_lock renderLock(mRenderMutex);
    const auto timestamp = std::get<Timestamp>(entry.userData);
    const auto frameIt = mFrameIndexByTimestamp.find(timestamp);
    if (frameIt == mFrameIndexByTimestamp.end()) return false;
    const bool converted = mHasFrameNumberSequence &&
        (mConfig.options & RENDER_OPT_FRAMERATE_CONVERSION);
    const Timestamp outputTimestamp = converted
        ? vfs::outputTimestamp(entry, timestamp, mDecoder->getFrames().front().timestamp,
                               mFps, true)
        : timestamp - mDecoder->getFrames().front().timestamp;
    DNGDecoder::beginForegroundWork();
    struct ForegroundGuard { ~ForegroundGuard() { DNGDecoder::endForegroundWork(); } } guard;
    std::unique_lock<std::mutex> materializeLock(mMaterializeMutex);
    bool fallbackGainMapApplied = false;
    // Decode the source container once and run the shared in-memory preview
    // processor, including gain-map-only rendering.
    {
        try {
            auto prepared = prepareFrame(frameIt->second, false);
            DecodedDNGImage image;
            if (!DNGDecoder::decodeImage(std::move(prepared.dng), image))
                throw std::runtime_error("Could not decode source DNG preview");
            if (mConfig.options & RENDER_OPT_APPLY_VIGNETTE_CORRECTION) {
                fallbackGainMapApplied = !image.opcodeList2.empty();
                if (!(mConfig.options & RENDER_OPT_VIGNETTE_ONLY_COLOR))
                    fallbackGainMapApplied = fallbackGainMapApplied ||
                        (image.opcodeList3.size() == 1 &&
                         image.opcodeList3.front().channels == 1);
            }
            // Sequence calibration can declare higher-CFA topology which the
            // individual DNG tags intentionally describe only as a Bayer
            // phase. Carry the resolved sequence topology into the shared
            // in-memory processor instead of losing it at the DNG boundary.
            image.layout.cfaRepeatSize = prepared.cfaSize;
            image.layout.cfaPhase = prepared.cfaPhase;
            if (mCalibration) {
                if (mCalibration->hasColorMatrix1) {
                    image.metadata.colorMatrix1 = mCalibration->colorMatrix1;
                    image.metadata.hasColorMatrix1 = true;
                }
                if (mCalibration->hasColorMatrix2) {
                    image.metadata.colorMatrix2 = mCalibration->colorMatrix2;
                    image.metadata.hasColorMatrix2 = true;
                }
                if (mCalibration->hasForwardMatrix1) {
                    image.metadata.forwardMatrix1 = mCalibration->forwardMatrix1;
                    image.metadata.hasForwardMatrix1 = true;
                }
                if (mCalibration->hasForwardMatrix2) {
                    image.metadata.forwardMatrix2 = mCalibration->forwardMatrix2;
                    image.metadata.hasForwardMatrix2 = true;
                }
                if (mCalibration->hasCameraCalibration1) {
                    image.metadata.cameraCalibration1 = mCalibration->cameraCalibration1;
                    image.metadata.hasCameraCalibration1 = true;
                }
                if (mCalibration->hasCameraCalibration2) {
                    image.metadata.cameraCalibration2 = mCalibration->cameraCalibration2;
                    image.metadata.hasCameraCalibration2 = true;
                }
                if (mCalibration->hasAsShotNeutral) {
                    image.metadata.asShotNeutral = mCalibration->asShotNeutral;
                    image.metadata.hasAsShotNeutral = true;
                }
            }
            if (DNGDecoder::decodePreview(
                    std::move(image), mConfig, preview, true)) {
                preview.timestamp = outputTimestamp;
                if (mConfig.options & RENDER_OPT_BAKE_ISO) {
                    const auto iso = mIsoValues.find(timestamp);
                    const double value = iso != mIsoValues.end()
                        ? iso->second : preview.metadata.iso;
                    utils::bakeIsoOverlay(
                        reinterpret_cast<uint16_t*>(preview.rgb.data()),
                        preview.width, preview.height, 3, value, 0, 65535);
                }
                return true;
            }
        } catch (const std::exception& error) {
            spdlog::warn("Decoded DNG preview preparation failed for {}: {}",
                         entry.name, error.what());
        }
    }
    materializeLock.unlock();
    renderLock.unlock();
    try { return vfs::decodeProcessedDngPreview(
        materializeFile(entry, false), preview, fallbackGainMapApplied); }
    catch (const std::exception&) { return false; }
}

VirtualFileSystemImpl_DNG::PreparedFrame
VirtualFileSystemImpl_DNG::prepareFrame(size_t frameIndex, bool canonicalizeImage) {
    const auto& frames = mDecoder->getFrames();
    if (frameIndex >= frames.size()) throw std::out_of_range("DNG source frame index");
    PreparedFrame result;
    if (!mDecoder->extractFrame(static_cast<int>(frameIndex), result.dng))
        throw std::runtime_error("Could not read source DNG");
    if (!mDecoder->getFrameMetadata(static_cast<int>(frameIndex), result.sourceMetadata))
        throw std::runtime_error("Could not read DNG frame metadata");
    if (!DNGDecoder::removeThumbnails(result.dng))
        throw std::runtime_error("Could not remove source DNG thumbnails");
    if (canonicalizeImage && !DNGDecoder::ensureUncompressed(result.dng))
        throw std::runtime_error("Could not decode source DNG to an uncompressed DNG");

    result.cfaSize = mCfaSize;
    result.cfaPhase = mCfaPhase;
    result.hasCfa = DNGDecoder::getCFAMetadata(
        result.dng, result.cfaSize, result.cfaPhase);
    if (mCalibration && mCalibration->hasCfaSize && mCalibration->cfaSize > 0) {
        result.cfaSize = mCalibration->cfaSize;
        result.hasCfa = result.cfaSize >= 2;
    }
    if (mCalibration && !mCalibration->cfaPhase.empty())
        result.cfaPhase = mCfaPhase;

    vfs::replaceSidecarGainMapOpcodes(result.dng, mSidecarMetadata, frameIndex);
    if (mConfig.vignetteCorrection != VignetteCorrectionMode::Exclude &&
        !vfs::applyManualVignetteSidecar(result.dng, mManualVignetteSidecars))
        throw std::runtime_error("Could not apply manual DNG vignette sidecar");
    if (mConfig.vignetteCorrection == VignetteCorrectionMode::Exclude) {
        if (!DNGDecoder::replaceGainMaps(result.dng, 2, {}) ||
            !DNGDecoder::replaceGainMaps(result.dng, 3, {}))
            throw std::runtime_error("Could not exclude source DNG gain maps");
    }
    if (!DNGDecoder::canonicalizeGainMapOpcodes(result.dng))
        throw std::runtime_error("Could not canonicalize DNG gain-map override");
    const std::optional<bool> gainMapOrderOverride =
        mCalibration && mCalibration->hasNeedGainMapOrderFixed
            ? std::optional<bool>(mCalibration->needGainMapOrderFixed)
            : std::nullopt;
    if (!DNGDecoder::repairGainMapCfaPhase(result.dng, gainMapOrderOverride))
        throw std::runtime_error("Could not reconcile DNG gain maps with its CFA phase");
    // Camera Native finalization expects the original opcode geometry and
    // performs its own sensor-aware processing. Resample therefore falls back
    // to Uncropped for its staging DNGs.
    if (!mConfig.cameraNativeStaging &&
        mConfig.vignetteCorrection == VignetteCorrectionMode::Resample) {
        const auto manualResolution =
            vfs::manualVignetteSensorResolution(mManualVignetteSidecars);
        const auto sensorResolution = mCalibration && mCalibration->hasFullSensorResolution
            ? mCalibration->fullSensorResolution : manualResolution;
        if (sensorResolution[0] > 0 && sensorResolution[1] > 0 &&
            !DNGDecoder::cropGainMapsToFullSensor(
                result.dng, sensorResolution[0], sensorResolution[1]))
            throw std::runtime_error("Could not crop full-sensor DNG gain maps");
    }
    const auto resolved = resolvedFrameMetadata(frameIndex, result.sourceMetadata);
    if (resolved.orientation != result.sourceMetadata.orientation &&
        !DNGDecoder::setOrientation(result.dng, resolved.orientation))
        throw std::runtime_error("Could not override source DNG orientation");
    if (resolved.exposureTime != result.sourceMetadata.exposureTime)
        DNGDecoder::repairExposureTime(result.dng, resolved.exposureTime);
    const bool updateBaseline =
        resolved.baselineExposure != result.sourceMetadata.baselineExposure;
    const bool updateNeutral = resolved.asShotNeutral != result.sourceMetadata.asShotNeutral;
    if ((updateBaseline || updateNeutral) && !DNGDecoder::updateMetadata(
            result.dng, updateBaseline ? &resolved.baselineExposure : nullptr,
            updateNeutral ? &resolved.asShotNeutral : nullptr))
        throw std::runtime_error("Could not update DNG exposure/white-balance tags");
    return result;
}

DNGFrameMetadata VirtualFileSystemImpl_DNG::resolvedFrameMetadata(
        size_t frameIndex, DNGFrameMetadata metadata) const {
    const Timestamp timestamp = mDecoder->getFrames().at(frameIndex).timestamp;
    if (mCalibration && mCalibration->hasOrientation)
        metadata.orientation = mCalibration->orientation;
    if (const auto exposure = mExposureTimes.find(timestamp); exposure != mExposureTimes.end()) {
        metadata.exposureTime = exposure->second;
        metadata.hasExposure = true;
    }
    const bool normalize = mConfig.options & RENDER_OPT_NORMALIZE_EXPOSURE;
    const bool smoothExposure = mConfig.options & RENDER_OPT_SMOOTH_EXPOSURE;
    const auto baseline = smoothExposure
        ? mSmoothedExposureOffsets.find(timestamp) : mNormalizedExposureOffsets.find(timestamp);
    const auto baselineEnd = smoothExposure
        ? mSmoothedExposureOffsets.end() : mNormalizedExposureOffsets.end();
    if ((normalize || smoothExposure) && baseline != baselineEnd) {
        metadata.baselineExposure = baseline->second;
        metadata.hasBaselineExposure = true;
    }
    if (!mConfig.cameraNativeStaging) {
        metadata.baselineExposure +=
            boost::icontains(metadata.uniqueCameraModel, "Panasonic") ? 2.0 : 0.0;
        metadata.hasBaselineExposure = true;
    }
    if ((mConfig.options & RENDER_OPT_SMOOTH_WHITE_BALANCE)) {
        if (const auto neutral = mSmoothedAsShotNeutrals.find(timestamp);
            neutral != mSmoothedAsShotNeutrals.end()) {
            metadata.asShotNeutral = neutral->second;
            metadata.hasAsShotNeutral = true;
        }
    }
    return metadata;
}

std::vector<uint8_t> VirtualFileSystemImpl_DNG::transformFrame(
        size_t frameIndex, Timestamp outputTimestamp, bool jpegCompression,
        bool nativeResolution) {
    const auto& frames = mDecoder->getFrames();
    if (frameIndex >= frames.size()) throw std::out_of_range("DNG source frame index");
    const auto& frame = frames[frameIndex];
    const Timestamp timestamp = frame.timestamp;
    const int requestedScale = nativeResolution ? 1 :
        vfs::getScaleFromOptions(mConfig.options, mConfig.draftScale);
    spdlog::info(
        "DNG render request [{}]: scale={} native={} source={}x{} cfa={} cfa_size={} "
        "mode={} hq={} remosaic={} options={}",
        frame.filePath, requestedScale, nativeResolution, mWidth, mHeight,
        mHasCfa, mCfaSize, static_cast<int>(mConfig.quadBayerOption),
        static_cast<bool>(mConfig.options & RENDER_OPT_HIGHER_CFA_HQ),
        static_cast<bool>(mConfig.options & RENDER_OPT_REMOSAIC_TO_BAYER),
        optionsToString(mConfig.options));
    const auto started = std::chrono::steady_clock::now();
    auto stageStarted = started;
    auto logStage = [&](const char* stage, size_t byteCount) {
        const auto now = std::chrono::steady_clock::now();
        spdlog::info("DNG timing [{}]: {} {:.1f} ms ({:.2f} MiB)",
                     frame.filePath, stage,
                     std::chrono::duration<double, std::milli>(now - stageStarted).count(),
                     static_cast<double>(byteCount) / (1024.0 * 1024.0));
        stageStarted = now;
    };
    auto prepared = prepareFrame(frameIndex);
    auto bytes = std::move(prepared.dng);
    const auto& sourceMetadata = prepared.sourceMetadata;
    const int frameCfaSize = prepared.cfaSize;
    const auto frameCfaPhase = prepared.cfaPhase;
    const bool frameHasCfa = prepared.hasCfa;
    logStage("source preparation", bytes.size());

    uint32_t inputQuantizationWhite = 0;
    const auto levelSeparator = mConfig.levels.find('/');
    const std::string selectedWhite = levelSeparator == std::string::npos
        ? mConfig.levels : mConfig.levels.substr(0, levelSeparator);
    const bool usesSourceWhite = selectedWhite.empty() || selectedWhite == "Dynamic" ||
                                 selectedWhite == "Static";
    if (usesSourceWhite) {
        const uint32_t inputBits = frameIndex < mSourceInputBitDepths.size()
            ? mSourceInputBitDepths[frameIndex] : mSourceInputBitDepth;
        if (inputBits > 0 && inputBits <= 16)
            inputQuantizationWhite = (uint32_t{1} << inputBits) - 1;
    }
    vfs::DngPixelPipelineOptions pixels;
    pixels.cfaRepeatSize = frameCfaSize;
    pixels.cfaPhase = frameCfaPhase;
    pixels.hasCfa = frameHasCfa;
    pixels.outputScale = requestedScale;
    pixels.inputQuantizationWhite = inputQuantizationWhite;
    pixels.calibration = mCalibration ? &*mCalibration : nullptr;
    const auto resolved = resolvedFrameMetadata(frameIndex, sourceMetadata);
    pixels.iso = resolved.iso;
    pixels.exposureTime = resolved.exposureTime;
    pixels.sourceName = frame.filePath;
    vfs::processDngPixels(bytes, mConfig, pixels);
    logStage("topology/gain/log processing", bytes.size());
    vfs::DngFinalizeOptions finalize;
    finalize.frameRate = mFps;
    finalize.timestamp = outputTimestamp;
    if (mConfig.options & RENDER_OPT_BAKE_ISO)
        finalize.isoOverlay = resolved.iso;
    finalize.packToWhiteLevel = !mConfig.cameraNativeStaging;
    finalize.compression = jpegCompression;
    finalize.gyroflowLensProfile = mGyroflowLensProfile && !mConfig.cameraNativeStaging
        ? &*mGyroflowLensProfile : nullptr;
    finalize.sourceName = "source";
    vfs::finalizeDng(bytes, mConfig, finalize);
    logStage(jpegCompression ? "finalize and compression" : "log/timing/bit packing",
             bytes.size());
    const auto completed = std::chrono::steady_clock::now();
    spdlog::info("DNG timing [{}]: transform total {:.1f} ms",
                 frame.filePath,
                 std::chrono::duration<double, std::milli>(completed - started).count());
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

    const auto& calibPath = mSidecarPath;
    nlohmann::json sidecarMetadata;
    std::optional<CalibrationData> calibration;
    vfs::loadSidecar(calibPath, sidecarMetadata, calibration, true);
    auto manualVignetteSidecars =
        vfs::loadManualVignetteSidecars(mSrcPath);
    auto gyroflow = vfs::loadGyroflowLensProfile(
        mGyroflowSidecarPath, true);
    if (sameRenderSettings(mConfig, config) && sidecarMetadata == mSidecarMetadata &&
        !gyroflow && !mGyroflowLensProfile &&
        manualVignetteSidecars.candidates.empty() &&
        mManualVignetteSidecars.candidates.empty())
        return;

    mCache.clear();
    mPayloadHashes.clear();
    mConfig = config;
    mSidecarMetadata = std::move(sidecarMetadata);
    mCalibration = std::move(calibration);
    mManualVignetteSidecars = std::move(manualVignetteSidecars);
    mGyroflowLensProfile = std::move(gyroflow);
    if (mCalibration && mCalibration->hasLevels) mConfig.levels = mCalibration->levels;
    if (mCalibration && mCalibration->hasCenterCrop) {
        mConfig.cropTarget = std::to_string(mCalibration->centerCrop[0]) + "x" +
                             std::to_string(mCalibration->centerCrop[1]);
        mConfig.options |= RENDER_OPT_CROPPING;
    }
    if (boost::filesystem::exists(calibPath)) {
        if (mCalibration)
            spdlog::info("Reloaded calibration for DNG sequence: {}", calibPath.string());
    }
    mHasCfa = mDecoder->getCFAMetadata(0, mCfaSize, mCfaPhase);
    if (mCalibration && mCalibration->hasCfaSize && mCalibration->cfaSize > 0) {
        mCfaSize = mCalibration->cfaSize;
        mHasCfa = mCfaSize >= 2;
    }
    if (mCalibration && !mCalibration->cfaPhase.empty()) {
        const std::string phase =
            boost::algorithm::to_lower_copy(mCalibration->cfaPhase);
        if (phase == "rggb") mCfaPhase = {0, 1, 1, 2};
        else if (phase == "grbg") mCfaPhase = {1, 0, 2, 1};
        else if (phase == "gbrg") mCfaPhase = {1, 2, 0, 1};
        else if (phase == "bggr") mCfaPhase = {2, 1, 1, 0};
        else spdlog::warn("Ignoring invalid sidecar CFA phase '{}'",
                          mCalibration->cfaPhase);
    }

    init();
}

FileInfo VirtualFileSystemImpl_DNG::getFileInfo() const {
    FileInfo info = vfs::makeFileInfo(
        mFrameRateInfo, mFps, mTotalFrames, mDroppedFrames,
        mDuplicatedFrames, mWidth, mHeight);
    info.isSequence = mHasFrameNumberSequence;
    if (!mDecoder->getFrames().empty()) {
        DNGFrameMetadata metadata;
        if (mDecoder->getFrameMetadata(0, metadata)) info.orientation = metadata.orientation;
    }
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
    
    // DNG sequences are pass-through, so we show source format
    info.dataType = vfs::getDisplayDataType(!mHasCfa, mHasCfa ? mCfaSize : 0) + " (DNG)";
    const bool applyLogCurve = (mConfig.options & RENDER_OPT_LOG_TRANSFORM) &&
        mConfig.logTransform != LogTransformMode::Disabled &&
        (mConfig.logTransform != LogTransformMode::KeepInput ||
         (mSourceHasGainMap &&
          (mConfig.options & RENDER_OPT_APPLY_VIGNETTE_CORRECTION))) &&
        !(mConfig.options & RENDER_OPT_DEBUG_SHADING_MAP);
    info.levelsInfo = vfs::getDisplayDataLevels(
        mSourceWhiteLevel, mSourceBlackLevel,
        mSourceWhiteLevel, mSourceBlackLevel,
        mConfig.levels,
        applyLogCurve ? logTransformModeToString(mConfig.logTransform) : std::string(),
        mSourceHasGainMap && (mConfig.options & RENDER_OPT_APPLY_VIGNETTE_CORRECTION),
        mConfig.options & RENDER_OPT_NORMALIZE_SHADING_MAP,
        mSourceInputBitDepth);
    
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
    info.timingUsesCfrMapping = mHasFrameNumberSequence &&
        (mConfig.options & RENDER_OPT_FRAMERATE_CONVERSION);

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
