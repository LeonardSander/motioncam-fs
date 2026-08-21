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

namespace {

std::vector<double> temporalMedian(const std::vector<double>& values, int radius) {
    std::vector<double> result(values.size());
    std::vector<double> window;
    window.reserve(static_cast<size_t>(radius * 2 + 1));
    for (size_t i = 0; i < values.size(); ++i) {
        const size_t begin = i > static_cast<size_t>(radius) ? i - radius : 0;
        const size_t end = std::min(values.size(), i + static_cast<size_t>(radius) + 1);
        window.assign(values.begin() + begin, values.begin() + end);
        const auto middle = window.begin() + window.size() / 2;
        std::nth_element(window.begin(), middle, window.end());
        result[i] = *middle;
    }
    return result;
}

std::vector<double> temporalSmooth(const std::vector<double>& values, int radius) {
    const auto stable = temporalMedian(values, radius);
    std::vector<double> result(values.size());
    const double sigma = std::max(1.0, radius / 2.0);
    for (size_t i = 0; i < stable.size(); ++i) {
        const size_t begin = i > static_cast<size_t>(radius) ? i - radius : 0;
        const size_t end = std::min(stable.size(), i + static_cast<size_t>(radius) + 1);
        double sum = 0.0;
        double weightSum = 0.0;
        for (size_t j = begin; j < end; ++j) {
            const double distance = static_cast<double>(j) - static_cast<double>(i);
            const double weight = std::exp(-0.5 * distance * distance / (sigma * sigma));
            sum += stable[j] * weight;
            weightSum += weight;
        }
        result[i] = sum / weightSum;
    }
    return result;
}

} // namespace

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
        mMedFps(0),
        mAvgFps(0),
        mTotalFrames(0),
        mDroppedFrames(0),
        mDuplicatedFrames(0),
        mWidth(0),
        mHeight(0),
        mConfig(config) {
    
    // Load calibration JSON if it exists (for DNG folder)
    boost::filesystem::path srcPath(mSrcPath);
    boost::filesystem::path calibPath = boost::filesystem::is_directory(srcPath)
        ? srcPath / (srcPath.filename().string() + ".json")
        : srcPath.parent_path() / (srcPath.stem().string() + ".json");
    if (boost::filesystem::exists(calibPath)) {
        mCalibration = CalibrationData::loadFromFile(calibPath.string());
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
        std::vector<double> effectiveExposures;
        std::array<std::vector<double>, 3> logNeutrals;
        std::vector<DNGFrameMetadata> metadata(frames.size());
        effectiveExposures.reserve(frames.size());
        double minimumEffectiveExposure = std::numeric_limits<double>::max();
        for (size_t i = 0; i < frames.size(); ++i) {
            if (!mDecoder->getFrameMetadata(static_cast<int>(i), metadata[i]) ||
                !metadata[i].hasExposure) {
                throw std::runtime_error("DNG frame is missing ISO or ExposureTime: " + frames[i].filePath);
            }
            const double logExposure = std::log2(metadata[i].iso * metadata[i].exposureTime);
            const double effective = logExposure + metadata[i].baselineExposure;
            effectiveExposures.push_back(effective);
            minimumEffectiveExposure = std::min(minimumEffectiveExposure, effective);
            mHasBaselineExposure[frames[i].timestamp] = metadata[i].hasBaselineExposure;
            mHasAsShotNeutral[frames[i].timestamp] = metadata[i].hasAsShotNeutral;
            mExposureTimes[frames[i].timestamp] = metadata[i].exposureTime;
            mIsoValues[frames[i].timestamp] = metadata[i].iso;
            for (size_t c = 0; c < 3; ++c)
                logNeutrals[c].push_back(std::log(std::max(1e-6f, metadata[i].asShotNeutral[c])));
        }
        const int radius = std::max(1, static_cast<int>(std::lround(2.0 * std::max(1.0f, mMedFps))));
        const auto smoothedExposure = temporalSmooth(effectiveExposures, radius);
        std::array<std::vector<double>, 3> smoothedNeutral;
        for (size_t c = 0; c < 3; ++c)
            smoothedNeutral[c] = temporalSmooth(logNeutrals[c], radius);
        for (size_t i = 0; i < frames.size(); ++i) {
            const double logExposure = std::log2(metadata[i].iso * metadata[i].exposureTime);
            mNormalizedExposureOffsets[frames[i].timestamp] =
                static_cast<float>(minimumEffectiveExposure - logExposure);
            mSmoothedExposureOffsets[frames[i].timestamp] =
                static_cast<float>(smoothedExposure[i] - logExposure);
            const double green = smoothedNeutral[1][i];
            for (size_t c = 0; c < 3; ++c)
                mSmoothedAsShotNeutrals[frames[i].timestamp][c] =
                    static_cast<float>(std::exp(smoothedNeutral[c][i] - green));
        }
        
        spdlog::info("DNG sequence loaded: {}x{} @ {:.2f}fps (avg: {:.2f}, med: {:.2f}), {} frames", 
                     mWidth, mHeight, mFps, mAvgFps, mMedFps, mTotalFrames);
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
    FrameRateInfo frameRateInfo{};
    frameRateInfo.medianFrameRate = mMedFps;
    frameRateInfo.averageFrameRate = mAvgFps;
    mFps = vfs::determineCFRTarget(frameRateInfo, mConfig.cfrTarget, applyCFRConversion);

#ifdef _WIN32
    Entry desktopIni;
    desktopIni.type = EntryType::FILE_ENTRY;
    desktopIni.pathParts = {};
    desktopIni.name = "desktop.ini";
    desktopIni.size = vfs::DESKTOP_INI.size();
    mFiles.push_back(desktopIni);
#endif

    // Size a source entry for each DNG, then map those entries to the output
    // cadence in exactly the same way as MCRAW and DirectLog.
    const auto& frames = mDecoder->getFrames();
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
        const bool addBaseline = (mConfig.options & RENDER_OPT_NORMALIZE_EXPOSURE) &&
                                 !mHasBaselineExposure[frames[i].timestamp];
        const bool addNeutral = (mConfig.options & RENDER_OPT_SMOOTH_WHITE_BALANCE) &&
                                !mHasAsShotNeutral[frames[i].timestamp];
        GainMap sizeGainMap;
        const bool hasGainMap = mDecoder->getGainMap(static_cast<int>(i), sizeGainMap);
        const bool bakeGainMap = (mConfig.options & RENDER_OPT_APPLY_VIGNETTE_CORRECTION) &&
                                 hasGainMap;
        const bool processHigher = mCfaSize > 2 || (mHasCfa && mConfig.cameraNativeStaging) ||
            vfs::getScaleFromOptions(mConfig.options, mConfig.draftScale) > 1 ||
            (mConfig.options & RENDER_OPT_REMOSAIC_TO_BAYER);
        {
            std::vector<uint8_t> sizedData;
            if (!mDecoder->extractFrame(static_cast<int>(i), sizedData) ||
                !DNGDecoder::ensureUncompressed(sizedData))
                throw std::runtime_error("Could not size uncompressed DNG");
            const std::optional<bool> gainMapOrderOverride =
                mCalibration && mCalibration->hasNeedGainMapOrderFixed
                    ? std::optional<bool>(mCalibration->needGainMapOrderFixed)
                    : std::nullopt;
            if (!DNGDecoder::repairGainMapCfaPhase(sizedData, gainMapOrderOverride))
                throw std::runtime_error("Could not reconcile DNG gain maps with its CFA phase");
            if (mCalibration && mCalibration->hasFullSensorResolution &&
                !DNGDecoder::cropGainMapsToFullSensor(
                    sizedData, mCalibration->fullSensorResolution[0],
                    mCalibration->fullSensorResolution[1]))
                throw std::runtime_error("Could not crop full-sensor DNG gain maps");
            if (!DNGDecoder::overrideDataLevels(sizedData, mConfig.levels))
                throw std::runtime_error("Could not override source DNG data levels");
            DNGDecoder::repairExposureTime(sizedData, mExposureTimes.at(frames[i].timestamp));
            double baseline = mNormalizedExposureOffsets.at(frames[i].timestamp);
            const auto& neutral = mSmoothedAsShotNeutrals.at(frames[i].timestamp);
            if (!DNGDecoder::updateMetadata(sizedData, addBaseline ? &baseline : nullptr,
                                            addNeutral ? &neutral : nullptr))
                throw std::runtime_error("Could not size transformed DNG metadata");
            if (hasGainMap && !bakeGainMap &&
                (mConfig.options & RENDER_OPT_VIGNETTE_ONLY_COLOR) &&
                !DNGDecoder::transformGainMaps(sizedData, false, true, false))
                throw std::runtime_error("Unsupported DNG gain-map color transform: " + frames[i].filePath);
            if (hasGainMap && (mConfig.options & RENDER_OPT_OPTIMIZE_GAIN_MAPS) &&
                !DNGDecoder::transformGainMaps(sizedData, false, false, true))
                throw std::runtime_error("Unsupported DNG gain-map optimization: " + frames[i].filePath);
            if (bakeGainMap && !DNGDecoder::bakeGainMaps(
                    sizedData, mConfig.options & RENDER_OPT_NORMALIZE_SHADING_MAP,
                    mConfig.options & RENDER_OPT_VIGNETTE_ONLY_COLOR,
                    mConfig.options & RENDER_OPT_OPTIMIZE_GAIN_MAPS,
                    mConfig.options & RENDER_OPT_DEBUG_SHADING_MAP))
                throw std::runtime_error("Unsupported DNG layout for vignette baking: " + frames[i].filePath);
            if (hasGainMap && !bakeGainMap &&
                !DNGDecoder::canonicalizeGainMapOpcodes(sizedData))
                throw std::runtime_error("Could not canonicalize DNG gain maps: " + frames[i].filePath);
            if (processHigher && !DNGDecoder::processHigherCFA(
                    sizedData, mCfaSize, mCfaPhase, mConfig.quadBayerOption,
                    mConfig.options & RENDER_OPT_REMOSAIC_TO_BAYER,
                    vfs::getScaleFromOptions(mConfig.options, mConfig.draftScale),
                    mConfig.options & RENDER_OPT_HIGHER_CFA_HQ))
                throw std::runtime_error("Unsupported DNG layout for higher CFA processing: " + frames[i].filePath);
            if ((mConfig.options & RENDER_OPT_BAKE_ISO) &&
                !DNGDecoder::bakeIsoOverlay(sizedData, mIsoValues.at(frames[i].timestamp)))
                throw std::runtime_error("Unsupported DNG layout for ISO overlay: " + frames[i].filePath);
            if ((mConfig.options & RENDER_OPT_LOG_TRANSFORM) &&
                !(mConfig.options & RENDER_OPT_DEBUG_SHADING_MAP) &&
                (mConfig.logTransform != LogTransformMode::KeepInput || bakeGainMap) &&
                !DNGDecoder::applyLogTransform(sizedData, mConfig.logTransform))
                throw std::runtime_error("Could not apply DNG log transform: " + frames[i].filePath);
            if (!DNGDecoder::setTimingMetadata(sizedData, mFps, 0))
                throw std::runtime_error("Could not size DNG timing metadata");
            if (!mConfig.cameraNativeStaging &&
                !DNGDecoder::packUncompressedToWhiteLevel(sizedData))
                throw std::runtime_error("Could not pack uncompressed DNG to its sensor bit depth");
            dngEntry.size = sizedData.size();
        }
        sourceEntries.push_back(dngEntry);
    }

    int nextOutput = 0;
    if (applyCFRConversion && !sourceEntries.empty()) {
        size_t previousSource = 0;
        for (size_t i = 0; i < sourceEntries.size(); ++i) {
            const int pts = vfs::getFrameNumberFromTimestamp(
                frames[i].timestamp, frames.front().timestamp, mFps);
            if (pts < nextOutput) {
                ++mDroppedFrames;
                continue;
            }
            while (nextOutput < pts) {
                Entry held = sourceEntries[previousSource];
                held.duplicateFrame = true;
                held.name = vfs::constructFrameFilename(mBaseName, nextOutput++, 6, "dng");
                mFiles.push_back(std::move(held));
                ++mDuplicatedFrames;
            }
            Entry current = sourceEntries[i];
            current.name = vfs::constructFrameFilename(mBaseName, nextOutput++, 6, "dng");
            mFiles.push_back(std::move(current));
            previousSource = i;
        }
    } else {
        for (auto& entry : sourceEntries) {
            entry.name = vfs::constructFrameFilename(mBaseName, nextOutput++, 6, "dng");
            mFiles.push_back(std::move(entry));
        }
    }

    mTypicalDngSize = mFiles.empty() ? 0 : mFiles.back().size;
}

