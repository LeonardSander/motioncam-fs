#pragma once

#include <string>
#include <vector>
#include <cstdint>
#include "Types.h"

namespace motioncam {

// Forward declare AudioChunk from Types.h
using AudioChunk = std::pair<Timestamp, std::vector<int16_t>>;

namespace vfs {

// Frame rate calculation
struct FrameRateInfo {
    float medianFrameRate;
    float averageFrameRate;
};

FrameRateInfo calculateFrameRate(const std::vector<Timestamp>& frames);

// CFR (Constant Frame Rate) conversion
float determineCFRTarget(
    float medianFps,
    const std::string& cfrTarget,
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

} // namespace vfs
} // namespace motioncam
