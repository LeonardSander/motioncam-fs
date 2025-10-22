#include "VirtualFileSystemImpl_DirectLog.h"
#include "DirectLogDecoder.h"
#include "CalibrationData.h"
#include "Utils.h"
#include "LRUCache.h"
#include "Types.h"

#include <boost/filesystem.hpp>
#include <boost/algorithm/string.hpp>

#include <BS_thread_pool.hpp>
#include <spdlog/spdlog.h>
#include "tinydng/tiny_dng_writer.h"

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
    
    int getFrameNumberFromTimestamp(Timestamp timestamp, Timestamp referenceTimestamp, float frameRate) {
        if (frameRate <= 0) {
            return -1; // Invalid frame rate
        }

        int64_t timeDifference = timestamp - referenceTimestamp;
        if (timeDifference < 0) {
            return -1;
        }

        // Calculate nanoseconds per frame
        double nanosecondsPerFrame = 1000000000.0 / frameRate;

        // Calculate expected frame number
        return static_cast<int>(std::round(timeDifference / nanosecondsPerFrame));
    }
}

VirtualFileSystemImpl_DirectLog::VirtualFileSystemImpl_DirectLog(
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
        const std::string& quadBayerOption,
        const std::string& cfaPhase) :
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
        mCfaPhase(cfaPhase),
        mOptions(options),
        mIsHLG(false) {
    
    // Parse exposure keyframes if the input contains keyframe syntax
    mExposureKeyframes = ExposureKeyframes::parse(exposureCompensation);
    
    // Load calibration JSON if it exists
    boost::filesystem::path srcPath(mSrcPath);
    boost::filesystem::path calibPath = srcPath.parent_path() / (srcPath.stem().string() + ".json");
    if (boost::filesystem::exists(calibPath)) {
        mCalibration = CalibrationData::loadFromFile(calibPath.string());
        if (mCalibration.has_value()) {
            spdlog::info("Loaded calibration for DirectLog: {}", calibPath.string());
        }
    }
    
    // Initialize DirectLogDecoder
    try {
        mDecoder = std::make_unique<DirectLogDecoder>(mSrcPath);
        const auto& videoInfo = mDecoder->getVideoInfo();
        
        mWidth = videoInfo.width;
        mHeight = videoInfo.height;
        mTotalFrames = static_cast<int>(videoInfo.totalFrames);
        mPixelFormat = videoInfo.pixelFormat;
        mIsHLG = videoInfo.isHLG;
        mDroppedFrames = 0;
        mDuplicatedFrames = 0;
        
        // Calculate frame rate statistics from actual frame timestamps
        calculateFrameRateStats();
        
        spdlog::info("DirectLog video loaded: {}x{} @ {:.2f}fps (avg: {:.2f}, med: {:.2f}), {} frames, format: {}, HLG: {}", 
                     mWidth, mHeight, mFps, mAvgFps, mMedFps, mTotalFrames, mPixelFormat, mIsHLG);
    }
    catch (const std::exception& e) {
        spdlog::error("Failed to initialize DirectLogDecoder: {}", e.what());
        throw;
    }
    
    this->init(options);
}

VirtualFileSystemImpl_DirectLog::~VirtualFileSystemImpl_DirectLog() {
    spdlog::info("Destroying VirtualFileSystemImpl_DirectLog({})", mSrcPath);
}

