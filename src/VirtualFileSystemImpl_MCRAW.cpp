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
#include <cmath>
#include <tuple>

namespace {

// A wide centred median strongly rejects alternating auto-adjustments and
// isolated manual jumps.  The centred window deliberately provides lookahead.
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
    mSettings.draftScale =
        vfs::getScaleFromOptions(mSettings.options, mSettings.draftScale);
    
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
    std::vector<double> logExposures;
    std::array<std::vector<double>, 3> logNeutrals;
    logExposures.reserve(frames.size());
    for(const auto& frame : frames) {
        decoder.loadFrameMetadata(frame, metadata);
        const auto cameraFrameMetadata = CameraFrameMetadata::limitedParse(metadata);
        mBaselineExpValue = std::min(mBaselineExpValue, cameraFrameMetadata.iso * cameraFrameMetadata.exposureTime);
        logExposures.push_back(std::log2(std::max(1e-12, cameraFrameMetadata.iso * cameraFrameMetadata.exposureTime)));
        for (size_t channel = 0; channel < 3; ++channel) {
            const double neutral = metadata.contains("asShotNeutral") &&
                    metadata["asShotNeutral"].is_array() &&
                    metadata["asShotNeutral"].size() > channel
                ? metadata["asShotNeutral"][channel].get<double>()
                : 1.0;
            logNeutrals[channel].push_back(std::log(std::max(1e-6, neutral)));
        }
    }

