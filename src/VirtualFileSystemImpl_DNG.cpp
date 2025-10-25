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
#include <cstring>

using motioncam::Timestamp;

namespace motioncam {

VirtualFileSystemImpl_DNG::VirtualFileSystemImpl_DNG(
        BS::thread_pool& ioThreadPool,
        BS::thread_pool& processingThreadPool,
        LRUCache& lruCache,
        const RenderConfig& config,
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
            if (mConfig.options & RENDER_OPT_APPLY_VIGNETTE_CORRECTION) {
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

void VirtualFileSystemImpl_DNG::updateOptions(const RenderConfig& config) {
    std::lock_guard<std::mutex> lock(mMutex);
    
    mConfig = config;
    
    init();
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