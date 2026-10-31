#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <vector>

namespace motioncam {

struct VulkanJpegTile {
    const uint8_t* bytes = nullptr;
    size_t size = 0;
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t bits = 0;
    uint32_t channels = 1;
};
struct VulkanJpegContiguousSource;

// Decode independent one- or three-component lossless JPEG tiles. This variant
// reads pixels back to the CPU and is used for suitable tiled DNG imports. The
// resident variant below can feed Vulkan processing without that copy.
bool jpegDecodeTilesVulkan(const std::vector<VulkanJpegTile>& tiles,
                           std::vector<std::vector<uint16_t>>& pixels,
                           const VulkanJpegContiguousSource* contiguous = nullptr);

struct VulkanDecodedJpegImage {
    const void* gpu = nullptr;
    const void* buffer = nullptr;
    // GPU word containing 1 only if every tile decoded successfully. Pass it
    // to the terminal JPEG encoder for validation at the final readback.
    const void* validationBuffer = nullptr;
    uint32_t width = 0, height = 0, channels = 0, bits = 0, wordsPerRow = 0;
    // Optional ownership transferred from the decoder for deferred consumers.
    std::shared_ptr<void> lease;
};
struct VulkanJpegContiguousSource {
    const uint8_t* bytes = nullptr;
    size_t size = 0;
};

// Full, even-sized CFA or RGB tiles in row-major order. When contiguous is
// supplied, every tile byte range must point into that source buffer.
// The callback receives a
// row-aligned 16-bit image buffer while the decoder owns it. It may run Vulkan
// processing and JPEG encoding synchronously without an intermediate readback.
// Any cached processing device using this GPU must be released before its
// Vulkan context is destroyed.
bool jpegDecodeTiledImageVulkan(
    const std::vector<VulkanJpegTile>& tiles, uint32_t width, uint32_t height,
    uint32_t tileWidth, uint32_t tileHeight,
    const std::function<bool(const VulkanDecodedJpegImage&)>& consume,
    const VulkanJpegContiguousSource* contiguous = nullptr);
// Retains the assembled allocation without copying pixels. Reads only the
// validation word before returning; deferred consumers own the output lease.
bool jpegDecodeTiledImageVulkanRetained(
    const std::vector<VulkanJpegTile>& tiles, uint32_t width, uint32_t height,
    uint32_t tileWidth, uint32_t tileHeight, VulkanDecodedJpegImage& image,
    const VulkanJpegContiguousSource* contiguous = nullptr);
bool downloadVulkanDecodedJpegImage(const VulkanDecodedJpegImage& image,
                                  uint16_t* pixels, size_t samples);
// Waits for decode and tile assembly, then reads only the GPU validation word.
// Intended for decode-only timing without downloading pixels or re-encoding.
bool jpegDecodeTiledImageVulkanValidated(
    const std::vector<VulkanJpegTile>& tiles, uint32_t width, uint32_t height,
    uint32_t tileWidth, uint32_t tileHeight,
    const VulkanJpegContiguousSource* contiguous = nullptr);

// Consume a resident JPEG tile stream produced on the same GPU. Each complete
// JPEG occupies one fixed-capacity slot; lengths and the shared Huffman table
// stay resident.
// Release the cached decode state before destroying the GPU device.
bool jpegDecodeResidentTiledImageVulkan(
    const void* gpu, const void* jpegBuffer,
    const void* bodyLengthsBuffer, const void* tableBuffer,
    uint32_t width, uint32_t height, uint32_t channels,
    uint32_t tileWidth, uint32_t tileHeight, uint32_t tileCount,
    uint32_t slotWords,
    const std::function<bool(const VulkanDecodedJpegImage&)>& consume);
void releaseVulkanJpegDecodeDevice(const void* gpu);

} // namespace motioncam
