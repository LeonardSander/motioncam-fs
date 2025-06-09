#pragma once

#include <vector>
#include <memory>

#include "Types.h"

namespace motioncam {

struct CameraFrameMetadata;
struct CameraConfiguration;

namespace utils {

std::shared_ptr<std::vector<char>> generateDng(
    std::vector<uint8_t>& data,
    const CameraFrameMetadata& metadata,
    const CameraConfiguration& cameraConfiguration,
    float recordingFps,
    int frameNumber,
    FileRenderOptions options,
    int scale=1,
    double baselineExpValue=0.0);

std::pair<int, int> toFraction(float frameRate, int base = 1000);

} // namespace utils
} // namespace motioncam
