#include "VirtualFileSystemImpl.h"
#include <algorithm>
#include <cmath>
#include <sstream>
#include <iomanip>
#include <boost/filesystem.hpp>
#include <spdlog/spdlog.h>

namespace motioncam {
namespace vfs {

FrameRateInfo calculateFrameRate(const std::vector<Timestamp>& frames) {
    if (frames.size() < 2) {
        return {0.0f, 0.0f};
    }

    double avgDuration = 0.0;
    int validFrames = 0;
    std::vector<double> durations;
    durations.reserve(frames.size() - 1);

    for (size_t i = 1; i < frames.size(); ++i) {
        double duration = static_cast<double>(frames[i] - frames[i-1]);
        if (duration > 0) {
            avgDuration = avgDuration + (duration - avgDuration) / (validFrames + 1);
            durations.push_back(duration);
            validFrames++;
        }
    }

    if (validFrames == 0) {
        return {0.0f, 0.0f};
    }

    std::sort(durations.begin(), durations.end());
    double medianDuration;
    size_t mid = durations.size() / 2;
    if (durations.size() % 2 == 0) {
        medianDuration = (durations[mid - 1] + durations[mid]) / 2.0;
    } else {
        medianDuration = durations[mid];
    }

    return {
        static_cast<float>(1000000000.0 / medianDuration),
        static_cast<float>(1000000000.0 / avgDuration)
    };
}

float determineCFRTarget(float medianFps, const std::string& cfrTarget, bool applyCFRConversion) {
    if (!applyCFRConversion || cfrTarget.empty()) {
        try {
            return std::stof(cfrTarget);
        } catch (const std::exception&) {
            spdlog::warn("Invalid CFR target '{}', using median frame rate", cfrTarget);
            return medianFps;
        }
    }

    if (cfrTarget == "Prefer Integer") {
        if (medianFps <= 23.0 || medianFps >= 1000.0) return medianFps;
        else if (medianFps < 24.5) return 24.0f;
        else if (medianFps < 26.0) return 25.0f;
        else if (medianFps < 33.0) return 30.0f;
        else if (medianFps < 49.0) return 48.0f;
        else if (medianFps < 52.0) return 50.0f;
        else if (medianFps > 56.0 && medianFps < 63.0) return 60.0f;
        else if (medianFps > 112.0 && medianFps < 125.0) return 120.0f;
        else if (medianFps > 224.0 && medianFps < 250.0) return 240.0f;
        else if (medianFps > 448.0 && medianFps < 500.0) return 480.0f;
        else if (medianFps > 896.0 && medianFps < 1000.0) return 960.0f;
        else if (medianFps >= 63.0) return 120.0f;
        else return 60.0f;
    }
    else if (cfrTarget == "Prefer Drop Frame") {
        if (medianFps <= 23.0 || medianFps >= 1000.0) return medianFps;
        else if (medianFps < 24.5) return 23.976f;
        else if (medianFps < 26.0) return 25.0f;
        else if (medianFps < 33.0) return 29.97f;
        else if (medianFps < 49.0) return 47.952f;
        else if (medianFps < 52.0) return 50.0f;
        else if (medianFps > 56.0 && medianFps < 63.0) return 59.94f;
        else if (medianFps > 112.0 && medianFps < 125.0) return 119.88f;
        else if (medianFps > 224.0 && medianFps < 250.0) return 240.0f;
        else if (medianFps > 448.0 && medianFps < 500.0) return 480.0f;
        else if (medianFps > 896.0 && medianFps < 1000.0) return 960.0f;
        else if (medianFps >= 63.0) return 119.88f;
        else return 59.94f;
    }
    else if (cfrTarget == "Median (Slowmotion)") {
        return medianFps;
    }
    else if (cfrTarget == "Average (Testing)") {
        return medianFps;
    }
    else {
        try {
            return std::stof(cfrTarget);
        } catch (const std::exception&) {
            spdlog::warn("Invalid CFR target '{}', using median frame rate", cfrTarget);
            return medianFps;
        }
    }
}

int getFrameNumberFromTimestamp(Timestamp timestamp, Timestamp referenceTimestamp, float frameRate) {
    if (frameRate <= 0) {
        return -1;
    }

    int64_t timeDifference = timestamp - referenceTimestamp;
    if (timeDifference < 0) {
        return -1;
    }

    double nanosecondsPerFrame = 1000000000.0 / frameRate;
    return static_cast<int>(std::round(timeDifference / nanosecondsPerFrame));
}

std::string constructFrameFilename(
    const std::string& baseName,
    int frameNumber,
    int padding,
    const std::string& extension)
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

std::string extractFilenameWithoutExtension(const std::string& fullPath) {
    boost::filesystem::path p(fullPath);
    return p.stem().string();
}

int getScaleFromOptions(FileRenderOptions options, int draftScale) {
    if (options & RENDER_OPT_DRAFT)
        return draftScale;
    return 1;
}

void syncAudio(
    Timestamp videoTimestamp,
    std::vector<AudioChunk>& audioChunks,
    int sampleRate,
    int numChannels)
{
    if (audioChunks.empty()) {
        return;
    }

    auto audioVideoDriftMs = (audioChunks[0].first - videoTimestamp) * 1e-6f;
    if (std::abs(audioVideoDriftMs) > 1000) {
        spdlog::warn("Audio drift too large, not syncing audio");
        return;
    }

    if (audioVideoDriftMs > 0) {
        int audioFramesToRemove = static_cast<int>(std::round(audioVideoDriftMs * sampleRate / 1000));
        int samplesToRemove = audioFramesToRemove * numChannels;

        int samplesRemoved = 0;
        auto it = audioChunks.begin();

        while (it != audioChunks.end() && samplesRemoved < samplesToRemove) {
            int remainingSamplesToRemove = samplesToRemove - samplesRemoved;

            if (it->second.size() <= static_cast<size_t>(remainingSamplesToRemove)) {
                samplesRemoved += it->second.size();
                it = audioChunks.erase(it);
            }
            else {
                it->second.erase(it->second.begin(), it->second.begin() + remainingSamplesToRemove);
                it->first += static_cast<Timestamp>(remainingSamplesToRemove * 1000 / sampleRate);
                break;
            }
        }
    }
    else {
        auto silenceDuration = -audioVideoDriftMs;
        int silenceFrames = static_cast<int>(std::round(silenceDuration * sampleRate / 1000));
        int silenceSamples = silenceFrames * numChannels;

        std::vector<int16_t> silenceData(silenceSamples, 0);
        AudioChunk silenceChunk = std::make_pair(videoTimestamp, silenceData);

        audioChunks.insert(audioChunks.begin(), silenceChunk);

        for (auto it = audioChunks.begin() + 1; it != audioChunks.end(); ++it) {
            it->first += silenceDuration;
        }
    }
}

} // namespace vfs
} // namespace motioncam