void VirtualFileSystemImpl_DirectLog::init(FileRenderOptions options) {
    spdlog::debug("VirtualFileSystemImpl_DirectLog::init(options={})", optionsToString(options));
    
    mFiles.clear();

#ifdef _WIN32
    Entry desktopIni;
    desktopIni.type = EntryType::FILE_ENTRY;
    desktopIni.pathParts = {};
    desktopIni.name = "desktop.ini";
    desktopIni.size = DESKTOP_INI.size();
    mFiles.push_back(desktopIni);
#endif

    const auto& frames = mDecoder->getFrames();
    if (frames.empty()) {
        return;
    }
    
    // Determine target FPS based on CFR conversion options (similar to MCRAW)
    bool applyCFRConversion = options & RENDER_OPT_FRAMERATE_CONVERSION;
    
    if (applyCFRConversion && !mCFRTarget.empty()) {
        if (mCFRTarget == "Prefer Integer") {
            if (mMedFps <= 23.0 || mMedFps >= 1000.0) 
                mFps = mMedFps;
            else if (mMedFps < 24.5)
                mFps = 24.0f;
            else if (mMedFps < 26.0)
                mFps = 25.0f;
            else if (mMedFps < 33.0)
                mFps = 30.0f;
            else if (mMedFps < 49.0)
                mFps = 48.0f;
            else if (mMedFps < 52.0)
                mFps = 50.0f;
            else if (mMedFps > 56.0 && mMedFps < 63.0)
                mFps = 60.0f;
            else if (mMedFps > 112.0 && mMedFps < 125.0)
                mFps = 120.0f;
            else if (mMedFps > 224.0 && mMedFps < 250.0)
                mFps = 240.0f;
            else if (mMedFps > 448.0 && mMedFps < 500.0)
                mFps = 480.0f;
            else if (mMedFps > 896.0 && mMedFps < 1000.0)
                mFps = 960.0f;
            else if (mMedFps >= 63.0)
                mFps = 120.0f;
            else   
                mFps = 60.0f;
        }
        else if (mCFRTarget == "Prefer Drop Frame") {
            if (mMedFps <= 23.0 || mMedFps >= 1000.0) 
                mFps = mMedFps;
            else if (mMedFps < 24.5)
                mFps = 23.976f;
            else if (mMedFps < 26.0)
                mFps = 25.0f;
            else if (mMedFps < 33.0)
                mFps = 29.97f;
            else if (mMedFps < 49.0)
                mFps = 47.952f;
            else if (mMedFps < 52.0)
                mFps = 50.0f;
            else if (mMedFps > 56.0 && mMedFps < 63.0)
                mFps = 59.94f;
            else if (mMedFps > 112.0 && mMedFps < 125.0)
                mFps = 119.88f;
            else if (mMedFps > 224.0 && mMedFps < 250.0)
                mFps = 240.0f;
            else if (mMedFps > 448.0 && mMedFps < 500.0)
                mFps = 480.0f;
            else if (mMedFps > 896.0 && mMedFps < 1000.0)
                mFps = 960.0f;
            else if (mMedFps >= 63.0)
                mFps = 119.88f;
            else    
                mFps = 59.94f;
        }
        else if (mCFRTarget == "Median (Slowmotion)") {
            mFps = mMedFps;
        }
        else if (mCFRTarget == "Average (Testing)") {
            mFps = mAvgFps;
        }
        else {
            // Custom framerate - try to parse as float
            try {
                mFps = std::stof(mCFRTarget);
            } catch (const std::exception& e) {
                spdlog::warn("Invalid CFR target '{}', using median frame rate", mCFRTarget);
                mFps = mMedFps;
            }
        }
    } else {
        // No CFR conversion - try to parse custom framerate or use average
        try {
            mFps = std::stof(mCFRTarget);
        } catch (const std::exception& e) {
            spdlog::warn("Invalid CFR target '{}', using average for no cfr conversion", mCFRTarget);
            mFps = mAvgFps;
        }
    }
    
    spdlog::info("DirectLog target FPS: {:.2f} (CFR conversion: {})", mFps, applyCFRConversion);
    
    // Generate one sample DNG to determine actual file size (mimics MCRAW approach)
    if (!frames.empty()) {
        std::vector<uint16_t> sampleRgbData;
        if (mDecoder->extractFrame(0, sampleRgbData)) {
            std::vector<uint8_t> sampleDngData;
            if (convertRGBToDNG(sampleRgbData, sampleDngData, 0, frames[0].timestamp)) {
                mTypicalDngSize = sampleDngData.size();
                spdlog::info("DirectLog DNG size determined from sample: {} bytes ({:.2f} MB)", 
                            mTypicalDngSize, mTypicalDngSize / (1024.0 * 1024.0));
            } else {
                // Fallback to calculated estimate if conversion fails
                const auto& videoInfo = mDecoder->getVideoInfo();
                mTypicalDngSize = static_cast<size_t>(videoInfo.width) * videoInfo.height * 3 * 2 + (1024 * 1024);
                spdlog::warn("Failed to generate sample DNG, using estimated size: {} bytes", mTypicalDngSize);
            }
        }
    }
    
    // Generate file entries with CFR conversion if enabled
    int lastPts = 0;
    mDroppedFrames = 0;
    mDuplicatedFrames = 0;
    
    if (applyCFRConversion) {
        // CFR conversion: duplicate/drop frames to match target framerate
        for (size_t i = 0; i < frames.size(); ++i) {
            int pts = getFrameNumberFromTimestamp(frames[i].timestamp, frames[0].timestamp, mFps);
            
            // Count duplicated frames before this frame
            mDuplicatedFrames += std::max(0, pts - lastPts - 1);
            
            if (lastPts > 0 && lastPts == pts) {
                mDroppedFrames += 1;
            }
            
            // Duplicate frames to account for dropped frames
            while (lastPts < pts) {
                Entry dngEntry;
                dngEntry.type = EntryType::FILE_ENTRY;
                dngEntry.pathParts = {};
                dngEntry.name = constructFrameFilename(mBaseName + "-", lastPts, 6, "dng");
                dngEntry.size = mTypicalDngSize;
                dngEntry.userData = frames[i].timestamp;
                mFiles.push_back(dngEntry);
                ++lastPts;
            }
        }
    } else {
        // No CFR conversion: use frames as-is
        for (size_t i = 0; i < frames.size(); ++i) {
            Entry dngEntry;
            dngEntry.type = EntryType::FILE_ENTRY;
            dngEntry.pathParts = {};
            dngEntry.name = constructFrameFilename(mBaseName + "-", lastPts, 6, "dng");
            dngEntry.size = mTypicalDngSize;
            dngEntry.userData = frames[i].timestamp;
            mFiles.push_back(dngEntry);
            ++lastPts;
        }
    }
    
    spdlog::info("DirectLog generated {} DNG entries (dropped: {}, duplicated: {})", 
                 mFiles.size(), mDroppedFrames, mDuplicatedFrames);
}

