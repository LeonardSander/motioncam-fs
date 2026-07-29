#include "VirtualFileSystemImpl_DirectLog.h"
#include "VirtualFileSystemImpl.h"
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



VirtualFileSystemImpl_DirectLog::VirtualFileSystemImpl_DirectLog(
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
        mFrameRateInfo(),
        mTotalFrames(0),
        mDroppedFrames(0),
        mDuplicatedFrames(0),
        mWidth(0),
        mHeight(0),
        mConfig(config),
        mIsHLG(false) {
    
    // Parse exposure keyframes if the input contains keyframe syntax
    mExposureKeyframes = ExposureKeyframes::parse(config.exposureCompensation);
    
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

#ifdef _WIN32
    Entry desktopIni;
    desktopIni.type = EntryType::FILE_ENTRY;
    desktopIni.pathParts = {};
    desktopIni.name = "desktop.ini";
    desktopIni.size = vfs::DESKTOP_INI.size();
    mFiles.push_back(desktopIni);
#endif

    const auto& frames = mDecoder->getFrames();
    if (frames.empty()) {
        return;
    }
    
    // Determine target FPS based on CFR conversion options
    bool applyCFRConversion = mConfig.options & RENDER_OPT_FRAMERATE_CONVERSION;
    mFps = vfs::determineCFRTarget(mFrameRateInfo, mConfig.cfrTarget, applyCFRConversion);
    
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
            int pts = vfs::getFrameNumberFromTimestamp(frames[i].timestamp, frames[0].timestamp, mFps);
            
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
                dngEntry.name = vfs::constructFrameFilename(mBaseName + "-", lastPts, 6, "dng");
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
            dngEntry.name = vfs::constructFrameFilename(mBaseName + "-", lastPts, 6, "dng");
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
        bool applyLogCurve = (mConfig.logTransform != LogTransformMode::Disabled);
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
        bool shouldRemosaic = (mConfig.options & RENDER_OPT_REMOSAIC_TO_BAYER) != 0;
        
        // Get CFA phase from calibration JSON if available, otherwise use UI setting
        std::string cfaPhase = "bggr";
        if (mCalibration.has_value() && !mCalibration->cfaPhase.empty()) {
            // JSON override takes priority
            cfaPhase = mCalibration->cfaPhase;
        } else if (!mConfig.cfaPhase.empty() && mConfig.cfaPhase != "Don't override CFA") {
            // Use UI setting if not "Don't override CFA"
            cfaPhase = mConfig.cfaPhase;
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
            uint32_t w = width, h = height;
            if (encodeBits <= 4) {
                if (shouldRemosaic)                    
                    utils::encodeTo4Bit(imageBytes, w, h);
                else 
                    utils::encodeRGBTo4Bit(imageBytes, w, h);                
                encodeBits = 4;
            } else if (encodeBits <= 6) {
                if (shouldRemosaic)
                    utils::encodeTo6Bit(imageBytes, w, h);
                else
                    utils::encodeRGBTo6Bit(imageBytes, w, h);                
                encodeBits = 6;
            } else if (encodeBits <= 8) {
                if (shouldRemosaic) 
                    utils::encodeTo8Bit(imageBytes, w, h);
                else 
                    utils::encodeRGBTo8Bit(imageBytes, w, h);                
                encodeBits = 8;
            } else if (encodeBits <= 10) {
                if (shouldRemosaic) 
                    utils::encodeTo10Bit(imageBytes, w, h);
                else 
                    utils::encodeRGBTo10Bit(imageBytes, w, h);                
                encodeBits = 10;
            } else if (encodeBits <= 12) {
                if (shouldRemosaic) 
                    utils::encodeTo12Bit(imageBytes, w, h);
                else 
                    utils::encodeRGBTo12Bit(imageBytes, w, h);                
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
        dng.SetCameraModelName(mConfig.cameraModel.empty() ? "DirectLog Video" : mConfig.cameraModel);
        dng.SetUniqueCameraModel(mConfig.cameraModel.empty() ? "DirectLog Video" : mConfig.cameraModel);
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
        float exposureOffset = (mConfig.cameraModel == "Panasonic" ? -2.0f : 0.0f);
        if (mExposureKeyframes.has_value()) {
            exposureOffset += mExposureKeyframes->getExposureAtFrame(frameNumber, mTotalFrames);
        } else if (!mConfig.exposureCompensation.empty()) {
            try {
                exposureOffset += std::stof(mConfig.exposureCompensation);
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

void VirtualFileSystemImpl_DirectLog::updateOptions(const RenderSettings& config) {
    std::lock_guard<std::mutex> lock(mMutex);
    
    mConfig = config;
    
    // Re-parse exposure keyframes
    mExposureKeyframes = ExposureKeyframes::parse(config.exposureCompensation);
    
    // Reload calibration JSON if it exists
    boost::filesystem::path srcPath(mSrcPath);
    boost::filesystem::path calibPath = srcPath.parent_path() / (srcPath.stem().string() + ".json");
    if (boost::filesystem::exists(calibPath)) {
        mCalibration = CalibrationData::loadFromFile(calibPath.string());
        if (mCalibration.has_value()) {
            spdlog::info("Reloaded calibration for DirectLog: {}", calibPath.string());
        }
    }
    
    init();
}

FileInfo VirtualFileSystemImpl_DirectLog::getFileInfo() const {
    FileInfo info;
    info.frameRateInfo = mFrameRateInfo;
    info.fps = mFps;
    info.totalFrames = mTotalFrames;
    info.droppedFrames = mDroppedFrames;
    info.duplicatedFrames = mDuplicatedFrames;
    info.width = mWidth;
    info.height = mHeight;
    
    // Determine data type based on remosaic option
    bool shouldRemosaic = (mConfig.options & RENDER_OPT_REMOSAIC_TO_BAYER) != 0;
    info.dataType = shouldRemosaic ? "Bayer CFA" : "RGB";
    
    // Determine levels info
    bool applyLogCurve = (mConfig.logTransform != LogTransformMode::Disabled);
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
                      std::to_string(dstWhiteLevel) + "/0 RAW" + std::to_string(dstBits) +
                      (applyLogCurve ? " log" : "");
    
    // Calculate runtime from video duration
    info.runtimeSeconds = (mFps > 0) ? (static_cast<float>(mTotalFrames) / mFps) : 0.0f;
    
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
