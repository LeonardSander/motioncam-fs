#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <vector>

namespace motioncam {
struct VulkanRgbGainMap {
    uint32_t width = 0, height = 0, channels = 0;
    uint32_t left = 0, top = 0, right = 0, bottom = 0;
    uint32_t rowPitch = 1, colPitch = 1;
    float originH = 0, originV = 0, spacingH = 0, spacingV = 0;
    const std::vector<float>* data = nullptr;
};
// An image produced by DirectLog's Vulkan renderer. The renderer keeps the
// texture alive until the bake finishes; download is used only for CPU fallback.
struct VulkanGainBakeTextureSource {
    const void* gpu = nullptr;
    const void* texture = nullptr;
    std::function<bool(uint16_t*, size_t)> download;
    std::function<bool(uint16_t*, size_t)> recoverCpu;
};
struct VulkanGainBakeBufferSource {
    const void* gpu = nullptr;
    const void* buffer = nullptr;
};
struct VulkanPackedImage {
    const void* gpu = nullptr;
    const void* buffer = nullptr;
    uint32_t width = 0, height = 0, channels = 0, bits = 0;
    uint32_t wordsPerRow = 0;
};
struct VulkanGainBakeParams {
    std::array<float, 4> black{}, destinationBlack{}, scale{};
    uint32_t width = 0, height = 0, channels = 0, outputBits = 16;
    uint32_t sourceNominalWhite = 65535;
    float destinationWhite = 65535.0f;
    uint32_t encodedWhite = 0, ditherSeed = 0;
    bool log60 = false, logHighQuality = false, debugGain = false;
    const std::vector<uint16_t>* linearization = nullptr;
    const std::vector<float>* logLookup = nullptr;
    const VulkanGainBakeTextureSource* textureSource = nullptr;
    const VulkanGainBakeBufferSource* bufferSource = nullptr;
    // Called while the packed GPU output remains live. A true result means
    // the caller has consumed it and no uncompressed readback is needed.
    std::function<bool(const VulkanPackedImage&)> consumePackedOutput;
    VulkanRgbGainMap map;
    std::array<VulkanRgbGainMap, 4> cfaMaps{};
    uint32_t cfaMapCount = 0;
    std::vector<VulkanRgbGainMap> maps;
    std::array<uint8_t, 4> cfaPhase{0, 1, 1, 2};
};
// Begin Vulkan device and shader setup while the source prepares its first frame.
void warmVulkanGainBake();
void releaseVulkanGainBakeTextureDevice(const void* gpu);
void releaseVulkanGainBakeBufferDevice(const void* gpu);
// Source is an unpacked 16-bit DNG strip. Output is packed per DNG row.
// On failure the caller must restore a texture-backed source before CPU bake;
// readback may have written part of the destination.
bool bakeRgbGainVulkan(const uint16_t* source,
                       const VulkanGainBakeParams& params,
                       uint8_t* destination, size_t destinationBytes);
bool bakeCfaGainVulkan(const uint16_t* source,
                       const VulkanGainBakeParams& params,
                       uint8_t* destination, size_t destinationBytes);
}
