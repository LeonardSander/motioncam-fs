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
        const RenderSettings& settings,
        const std::string& file,
        const std::string& baseName) :
        mCache(lruCache),
        mIoThreadPool(ioThreadPool),
        mProcessingThreadPool(processingThreadPool),
        mSrcPath(file),
        mBaseName(baseName),
        mSettings(settings) {
    
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
    nlohmann::json metadata;
    for(const auto& frame : frames) {
        decoder.loadFrameMetadata(frame, metadata);
        const auto& cameraFrameMetadata = CameraFrameMetadata::limitedParse(metadata);
        mBaselineExpValue = std::min(mBaselineExpValue, cameraFrameMetadata.iso * cameraFrameMetadata.exposureTime);
    }
    this->init(/*mOptions*/);
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

    spdlog::debug("VirtualFileSystemImpl_MCRAW::init(options={})", optionsToString(mSettings.options));

    // Clear everything
    mFiles.clear();

    mFrameRateInfo = vfs::calculateFrameRate(frames);

    bool applyCFRConversion = mSettings.options & RENDER_OPT_FRAMERATE_CONVERSION;
    mFps = vfs::determineCFRTarget(mFrameRateInfo, mSettings.cfrTarget, applyCFRConversion);

    /*bool applyCFRConversion = options & RENDER_OPT_FRAMERATE_CONVERSION;

    if (applyCFRConversion && mCFRTarget.mode != CFRMode::Disabled) {
        if (mCFRTarget.mode == CFRMode::PreferInteger) {
            if (mMedFps <=  23.0 || mMedFps >= 1000.0)
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
            else if (mMedFps > 56.0  && mMedFps < 63.0)
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
        else if (mCFRTarget.mode == CFRMode::PreferDropFrame) {
            if (mMedFps <=  23.0 || mMedFps >= 1000.0)
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
            else if (mMedFps > 56.0  && mMedFps < 63.0)
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
        else if (mCFRTarget.mode == CFRMode::MedianSlowMotion) {
            // Use median frame rate for non real time playback
            mFps = mMedFps;
        }
        else if (mCFRTarget.mode == CFRMode::AverageTesting) {
            // legacy framerate target determination
            mFps = mAvgFps;
        }
        else if (mCFRTarget.mode == CFRMode::Custom) {
            // Custom framerate
            mFps = mCFRTarget.customValue;
        }
    } else {
        // No CFR conversion - use custom value if provided, otherwise use average
        if (mCFRTarget.mode == CFRMode::Custom) {
            mFps = mCFRTarget.customValue;
        } else {
            mFps = mAvgFps;
        }
    }*/       

    // Calculate typical DNG size that we can use for all files
    std::vector<uint8_t> data;
    nlohmann::json metadata;

    decoder.loadFrame(frames[0], data, metadata);

    auto cameraConfig = CameraConfiguration::parse(decoder.getContainerMetadata());
    auto cameraFrameMetadata = CameraFrameMetadata::parse(metadata);
   
    // Parse exposure keyframes if the input contains keyframe syntax
    std::optional<ExposureKeyframes> exposureKeyframes = ExposureKeyframes::parse(mSettings.exposureCompensation);

        // Store frame information
    /*mWidth = cameraFrameMetadata.width;
    mHeight = cameraFrameMetadata.height;
    mTotalFrames = static_cast<int>(frames.size());
    mDroppedFrames = 0; // Will be calculated during frame processing
    mDuplicatedFrames = 0;
    mNeedRemosaic = cameraFrameMetadata.needRemosaic;
    mSrcWhiteLevel = cameraFrameMetadata.dynamicWhiteLevel;
    mSrcBlackLevel = cameraFrameMetadata.dynamicBlackLevel;	*/
    
    auto dngData = utils::generateDng(
        data,
        cameraFrameMetadata,
        cameraConfig,
        mFps,
        0,
        mBaselineExpValue,
        mSettings,
        exposureKeyframes,
        mCalibration,
        false  // Compression always false for virtual filesystem
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

    float audioDurationSec = 0.0f;

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
            audioDurationSec = static_cast<float>(totalSamples) / static_cast<float>(decoder.audioSampleRateHz());
        }
    }

    if(!mAudioFile.empty()) {
        audioEntry.type = EntryType::FILE_ENTRY;
        audioEntry.size = mAudioFile.size();
        audioEntry.name = "audio.wav";

        mFiles.emplace_back(audioEntry);
    }

    int duplicatedFrames = 0;
    int droppedFrames = 0;

    // Add video frames
    for(auto& x : frames) {
        if(applyCFRConversion) {
            int pts = vfs::getFrameNumberFromTimestamp(x, frames[0], mFps);

            // Count dropped frames before this frame
            duplicatedFrames += (std::max)(0, pts - lastPts - 1);

            if (lastPts > 0 && lastPts == pts)
                droppedFrames += 1;

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

    // Store frame information
    mFileInfo.frameRateInfo = mFrameRateInfo;
    mFileInfo.fps = mFps;
    mFileInfo.totalFrames = static_cast<int>(frames.size());
    mFileInfo.droppedFrames = droppedFrames;
    mFileInfo.duplicatedFrames = duplicatedFrames;
    mFileInfo.width = cameraFrameMetadata.width;
    mFileInfo.height = cameraFrameMetadata.height;
    mFileInfo.dataType = vfs::getDisplayDataType(
        false,
        cameraFrameMetadata.needRemosaic, 
        mSettings.options & RENDER_OPT_INTERPRET_AS_QUAD_BAYER,
        mSettings.options & RENDER_OPT_REMOSAIC_TO_BAYER);
    mFileInfo.levelsInfo = vfs::getDisplayDataLevels(
        cameraFrameMetadata.dynamicWhiteLevel, cameraFrameMetadata.dynamicBlackLevel,
        cameraConfig.whiteLevel, cameraConfig.blackLevel,
        mSettings.levels, logTransformModeToString(mSettings.logTransform),
        mSettings.options & RENDER_OPT_APPLY_VIGNETTE_CORRECTION,
        mSettings.options & RENDER_OPT_NORMALIZE_SHADING_MAP);
    mFileInfo.runtimeSeconds = audioDurationSec;   
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
    auto frameDataFuture = mIoThreadPool.submit_task([entry, &srcPath = mSrcPath, &options = mSettings.options]() -> FrameData {
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
    const auto settings = mSettings;
    const auto baselineExpValue = mBaselineExpValue;
    const auto calibration = mCalibration;

    auto generateTask = [this, &cache = mCache, entry, sharableFuture, fps, settings, baselineExpValue, calibration, pos, len, dst, result]() {
        size_t readBytes = 0;
        int errorCode = -1;

        try {
            auto decodedFrame = sharableFuture.get();
            auto [frameIndex, containerMetadata, frameMetadata, frameData] = std::move(decodedFrame);

            spdlog::debug("Generating {}", entry.name);

            // Parse exposure keyframes if the input contains keyframe syntax
            std::optional<ExposureKeyframes> exposureKeyframes = ExposureKeyframes::parse(settings.exposureCompensation);

            /*std::string frameExposureComp = mConfig.exposureCompensation;
            if (mExposureKeyframes.has_value()) {
                float exposureValue = mExposureKeyframes->getExposureAtFrame(frameIndex, mTotalFrames);
                frameExposureComp = std::to_string(exposureValue);
            }
            RenderSettings settings(
                options,
                draftScale,
                mCFRTarget,
                mCropTarget,
                mCameraModel,
                mLevels,
                mLogTransform,
                mExposureCompensation,
                mQuadBayerOption
            );*/
            
            bool enableCompression = settings.options & RENDER_OPT_JPEG_COMPRESSION;

            auto dngData = utils::generateDng(
                *frameData,
                frameMetadata,
                containerMetadata,
                fps,
                frameIndex,
                baselineExpValue,
                settings,
                exposureKeyframes,
                calibration,
                enableCompression);

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

void VirtualFileSystemImpl_MCRAW::updateOptions(const RenderSettings& settings) {
    mSettings = settings;
    mCache.clear();
    init();
}

FileInfo VirtualFileSystemImpl_MCRAW::getFileInfo() const {
    return mFileInfo;
}

} // namespace motioncam

