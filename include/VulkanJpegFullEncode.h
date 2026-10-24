#pragma once
#include <cstddef>
#include <cstdint>
#include <functional>

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
// `buffer` contains DNG-packed samples in row-aligned 32-bit GPU words.
// The buffer and device must remain valid until consume returns.
bool jpegEncodeFullyVulkanPackedBufferBorrowed(
    const void *gpu, const void *buffer, uint32_t width, uint32_t height,
    uint32_t channels, uint32_t bits, uint32_t wordsPerRow,
    const JpegOutputConsumer &consume);
void releaseVulkanJpegFullEncodeDevice(const void *gpu);
} // namespace motioncam
