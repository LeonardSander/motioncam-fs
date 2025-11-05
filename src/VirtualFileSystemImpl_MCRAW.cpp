#include "VirtualFileSystemImpl_MCRAW.h"
#include "CameraFrameMetadata.h"
#include "CameraMetadata.h"
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
#include <sstream>
#include <tuple>

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

    std::string extractFilenameWithoutExtension(const std::string& fullPath) {
        boost::filesystem::path p(fullPath);
        return p.stem().string();
    }

    struct FrameRateInfo {
        float medianFrameRate;
        float averageFrameRate;
    };

    FrameRateInfo calculateFrameRate(const std::vector<Timestamp>& frames) {
        // Need at least 2 frames to calculate frame rate
        if (frames.size() < 2) {
            return {0.0f, 0.0f};
        }

        // Use running average to prevent overflow
        double avgDuration = 0.0;
        int validFrames = 0;
        std::vector<double> durations;  // Store all valid durations for median calculation

        for (size_t i = 1; i < frames.size(); ++i) {
            double duration = static_cast<double>(frames[i] - frames[i-1]);

            if (duration > 0) {
                // Update running average
                // new_avg = old_avg + (new_value - old_avg) / (count + 1)
                avgDuration = avgDuration + (duration - avgDuration) / (validFrames + 1);
                durations.push_back(duration);  // Store duration for median calculation
                validFrames++;
            }
        }

        // Calculate median duration
        double medianDuration = 0.0;
        if (!durations.empty()) {
            std::sort(durations.begin(), durations.end());
            size_t mid = durations.size() / 2;
            if (durations.size() % 2 == 0) {
                // Even number of elements - average of two middle values
                medianDuration = (durations[mid - 1] + durations[mid]) / 2.0;
            } else {
                // Odd number of elements - middle value
                medianDuration = durations[mid];
            }
        }

        if (validFrames == 0) {
            return {0.0f, 0.0f};
        }

        return {
            static_cast<float>(1000000000.0 / medianDuration),
            static_cast<float>(1000000000.0 / avgDuration)
        };
    }

    int64_t getFrameNumberFromTimestamp(Timestamp timestamp, Timestamp referenceTimestamp, float frameRate) {
        if (frameRate <= 0) {
            return -1; // Invalid frame rate
        }

        int64_t timeDifference = timestamp - referenceTimestamp;
        if (timeDifference < 0) {
            return -1;
        }

        // Calculate microseconds per frame
        double nanosecondsPerFrame = 1000000000.0 / frameRate;

        // Calculate expected frame number
        return static_cast<int64_t>(std::round(timeDifference / nanosecondsPerFrame));
    }

    std::string constructFrameFilename(
        const std::string& baseName, int frameNumber, int padding = 6, const std::string& extension = "")
    {
        std::ostringstream oss;

        // Add the base name
        oss << baseName;

        // Add the zero-padded frame number
        oss << std::setfill('0') << std::setw(padding) << frameNumber;

        // Add the extension if provided
        if (!extension.empty()) {
            // Check if extension already has a dot prefix
            if (extension[0] != '.') {
                oss << '.';
            }
            oss << extension;
        }

        return oss.str();
    }

    void syncAudio(Timestamp videoTimestamp, std::vector<AudioChunk>& audioChunks, int sampleRate, int numChannels) {
        // Calculate drift between the video and audio
        auto audioVideoDriftMs = (audioChunks[0].first - videoTimestamp) * 1e-6f;
        if(std::abs(audioVideoDriftMs) > 1000) {
            spdlog::warn("Audio drift too large, not syncing audio");
            return;
        }

        if(audioVideoDriftMs > 0) {
            // Calculate how many audio frames to remove
            int audioFramesToRemove = static_cast<int>(std::round(audioVideoDriftMs * sampleRate / 1000));
            int samplesToRemove = audioFramesToRemove * numChannels;

            // Remove samples from the beginning of audio chunks
            int samplesRemoved = 0;
            auto it = audioChunks.begin();

            while(it != audioChunks.end() && samplesRemoved < samplesToRemove) {
                int remainingSamplesToRemove = samplesToRemove - samplesRemoved;

                if(it->second.size() <= remainingSamplesToRemove) {
                    // Remove entire chunk
                    samplesRemoved += it->second.size();
                    it = audioChunks.erase(it);
                }
                else {
                    // Trim partial chunk from the beginning
                    it->second.erase(it->second.begin(), it->second.begin() + remainingSamplesToRemove);

                    // Update timestamp for the trimmed chunk
                    it->first += static_cast<Timestamp>(remainingSamplesToRemove * 1000 / sampleRate);
                    break;
                }
            }
        }
        else {
            // Otherwise video starts before audio, add silence
            auto silenceDuration = -audioVideoDriftMs; // Make positive

            int silenceFrames = static_cast<int>(std::round(silenceDuration * sampleRate / 1000));
            int silenceSamples = silenceFrames * numChannels;

            // Create silence chunk at the beginning
            std::vector<int16_t> silenceData(silenceSamples, 0);
            AudioChunk silenceChunk = std::make_pair(videoTimestamp, silenceData);

            // Insert silence at the beginning
            audioChunks.insert(audioChunks.begin(), silenceChunk);

            // Update timestamps of existing chunks
            for(auto it = audioChunks.begin() + 1; it != audioChunks.end(); ++it) {
                it->first += silenceDuration;
            }
        }
    }

    int getScaleFromOptions(FileRenderOptions options, int draftScale) {
        if(options & RENDER_OPT_DRAFT)
            return draftScale;

        return 1;
    }
}

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
        mTypicalDngSize(0),
        mFps(0),
        mMedFps(0),
        mAvgFps(0),
        mTotalFrames(0),
        mDroppedFrames(0),
        mDuplicatedFrames(0),
        mWidth(0),
        mHeight(0),
        mDraftScale(settings.draftScale),
        mCFRTarget(settings.cfrTarget),
        mCropTarget(settings.cropTarget),
        mCameraModel(settings.cameraModel),
        mLevels(settings.levels),
        mLogTransform(settings.logTransform),
        mExposureCompensation(settings.exposureCompensation),
        mQuadBayerOption(settings.quadBayerOption),
        mOptions(settings.options) {
    
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
    this->init(mOptions);
}

