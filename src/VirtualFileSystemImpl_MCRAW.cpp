#include "VirtualFileSystemImpl_MCRAW.h"
#include "CameraFrameMetadata.h"
#include "CameraMetadata.h"
#include "Measure.h"
#include "Utils.h"
#include "AudioWriter.h"

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
    constexpr auto IO_THREADS = 4;

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

    float calculateFrameRate(const std::vector<Timestamp>& frames) {
        // Need at least 2 frames to calculate frame rate
        if (frames.size() < 2) {
            return 0.0f;
        }

        // Use running average to prevent overflow
        double avgDuration = 0.0;
        int validFrames = 0;

        for (size_t i = 1; i < frames.size(); ++i) {
            double duration = static_cast<double>(frames[i] - frames[i-1]);

            if (duration > 0) {
                // Update running average
                // new_avg = old_avg + (new_value - old_avg) / (count + 1)
                avgDuration = avgDuration + (duration - avgDuration) / (validFrames + 1);
                validFrames++;
            }
        }

        if (validFrames == 0) {
            return 0.0f;
        }

        return static_cast<float>(1000000000.0 / avgDuration);
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
    FileRenderOptions options, int draftScale, const std::string& file) :
        mIoThreadPool(std::make_unique<BS::thread_pool>(IO_THREADS)),
        mProcessingThreadPool(std::make_unique<BS::thread_pool>()),
        mSrcPath(file),
        mBaseName(extractFilenameWithoutExtension(file)),
        mTypicalDngSize(0),
        mFps(0),
        mDraftScale(draftScale) {
    
    Decoder decoder(mSrcPath);
    auto frames = decoder.getFrames();
    std::sort(frames.begin(), frames.end());

    if(frames.empty())
        return;

    mBaselineExpValue = std::numeric_limits<double>::max();  // find most insensible exposure settings in sequence, Start with maximum possible value

    for(const auto& frame : frames) {
        nlohmann::json metadata;
        decoder.loadFrameMetadata(frame, metadata);
        const auto& cameraFrameMetadata = CameraFrameMetadata::limitedParse(metadata);
        //spdlog::debug("ISO={}, EXP={}", cameraFrameMetadata.iso, cameraFrameMetadata.exposureTime);
        mBaselineExpValue = std::min(mBaselineExpValue, cameraFrameMetadata.iso * cameraFrameMetadata.exposureTime);
        //spdlog::debug("VirtualFileSystemImpl_MCRAW::VirtualFileSystemImpl_MCRAW: mBaselineExpValue={}", mBaselineExpValue);
    } 

    init(options);
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

    mFps = calculateFrameRate(frames);

    // Calculate typical DNG size that we can use for all files
    std::vector<uint8_t> data;
    nlohmann::json metadata;

    decoder.loadFrame(frames[0], data, metadata);

    auto cameraConfig = CameraConfiguration::parse(decoder.getContainerMetadata());
    auto cameraFrameMetadata = CameraFrameMetadata::parse(metadata);

    auto dngData = utils::generateDng(
        data,
        cameraFrameMetadata,
        cameraConfig,
        mFps,
        0,
        options,
        getScaleFromOptions(options, mDraftScale),
        mBaselineExpValue);

    mTypicalDngSize = dngData->size();

    // Generate file entries
    int lastPts = 0;

    mFiles.reserve(frames.size()*2);

#ifdef _WIN32
    Entry desktopIni;

    desktopIni.type = FILE_ENTRY;
    desktopIni.size = DESKTOP_INI.size();
    desktopIni.name = "desktop.ini";

    mFiles.emplace_back(desktopIni);
#endif

    // Generate and add audio (TODO: We're loading all the audio into memory + trim to sync with video)
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
        int pts = getFrameNumberFromTimestamp(x, frames[0], mFps);

        // Duplicate frames to account for dropped frames
        while(lastPts < pts) {
            Entry entry;

            // Add main entry
            entry.type = EntryType::FILE_ENTRY;
            entry.size = mTypicalDngSize;
            entry.name = constructFrameFilename("frame-", lastPts, 6, "dng");
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
        if(boost::filesystem::path(fullPath) == e.getFullPath())
            return e;
    }

    return {};
}