std::vector<Entry> VirtualFileSystemImpl_DirectLog::listFiles(const std::string& filter) const {
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

std::optional<Entry> VirtualFileSystemImpl_DirectLog::findEntry(const std::string& fullPath) const {
    std::lock_guard<std::mutex> lock(mMutex);
    
    for (const auto& entry : mFiles) {
        if (entry.getFullPath().string() == fullPath) {
            return entry;
        }
    }
    
    return std::nullopt;
}

int VirtualFileSystemImpl_DirectLog::readFile(
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

size_t VirtualFileSystemImpl_DirectLog::generateFrame(
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
            
            // Extract RGB data from video frame (16-bit per channel)
            std::vector<uint16_t> rgbData;
            if (!mDecoder->extractFrame(frameNumber, rgbData)) {
                spdlog::error("Failed to extract frame {} (timestamp: {})", frameNumber, timestamp);
                result(0, -1);
                return;
            }
            
            // Convert RGB to DNG
            std::vector<uint8_t> dngData;
            if (!convertRGBToDNG(rgbData, dngData, frameNumber, timestamp)) {
                spdlog::error("Failed to convert RGB to DNG for frame {}", frameNumber);
                result(0, -1);
                return;
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

namespace {
    // Pack RGB data to 12-bit (2 pixels = 6 samples * 12 bits = 72 bits = 9 bytes)
    void encodeRGBTo12Bit(std::vector<uint8_t>& data, uint32_t width, uint32_t height) {
        uint16_t* srcPtr = reinterpret_cast<uint16_t*>(data.data());
        uint8_t* dstPtr = data.data();
        
        for (uint32_t y = 0; y < height; y++) {
            for (uint32_t x = 0; x < width; x += 2) {
                // Read 6 samples (2 RGB pixels)
                uint16_t r0 = srcPtr[0];
                uint16_t g0 = srcPtr[1];
                uint16_t b0 = srcPtr[2];
                uint16_t r1 = srcPtr[3];
                uint16_t g1 = srcPtr[4];
                uint16_t b1 = srcPtr[5];
                
                // Pack into 9 bytes
                dstPtr[0] = r0 >> 4;
                dstPtr[1] = ((r0 & 0x0F) << 4) | (g0 >> 8);
                dstPtr[2] = g0 & 0xFF;
                dstPtr[3] = b0 >> 4;
                dstPtr[4] = ((b0 & 0x0F) << 4) | (r1 >> 8);
                dstPtr[5] = r1 & 0xFF;
                dstPtr[6] = g1 >> 4;
                dstPtr[7] = ((g1 & 0x0F) << 4) | (b1 >> 8);
                dstPtr[8] = b1 & 0xFF;
                
                srcPtr += 6;
                dstPtr += 9;
            }
        }
        
        auto newSize = dstPtr - data.data();
        data.resize(newSize);
    }

    // Pack RGB data to 10-bit (4 pixels = 12 samples * 10 bits = 120 bits = 15 bytes)
    void encodeRGBTo10Bit(std::vector<uint8_t>& data, uint32_t width, uint32_t height) {
        uint16_t* srcPtr = reinterpret_cast<uint16_t*>(data.data());
        uint8_t* dstPtr = data.data();
        
        for (uint32_t y = 0; y < height; y++) {
            for (uint32_t x = 0; x < width; x += 4) {
                // Read 12 samples (4 RGB pixels)
                uint16_t s[12];
                for (int i = 0; i < 12; i++) {
                    s[i] = srcPtr[i];
                }
                
                // Pack 12 samples * 10 bits = 120 bits = 15 bytes
                dstPtr[0] = s[0] >> 2;
                dstPtr[1] = ((s[0] & 0x03) << 6) | (s[1] >> 4);
                dstPtr[2] = ((s[1] & 0x0F) << 4) | (s[2] >> 6);
                dstPtr[3] = ((s[2] & 0x3F) << 2) | (s[3] >> 8);
                dstPtr[4] = s[3] & 0xFF;
                
                dstPtr[5] = s[4] >> 2;
                dstPtr[6] = ((s[4] & 0x03) << 6) | (s[5] >> 4);
                dstPtr[7] = ((s[5] & 0x0F) << 4) | (s[6] >> 6);
                dstPtr[8] = ((s[6] & 0x3F) << 2) | (s[7] >> 8);
                dstPtr[9] = s[7] & 0xFF;
                
                dstPtr[10] = s[8] >> 2;
                dstPtr[11] = ((s[8] & 0x03) << 6) | (s[9] >> 4);
                dstPtr[12] = ((s[9] & 0x0F) << 4) | (s[10] >> 6);
                dstPtr[13] = ((s[10] & 0x3F) << 2) | (s[11] >> 8);
                dstPtr[14] = s[11] & 0xFF;
                
                srcPtr += 12;
                dstPtr += 15;
            }
        }
        
        auto newSize = dstPtr - data.data();
        data.resize(newSize);
    }

    // Pack RGB data to 8-bit (1 pixel = 3 samples * 8 bits = 24 bits = 3 bytes)
    void encodeRGBTo8Bit(std::vector<uint8_t>& data, uint32_t width, uint32_t height) {
        uint16_t* srcPtr = reinterpret_cast<uint16_t*>(data.data());
        uint8_t* dstPtr = data.data();
        
        for (uint32_t y = 0; y < height; y++) {
            for (uint32_t x = 0; x < width; x++) {
                // Read 3 samples (1 RGB pixel)
                dstPtr[0] = srcPtr[0] & 0xFF;
                dstPtr[1] = srcPtr[1] & 0xFF;
                dstPtr[2] = srcPtr[2] & 0xFF;
                
                srcPtr += 3;
                dstPtr += 3;
            }
        }
        
        auto newSize = dstPtr - data.data();
        data.resize(newSize);
    }

    // Pack RGB data to 6-bit (4 pixels = 12 samples * 6 bits = 72 bits = 9 bytes)
    void encodeRGBTo6Bit(std::vector<uint8_t>& data, uint32_t width, uint32_t height) {
        uint16_t* srcPtr = reinterpret_cast<uint16_t*>(data.data());
        uint8_t* dstPtr = data.data();
        
        for (uint32_t y = 0; y < height; y++) {
            for (uint32_t x = 0; x < width; x += 4) {
                // Read 12 samples (4 RGB pixels)
                uint8_t v[12];
                for (int i = 0; i < 12; i++) {
                    v[i] = srcPtr[i] & 0x3F;
                }
                
                // Pack 12 samples * 6 bits = 72 bits = 9 bytes
                dstPtr[0] = (v[0] << 2) | (v[1] >> 4);
                dstPtr[1] = ((v[1] & 0x0F) << 4) | (v[2] >> 2);
                dstPtr[2] = ((v[2] & 0x03) << 6) | v[3];
                
                dstPtr[3] = (v[4] << 2) | (v[5] >> 4);
                dstPtr[4] = ((v[5] & 0x0F) << 4) | (v[6] >> 2);
                dstPtr[5] = ((v[6] & 0x03) << 6) | v[7];
                
                dstPtr[6] = (v[8] << 2) | (v[9] >> 4);
                dstPtr[7] = ((v[9] & 0x0F) << 4) | (v[10] >> 2);
                dstPtr[8] = ((v[10] & 0x03) << 6) | v[11];
                
                srcPtr += 12;
                dstPtr += 9;
            }
        }
        
        auto newSize = dstPtr - data.data();
        data.resize(newSize);
    }

    // Pack RGB data to 4-bit (2 pixels = 6 samples * 4 bits = 24 bits = 3 bytes)
    void encodeRGBTo4Bit(std::vector<uint8_t>& data, uint32_t width, uint32_t height) {
        uint16_t* srcPtr = reinterpret_cast<uint16_t*>(data.data());
        uint8_t* dstPtr = data.data();
        
        for (uint32_t y = 0; y < height; y++) {
            for (uint32_t x = 0; x < width; x += 2) {
                // Read 6 samples (2 RGB pixels)
                uint8_t v[6];
                for (int i = 0; i < 6; i++) {
                    v[i] = srcPtr[i] & 0x0F;
                }
                
                // Pack 6 samples * 4 bits = 24 bits = 3 bytes
                dstPtr[0] = (v[0] << 4) | v[1];
                dstPtr[1] = (v[2] << 4) | v[3];
                dstPtr[2] = (v[4] << 4) | v[5];
                
                srcPtr += 6;
                dstPtr += 3;
            }
        }
        
        auto newSize = dstPtr - data.data();
        data.resize(newSize);
    }

    // Single-channel Bayer encoding functions
    void encodeTo12Bit(std::vector<uint8_t>& data, uint32_t width, uint32_t height) {
        uint16_t* srcPtr = reinterpret_cast<uint16_t*>(data.data());
        uint8_t* dstPtr = data.data();
        
        for (uint32_t y = 0; y < height; y++) {
            for (uint32_t x = 0; x < width; x += 2) {
                uint16_t v0 = srcPtr[0];
                uint16_t v1 = srcPtr[1];
                
                dstPtr[0] = v0 >> 4;
                dstPtr[1] = ((v0 & 0x0F) << 4) | (v1 >> 8);
                dstPtr[2] = v1 & 0xFF;
                
                srcPtr += 2;
                dstPtr += 3;
            }
        }
        
        auto newSize = dstPtr - data.data();
        data.resize(newSize);
    }

    void encodeTo10Bit(std::vector<uint8_t>& data, uint32_t width, uint32_t height) {
        uint16_t* srcPtr = reinterpret_cast<uint16_t*>(data.data());
        uint8_t* dstPtr = data.data();
        
        for (uint32_t y = 0; y < height; y++) {
            for (uint32_t x = 0; x < width; x += 4) {
                uint16_t s[4];
                for (int i = 0; i < 4; i++) {
                    s[i] = srcPtr[i];
                }
                
                dstPtr[0] = s[0] >> 2;
                dstPtr[1] = ((s[0] & 0x03) << 6) | (s[1] >> 4);
                dstPtr[2] = ((s[1] & 0x0F) << 4) | (s[2] >> 6);
                dstPtr[3] = ((s[2] & 0x3F) << 2) | (s[3] >> 8);
                dstPtr[4] = s[3] & 0xFF;
                
                srcPtr += 4;
                dstPtr += 5;
            }
        }
        
        auto newSize = dstPtr - data.data();
        data.resize(newSize);
    }

    void encodeTo8Bit(std::vector<uint8_t>& data, uint32_t width, uint32_t height) {
        uint16_t* srcPtr = reinterpret_cast<uint16_t*>(data.data());
        uint8_t* dstPtr = data.data();
        
        for (uint32_t y = 0; y < height; y++) {
            for (uint32_t x = 0; x < width; x++) {
                dstPtr[0] = srcPtr[0] & 0xFF;
                srcPtr += 1;
                dstPtr += 1;
            }
        }
        
        auto newSize = dstPtr - data.data();
        data.resize(newSize);
    }

    void encodeTo6Bit(std::vector<uint8_t>& data, uint32_t width, uint32_t height) {
        uint16_t* srcPtr = reinterpret_cast<uint16_t*>(data.data());
        uint8_t* dstPtr = data.data();
        
        for (uint32_t y = 0; y < height; y++) {
            for (uint32_t x = 0; x < width; x += 4) {
                uint8_t v[4];
                for (int i = 0; i < 4; i++) {
                    v[i] = srcPtr[i] & 0x3F;
                }
                
                dstPtr[0] = (v[0] << 2) | (v[1] >> 4);
                dstPtr[1] = ((v[1] & 0x0F) << 4) | (v[2] >> 2);
                dstPtr[2] = ((v[2] & 0x03) << 6) | v[3];
                
                srcPtr += 4;
                dstPtr += 3;
            }
        }
        
        auto newSize = dstPtr - data.data();
        data.resize(newSize);
    }

    void encodeTo4Bit(std::vector<uint8_t>& data, uint32_t width, uint32_t height) {
        uint16_t* srcPtr = reinterpret_cast<uint16_t*>(data.data());
        uint8_t* dstPtr = data.data();
        
        for (uint32_t y = 0; y < height; y++) {
            for (uint32_t x = 0; x < width; x += 2) {
                uint8_t v[2];
                for (int i = 0; i < 2; i++) {
                    v[i] = srcPtr[i] & 0x0F;
                }
                
                dstPtr[0] = (v[0] << 4) | v[1];
                
                srcPtr += 2;
                dstPtr += 1;
            }
        }
        
        auto newSize = dstPtr - data.data();
        data.resize(newSize);
    }
}

bool VirtualFileSystemImpl_DirectLog::convertRGBToDNG(
    const std::vector<uint16_t>& rgbData, 
    std::vector<uint8_t>& dngData, 
    int frameNumber, 
    Timestamp timestamp) {
    
    try {
        const auto& videoInfo = mDecoder->getVideoInfo();
        const int width = videoInfo.width;
        const int height = videoInfo.height;
        
        // Determine if we should apply log curve and bit reduction
        bool applyLogCurve = !mLogTransform.empty();
        int bitReduction = 0;
        
        if (applyLogCurve) {
            // Parse bit reduction from logTransform option
            if (mLogTransform == "Reduce by 2bit") {
                bitReduction = 2;
            } else if (mLogTransform == "Reduce by 4bit") {
                bitReduction = 4;
            } else if (mLogTransform == "Reduce by 6bit") {
                bitReduction = 6;
            } else if (mLogTransform == "Reduce by 8bit") {
                bitReduction = 8;
            } else if (mLogTransform == "Keep Input") {
                bitReduction = 0;
            }
        }
        
        // Process RGB data: apply log curve to reduce to 12-bit, then apply additional bit reduction
        std::vector<uint16_t> processedRgbData;
        float dstWhiteLevel = 65535.0f;
        int encodeBits = 16;
        
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
            processedRgbData.resize(rgbData.size());
            
            // Apply log curve to each pixel
            for (size_t i = 0; i < rgbData.size(); i += 3) {
                for (int c = 0; c < 3; ++c) {
                    // Normalize input to [0, 1]
                    float normalized = rgbData[i + c] / 65535.0f;
                    
                    // Apply log2 transform: log2(1 + k*x) / log2(1 + k)
                    // Using k=60 to match MCRAW implementation
                    float logValue = std::log2(1.0f + 60.0f * normalized) / std::log2(61.0f);
                    
                    // Scale to target bit depth
                    float scaled = logValue * dstWhiteLevel;
                    
                    // Clamp and round
                    processedRgbData[i + c] = static_cast<uint16_t>(
                        std::clamp(std::round(scaled), 0.0f, dstWhiteLevel)
                    );
                }
            }
        } else {
            // No log curve - use original data
            processedRgbData = rgbData;
        }
        
        // Check if remosaicing is requested (from render options)
        bool shouldRemosaic = (mOptions & RENDER_OPT_REMOSAIC_TO_BAYER) != 0;
        
        // Get CFA phase from calibration JSON if available, otherwise use UI setting
        std::string cfaPhase = "bggr";
        if (mCalibration.has_value() && !mCalibration->cfaPhase.empty()) {
            // JSON override takes priority
            cfaPhase = mCalibration->cfaPhase;
        } else if (!mCfaPhase.empty() && mCfaPhase != "Don't override CFA") {
            // Use UI setting if not "Don't override CFA"
            cfaPhase = mCfaPhase;
        }
        // else: keep default "bggr"
        
        // Convert to lowercase for consistency
        std::transform(cfaPhase.begin(), cfaPhase.end(), cfaPhase.begin(), ::tolower);
        
        std::vector<uint8_t> imageBytes;
        int samplesPerPixel = 3;
        int photometric = 2; // RGB
        
        if (shouldRemosaic) {
            // Convert RGB to Bayer CFA pattern
            std::vector<uint16_t> bayerData;
            utils::remosaicRGBToBayer(processedRgbData, bayerData, width, height, cfaPhase);
            
            // Pack Bayer data to actual bit depth
            imageBytes.resize(width * height * 2);
            std::memcpy(imageBytes.data(), bayerData.data(), bayerData.size() * sizeof(uint16_t));
            
            samplesPerPixel = 1; // Single channel for CFA
            photometric = 32803; // CFA (Color Filter Array)
        } else {
            // Pack RGB data to actual bit depth
            imageBytes.resize(width * height * 3 * 2);
            std::memcpy(imageBytes.data(), processedRgbData.data(), processedRgbData.size() * sizeof(uint16_t));
        }
        
        if (applyLogCurve) {
            if (encodeBits <= 4) {
                if (shouldRemosaic) {
                    encodeTo4Bit(imageBytes, width, height);
                } else {
                    encodeRGBTo4Bit(imageBytes, width, height);
                }
                encodeBits = 4;
            } else if (encodeBits <= 6) {
                if (shouldRemosaic) {
                    encodeTo6Bit(imageBytes, width, height);
                } else {
                    encodeRGBTo6Bit(imageBytes, width, height);
                }
                encodeBits = 6;
            } else if (encodeBits <= 8) {
                if (shouldRemosaic) {
                    encodeTo8Bit(imageBytes, width, height);
                } else {
                    encodeRGBTo8Bit(imageBytes, width, height);
                }
                encodeBits = 8;
            } else if (encodeBits <= 10) {
                if (shouldRemosaic) {
                    encodeTo10Bit(imageBytes, width, height);
                } else {
                    encodeRGBTo10Bit(imageBytes, width, height);
                }
                encodeBits = 10;
            } else if (encodeBits <= 12) {
                if (shouldRemosaic) {
                    encodeTo12Bit(imageBytes, width, height);
                } else {
                    encodeRGBTo12Bit(imageBytes, width, height);
                }
                encodeBits = 12;
            }
            // else keep 16-bit
        }
        
        // Create DNG image
        tinydngwriter::DNGImage dng;
        
        // Set basic image properties
        dng.SetBigEndian(false);
        dng.SetImageWidth(width);
        dng.SetImageLength(height);
        dng.SetSamplesPerPixel(samplesPerPixel);
        
        unsigned short bitsPerSample[3] = {
            static_cast<unsigned short>(encodeBits),
            static_cast<unsigned short>(encodeBits),
            static_cast<unsigned short>(encodeBits)
        };
        dng.SetBitsPerSample(samplesPerPixel, bitsPerSample);
        
        // Photometric interpretation
        dng.SetPhotometric(photometric);
        dng.SetPlanarConfig(1); // Chunky
        dng.SetCompression(1);  // No compression
        
        unsigned short sampleFormat[3] = {1, 1, 1}; // Unsigned integer
        dng.SetSampleFormat(samplesPerPixel, sampleFormat);
        
        // Set CFA pattern if remosaicing
        if (shouldRemosaic) {
            unsigned char cfaPattern[4];
            if (cfaPhase == "bggr") {
                cfaPattern[0] = 2; cfaPattern[1] = 1; cfaPattern[2] = 1; cfaPattern[3] = 0; // B G G R
            } else if (cfaPhase == "rggb") {
                cfaPattern[0] = 0; cfaPattern[1] = 1; cfaPattern[2] = 1; cfaPattern[3] = 2; // R G G B
            } else if (cfaPhase == "grbg") {
                cfaPattern[0] = 1; cfaPattern[1] = 0; cfaPattern[2] = 2; cfaPattern[3] = 1; // G R B G
            } else { // gbrg
                cfaPattern[0] = 1; cfaPattern[1] = 2; cfaPattern[2] = 0; cfaPattern[3] = 1; // G B R G
            }
            dng.SetCFAPattern(4, cfaPattern);
            dng.SetCFALayout(1); // Rectangular (or square) layout
        }
        
        // Set DNG version
        dng.SetDNGVersion(1, 4, 0, 0);
        dng.SetDNGBackwardVersion(1, 4, 0, 0);
        
        // Set camera/software metadata
        dng.SetMake("DirectLog");
        dng.SetCameraModelName(mCameraModel.empty() ? "DirectLog Video" : mCameraModel);
        dng.SetUniqueCameraModel(mCameraModel.empty() ? "DirectLog Video" : mCameraModel);
        dng.SetSoftware("MotionCam DirectLog Decoder");
        
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
        
        // Set baseline exposure with keyframe support
        float exposureOffset = (mCameraModel == "Panasonic" ? -2.0f : 0.0f);
        if (mExposureKeyframes.has_value()) {
            exposureOffset += mExposureKeyframes->getExposureAtFrame(frameNumber, mTotalFrames);
        } else if (!mExposureCompensation.empty()) {
            try {
                exposureOffset += std::stof(mExposureCompensation);
            } catch (const std::exception&) {
                // If parsing fails, keep the original exposureOffset value
            }
        }
        dng.SetBaselineExposure(exposureOffset);
        
        // Set white/black levels and linearization table
        if (applyLogCurve) {
            // Create linearization table to reverse the log curve
            const int tableSize = static_cast<int>(dstWhiteLevel) + 1;
            std::vector<unsigned short> linearizationTable(tableSize);
            
            for (int i = 0; i < tableSize; i++) {
                float normalizedLogValue = static_cast<float>(i) / dstWhiteLevel;
                
                float linearValue;
                if (i == 0) {
                    linearValue = 0.0f;  // Exact identity: stored 0 → linear 0
                } else if (i == tableSize - 1) {
                    linearValue = 1.0f;  // Force maximum table entry → linear 1
                } else {
                    // Inverse of: logValue = log2(1 + k*x) / log2(1 + k)
                    // x = (2^(logValue * log2(1 + k)) - 1) / k
                    linearValue = (std::pow(2.0f, normalizedLogValue * std::log2(61.0f)) - 1.0f) / 60.0f;
                    linearValue = std::clamp(linearValue, 0.0f, 1.0f);
                }
                
                // Scale to 16-bit range
                linearizationTable[i] = static_cast<unsigned short>(linearValue * 65535.0f);
            }
            
            dng.SetLinearizationTable(tableSize, linearizationTable.data());
            
            // Set black level to 0 and white level to 65534 (as per MCRAW implementation)
            unsigned short blackLevel[3] = {0, 0, 0};
            dng.SetBlackLevel(3, blackLevel);
            dng.SetWhiteLevel(65534);
        } else {
            // No log curve - use standard levels
            dng.SetWhiteLevel(65535);
            unsigned short blackLevel[3] = {0, 0, 0};
            dng.SetBlackLevel(3, blackLevel);
        }
        
        dng.SetImageData(imageBytes.data(), imageBytes.size());
        
        // Apply calibration if available
        if (mCalibration.has_value()) {
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
            if (mCalibration->hasAsShotNeutral) {
                dng.SetAsShotNeutral(3, mCalibration->asShotNeutral.data());
            }
        }
        
        // Write DNG to memory stream
        std::ostringstream oss;
        tinydngwriter::DNGWriter writer(false); // little-endian
        writer.AddImage(&dng);
        
        std::string err;
        if (!writer.WriteToFile(oss, &err)) {
            spdlog::error("Failed to write DNG for frame {}: {}", frameNumber, err);
            return false;
        }
        
        // Copy to output vector
        std::string dngStr = oss.str();
        dngData.assign(dngStr.begin(), dngStr.end());
        
        return true;
    }
    catch (const std::exception& e) {
        spdlog::error("Exception in convertRGBToDNG for frame {}: {}", frameNumber, e.what());
        return false;
    }
}

void VirtualFileSystemImpl_DirectLog::updateOptions(
    FileRenderOptions options, 
    int draftScale, 
    const std::string& cfrTarget, 
    const std::string& cropTarget, 
    const std::string& cameraModel, 
    const std::string& levels, 
    const std::string& logTransform, 
    const std::string& exposureCompensation, 
    const std::string& quadBayerOption,
    const std::string& cfaPhase) {
    
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
    mCfaPhase = cfaPhase;
    
    // Re-parse exposure keyframes
    mExposureKeyframes = ExposureKeyframes::parse(exposureCompensation);
    
    // Reload calibration JSON if it exists
    boost::filesystem::path srcPath(mSrcPath);
    boost::filesystem::path calibPath = srcPath.parent_path() / (srcPath.stem().string() + ".json");
    if (boost::filesystem::exists(calibPath)) {
        mCalibration = CalibrationData::loadFromFile(calibPath.string());
        if (mCalibration.has_value()) {
            spdlog::info("Reloaded calibration for DirectLog: {}", calibPath.string());
        }
    }
    
    init(options);
}

FileInfo VirtualFileSystemImpl_DirectLog::getFileInfo() const {
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

bool VirtualFileSystemImpl_DirectLog::isHLGVideo() const {
    return mIsHLG;
}

void VirtualFileSystemImpl_DirectLog::calculateFrameRateStats() {
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
    
    spdlog::debug("DirectLog frame rate stats: avg={:.2f}fps, median={:.2f}fps", mAvgFps, mMedFps);
}

} // namespace motioncam