VirtualFileSystemImpl_MCRAW::~VirtualFileSystemImpl_MCRAW() {
    spdlog::info("Destroying VirtualFileSystemImpl_MCRAW({})", mSrcPath);
}

void VirtualFileSystemImpl_MCRAW::init(FileRenderOptions options) {
    Decoder decoder(mSrcPath);
    auto frames = decoder.getFrames();
    std::sort(frames.begin(), frames.end());

    if(frames.empty())
        return;

    spdlog::debug("VirtualFileSystemImpl_MCRAW::init(options={})", optionsToString(options));

    // Clear everything
    mFiles.clear();

    auto frameRateInfo = calculateFrameRate(frames);
    mMedFps = frameRateInfo.medianFrameRate;
    mAvgFps = frameRateInfo.averageFrameRate;

    bool applyCFRConversion = options & RENDER_OPT_FRAMERATE_CONVERSION;

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
    }       

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

    RenderSettings settingsForInit(
        options,
        mDraftScale,
        mCFRTarget,
        mCropTarget,
        mCameraModel,
        mLevels,
        mLogTransform,
        mExposureCompensation,
        mQuadBayerOption
    );

    auto dngData = utils::generateDng(
        data,
        cameraFrameMetadata,
        cameraConfig,
        mFps,
        0,
        mBaselineExpValue,
        settingsForInit
    );

    mTypicalDngSize = dngData->size();

    // Generate file entries
    int lastPts = 0;

    mFiles.reserve(frames.size()*2);

// Disable icon previews in Windows/MacOS
#ifdef _WIN32
    Entry desktopIni;

    desktopIni.type = FILE_ENTRY;
    desktopIni.size = DESKTOP_INI.size();
    desktopIni.name = "desktop.ini";

    mFiles.emplace_back(desktopIni);
#endif

    // Generate and add audio (TODO: We're loading all the audio into memory)
    Entry audioEntry;

    std::vector<AudioChunk> audioChunks;
    decoder.loadAudio(audioChunks);

    if(!audioChunks.empty()) {
        auto fpsFraction = utils::toFraction(mFps);
        AudioWriter audioWriter(mAudioFile, decoder.numAudioChannels(), decoder.audioSampleRateHz(), fpsFraction.first, fpsFraction.second);

        // Sync the audio to the video
        syncAudio(
            frames[0],
            audioChunks,
            decoder.audioSampleRateHz(),
            decoder.numAudioChannels());

        for(auto& x : audioChunks)
            audioWriter.write(x.second, x.second.size() / decoder.numAudioChannels());
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
            int pts = getFrameNumberFromTimestamp(x, frames[0], mFps);

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
                entry.name = constructFrameFilename(mBaseName + std::string("-"), lastPts, 6, "dng");     
                entry.userData = x;

                mFiles.emplace_back(entry);
                ++lastPts;
            }
        } else {
            Entry entry;

            // Add main entry
            entry.type = EntryType::FILE_ENTRY;
            entry.size = mTypicalDngSize;
            entry.name = constructFrameFilename(mBaseName + std::string("-"), lastPts, 6, "dng");     
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
    auto frameDataFuture = mIoThreadPool.submit_task([entry, &srcPath = mSrcPath, &options = mOptions]() -> FrameData {
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
    const auto draftScale = mDraftScale;
    const auto baselineExpValue = mBaselineExpValue;
    const auto options = mOptions;

    auto generateTask = [this, &cache = mCache, entry, sharableFuture, fps, draftScale, baselineExpValue, options, pos, len, dst, result]() {
        size_t readBytes = 0;
        int errorCode = -1;

        try {
            auto decodedFrame = sharableFuture.get();
            auto [frameIndex, containerMetadata, frameMetadata, frameData] = std::move(decodedFrame);

            spdlog::debug("Generating {}", entry.name);

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
            );

            auto dngData = utils::generateDng(
                *frameData,
                frameMetadata,
                containerMetadata,
                fps,
                frameIndex,
                baselineExpValue,
                settings);

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
            const size_t actualLen = (std::min)(len, DESKTOP_INI.size() - pos);
            std::memcpy(dst, DESKTOP_INI.data() + pos, actualLen);

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
    mDraftScale = settings.draftScale;
    mOptions = settings.options;
    mCFRTarget = settings.cfrTarget;
    mCropTarget = settings.cropTarget;
    mCameraModel = settings.cameraModel;
    mLevels = settings.levels;
    mLogTransform = settings.logTransform;
    mExposureCompensation = settings.exposureCompensation;
    mQuadBayerOption = settings.quadBayerOption;

    mCache.clear();
    init(settings.options);
}

FileInfo VirtualFileSystemImpl_MCRAW::getFileInfo() const {
    return FileInfo{
        mMedFps,
        mAvgFps,
        mFps,
        mTotalFrames,
        mDroppedFrames,
        mDuplicatedFrames,
        mWidth,
        mHeight
    };
}

} // namespace motioncam

