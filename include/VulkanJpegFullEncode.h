#pragma once
#include <cstddef>
#include <cstdint>
#include <functional>
#include <vector>

namespace motioncam {
// Returns a malloc-owned JPEG stream. A false result leaves the CPU codec as
// fallback.
bool jpegEncodeFullyVulkan(const uint16_t *pixels, uint32_t width,
                           uint32_t height, uint32_t channels, uint32_t bits,
                           uint8_t **encoded, int *encodedLength);
// Calls consume while the finished JPEG is available in the completed mapped
// staging buffer. The pointer is valid only for the duration of the callback.
using JpegOutputConsumer = std::function<bool(const uint8_t *, size_t)>;
bool jpegEncodeFullyVulkanBorrowed(const uint16_t *pixels, uint32_t width,
                                   uint32_t height, uint32_t channels,
                                   uint32_t bits,
                                   const JpegOutputConsumer &consume);
// Encodes a single-channel 16-bit libplacebo texture on its existing GPU.
// The caller keeps the texture and device alive through this call.
bool jpegEncodeFullyVulkanTexture(const void *gpu, const void *texture,
                                  uint32_t width, uint32_t height,
                                  uint8_t **encoded, int *encodedLength);
// RGB textures use 16-bit UNORM R/G/B components (RGB or RGBA storage).
// `bits` selects the 8..16-bit lossless JPEG sample precision.
bool jpegEncodeFullyVulkanTexture(const void *gpu, const void *texture,
                                  uint32_t width, uint32_t height,
                                  uint32_t channels, uint32_t bits,
                                  uint8_t **encoded, int *encodedLength);
bool jpegEncodeFullyVulkanTextureBorrowed(
    const void *gpu, const void *texture, uint32_t width, uint32_t height,
    uint32_t channels, uint32_t bits, const JpegOutputConsumer &consume);
// Encode independent lossless JPEG tiles from a resident 16-bit texture.
// The resulting JPEG streams are ordered left-to-right, top-to-bottom.
bool jpegEncodeTiledVulkanTexture(
    const void *gpu, const void *texture, uint32_t width, uint32_t height,
    uint32_t channels, uint32_t tileWidth, uint32_t tileHeight,
    std::vector<std::vector<uint8_t>>& tiles);
struct VulkanJpegTiledPayload {
    // Each offset/length selects one complete JPEG. bytes may contain padding
    // between tiles to preserve GPU word alignment.
    std::vector<uint8_t> bytes;
    std::vector<uint32_t> offsets;
    std::vector<uint32_t> lengths;
};
// The callback receives complete JPEGs in fixed-capacity GPU slots. Tile body
// lengths remain on the GPU; no JPEG bytes are downloaded by this API.
using JpegGpuTiledConsumer = std::function<bool(
    const void* gpu, const void* jpegBuffer,
    const void* bodyLengthBuffer, const void* tableBuffer,
    uint32_t width, uint32_t height, uint32_t channels,
    uint32_t tileWidth, uint32_t tileHeight, uint32_t tileCount,
    uint32_t slotWords)>;
bool jpegEncodeTiledVulkanTextureResident(
    const void* gpu, const void* texture, uint32_t width, uint32_t height,
    uint32_t channels, uint32_t tileWidth, uint32_t tileHeight,
    const JpegGpuTiledConsumer& consume);
bool jpegEncodeTiledVulkanTexture(
    const void *gpu, const void *texture, uint32_t width, uint32_t height,
    uint32_t channels, uint32_t tileWidth, uint32_t tileHeight,
    VulkanJpegTiledPayload& payload);
// Source is a row-aligned, 8..16-bit packed GPU buffer from the gain/LOG bake.
bool jpegEncodeTiledVulkanPackedBuffer(
    const void* gpu, const void* buffer, uint32_t width, uint32_t height,
    uint32_t channels, uint32_t bits, uint32_t wordsPerRow,
    uint32_t tileWidth, uint32_t tileHeight,
    std::vector<std::vector<uint8_t>>& tiles);
// `buffer` contains DNG-packed samples in row-aligned 32-bit GPU words.
// The buffer and device must remain valid until consume returns.
bool jpegEncodeFullyVulkanPackedBufferBorrowed(
    const void *gpu, const void *buffer, uint32_t width, uint32_t height,
    uint32_t channels, uint32_t bits, uint32_t wordsPerRow,
    const JpegOutputConsumer &consume,
    const void *validationBuffer = nullptr);
void releaseVulkanJpegFullEncodeDevice(const void *gpu);
} // namespace motioncam