std::vector<Entry> VirtualFileSystemImpl_DNG::listFiles(const std::string& filter) const {
    std::lock_guard<std::mutex> lock(mMutex);
    
    if (filter.empty()) {
        return mFiles;
    }
    
    std::vector<Entry> filteredFiles;
    for (const auto& file : mFiles) {
        if (file.name.find(filter) != std::string::npos) {
            filteredFiles.push_back(file);
        }
    }
    
    return filteredFiles;
}

std::optional<Entry> VirtualFileSystemImpl_DNG::findEntry(const std::string& fullPath) const {
    std::lock_guard<std::mutex> lock(mMutex);
    
    for (const auto& entry : mFiles) {
        if (entry.getFullPath() == boost::filesystem::path(fullPath).relative_path()) {
            return entry;
        }
    }
    
    return std::nullopt;
}

int VirtualFileSystemImpl_DNG::readFile(
    const Entry& entry,
    const size_t pos,
    const size_t len,
    void* dst,
    std::function<void(size_t, int)> result,
    bool async) {
    
    if (entry.name == "desktop.ini") {
#ifdef _WIN32
        size_t copyLen = std::min(len, vfs::DESKTOP_INI.size() - pos);
        if (copyLen > 0) {
            memcpy(dst, vfs::DESKTOP_INI.data() + pos, copyLen);
        }
        result(copyLen, 0);
        return 0;
#endif
    }
    
    if (boost::ends_with(entry.name, ".dng")) {
        return generateFrame(entry, pos, len, dst, result, async);
    }
    
    result(0, -1);
    return -1;
}

