#include "VirtualFileSystemImpl.h"
#include <algorithm>
#include <cmath>
#include <sstream>
#include <iomanip>
#include <array>
#include <boost/filesystem.hpp>
#include <spdlog/spdlog.h>

namespace motioncam {
namespace vfs {

FrameRateInfo calculateFrameRate(const std::vector<Timestamp>& frames) {
    if (frames.size() < 2) {
        return {0,0,0,0,0,0};
    }

    double avgDuration = 0.0;
    int validFrames = 0;
    std::vector<double> durations;
    durations.reserve(frames.size() - 1);

    for (size_t i = 1; i < frames.size(); ++i) {
        double duration = static_cast<double>(frames[i] - frames[i - 1]);
        if (duration > 0) {
            avgDuration += (duration - avgDuration) / (validFrames + 1);
            durations.push_back(duration);
            validFrames++;
        }
    }

    if (validFrames == 0) {
        return {0,0,0,0,0,0};
    }

    std::sort(durations.begin(), durations.end());

    // Helper for percentile (with linear interpolation)
    auto percentile = [&](double p) -> double {
        if (durations.empty()) return 0.0;
        double pos = p * (durations.size() - 1);
        size_t idx = static_cast<size_t>(pos);
        double frac = pos - idx;
        if (idx + 1 < durations.size()) {
            return durations[idx] * (1.0 - frac) + durations[idx + 1] * frac;
        }
        return durations.back();
    };

    // Duration percentiles (in nanoseconds)
    double minDur = durations.front();
    double q1Dur   = percentile(0.25);
    double medianDur = percentile(0.5);
    double q3Dur   = percentile(0.75);
    double maxDur  = durations.back();

    // Convert durations → frame rates (FPS)
    auto toFps = [](double dur) -> float {
        return dur > 0.0 ? static_cast<float>(1e9 / dur) : 0.0f;
    };

    return {
        toFps(maxDur),  // minFrameRate (worst)
        toFps(q3Dur),   // lower quartile
        toFps(medianDur),
        toFps(q1Dur),   // upper quartile
        toFps(minDur),  // max FPS (best)
        toFps(avgDuration)
    };

    /*double medianDuration;
    size_t mid = durations.size() / 2;
    if (durations.size() % 2 == 0) {
        medianDuration = (durations[mid - 1] + durations[mid]) / 2.0;
    } else {
        medianDuration = durations[mid];
    }

    return {
        static_cast<float>(1000000000.0 / medianDuration),
        static_cast<float>(1000000000.0 / avgDuration)
    };*/
}

float determineCFRTarget(FrameRateInfo fpsInfo, const CFRTarget& cfrTarget, bool applyCFRConversion) {
    if (!applyCFRConversion) {
        if (cfrTarget.mode == CFRMode::Custom) {
            return cfrTarget.customValue;
        }
        return fpsInfo.averageFrameRate;
    }
    
    switch (cfrTarget.mode) {
        case CFRMode::Disabled:
            return fpsInfo.averageFrameRate;
            
        case CFRMode::PreferInteger:
            if (fpsInfo.medianFrameRate <= 23.0 || fpsInfo.medianFrameRate >= 1000.0) return fpsInfo.medianFrameRate;
            else if (fpsInfo.medianFrameRate < 24.5) return 24.0f;
            else if (fpsInfo.medianFrameRate < 26.0) return 25.0f;
            else if (fpsInfo.medianFrameRate < 33.0) return 30.0f;
            else if (fpsInfo.medianFrameRate < 49.0) return 48.0f;
            else if (fpsInfo.medianFrameRate < 52.0) return 50.0f;
            else if (fpsInfo.medianFrameRate > 56.0 && fpsInfo.medianFrameRate < 63.0) return 60.0f;
            else if (fpsInfo.medianFrameRate > 112.0 && fpsInfo.medianFrameRate < 125.0) return 120.0f;
            else if (fpsInfo.medianFrameRate > 224.0 && fpsInfo.medianFrameRate < 250.0) return 240.0f;
            else if (fpsInfo.medianFrameRate > 448.0 && fpsInfo.medianFrameRate < 500.0) return 480.0f;
            else if (fpsInfo.medianFrameRate > 896.0 && fpsInfo.medianFrameRate < 1000.0) return 960.0f;
            else if (fpsInfo.medianFrameRate >= 63.0) return 120.0f;
            else return 60.0f;
            
        case CFRMode::PreferDropFrame:
            if (fpsInfo.medianFrameRate <= 23.0 || fpsInfo.medianFrameRate >= 1000.0) return fpsInfo.medianFrameRate;
            else if (fpsInfo.medianFrameRate < 24.5) return 23.976f;
            else if (fpsInfo.medianFrameRate < 26.0) return 25.0f;
            else if (fpsInfo.medianFrameRate < 33.0) return 29.97f;
            else if (fpsInfo.medianFrameRate < 49.0) return 47.952f;
            else if (fpsInfo.medianFrameRate < 52.0) return 50.0f;
            else if (fpsInfo.medianFrameRate > 56.0 && fpsInfo.medianFrameRate < 63.0) return 59.94f;
            else if (fpsInfo.medianFrameRate > 112.0 && fpsInfo.medianFrameRate < 125.0) return 119.88f;
            else if (fpsInfo.medianFrameRate > 224.0 && fpsInfo.medianFrameRate < 250.0) return 240.0f;
            else if (fpsInfo.medianFrameRate > 448.0 && fpsInfo.medianFrameRate < 500.0) return 480.0f;
            else if (fpsInfo.medianFrameRate > 896.0 && fpsInfo.medianFrameRate < 1000.0) return 960.0f;
            else if (fpsInfo.medianFrameRate >= 63.0) return 119.88f;
            else return 59.94f;
            
        case CFRMode::MedianSlowMotion:
            return fpsInfo.medianFrameRate;
            
        case CFRMode::AverageTesting:
            return fpsInfo.averageFrameRate;
            
        case CFRMode::Custom:
            return cfrTarget.customValue;
            
        default:
            return fpsInfo.medianFrameRate;
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

std::string getDisplayDataType(
    bool directLogRGB, bool quadBayerCapture, bool interpretAsQuad, bool remosaic) {
    if(!(quadBayerCapture || interpretAsQuad || directLogRGB)) 
        return "Bayer CFA";
    else if (directLogRGB)
        return remosaic ? "RGB -> Bayer CFA" : "RGB";
    else if (quadBayerCapture || interpretAsQuad)
        //return remosaic ? "Quad -> Bayer CFA" : "Quad Bayer CFA";  // when QB demosaic and remosaic is implemented
        return "Quad Bayer CFA";
    return "ERROR";
}

std::string getDisplayDataLevels(
    float dynWhiteLevel, std::array<float, 4> dynBlackLevel, 
    float statWhiteLevel, std::array<float, 4> statBlackLevel, 
    std::string levels, std::string logTransform,
    bool applyShadingMap, bool normalizeShadingMap) {

    float srcWhiteLevel = 0.0;
    std::array<float, 4> srcBlackLevel = {0.0, 0.0, 0.0, 0.0};

    if (levels == "Dynamic") {
        srcWhiteLevel = dynWhiteLevel;
        srcBlackLevel = dynBlackLevel;
    } else if (levels == "Static") {
        srcWhiteLevel = dynWhiteLevel;
        srcBlackLevel = dynBlackLevel;
    } else {
        const size_t separatorPos = levels.find('/');
        if (separatorPos != std::string::npos) {
            try {            
                srcWhiteLevel = std::stof(levels.substr(0, separatorPos));
                if (levels.substr(separatorPos + 1).find(',') == std::string::npos) {
                    float blackLevelValue = std::stof(levels.substr(separatorPos + 1));
                    srcBlackLevel = {blackLevelValue, blackLevelValue, blackLevelValue, blackLevelValue};
                } else {
                    //TODO: implement different bl per channel like input 1023.0/64.0,63.8,63.9,63.9
                }
            } catch (const std::exception&) {
                srcWhiteLevel = dynWhiteLevel;
                srcBlackLevel = dynBlackLevel;
            }
        } else {
            srcWhiteLevel = dynWhiteLevel;
            srcBlackLevel = dynBlackLevel;
        }   
    }

    float dstWhiteLevel = srcWhiteLevel;
    std::array<float, 4> dstBlackLevel = srcBlackLevel;

    int useBits = std::min(16, static_cast<int>(std::ceil(std::log2(srcWhiteLevel + 1))));

    if(logTransform.empty()) {
        if(applyShadingMap) {
            useBits += 2;
            if(normalizeShadingMap)
                useBits += 2;
            dstWhiteLevel = std::pow(2.0f, std::min(16, useBits)) - 1;
            for (auto& v : dstBlackLevel)
                v = 0;
        }            
    } else {
        if (logTransform == "Reduce by 2bit" || logTransform == "Reduce by 2bit lq")
            useBits -= 2;
        if (logTransform == "Reduce by 4bit" || logTransform == "Reduce by 4bit lq")
            useBits -= 4;
        if (logTransform == "Reduce by 6bit" || logTransform == "Reduce by 6bit lq")
            useBits -= 6;
        if (logTransform == "Reduce by 8bit" || logTransform == "Reduce by 8bit lq")
            useBits -= 8;
        dstWhiteLevel = std::pow(2.0f, std::min(16, useBits)) - 1;
        for (auto& v : dstBlackLevel)
            v = 0;
    }    
    
    std::string result = std::to_string(static_cast<int>(srcWhiteLevel)) + "/" + 
                         std::to_string(static_cast<int>(srcBlackLevel[0]));    // Build levels info string
    
    if (srcBlackLevel[0] != dstBlackLevel[0]) {
        result += " -> " + std::to_string(static_cast<int>(dstWhiteLevel)) + "/" + 
                           std::to_string(static_cast<int>(dstBlackLevel[0]));
    }       // Show transformation if levels changed
    
    result += " RAW" + std::to_string(std::min(16, useBits));
    if (!logTransform.empty()) 
        result += " log";    

    return result;
}

} // namespace vfs
} // namespace motioncam