    // Two seconds on either side is intentionally large enough to absorb
    // aggressive frame-to-frame changes while retaining deliberate trends.
    const double frameRate = frames.size() > 1
        ? vfs::calculateFrameRate(frames).medianFrameRate
        : 30.0;
    const int smoothingRadius = std::max(1, static_cast<int>(std::lround(2.0 * frameRate)));
    const auto smoothExposure = temporalSmooth(logExposures, smoothingRadius);
    std::array<std::vector<double>, 3> smoothNeutral;
    for (size_t channel = 0; channel < 3; ++channel)
        smoothNeutral[channel] = temporalSmooth(logNeutrals[channel], smoothingRadius);
    for (size_t i = 0; i < frames.size(); ++i) {
        mSmoothedExposureOffsets[frames[i]] = static_cast<float>(smoothExposure[i] - logExposures[i]);
        std::array<float, 3> neutral;
        const double green = smoothNeutral[1][i];
        for (size_t channel = 0; channel < 3; ++channel)
            neutral[channel] = static_cast<float>(std::exp(smoothNeutral[channel][i] - green));
        mSmoothedAsShotNeutrals[frames[i]] = neutral;
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
        AudioSampleFormat audioFormat = audioChunks[0].format;
        int bitDepth = audioFormat == AudioSampleFormat::Float32 ? 32 : 16;
        AudioWriter audioWriter(
            mAudioFile,
            decoder.numAudioChannels(),
            decoder.audioSampleRateHz(),
            fpsFraction.first,
            fpsFraction.second,
            bitDepth);

        // Sync the audio to the video
        vfs::syncAudio(
            frames[0],
            audioChunks,
            decoder.audioSampleRateHz(),
            decoder.numAudioChannels());

        // Calculate total audio duration
        size_t totalSamples = 0;
        for(auto& x : audioChunks) {
            int numFrames = x.sampleCount() / decoder.numAudioChannels();
            if (audioFormat == AudioSampleFormat::Float32) {
                audioWriter.write(x.float32Data, numFrames);
            } else {
                audioWriter.write(x.int16Data, numFrames);
            }
            totalSamples += numFrames;
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
            if (pts < lastPts) {
                ++droppedFrames;
                continue;
            }
            duplicatedFrames += std::max(0, pts - lastPts);

            // lastPts is the next output position. Fill gaps and emit the
            // current frame at its mapped position.
            while(lastPts <= pts) {
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
    int displayCfaSize = cameraFrameMetadata.cfaSize;
    if (mCalibration && mCalibration->hasCfaSize && mCalibration->cfaSize > 0)
        displayCfaSize = mCalibration->cfaSize;
    mFileInfo.dataType = vfs::getDisplayDataType(false, displayCfaSize);
    mFileInfo.levelsInfo = vfs::getDisplayDataLevels(
        cameraFrameMetadata.dynamicWhiteLevel, cameraFrameMetadata.dynamicBlackLevel,
        cameraConfig.whiteLevel, cameraConfig.blackLevel,
        mSettings.levels, logTransformModeToString(mSettings.logTransform),
        mSettings.options & RENDER_OPT_APPLY_VIGNETTE_CORRECTION,
        mSettings.options & RENDER_OPT_NORMALIZE_SHADING_MAP);
    mFileInfo.runtimeSeconds = audioDurationSec;   
}

std::vector<Entry> VirtualFileSystemImpl_MCRAW::listFiles(const std::string& filter) const {
    if (filter.empty()) {
        return mFiles;
    }

    std::vector<Entry> filteredFiles;
    for (const auto& entry : mFiles) {
        if (entry.name.find(filter) != std::string::npos) {
            filteredFiles.push_back(entry);
        }
    }
    return filteredFiles;
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
    auto task = [this, entry, pos, len, dst, result]() -> size_t {
        try {
            auto data = materializeFile(entry, false);
            const size_t count = data && pos < data->size()
                ? std::min(len, data->size() - pos) : 0;
            if (count)
                std::memcpy(dst, data->data() + pos, count);
            result(count, 0);
            return count;
        } catch (const std::exception& e) {
            spdlog::error("Failed to generate DNG (error: {})", e.what());
            result(0, -1);
            return 0;
        }
    };
    auto future = mProcessingThreadPool.submit_task(task);
    return async ? 0 : future.get();
}

std::shared_ptr<std::vector<char>> VirtualFileSystemImpl_MCRAW::materializeFile(
    const Entry& entry, bool jpegCompression) {
    if (boost::ends_with(entry.name, "wav"))
        return std::make_shared<std::vector<char>>(mAudioFile.begin(), mAudioFile.end());

    if (!jpegCompression) {
        if (auto cached = mCache.get(entry)) {
            mCache.put(entry, cached);
            return cached;
        }
    }

    try {
        thread_local std::map<std::string, std::unique_ptr<Decoder>> decoders;
        auto& decoder = decoders[mSrcPath];
        if (!decoder)
            decoder = std::make_unique<Decoder>(mSrcPath);
        const auto timestamp = std::get<Timestamp>(entry.userData);
        auto frames = decoder->getFrames();
        std::sort(frames.begin(), frames.end());
        const auto it = std::find(frames.begin(), frames.end(), timestamp);
        if (it == frames.end())
            throw std::runtime_error("MCRAW source frame not found");

        std::vector<uint8_t> frameData;
        nlohmann::json metadata;
        decoder->loadFrame(timestamp, frameData, metadata);
        const auto frameDigits = entry.name.substr(entry.name.size() - 10, 6);
        const int outputFrameNumber = std::stoi(frameDigits);
        std::optional<float> exposureOverride;
        if ((mSettings.options & RENDER_OPT_NORMALIZE_EXPOSURE) &&
            (mSettings.options & RENDER_OPT_SMOOTH_EXPOSURE))
            exposureOverride = mSmoothedExposureOffsets.at(timestamp);
        std::optional<std::array<float, 3>> neutralOverride;
        if (mSettings.options & RENDER_OPT_SMOOTH_WHITE_BALANCE)
            neutralOverride = mSmoothedAsShotNeutrals.at(timestamp);
        auto output = utils::generateDng(
            frameData,
            CameraFrameMetadata::parse(metadata),
            CameraConfiguration::parse(decoder->getContainerMetadata()),
            mFps,
            outputFrameNumber,
            mBaselineExpValue,
            mSettings,
            mCalibration,
            jpegCompression,
            exposureOverride,
            neutralOverride);
        if (!output)
            throw std::runtime_error("DNG generation returned no data");
        if (!jpegCompression)
            mCache.put(entry, output);
        return output;
    } catch (...) {
        if (!jpegCompression)
            mCache.markLoadFailed(entry);
        throw;
    }
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
    mSettings.draftScale =
        vfs::getScaleFromOptions(mSettings.options, mSettings.draftScale);
    mCache.clear();
    init();
}

FileInfo VirtualFileSystemImpl_MCRAW::getFileInfo() const {
    return mFileInfo;
}

} // namespace motioncam