size_t VirtualFileSystemImpl_DNG::generateFrame(
    const Entry& entry,
    const size_t pos,
    const size_t len,
    void* dst,
    std::function<void(size_t, int)> result,
    bool async) {
    auto task = [this, entry, pos, len, dst, result]() {
        try {
            auto data = materializeFile(entry, false);
            const size_t count = data && pos < data->size()
                ? std::min(len, data->size() - pos) : 0;
            if (count)
                std::memcpy(dst, data->data() + pos, count);
            result(count, 0);
        } catch (const std::exception& e) {
            spdlog::error("Error reading DNG frame: {}", e.what());
            result(0, -1);
        }
    };
    if (async) {
        mProcessingThreadPool.detach_task(task);
        return 0;
    }
    task();
    return 0;
}

std::shared_ptr<std::vector<char>> VirtualFileSystemImpl_DNG::materializeFile(
    const Entry& entry, bool jpegCompression) {
    if (!jpegCompression) if (auto cached = mCache.get(entry)) {
        mCache.put(entry, cached);
        return cached;
    }
    try {
    const auto timestamp = std::get<Timestamp>(entry.userData);
    const auto& frames = mDecoder->getFrames();
    const auto it = std::find_if(frames.begin(), frames.end(), [timestamp](const auto& frame) {
        return frame.timestamp == timestamp;
    });
    if (it == frames.end())
        throw std::runtime_error("DNG source frame not found");

    std::vector<uint8_t> bytes;
    if (!mDecoder->extractFrame(static_cast<int>(std::distance(frames.begin(), it)), bytes))
        throw std::runtime_error("Could not read source DNG");
    if (!DNGDecoder::ensureUncompressed(bytes))
        throw std::runtime_error("Could not decode source DNG to an uncompressed DNG");
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
    DNGDecoder::repairExposureTime(bytes, mExposureTimes.at(timestamp));

    const auto frameDigits = entry.name.substr(entry.name.size() - 10, 6);
    const int outputFrameNumber = std::stoi(frameDigits);
    const bool converted = mConfig.options & RENDER_OPT_FRAMERATE_CONVERSION;
    const Timestamp outputTimestamp = converted
        ? static_cast<Timestamp>(std::llround(outputFrameNumber * 1e9 / mFps))
        : timestamp - frames.front().timestamp;
    const bool normalize = mConfig.options & RENDER_OPT_NORMALIZE_EXPOSURE;
    const bool smoothExposure = mConfig.options & RENDER_OPT_SMOOTH_EXPOSURE;
    const bool smoothWhiteBalance = mConfig.options & RENDER_OPT_SMOOTH_WHITE_BALANCE;
    if (normalize || smoothWhiteBalance) {
        double baseline = smoothExposure
            ? mSmoothedExposureOffsets.at(timestamp)
            : mNormalizedExposureOffsets.at(timestamp);
        if (!mConfig.exposureCompensation.empty()) {
            try { baseline += std::stof(mConfig.exposureCompensation); }
            catch (const std::exception&) {}
        }
        const auto neutralIt = mSmoothedAsShotNeutrals.find(timestamp);
        const double* baselinePtr = normalize ? &baseline : nullptr;
        const std::array<float, 3>* neutralPtr = smoothWhiteBalance
            ? &neutralIt->second : nullptr;
        if (!DNGDecoder::updateMetadata(bytes, baselinePtr, neutralPtr))
            throw std::runtime_error("Could not update DNG exposure/white-balance tags");
    }

    const int frameIndex = static_cast<int>(std::distance(frames.begin(), it));
    GainMap gainMap;
    const bool hasGainMap = mDecoder->getGainMap(frameIndex, gainMap);
    const bool bakeGainMap = (mConfig.options & RENDER_OPT_APPLY_VIGNETTE_CORRECTION) &&
                             hasGainMap;
    if (hasGainMap && !bakeGainMap &&
        (mConfig.options & RENDER_OPT_VIGNETTE_ONLY_COLOR) &&
        !DNGDecoder::transformGainMaps(bytes, false, true, false))
        throw std::runtime_error("Unsupported DNG gain-map color transform: " + it->filePath);
    if (hasGainMap && (mConfig.options & RENDER_OPT_OPTIMIZE_GAIN_MAPS) &&
        !DNGDecoder::transformGainMaps(bytes, false, false, true))
        throw std::runtime_error("Unsupported DNG gain-map optimization: " + it->filePath);
    if (bakeGainMap && !DNGDecoder::bakeGainMaps(
            bytes, mConfig.options & RENDER_OPT_NORMALIZE_SHADING_MAP,
            mConfig.options & RENDER_OPT_VIGNETTE_ONLY_COLOR,
            mConfig.options & RENDER_OPT_OPTIMIZE_GAIN_MAPS,
            mConfig.options & RENDER_OPT_DEBUG_SHADING_MAP))
        throw std::runtime_error("Unsupported DNG layout for vignette baking: " + it->filePath);
    if (hasGainMap && !bakeGainMap && !DNGDecoder::canonicalizeGainMapOpcodes(bytes))
        throw std::runtime_error("Could not canonicalize DNG gain maps: " + it->filePath);

    if ((mCfaSize > 2 || (mHasCfa && mConfig.cameraNativeStaging) ||
         vfs::getScaleFromOptions(mConfig.options, mConfig.draftScale) > 1 ||
         (mConfig.options & RENDER_OPT_REMOSAIC_TO_BAYER)) &&
        !DNGDecoder::processHigherCFA(
            bytes, mCfaSize, mCfaPhase, mConfig.quadBayerOption,
            mConfig.options & RENDER_OPT_REMOSAIC_TO_BAYER,
            vfs::getScaleFromOptions(mConfig.options, mConfig.draftScale),
            mConfig.options & RENDER_OPT_HIGHER_CFA_HQ))
        throw std::runtime_error("Unsupported DNG layout for higher CFA processing: " + it->filePath);

    if ((mConfig.options & RENDER_OPT_BAKE_ISO) &&
        !DNGDecoder::bakeIsoOverlay(bytes, mIsoValues.at(timestamp)))
        throw std::runtime_error("Unsupported DNG layout for ISO overlay: " + it->filePath);
    if ((mConfig.options & RENDER_OPT_LOG_TRANSFORM) &&
        !(mConfig.options & RENDER_OPT_DEBUG_SHADING_MAP) &&
        (mConfig.logTransform != LogTransformMode::KeepInput || bakeGainMap) &&
        !DNGDecoder::applyLogTransform(bytes, mConfig.logTransform))
        throw std::runtime_error("Could not apply DNG log transform: " + it->filePath);
    if (!DNGDecoder::setTimingMetadata(bytes, mFps, outputTimestamp))
        throw std::runtime_error("Could not update DNG timing metadata");
    if (!mConfig.cameraNativeStaging && !DNGDecoder::packUncompressedToWhiteLevel(bytes))
        throw std::runtime_error("Could not pack uncompressed DNG to its sensor bit depth");
    if (jpegCompression) {
        const bool compressed = mConfig.jxlDistance < 0.0f
            ? DNGDecoder::compressLosslessJPEG(bytes)
            : DNGDecoder::compressJPEGXL(bytes, mConfig.jxlDistance);
        if (!compressed) throw std::runtime_error("Could not compress finalized DNG");
    }
    auto output = std::make_shared<std::vector<char>>(bytes.begin(), bytes.end());
    if (!jpegCompression) mCache.put(entry, output);
    return output;
    } catch (...) {
        if (!jpegCompression) mCache.markLoadFailed(entry);
        throw;
    }
}

