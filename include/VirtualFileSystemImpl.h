#pragma once

#include <string>
#include <vector>
#include <cstdint>
#include "Types.h"

namespace motioncam {

struct AudioChunk;

struct FrameRateInfo {
    float minFrameRate;
    float lowerQuartileFrameRate;  // 25th percentile
    float medianFrameRate;         // 50th percentile
    float upperQuartileFrameRate;  // 75th percentile
    float maxFrameRate;
    float averageFrameRate;
};

struct FileInfo {
    FrameRateInfo frameRateInfo;
    float fps;
    int totalFrames;
    int droppedFrames;
    int duplicatedFrames;
    int width;
    int height;
    std::string dataType;        // "Bayer CFA", "Quad Bayer CFA", or "RGB"
    std::string levelsInfo;      // e.g., "1023/64 -> 1023/0 RAW10"
    float runtimeSeconds;        // Runtime in seconds based on audio track
};

namespace vfs {

FrameRateInfo calculateFrameRate(const std::vector<Timestamp>& frames);

// CFR (Constant Frame Rate) conversion
float determineCFRTarget(
    FrameRateInfo fpsInfo,
    const CFRTarget& cfrTarget,
    bool applyCFRConversion);

int getFrameNumberFromTimestamp(
    Timestamp timestamp,
    Timestamp referenceTimestamp,
    float frameRate);

// File naming utilities
std::string constructFrameFilename(
    const std::string& baseName,
    int frameNumber,
    int padding = 6,
    const std::string& extension = "");

std::string extractFilenameWithoutExtension(const std::string& fullPath);

// Render options utilities
int getScaleFromOptions(FileRenderOptions options, int draftScale);

// Platform-specific constants
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

// Audio synchronization
void syncAudio(
    Timestamp videoTimestamp,
    std::vector<AudioChunk>& audioChunks,
    int sampleRate,
    int numChannels);

std::string getDisplayDataType(
    bool directLogRGB, bool quadBayerCapture, bool interpretAsQuad, bool remosaic);

std::string getDisplayDataLevels(
    float dynWhiteLevel, std::array<float, 4> dynBlackLevel, 
    float statWhiteLevel, std::array<float, 4> statBlackLevel, 
    std::string levels, std::string logTransform,
    bool applyShadingMap, bool normalizeShadingMap);

} // namespace vfs
} // namespace motioncam
