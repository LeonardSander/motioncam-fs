#pragma once

#include <array>
#include <cstdint>
#include <functional>
#include <memory>
#include <vector>
#include <string>

namespace motioncam {
struct DNGProfileTable;
struct VulkanGalleryBayerGainMap {
    uint32_t width = 0, height = 0, channels = 0;
    uint32_t left = 0, top = 0, right = 0, bottom = 0;
    uint32_t rowPitch = 1, colPitch = 1;
    float originH = 0, originV = 0, spacingH = 0, spacingV = 0;
    std::vector<float> values;
};
struct VulkanGalleryBayerGain {
    std::vector<VulkanGalleryBayerGainMap> maps;
    std::array<float, 4> black{}, scale{};
    std::array<uint8_t, 4> phase{0, 1, 1, 2};
    uint32_t encodedWhite = 0, ditherSeed = 0;
    bool highQualityDither = false, debugGain = false;
    std::vector<uint16_t> sourceLinearization;
};
struct VulkanGalleryGpuBuffer {
    int fd = -1;
    int readyFenceFd = -1;
    bool externalImageOwnership = false;
    bool opaqueFd = false;
    int sourceFrameIndex = -1;
    uint64_t debugSampleHash = 0;
    size_t bytes = 0;
    size_t allocationBytes = 0;
    size_t allocationOffset = 0;
    uint64_t drmFormatModifier = 0;
    uint32_t width = 0, height = 0;
    int orientation = 0;
    bool rgb10a2 = false;
    bool vulkanTexture = false;
    bool staticFrame = false;
    // Keep the source device alive until its exported resources are destroyed.
    std::shared_ptr<void> sourceLease;
    std::shared_ptr<void> resourceLease;
    // Atomically disable rejected image sharing; true only for the first rejection.
    std::function<bool()> rejectSharedImage;
    std::function<void()> release;
    ~VulkanGalleryGpuBuffer();
};
struct VulkanGalleryBayerSource {
    const uint16_t* samples = nullptr;
    const void* gpu = nullptr;
    const void* buffer = nullptr;
    const void* texture = nullptr;
    std::function<bool(std::vector<uint16_t>&)> download;
    uint32_t width = 0, height = 0;
    std::array<uint8_t, 4> phase{0, 1, 1, 2};
    std::array<double, 3> black{}, white{};
    const VulkanGalleryBayerGain* gain = nullptr;
    std::function<bool(std::vector<uint16_t>&)> recoverCpu;
};
struct VulkanGalleryOutput {
    uint32_t width = 0, height = 0;
    int orientation = 0;
    bool rgb24 = false;
    bool rgb10a2 = false;
    // Diagnostic contract: resident input and shared-image presentation only.
    bool requireZeroCopy = false;
    const std::vector<uint8_t>* clipping = nullptr;
    std::shared_ptr<const uint8_t>* hostPixels = nullptr;
    std::shared_ptr<VulkanGalleryGpuBuffer>* gpuBuffer = nullptr;
    std::vector<uint8_t>* thumbnailPixels = nullptr;
    uint32_t thumbnailWidth = 0, thumbnailHeight = 0;
    int thumbnailOrientation = 0;
    // Changes on seek or clip/settings replacement; queued results from older
    // generations must not populate the current thumbnail cache.
    uint64_t thumbnailGeneration = 0;
    int thumbnailSourceFrame = -1;
    int* completedThumbnailSourceFrame = nullptr;
};
struct VulkanGalleryTextureSource {
    const void* gpu = nullptr;
    const void* texture = nullptr;
    const void* buffer = nullptr;
    uint32_t width = 0, height = 0;
    std::array<double, 3> black{}, white{};
    const struct VulkanGalleryGain* gain = nullptr;
};
struct VulkanGalleryGain {
    uint32_t width = 0, height = 0;
    uint32_t left = 0, top = 0, right = 0, bottom = 0;
    float originH = 0, originV = 0, spacingH = 0, spacingV = 0;
    std::vector<float> values;
    uint32_t secondaryWidth = 0, secondaryHeight = 0;
    uint32_t secondaryLeft = 0, secondaryTop = 0;
    uint32_t secondaryRight = 0, secondaryBottom = 0;
    float secondaryOriginH = 0, secondaryOriginV = 0;
    float secondarySpacingH = 0, secondarySpacingV = 0;
    std::vector<float> secondaryValues;
    uint32_t encodedWhite = 0;
    uint32_t ditherSeed = 0;
    bool highQualityDither = false;
    bool debugGainMap = false;
};

// Reconstruct and normalize the ordinary 2x2 Bayer gallery preview. Returns
// false when Vulkan is unavailable so callers can use the CPU implementation.
#ifdef MOTIONCAM_HAS_DIRECTLOG_PLACEBO
void warmVulkanGalleryPreview(const std::string& cacheDirectory = {});
bool vulkanGalleryPreviewReady();
bool waitVulkanGalleryPreviewReady();
void releaseVulkanGalleryTextureDevice(const void* gpu);
bool demosaicGalleryBayerVulkan(
    const uint16_t* cfa, std::vector<uint8_t>& rgb,
    uint32_t width, uint32_t height,
    const std::array<uint8_t, 4>& phase,
    const std::array<double, 3>& black,
    const std::array<double, 3>& white);
bool colorGalleryRgbVulkan(
    std::vector<uint8_t>& rgb,
    const std::array<float, 3>& gains,
    const std::array<float, 9>& cameraToDisplay,
    const std::array<float, 9>& cameraToXyz,
    float exposure, const DNGProfileTable* hueSat,
    const DNGProfileTable* look,
    bool useMatrix, bool gainMapOnlyDebug,
    const VulkanGalleryBayerSource* bayer = nullptr,
    const VulkanGalleryOutput* output = nullptr,
    const VulkanGalleryTextureSource* texture = nullptr);
#else
inline void warmVulkanGalleryPreview(const std::string& = {}) {}
inline bool vulkanGalleryPreviewReady() { return false; }
inline bool waitVulkanGalleryPreviewReady() { return false; }
inline void releaseVulkanGalleryTextureDevice(const void*) {}
inline bool demosaicGalleryBayerVulkan(
    const uint16_t*, std::vector<uint8_t>&, uint32_t, uint32_t,
    const std::array<uint8_t, 4>&,
    const std::array<double, 3>&,
    const std::array<double, 3>&) { return false; }
inline bool colorGalleryRgbVulkan(
    std::vector<uint8_t>&, const std::array<float, 3>&,
    const std::array<float, 9>&, const std::array<float, 9>&,
    float, const DNGProfileTable*, const DNGProfileTable*,
    bool, bool, const VulkanGalleryBayerSource* = nullptr,
    const VulkanGalleryOutput* = nullptr,
    const VulkanGalleryTextureSource* = nullptr) { return false; }
#endif

} // namespace motioncam
