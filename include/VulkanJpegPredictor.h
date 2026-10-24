#pragma once

#include <cstdint>
#include <vector>

namespace motioncam {
// Computes liblj92's sampled lossless-JPEG predictor histogram on Vulkan.
// Returns false when a compute device is unavailable or the input is too large.
bool jpegPredictorHistogramVulkan(const uint16_t* pixels, uint32_t width,
                                  uint32_t height, uint32_t channels,
                                  uint32_t bits, int& predictor,
                                  int histogram[18],
                                  std::vector<int16_t>* residuals = nullptr);
}
