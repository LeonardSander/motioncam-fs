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
    boost::filesystem::path calibPath = srcPath.parent_path() / (srcPath.stem().string() + ".json");
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

#ifdef _WIN32
    Entry desktopIni;
    desktopIni.type = EntryType::FILE_ENTRY;
    desktopIni.pathParts = {};
    desktopIni.name = "desktop.ini";
    desktopIni.size = vfs::DESKTOP_INI.size();
    mFiles.push_back(desktopIni);
#endif

    // Generate DNG files for each frame using actual timestamps
    const auto& frames = mDecoder->getFrames();
    for (size_t i = 0; i < frames.size(); ++i) {
        Entry dngEntry;
        dngEntry.type = EntryType::FILE_ENTRY;
        dngEntry.pathParts = {};
        dngEntry.name = vfs::constructFrameFilename(mBaseName, static_cast<int>(i), 6, "dng");
        dngEntry.size = boost::filesystem::file_size(frames[i].filePath);
        dngEntry.userData = frames[i].timestamp;
        const bool addBaseline = (mConfig.options & RENDER_OPT_NORMALIZE_EXPOSURE) &&
                                 !mHasBaselineExposure[frames[i].timestamp];
        const bool addNeutral = (mConfig.options & RENDER_OPT_SMOOTH_WHITE_BALANCE) &&
                                !mHasAsShotNeutral[frames[i].timestamp];
        GainMap sizeGainMap;
        const bool bakeGainMap = (mConfig.options & RENDER_OPT_APPLY_VIGNETTE_CORRECTION) &&
                                 mDecoder->getGainMap(static_cast<int>(i), sizeGainMap);
        if (addBaseline || addNeutral || bakeGainMap) {
            std::vector<uint8_t> sizedData;
            if (!mDecoder->extractFrame(static_cast<int>(i), sizedData))
                throw std::runtime_error("Could not size transformed DNG");
            if (bakeGainMap && !DNGDecoder::bakeGainMaps(
                    sizedData,
                    mConfig.options & RENDER_OPT_NORMALIZE_SHADING_MAP,
                    mConfig.options & RENDER_OPT_VIGNETTE_ONLY_COLOR))
                throw std::runtime_error("Unsupported DNG layout for vignette baking: " + frames[i].filePath);
            double baseline = mNormalizedExposureOffsets.at(frames[i].timestamp);
            const auto& neutral = mSmoothedAsShotNeutrals.at(frames[i].timestamp);
            if (!DNGDecoder::updateMetadata(sizedData, addBaseline ? &baseline : nullptr,
                                            addNeutral ? &neutral : nullptr))
                throw std::runtime_error("Could not size transformed DNG metadata");
            dngEntry.size = sizedData.size();
        }
        mFiles.push_back(dngEntry);
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
    const Entry& entry, bool /*jpegCompression*/) {
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

    const int frameIndex = static_cast<int>(std::distance(frames.begin(), it));
    GainMap gainMap;
    if ((mConfig.options & RENDER_OPT_APPLY_VIGNETTE_CORRECTION) &&
        mDecoder->getGainMap(frameIndex, gainMap) &&
        !DNGDecoder::bakeGainMaps(
            bytes,
            mConfig.options & RENDER_OPT_NORMALIZE_SHADING_MAP,
            mConfig.options & RENDER_OPT_VIGNETTE_ONLY_COLOR))
        throw std::runtime_error("Unsupported DNG layout for vignette baking: " + it->filePath);

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
    return std::make_shared<std::vector<char>>(bytes.begin(), bytes.end());
}

void VirtualFileSystemImpl_DNG::updateOptions(const RenderSettings& config) {
    std::lock_guard<std::mutex> lock(mMutex);
    
    mConfig = config;
    
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
    info.dataType = "Bayer CFA (DNG)";
    info.levelsInfo = "Source DNG";
    
    // Calculate runtime from frame count and fps
    info.runtimeSeconds = (mFps > 0) ? (static_cast<float>(mTotalFrames) / mFps) : 0.0f;
    
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
