#include "VirtualFileSystemImpl_DirectLog.h"
#include "VirtualFileSystemImpl.h"
#include "DirectLogDecoder.h"
#include "DNGDecoder.h"
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

namespace {

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

} // namespace

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
                if (!DNGDecoder::setTimingMetadata(sampleDngData, mFps, 0))
                    throw std::runtime_error("Could not size DirectLog DNG timing metadata");
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
        Timestamp previousTimestamp = frames.front().timestamp;
        for (size_t i = 0; i < frames.size(); ++i) {
            int pts = vfs::getFrameNumberFromTimestamp(frames[i].timestamp, frames[0].timestamp, mFps);
            
            if (pts < lastPts) {
                mDroppedFrames += 1;
                continue;
            }

            // Hold the preceding frame through gaps, then switch to the
            // current frame at its mapped output position.
            while (lastPts < pts) {
                Entry dngEntry;
                dngEntry.type = EntryType::FILE_ENTRY;
                dngEntry.pathParts = {};
                dngEntry.name = vfs::constructFrameFilename(mBaseName + "-", lastPts, 6, "dng");
                dngEntry.size = mTypicalDngSize;
                dngEntry.userData = previousTimestamp;
                mFiles.push_back(dngEntry);
                ++lastPts;
                ++mDuplicatedFrames;
            }
            Entry dngEntry;
            dngEntry.type = EntryType::FILE_ENTRY;
            dngEntry.pathParts = {};
            dngEntry.name = vfs::constructFrameFilename(mBaseName + "-", lastPts, 6, "dng");
            dngEntry.size = mTypicalDngSize;
            dngEntry.userData = frames[i].timestamp;
            mFiles.push_back(dngEntry);
            ++lastPts;
            previousTimestamp = frames[i].timestamp;
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
        if (entry.getFullPath() == boost::filesystem::path(fullPath).relative_path()) {
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

    auto renderTask = [this, entry, pos, len, dst, result]() -> size_t {
        try {
            auto data = materializeFile(entry, false);
            const size_t count = data && pos < data->size()
                ? std::min(len, data->size() - pos) : 0;
            if (count)
                std::memcpy(dst, data->data() + pos, count);
            result(count, 0);
            return count;
        } catch (const std::exception& e) {
            spdlog::error("Error generating DirectLog frame: {}", e.what());
            result(0, -1);
            return 0;
        }
    };
    auto renderFuture = mProcessingThreadPool.submit_task(renderTask);
    return async ? 0 : renderFuture.get();
}

bool VirtualFileSystemImpl_DirectLog::convertRGBToDNG(
    const std::vector<uint16_t>& rgbData, 
    std::vector<uint8_t>& dngData, 
    int frameNumber, 
    Timestamp timestamp,
    bool jpegCompression) {
    
    try {
        const auto& videoInfo = mDecoder->getVideoInfo();
        const int width = videoInfo.width;
        const int height = videoInfo.height;
        
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
        
        std::vector<uint16_t> imageSamples;
        int samplesPerPixel = 3;
        int photometric = tinydngwriter::PHOTOMETRIC_LINEARRAW;
        
        if (shouldRemosaic) {
            // Convert RGB to Bayer CFA pattern
            std::vector<uint16_t> bayerData;
            utils::remosaicRGBToBayer(processedRgbData, bayerData, width, height, cfaPhase);
            
            // Pack Bayer data to actual bit depth
            imageSamples = std::move(bayerData);
            
            samplesPerPixel = 1; // Single channel for CFA
            photometric = 32803; // CFA (Color Filter Array)
        } else {
            // Pack RGB data to actual bit depth
            imageSamples = std::move(processedRgbData);
        }

        const bool jpegXlCompression = jpegCompression && mConfig.jxlDistance >= 0.0f;
        std::vector<uint8_t> imageBytes;
        if (jpegCompression || encodeBits == 16) {
            imageBytes.resize(imageSamples.size() * sizeof(uint16_t));
            std::memcpy(imageBytes.data(), imageSamples.data(), imageBytes.size());
        } else {
            imageBytes = packSamples(
                imageSamples,
                static_cast<size_t>(width) * samplesPerPixel,
                static_cast<unsigned int>(encodeBits));
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
            static_cast<unsigned short>(jpegXlCompression ? 16 : encodeBits),
            static_cast<unsigned short>(jpegXlCompression ? 16 : encodeBits),
            static_cast<unsigned short>(jpegXlCompression ? 16 : encodeBits)
        };
        dng.SetBitsPerSample(samplesPerPixel, bitsPerSample);
        
        // Photometric interpretation
        dng.SetPhotometric(photometric);
        dng.SetPlanarConfig(1); // Chunky
        dng.SetCompression(jpegCompression
            ? (mConfig.jxlDistance < 0.0f ? tinydngwriter::COMPRESSION_JPEG
                                         : tinydngwriter::COMPRESSION_JPEG_XL)
            : tinydngwriter::COMPRESSION_NONE);
        if (jpegCompression && mConfig.jxlDistance >= 0.0f)
            dng.SetJXLDistance(mConfig.jxlDistance);
        
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
            dng.SetCFARepeatPatternDim(2, 2);
            dng.SetCFAPattern(4, cfaPattern);
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
        if (jpegXlCompression) {
            dng.SetDNGBackwardVersion(1, 7, 0, 0);
        } else if (shouldRemosaic) {
            dng.SetDNGBackwardVersion(1, 1, 0, 0);
        } else {
            dng.SetDNGBackwardVersion(1, 4, 0, 0);
        }
        
        // Set camera/software metadata
        if (mConfig.cameraModel == "Blackmagic") {
            dng.SetUniqueCameraModel("Blackmagic Pocket Cinema Camera 4K");
        } else if (mConfig.cameraModel == "Panasonic") {
            dng.SetUniqueCameraModel("Panasonic Varicam RAW");
        } else if (mConfig.cameraModel == "Fujifilm" ||
                   mConfig.cameraModel == "Fujifilm X-T5") {
            dng.SetUniqueCameraModel("Fujifilm X-T5");
            dng.SetMake("Fujifilm");
            dng.SetCameraModelName("X-T5");
        } else if (!mConfig.cameraModel.empty()) {
            dng.SetUniqueCameraModel(mConfig.cameraModel);
        } else {
            dng.SetUniqueCameraModel("DirectLog Video");
        }
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
        if (mFps > 0.0f) {
            dng.SetFrameRate(mFps);
        }

        // Set baseline exposure with the optional gain offset
        float exposureOffset = (mConfig.cameraModel == "Panasonic" ? -2.0f : 0.0f);
        if (!mConfig.exposureCompensation.empty()) {
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
        
        if (!dng.SetImageData(imageBytes.data(), imageBytes.size())) {
            throw std::runtime_error("Failed to attach DirectLog image data");
        }
        
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

std::shared_ptr<std::vector<char>> VirtualFileSystemImpl_DirectLog::materializeFile(
    const Entry& entry, bool jpegCompression) {
    if (!jpegCompression) {
        if (auto cached = mCache.get(entry)) {
            mCache.put(entry, cached);
            return cached;
        }
    }

    try {
        const auto timestamp = std::get<Timestamp>(entry.userData);
        const auto& frames = mDecoder->getFrames();
        const auto it = std::find_if(frames.begin(), frames.end(), [timestamp](const auto& frame) {
            return frame.timestamp == timestamp;
        });
        if (it == frames.end())
            throw std::runtime_error("DirectLog source frame not found");
        const int frameNumber = static_cast<int>(std::distance(frames.begin(), it));

        std::vector<uint16_t> rgbData;
        if (!mDecoder->extractFrame(frameNumber, rgbData))
            throw std::runtime_error("Could not decode DirectLog frame");
        const auto frameDigits = entry.name.substr(entry.name.size() - 10, 6);
        const int outputFrameNumber = std::stoi(frameDigits);
        std::vector<uint8_t> dngData;
        if (!convertRGBToDNG(rgbData, dngData, outputFrameNumber, timestamp, jpegCompression))
            throw std::runtime_error("Could not generate DirectLog DNG");
        const bool converted = mConfig.options & RENDER_OPT_FRAMERATE_CONVERSION;
        const Timestamp outputTimestamp = converted
            ? static_cast<Timestamp>(std::llround(outputFrameNumber * 1e9 / mFps))
            : timestamp - frames.front().timestamp;
        if (!DNGDecoder::setTimingMetadata(dngData, mFps, outputTimestamp))
            throw std::runtime_error("Could not write DirectLog DNG timing metadata");

        auto output = std::make_shared<std::vector<char>>(dngData.begin(), dngData.end());
        if (!jpegCompression)
            mCache.put(entry, output);
        return output;
    } catch (...) {
        if (!jpegCompression)
            mCache.markLoadFailed(entry);
        throw;
    }
}

void VirtualFileSystemImpl_DirectLog::updateOptions(const RenderSettings& config) {
    std::lock_guard<std::mutex> lock(mMutex);

    mCache.clear();
    mConfig = config;
    
    // Reload calibration JSON if it exists
    boost::filesystem::path srcPath(mSrcPath);
    boost::filesystem::path calibPath = srcPath.parent_path() / (srcPath.stem().string() + ".json");
    mCalibration.reset();
    if (boost::filesystem::exists(calibPath)) {
        mCalibration = CalibrationData::loadFromFile(calibPath.string());
        if (mCalibration.has_value()) {
            spdlog::info("Reloaded calibration for DirectLog: {}", calibPath.string());
        }
    }
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
    FileInfo info;
    info.frameRateInfo = mFrameRateInfo;
    info.fps = mFps;
    info.totalFrames = mTotalFrames;
    info.droppedFrames = mDroppedFrames;
    info.duplicatedFrames = mDuplicatedFrames;
    info.width = mWidth;
    info.height = mHeight;
    
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
