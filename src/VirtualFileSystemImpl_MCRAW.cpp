#include "VirtualFileSystemImpl_MCRAW.h"
#include "VirtualFileSystemImpl.h"
#include "CameraFrameMetadata.h"
#include "CameraMetadata.h"
#include "CalibrationData.h"
#include "Utils.h"
#include "AudioWriter.h"
#include "LRUCache.h"

#include <motioncam/Decoder.hpp>

#include <boost/filesystem.hpp>
#include <boost/regex.hpp>
#include <boost/algorithm/string.hpp>

#include <BS_thread_pool.hpp>
#include <spdlog/spdlog.h>
#include <audiofile/AudioFile.h>

#include <algorithm>
#include <tuple>

namespace motioncam {

VirtualFileSystemImpl_MCRAW::VirtualFileSystemImpl_MCRAW(
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
    
    // Parse exposure keyframes if the input contains keyframe syntax
    mExposureKeyframes = ExposureKeyframes::parse(config.exposureCompensation);
    
    // Load calibration JSON if it exists
    boost::filesystem::path srcPath(mSrcPath);
    boost::filesystem::path calibPath = srcPath.parent_path() / (srcPath.stem().string() + ".json");
    if (boost::filesystem::exists(calibPath)) {
        mCalibration = CalibrationData::loadFromFile(calibPath.string());
        if (mCalibration.has_value()) {
            spdlog::info("Loaded calibration for MCRAW: {}", calibPath.string());
        }
    }
    
    Decoder decoder(mSrcPath);
    auto frames = decoder.getFrames();
    std::sort(frames.begin(), frames.end());
    if(frames.empty())
        return;
    mBaselineExpValue = std::numeric_limits<double>::max();
    for(const auto& frame : frames) {
        nlohmann::json metadata;
        decoder.loadFrameMetadata(frame, metadata);
        const auto& cameraFrameMetadata = CameraFrameMetadata::limitedParse(metadata);
        mBaselineExpValue = std::min(mBaselineExpValue, cameraFrameMetadata.iso * cameraFrameMetadata.exposureTime);
    }
    this->init();
}

VirtualFileSystemImpl_MCRAW::~VirtualFileSystemImpl_MCRAW() {
    spdlog::info("Destroying VirtualFileSystemImpl_MCRAW({})", mSrcPath);
}

void VirtualFileSystemImpl_MCRAW::init() {
    Decoder decoder(mSrcPath);
    auto frames = decoder.getFrames();
    std::sort(frames.begin(), frames.end());

    if(frames.empty())
        return;

    spdlog::debug("VirtualFileSystemImpl_MCRAW::init(options={})", optionsToString(mConfig.options));

    // Clear everything
    mFiles.clear();

    auto frameRateInfo = vfs::calculateFrameRate(frames);
    mMedFps = frameRateInfo.medianFrameRate;
    mAvgFps = frameRateInfo.averageFrameRate;

    bool applyCFRConversion = mConfig.options & RENDER_OPT_FRAMERATE_CONVERSION;
    mFps = vfs::determineCFRTarget(mMedFps, mConfig.cfrTarget, applyCFRConversion);       

    // Calculate typical DNG size that we can use for all files
    std::vector<uint8_t> data;
    nlohmann::json metadata;

    decoder.loadFrame(frames[0], data, metadata);

    auto cameraConfig = CameraConfiguration::parse(decoder.getContainerMetadata());
    auto cameraFrameMetadata = CameraFrameMetadata::parse(metadata);

    // Store frame information
    mWidth = cameraFrameMetadata.width;
    mHeight = cameraFrameMetadata.height;
    mTotalFrames = static_cast<int>(frames.size());
    mDroppedFrames = 0; // Will be calculated during frame processing
    mDuplicatedFrames = 0;
    mNeedRemosaic = cameraFrameMetadata.needRemosaic;
    mSrcWhiteLevel = cameraFrameMetadata.dynamicWhiteLevel;
    mSrcBlackLevel = cameraFrameMetadata.dynamicBlackLevel;	

    auto dngData = utils::generateDng(
        data,
        cameraFrameMetadata,
        cameraConfig,
        mFps,
        0,
        mConfig.options,
        vfs::getScaleFromOptions(mConfig.options, mConfig.draftScale),
        mBaselineExpValue,
        mConfig.cropTarget,
        mConfig.cameraModel,
        mConfig.levels,
        mConfig.logTransform,
        mConfig.exposureCompensation,
        mConfig.quadBayerOption,
        mCalibration
    );

    mTypicalDngSize = dngData->size();

    // Generate file entries
    int lastPts = 0;

    mFiles.reserve(frames.size()*2);

// Disable icon previews in Windows/MacOS
#ifdef _WIN32
    Entry desktopIni;

    desktopIni.type = FILE_ENTRY;
    desktopIni.size = vfs::DESKTOP_INI.size();
    desktopIni.name = "desktop.ini";

    mFiles.emplace_back(desktopIni);
#endif

    // Generate and add audio (TODO: We're loading all the audio into memory)
    Entry audioEntry;

    std::vector<AudioChunk> audioChunks;
    decoder.loadAudio(audioChunks);

    mAudioDurationSeconds = 0.0f;
    if(!audioChunks.empty()) {
        auto fpsFraction = utils::toFraction(mFps);
        AudioWriter audioWriter(mAudioFile, decoder.numAudioChannels(), decoder.audioSampleRateHz(), fpsFraction.first, fpsFraction.second);

        // Sync the audio to the video
        vfs::syncAudio(
            frames[0],
            audioChunks,
            decoder.audioSampleRateHz(),
            decoder.numAudioChannels());

        // Calculate total audio duration
        size_t totalSamples = 0;
        for(auto& x : audioChunks) {
            audioWriter.write(x.second, x.second.size() / decoder.numAudioChannels());
            totalSamples += x.second.size() / decoder.numAudioChannels();
        }
        
        if (decoder.audioSampleRateHz() > 0) {
            mAudioDurationSeconds = static_cast<float>(totalSamples) / static_cast<float>(decoder.audioSampleRateHz());
        }
    }

    if(!mAudioFile.empty()) {
        audioEntry.type = EntryType::FILE_ENTRY;
        audioEntry.size = mAudioFile.size();
        audioEntry.name = "audio.wav";

        mFiles.emplace_back(audioEntry);
    }

    // Add video frames
    for(auto& x : frames) {
        if(applyCFRConversion) {
            int pts = vfs::getFrameNumberFromTimestamp(x, frames[0], mFps);

            // Count dropped frames before this frame
            mDuplicatedFrames += (std::max)(0, pts - lastPts - 1);

            if (lastPts > 0 && lastPts == pts)
                mDroppedFrames += 1;

            // Duplicate frames to account for dropped frames
            while(lastPts < pts) {
                Entry entry;

                // Add main entry
                entry.type = EntryType::FILE_ENTRY;
                entry.size = mTypicalDngSize;
                entry.name = vfs::constructFrameFilename(mBaseName + std::string("-"), lastPts, 6, "dng");     
                entry.userData = x;

                mFiles.emplace_back(entry);
                ++lastPts;
            }
        } else {
            Entry entry;

            // Add main entry
            entry.type = EntryType::FILE_ENTRY;
            entry.size = mTypicalDngSize;
            entry.name = vfs::constructFrameFilename(mBaseName + std::string("-"), lastPts, 6, "dng");     
            entry.userData = x;

            mFiles.emplace_back(entry);
            ++lastPts;
        }
    }
}

std::vector<Entry> VirtualFileSystemImpl_MCRAW::listFiles(const std::string& filter) const {
    // TODO: Use filter
    return mFiles;
}

std::optional<Entry> VirtualFileSystemImpl_MCRAW::findEntry(const std::string& fullPath) const {
    for(const auto& e : mFiles) {
        if(boost::filesystem::path(fullPath).relative_path() == e.getFullPath())
            return e;
    }

    return {};
}

size_t VirtualFileSystemImpl_MCRAW::generateFrame(
    const Entry& entry,
    const size_t pos,
    const size_t len,
    void* dst,
    std::function<void(size_t, int)> result,
    bool async)
{
    using FrameData = std::tuple<size_t, CameraConfiguration, CameraFrameMetadata, std::shared_ptr<std::vector<uint8_t>>>;

    // Try to get from cache first
    auto cacheEntry = mCache.get(entry);
    if(cacheEntry && pos < cacheEntry->size()) {
        // Calculate length to copy
        const size_t actualLen = (std::min)(len, cacheEntry->size() - pos);

        // Copy the data from cache
        std::memcpy(dst, cacheEntry->data() + pos, actualLen);

        // Push entry to front
        mCache.put(entry, cacheEntry);

        return actualLen;
    }

    // Use IO thread pool to decode frame
    auto frameDataFuture = mIoThreadPool.submit_task([entry, &srcPath = mSrcPath, &options = mConfig.options]() -> FrameData {
        thread_local std::map<std::string, std::unique_ptr<Decoder>> decoders;

        auto timestamp = std::get<Timestamp>(entry.userData);

        spdlog::debug("Reading frame {} with options {}", timestamp, optionsToString(options));

        if(decoders.find(srcPath) == decoders.end()) {
            decoders[srcPath] = std::make_unique<Decoder>(srcPath);
        }

        auto& decoder = decoders[srcPath];
        auto data = std::make_shared<std::vector<uint8_t>>();

        nlohmann::json metadata;
        auto allFrames = decoder->getFrames();

        // Find the frame (index)
        auto it = std::find(allFrames.begin(), allFrames.end(), timestamp);
        if(it == allFrames.end()) {
            spdlog::error("Frame {} not found", timestamp);
            throw std::runtime_error("Failed to find frame");
        }

        decoder->loadFrame(timestamp, *data, metadata);

        size_t frameIndex = std::distance(allFrames.begin(), it);

        return std::make_tuple(
            frameIndex, CameraConfiguration::parse(decoder->getContainerMetadata()), CameraFrameMetadata::parse(metadata), std::move(data));
    });


    // Use processing thread pool to generate DNG
    auto sharableFuture = frameDataFuture.share();

    const auto fps = mFps;
    const auto draftScale = mConfig.draftScale;
    const auto baselineExpValue = mBaselineExpValue;
    const auto options = mConfig.options;

    auto generateTask = [this, &cache = mCache, entry, sharableFuture, fps, draftScale, baselineExpValue, options, pos, len, dst, result]() {
        size_t readBytes = 0;
        int errorCode = -1;

        try {
            auto decodedFrame = sharableFuture.get();
            auto [frameIndex, containerMetadata, frameMetadata, frameData] = std::move(decodedFrame);

            spdlog::debug("Generating {}", entry.name);

            // Calculate frame-specific exposure compensation
            std::string frameExposureComp = mConfig.exposureCompensation;
            if (mExposureKeyframes.has_value()) {
                float exposureValue = mExposureKeyframes->getExposureAtFrame(frameIndex, mTotalFrames);
                frameExposureComp = std::to_string(exposureValue);
            }

            auto dngData = utils::generateDng(
                *frameData,
                frameMetadata,
                containerMetadata,
                fps,
                frameIndex,
                options,
                vfs::getScaleFromOptions(options, draftScale),
                baselineExpValue,
                mConfig.cropTarget,
                mConfig.cameraModel,
                mConfig.levels,
                mConfig.logTransform,
                frameExposureComp,
                mConfig.quadBayerOption,
                mCalibration,
                mConfig.cfaPhase);

            if(dngData && pos < dngData->size()) {
                // Calculate length to copy
                const size_t actualLen = std::min(len, dngData->size() - pos);

                std::memcpy(dst, dngData->data() + pos, actualLen);

                readBytes = actualLen;
                errorCode = 0;
            }

            // Add to cache
            cache.put(entry, dngData);
        }
        catch(std::runtime_error& e) {
            spdlog::error("Failed to generate DNG (error: {})", e.what());
            cache.markLoadFailed(entry);
        }

        result(readBytes, errorCode);

        return readBytes;
    };


    auto processFuture = mProcessingThreadPool.submit_task(generateTask);
    if(!async)
        return processFuture.get();

    return 0;
}

size_t VirtualFileSystemImpl_MCRAW::generateAudio(
    const Entry& entry,
    const size_t pos,
    const size_t len,
    void* dst,
    std::function<void(size_t, int)> result,
    bool async)
{
    size_t readBytes = 0;

    if(pos < mAudioFile.size()) {
        // Calculate length to copy
        const size_t actualLen = (std::min)(len, mAudioFile.size() - pos);

        std::memcpy(dst, mAudioFile.data() + pos, actualLen);

        readBytes = actualLen;
    }

    // Always read synchronously for now
    return readBytes;
}

int VirtualFileSystemImpl_MCRAW::readFile(
    const Entry& entry,
    const size_t pos,
    const size_t len,
    void* dst,
    std::function<void(size_t, int)> result,
    bool async) {

    #ifdef _WIN32
        if(entry.name == "desktop.ini") {
            const size_t actualLen = (std::min)(len, vfs::DESKTOP_INI.size() - pos);
            std::memcpy(dst, vfs::DESKTOP_INI.data() + pos, actualLen);

            return actualLen;
        }
    #endif

    // Requestion audio?
    if(boost::ends_with(entry.name, "wav")) {
        return generateAudio(entry, pos, len, dst, result, async);
    }
    else if(boost::ends_with(entry.name, "dng")) {
        return generateFrame(entry, pos, len, dst, result, async);
    }

    return -1;
}

void VirtualFileSystemImpl_MCRAW::updateOptions(const RenderConfig& config) {
    mConfig = config;
    
    // Re-parse exposure keyframes
    mExposureKeyframes = ExposureKeyframes::parse(config.exposureCompensation);

    mCache.clear();
    init();
}

FileInfo VirtualFileSystemImpl_MCRAW::getFileInfo() const {
    FileInfo info;
    info.medFps = mMedFps;
    info.avgFps = mAvgFps;
    info.fps = mFps;
    info.totalFrames = mTotalFrames;
    info.droppedFrames = mDroppedFrames;
    info.duplicatedFrames = mDuplicatedFrames;
    info.width = mWidth;
    info.height = mHeight;
    
    // Determine data type based on source and options
    bool interpretAsQuadBayer = mNeedRemosaic || (mConfig.options & RENDER_OPT_INTERPRET_AS_QUAD_BAYER);
    if (interpretAsQuadBayer) {
        info.dataType = "Quad Bayer CFA";
    } else {
        info.dataType = "Bayer CFA";
    }
    
    // Determine levels info - need to calculate what the output will be
    float srcWhiteLevel = mSrcWhiteLevel;
    std::array<float, 4> srcBlackLevel = mSrcBlackLevel;
    
    // Apply levels override if specified
    if (mConfig.levels == "Static") {
        // Would use camera config values, but we don't have access here
        // Just show the dynamic values
    } else if (!mConfig.levels.empty() && mConfig.levels != "Dynamic") {
        // Custom levels specified - parse them
        const size_t separatorPos = mConfig.levels.find('/');
        if (separatorPos != std::string::npos) {
            try {
                const std::string whiteLevelStr = mConfig.levels.substr(0, separatorPos);
                const std::string blackLevelStr = mConfig.levels.substr(separatorPos + 1);
                
                if (whiteLevelStr.find('.') != std::string::npos) 
                    srcWhiteLevel = std::stof(whiteLevelStr);
                else 
                    srcWhiteLevel = std::stoul(whiteLevelStr);
                
                if (blackLevelStr.find(',') == std::string::npos) {
                    float blackLevelValue;
                    if (blackLevelStr.find('.') != std::string::npos) 
                        blackLevelValue = std::stof(blackLevelStr);
                    else 
                        blackLevelValue = std::stoul(blackLevelStr);
                    srcBlackLevel = {blackLevelValue, blackLevelValue, blackLevelValue, blackLevelValue};
                }
            } catch (const std::exception&) {
                // Keep original values on parse error
            }
        }
    }
    
    float dstWhiteLevel = srcWhiteLevel;
    std::array<float, 4> dstBlackLevel = srcBlackLevel;
    
    // Check if log transform is applied
    bool applyLogCurve = !mConfig.logTransform.empty();
    bool applyShadingMap = mConfig.options & RENDER_OPT_APPLY_VIGNETTE_CORRECTION;
    bool normalizeShadingMap = mConfig.options & RENDER_OPT_NORMALIZE_SHADING_MAP;
    
    int useBits = 0;
    if (applyShadingMap && normalizeShadingMap) {
        useBits = std::min(16, static_cast<int>(std::ceil(std::log2(dstWhiteLevel + 1))) + 4);
        dstWhiteLevel = std::pow(2.0f, useBits) - 1;
    } else if (applyLogCurve) {
        if (mConfig.logTransform == "Keep Input") {
            useBits = std::min(16, static_cast<int>(std::ceil(std::log2(dstWhiteLevel + 1))));
            dstWhiteLevel = std::pow(2.0f, useBits) - 1;
        } else if (mConfig.logTransform == "Reduce by 2bit") {
            useBits = std::min(16, static_cast<int>(std::ceil(std::log2(dstWhiteLevel + 1))) - 2);
            dstWhiteLevel = std::pow(2.0f, useBits) - 1;
        } else if (mConfig.logTransform == "Reduce by 4bit") {
            useBits = std::min(16, static_cast<int>(std::ceil(std::log2(dstWhiteLevel + 1))) - 4);
            dstWhiteLevel = std::pow(2.0f, useBits) - 1;
        } else if (mConfig.logTransform == "Reduce by 6bit") {
            useBits = std::min(16, static_cast<int>(std::ceil(std::log2(dstWhiteLevel + 1))) - 6);
            dstWhiteLevel = std::pow(2.0f, useBits) - 1;
        } else if (mConfig.logTransform == "Reduce by 8bit") {
            useBits = std::min(16, static_cast<int>(std::ceil(std::log2(dstWhiteLevel + 1))) - 8);
            dstWhiteLevel = std::pow(2.0f, useBits) - 1;
        }
    } else if (applyShadingMap) {
        useBits = std::min(16, static_cast<int>(std::ceil(std::log2(dstWhiteLevel + 1))) + 2);
        dstWhiteLevel = std::pow(2.0f, useBits) - 1;
    }

    if(applyShadingMap || applyLogCurve)
        for (auto& v : dstBlackLevel)
            v = 0;
    
    // Calculate actual output bits
    int outputBits = useBits > 0 ? useBits : static_cast<int>(std::ceil(std::log2(dstWhiteLevel + 1)));
    
    // Build levels info string - always show transformation even without vignette correction
    info.levelsInfo = std::to_string(static_cast<int>(srcWhiteLevel)) + "/" + 
                      std::to_string(static_cast<int>(srcBlackLevel[0]));
    
    // Show transformation if levels changed OR if any processing is applied
    if (static_cast<int>(srcWhiteLevel) != static_cast<int>(dstWhiteLevel) || 
        static_cast<int>(srcBlackLevel[0]) != static_cast<int>(dstBlackLevel[0]) ||
        applyShadingMap || applyLogCurve) {
        info.levelsInfo += " -> " + std::to_string(static_cast<int>(dstWhiteLevel)) + "/" + 
                           std::to_string(static_cast<int>(dstBlackLevel[0]));
    }
    
    info.levelsInfo += " RAW" + std::to_string(outputBits);
    if (applyLogCurve) {
        info.levelsInfo += " log";
    }
    
    // Set runtime from audio duration
    info.runtimeSeconds = mAudioDurationSeconds;
    
    return info;
}

} // namespace motioncam

