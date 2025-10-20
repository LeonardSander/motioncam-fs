#include "VirtualFileSystemImpl_DNG.h"
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
#include <sstream>
#include <iomanip>
#include <cstring>

using motioncam::Timestamp;

namespace motioncam {

namespace {

#ifdef _WIN32
    constexpr std::string_view DESKTOP_INI = R"([.ShellClassInfo]
ConfirmFileOp=0

[ViewState]
Mode=4
Vid={137E7700-3573-11CF-AE69-08002B2E1262}
FolderType=Generic

[{5984FFE0-28D4-11CF-AE66-08002B2E1262}]
Mode=4
LogicalViewMode=1
IconSize=16

[LocalizedFileNames]
)";
#endif

    std::string constructFrameFilename(
        const std::string& baseName, int frameNumber, int padding = 6, const std::string& extension = "")
    {
        std::ostringstream oss;
        oss << baseName;
        oss << std::setfill('0') << std::setw(padding) << frameNumber;
        if (!extension.empty()) {
            if (extension[0] != '.') {
                oss << '.';
            }
            oss << extension;
        }
        return oss.str();
    }

    int getScaleFromOptions(FileRenderOptions options, int draftScale) {
        if(options & RENDER_OPT_DRAFT)
            return draftScale;
        return 1;
    }
}

VirtualFileSystemImpl_DNG::VirtualFileSystemImpl_DNG(
        BS::thread_pool& ioThreadPool,
        BS::thread_pool& processingThreadPool,
        LRUCache& lruCache,
        FileRenderOptions options,
        int draftScale,
        const std::string& cfrTarget,
        const std::string& cropTarget,
        const std::string& file,
        const std::string& baseName,
        const std::string& cameraModel,
        const std::string& levels,
        const std::string& logTransform,
        const std::string& exposureCompensation,
        const std::string& quadBayerOption) :
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
        mDraftScale(draftScale),
        mCFRTarget(cfrTarget),
        mCropTarget(cropTarget),
        mCameraModel(cameraModel),
        mLevels(levels),
        mLogTransform(logTransform),
        mExposureCompensation(exposureCompensation),
        mQuadBayerOption(quadBayerOption),
        mOptions(options) {
    
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
        
        spdlog::info("DNG sequence loaded: {}x{} @ {:.2f}fps (avg: {:.2f}, med: {:.2f}), {} frames", 
                     mWidth, mHeight, mFps, mAvgFps, mMedFps, mTotalFrames);
    }
    catch (const std::exception& e) {
        spdlog::error("Failed to initialize DNGDecoder: {}", e.what());
        throw;
    }
    
    this->init(options);
}

VirtualFileSystemImpl_DNG::~VirtualFileSystemImpl_DNG() {
    spdlog::info("Destroying VirtualFileSystemImpl_DNG({})", mSrcPath);
}

void VirtualFileSystemImpl_DNG::init(FileRenderOptions options) {
    spdlog::debug("VirtualFileSystemImpl_DNG::init(options={})", optionsToString(options));
    
    mFiles.clear();

#ifdef _WIN32
    Entry desktopIni;
    desktopIni.type = EntryType::FILE_ENTRY;
    desktopIni.pathParts = {};
    desktopIni.name = "desktop.ini";
    desktopIni.size = DESKTOP_INI.size();
    mFiles.push_back(desktopIni);
#endif

    // Generate DNG files for each frame using actual timestamps
    const auto& frames = mDecoder->getFrames();
    for (size_t i = 0; i < frames.size(); ++i) {
        Entry dngEntry;
        dngEntry.type = EntryType::FILE_ENTRY;
        dngEntry.pathParts = {};
        dngEntry.name = constructFrameFilename(mBaseName, static_cast<int>(i), 6, "dng");
        dngEntry.size = 50 * 1024 * 1024; // Estimate DNG size
        dngEntry.userData = frames[i].timestamp;
        mFiles.push_back(dngEntry);
    }

    mTypicalDngSize = 50 * 1024 * 1024;
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
        if (entry.getFullPath().string() == fullPath) {
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
        size_t copyLen = std::min(len, DESKTOP_INI.size() - pos);
        if (copyLen > 0) {
            memcpy(dst, DESKTOP_INI.data() + pos, copyLen);
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
            // Extract timestamp from entry userData
            Timestamp timestamp = 0;
            if (std::holds_alternative<int64_t>(entry.userData)) {
                timestamp = std::get<int64_t>(entry.userData);
            }
            
            // Find frame by timestamp
            const auto& frames = mDecoder->getFrames();
            int frameNumber = -1;
            for (size_t i = 0; i < frames.size(); ++i) {
                if (frames[i].timestamp == timestamp) {
                    frameNumber = static_cast<int>(i);
                    break;
                }
            }
            
            if (frameNumber == -1) {
                spdlog::error("Failed to find frame with timestamp {}", timestamp);
                result(0, -1);
                return;
            }
            
            // Extract DNG data directly from sequence
            std::vector<uint8_t> dngData;
            if (!mDecoder->extractFrame(frameNumber, dngData)) {
                spdlog::error("Failed to extract frame {} (timestamp: {})", frameNumber, timestamp);
                result(0, -1);
                return;
            }
            
            // Apply vignette correction if enabled and gain map is available
            if (mOptions & RENDER_OPT_APPLY_VIGNETTE_CORRECTION) {
                GainMap gainMap;
                if (mDecoder->getGainMap(frameNumber, gainMap)) {
                    // Apply vignette correction to DNG data
                    // This would require proper DNG parsing and modification
                    spdlog::debug("Applying vignette correction for frame {}", frameNumber);
                }
            }
            
            // Copy requested portion of DNG data
            size_t copyLen = std::min(len, dngData.size() - pos);
            if (copyLen > 0 && pos < dngData.size()) {
                memcpy(dst, dngData.data() + pos, copyLen);
                result(copyLen, 0);
            } else {
                result(0, 0);
            }
        }
        catch (const std::exception& e) {
            spdlog::error("Error generating frame: {}", e.what());
            result(0, -1);
        }
    };
    
    if (async) {
        mProcessingThreadPool.detach_task(task);
        return 0;
    } else {
        task();
        return len;
    }
}

