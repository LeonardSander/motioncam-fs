#pragma once

#ifdef MOTIONCAM_HAS_DIRECTLOG_PLACEBO
#include <libplacebo/vulkan.h>
#include <cstddef>
#include <cstdint>
#include <mutex>

namespace motioncam {
// Shared by the lossless JPEG predictor and entropy passes. Both callers hold
// this mutex while issuing commands on the libplacebo GPU.
struct VulkanJpegContext {
    std::mutex mutex;
    pl_log log = nullptr;
    pl_vulkan vulkan = nullptr;
    bool unavailable = false;
    ~VulkanJpegContext();
    bool init();
};
VulkanJpegContext& jpegContext();
// Call only while holding jpegContext().mutex. The buffer remains valid until
// the next predictor scan; a source pointer mismatch signals that it changed.
pl_buf jpegResidentImageVulkan(const uint16_t* pixels, uint32_t width,
                               uint32_t height, uint32_t channels,
                               uint32_t bits);
}
#endif