bool VirtualFileSystemImpl_DNG::sourceImagePayloadsEqual(const Entry& left,
                                                         const Entry& right) {
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
        if (const auto cached = mPayloadHashes.find(index); cached != mPayloadHashes.end())
            return cached->second;
        std::vector<uint8_t> bytes;
        if (!mDecoder->extractFrame(static_cast<int>(index), bytes))
            return std::nullopt;
        uint64_t hash = 0;
        if (!DNGDecoder::imagePayloadHash(bytes, hash)) return std::nullopt;
        mPayloadHashes.emplace(index, hash);
        return hash;
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
    std::lock_guard<std::mutex> lock(mMutex);
    mCache.clear();
    mPayloadHashes.clear();
    mConfig = config;

    const boost::filesystem::path srcPath(mSrcPath);
    const boost::filesystem::path calibPath = boost::filesystem::is_directory(srcPath)
        ? srcPath / (srcPath.filename().string() + ".json")
        : srcPath.parent_path() / (srcPath.stem().string() + ".json");
    mCalibration.reset();
    if (boost::filesystem::exists(calibPath)) {
        mCalibration = CalibrationData::loadFromFile(calibPath.string());
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
    info.frameRateInfo.medianFrameRate = mMedFps;
    info.frameRateInfo.averageFrameRate = mAvgFps;
    info.fps = mFps;
    info.totalFrames = mTotalFrames;
    info.droppedFrames = mDroppedFrames;
    info.duplicatedFrames = mDuplicatedFrames;
    info.width = mWidth;
    info.height = mHeight;
    
    // DNG sequences are pass-through, so we show source format
    info.dataType = vfs::getDisplayDataType(!mHasCfa, mHasCfa ? mCfaSize : 0) + " (DNG)";
    const bool sourceLevels = mConfig.levels.empty() || mConfig.levels == "Dynamic" ||
        mConfig.levels == "Static" || mConfig.levels == "Dynamic/Dynamic" ||
        mConfig.levels == "Dynamic/Static" || mConfig.levels == "Static/Dynamic" ||
        mConfig.levels == "Static/Static";
    info.levelsInfo = sourceLevels ? "Source DNG" : mConfig.levels + " (DNG override)";
    
    // Calculate runtime from frame count and fps
    const int outputFrames = mTotalFrames - mDroppedFrames + mDuplicatedFrames;
    info.runtimeSeconds = (mFps > 0) ? (static_cast<float>(outputFrames) / mFps) : 0.0f;
    
    return info;
}

void VirtualFileSystemImpl_DNG::calculateFrameRateStats() {
    const auto& frames = mDecoder->getFrames();
    
    if (frames.size() < 2) {
        mMedFps = mFps;
        mAvgFps = mFps;
        return;
    }
    
    // Convert frame timestamps to vector of Timestamp
    std::vector<Timestamp> timestamps;
    timestamps.reserve(frames.size());
    for (const auto& frame : frames) {
        timestamps.push_back(frame.timestamp);
    }
    
    auto frameRateInfo = vfs::calculateFrameRate(timestamps);
    mMedFps = frameRateInfo.medianFrameRate;
    mAvgFps = frameRateInfo.averageFrameRate;
    
    spdlog::debug("DNG sequence frame rate stats: avg={:.2f}fps, median={:.2f}fps", mAvgFps, mMedFps);
}

} // namespace motioncam