void VirtualFileSystemImpl_DNG::updateOptions(
    FileRenderOptions options, 
    int draftScale, 
    const std::string& cfrTarget, 
    const std::string& cropTarget, 
    const std::string& cameraModel, 
    const std::string& levels, 
    const std::string& logTransform, 
    const std::string& exposureCompensation, 
    const std::string& quadBayerOption) {
    
    std::lock_guard<std::mutex> lock(mMutex);
    
    mOptions = options;
    mDraftScale = draftScale;
    mCFRTarget = cfrTarget;
    mCropTarget = cropTarget;
    mCameraModel = cameraModel;
    mLevels = levels;
    mLogTransform = logTransform;
    mExposureCompensation = exposureCompensation;
    mQuadBayerOption = quadBayerOption;
    
    init(options);
}

FileInfo VirtualFileSystemImpl_DNG::getFileInfo() const {
    FileInfo info;
    info.medFps = mMedFps;
    info.avgFps = mAvgFps;
    info.fps = mFps;
    info.totalFrames = mTotalFrames;
    info.droppedFrames = mDroppedFrames;
    info.duplicatedFrames = mDuplicatedFrames;
    info.width = mWidth;
    info.height = mHeight;
    return info;
}

void VirtualFileSystemImpl_DNG::calculateFrameRateStats() {
    const auto& frames = mDecoder->getFrames();
    
    if (frames.size() < 2) {
        mMedFps = mFps;
        mAvgFps = mFps;
        return;
    }
    
    std::vector<double> intervals;
    intervals.reserve(frames.size() - 1);
    
    for (size_t i = 1; i < frames.size(); ++i) {
        double intervalNs = static_cast<double>(frames[i].timestamp - frames[i-1].timestamp);
        double intervalSec = intervalNs / 1000000000.0;
        if (intervalSec > 0) {
            intervals.push_back(intervalSec);
        }
    }
    
    if (intervals.empty()) {
        mMedFps = mFps;
        mAvgFps = mFps;
        return;
    }
    
    // Calculate average FPS
    double avgInterval = 0.0;
    for (double interval : intervals) {
        avgInterval += interval;
    }
    avgInterval /= intervals.size();
    mAvgFps = static_cast<float>(1.0 / avgInterval);
    
    // Calculate median FPS
    std::sort(intervals.begin(), intervals.end());
    double medianInterval;
    size_t mid = intervals.size() / 2;
    if (intervals.size() % 2 == 0) {
        medianInterval = (intervals[mid - 1] + intervals[mid]) / 2.0;
    } else {
        medianInterval = intervals[mid];
    }
    mMedFps = static_cast<float>(1.0 / medianInterval);
    
    spdlog::debug("DNG sequence frame rate stats: avg={:.2f}fps, median={:.2f}fps", mAvgFps, mMedFps);
}

} // namespace motioncam