size_t VirtualFileSystemImpl_MCRAW::generateFrame(
    const Entry& entry, FileRenderOptions options, const size_t pos, const size_t len, void* dst, std::function<void(size_t, int)> result) const
{
    using FrameData = std::tuple<size_t, CameraConfiguration, CameraFrameMetadata, std::shared_ptr<std::vector<uint8_t>>>;

    // Use IO thread pool to decode frame
    auto frameDataFuture = mIoThreadPool->submit_task([this, entry, options]() -> FrameData {
        thread_local std::map<std::string, std::unique_ptr<Decoder>> decoders;

        auto timestamp = std::get<Timestamp>(entry.userData);

        spdlog::debug("Reading frame {} with options {}", timestamp, optionsToString(options));

        if(decoders.find(mSrcPath) == decoders.end()) {
            decoders[mSrcPath] = std::make_unique<Decoder>(mSrcPath);
        }

        auto& decoder = decoders[mSrcPath];
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

    auto generateTask = [sharableFuture, fps, draftScale, baselineExpValue, options, pos, len, dst, result]() {
        size_t readBytes = 0;
        int errorCode = -1;

        try {
            auto decodedFrame = sharableFuture.get();
            auto [frameIndex, containerMetadata, frameMetadata, frameData] = std::move(decodedFrame);

            auto dngData = utils::generateDng(
                *frameData,
                frameMetadata,
                containerMetadata,
                fps,
                frameIndex,
                options,
                getScaleFromOptions(options, draftScale),
                baselineExpValue);

            if(dngData && pos < dngData->size()) {
                // Calculate length to copy
                const size_t actualLen = (std::min)(len, dngData->size() - pos);

                std::memcpy(dst, dngData->data() + pos, actualLen);

                readBytes = actualLen;
                errorCode = 0;
            }
        }
        catch(std::runtime_error& e) {
            spdlog::error("Failed to read frame (error: {})", e.what());
        }

        result(readBytes, errorCode);
    };

    (void)mProcessingThreadPool->submit_task(generateTask);

    return 0;
}

size_t VirtualFileSystemImpl_MCRAW::generateAudio(
    const Entry& entry, FileRenderOptions options, const size_t pos, const size_t len, void* dst, std::function<void(size_t, int)> result) const
{
    size_t readBytes = 0;

    if(pos < mAudioFile.size()) {
        // Calculate length to copy
        const size_t actualLen = (std::min)(len, mAudioFile.size() - pos);

        std::memcpy(dst, mAudioFile.data() + pos, actualLen);

        readBytes = actualLen;
    }

    return readBytes;
}

size_t VirtualFileSystemImpl_MCRAW::readFile(
    const Entry& entry, FileRenderOptions options, const size_t pos, const size_t len, void* dst, std::function<void(size_t, int)> result) const {

    #ifdef _WIN32
        if(entry.name == "desktop.ini") {
            const size_t actualLen = (std::min)(len, DESKTOP_INI.size() - pos);
            std::memcpy(dst, DESKTOP_INI.data() + pos, actualLen);

            return actualLen;
        }
    #endif

    // Requestion audio?
    if(boost::ends_with(entry.name, "wav")) {
        return generateAudio(entry, options, pos, len, dst, result);
    }
    else if(boost::ends_with(entry.name, "dng")) {
        return generateFrame(entry, options, pos, len, dst, result);
    }

    result(0, -1);

    return 0;
}

void VirtualFileSystemImpl_MCRAW::updateOptions(FileRenderOptions options, int draftScale) {
    mDraftScale = draftScale;

    init(options);
}

} // namespace motioncam

