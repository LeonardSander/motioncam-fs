#include "VirtualFileSystemImpl.h"
#include <motioncam/Decoder.hpp>
#include <algorithm>
#include <cmath>
#include <sstream>
#include <iomanip>
#include <array>
#include <fstream>
#include <filesystem>
#include <boost/filesystem.hpp>
#include <spdlog/spdlog.h>

namespace motioncam {
namespace vfs {

void finalize(
    IVirtualFileSystem& filesystem,
    const std::string& destination,
    bool jpegCompression,
    const std::function<bool(size_t, size_t, const std::string&)>& progress) {
    namespace stdfs = std::filesystem;

    auto entries = filesystem.listFiles("");
    entries.erase(std::remove_if(entries.begin(), entries.end(), [](const Entry& entry) {
        return entry.type != EntryType::FILE_ENTRY || entry.name == "desktop.ini";
    }), entries.end());

    stdfs::create_directories(destination);
    size_t completed = 0;
    for (const auto& entry : entries) {
        if (progress && !progress(completed, entries.size(), entry.name)) {
            throw std::runtime_error("Finalization cancelled");
        }

        auto data = filesystem.materializeFile(entry, jpegCompression);
        if (!data) {
            throw std::runtime_error("Failed to render " + entry.name);
        }

        stdfs::path output = stdfs::path(destination);
        for (const auto& part : entry.pathParts)
            output /= part;
        output /= entry.name;
        stdfs::create_directories(output.parent_path());

        std::ofstream stream(output, std::ios::binary | std::ios::trunc);
        if (!stream)
            throw std::runtime_error("Could not create " + output.string());
        stream.write(data->data(), static_cast<std::streamsize>(data->size()));
        if (!stream)
            throw std::runtime_error("Could not completely write " + output.string());
        ++completed;
    }

    if (progress)
        progress(completed, entries.size(), "");
}

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

    auto audioVideoDriftMs = (audioChunks[0].timestamp - videoTimestamp) * 1e-6f;
    if (std::abs(audioVideoDriftMs) > 1000) {
        spdlog::warn("Audio drift too large, not syncing audio");
        return;
    }

    AudioSampleFormat format = audioChunks[0].format;

    if (audioVideoDriftMs > 0) {
        int audioFramesToRemove = static_cast<int>(std::round(audioVideoDriftMs * sampleRate / 1000));
        int samplesToRemove = audioFramesToRemove * numChannels;

        int samplesRemoved = 0;
        auto it = audioChunks.begin();

        while (it != audioChunks.end() && samplesRemoved < samplesToRemove) {
            int remainingSamplesToRemove = samplesToRemove - samplesRemoved;

            size_t chunkSize = it->sampleCount();
            if (chunkSize <= static_cast<size_t>(remainingSamplesToRemove)) {
                samplesRemoved += chunkSize;
                it = audioChunks.erase(it);
            }
            else {
                if (format == AudioSampleFormat::Float32) {
                    it->float32Data.erase(
                        it->float32Data.begin(),
                        it->float32Data.begin() + remainingSamplesToRemove);
                } else {
                    it->int16Data.erase(
                        it->int16Data.begin(),
                        it->int16Data.begin() + remainingSamplesToRemove);
                }
                it->timestamp += static_cast<Timestamp>(remainingSamplesToRemove * 1000 / sampleRate);
                break;
            }
        }
    }
    else {
        auto silenceDuration = -audioVideoDriftMs;
        int silenceFrames = static_cast<int>(std::round(silenceDuration * sampleRate / 1000));
        int silenceSamples = silenceFrames * numChannels;

        AudioChunk silenceChunk;
        silenceChunk.timestamp = videoTimestamp;
        silenceChunk.format = format;
        if (format == AudioSampleFormat::Float32) {
            silenceChunk.float32Data.resize(silenceSamples, 0.0f);
        } else {
            silenceChunk.int16Data.resize(silenceSamples, 0);
        }

        audioChunks.insert(audioChunks.begin(), std::move(silenceChunk));

        for (auto it = audioChunks.begin() + 1; it != audioChunks.end(); ++it) {
            it->timestamp += silenceDuration;
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
        srcWhiteLevel = statWhiteLevel;
        srcBlackLevel = statBlackLevel;
    } else {
        const size_t separatorPos = levels.find('/');
        if (separatorPos != std::string::npos) {
            try {            
                srcWhiteLevel = std::stof(levels.substr(0, separatorPos));
                const std::string blackLevels = levels.substr(separatorPos + 1);
                if (blackLevels.find(',') == std::string::npos) {
                    float blackLevelValue = std::stof(blackLevels);
                    srcBlackLevel = {blackLevelValue, blackLevelValue, blackLevelValue, blackLevelValue};
                } else {
                    std::stringstream values(blackLevels);
                    std::string value;
                    size_t channel = 0;
                    while (channel < srcBlackLevel.size() &&
                           std::getline(values, value, ',')) {
                        srcBlackLevel[channel++] = std::stof(value);
                    }
                    if (channel != srcBlackLevel.size() ||
                        std::getline(values, value, ',')) {
                        throw std::invalid_argument(
                            "Expected exactly four black-level values");
                    }
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
        useBits = std::clamp(useBits, 1, 16);
        dstWhiteLevel = std::pow(2.0f, useBits) - 1;
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
