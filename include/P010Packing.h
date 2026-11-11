#pragma once
#include <cstdint>
#include <cstring>
#include <limits>
#include <stdexcept>
extern "C" {
#include <libavutil/frame.h>
#include <libavutil/pixfmt.h>
}
namespace rawpreamp {
// Preserve P010's ten most significant bits while adapting FFmpeg's separate
// planes to Android's contiguous, possibly padded input allocation.
inline size_t packP010Input(const AVFrame& frame, uint8_t* destination, size_t capacity,
                           int strideBytes, int sliceHeight) {
    if (frame.format != AV_PIX_FMT_P010LE || frame.width <= 0 || frame.height <= 0 ||
        frame.width % 2 || frame.height % 2 || frame.width > std::numeric_limits<int>::max() / 2 ||
        strideBytes < frame.width * 2 || strideBytes % 2 || sliceHeight < frame.height || sliceHeight % 2 ||
        !frame.data[0] || !frame.data[1] || frame.linesize[0] < frame.width * 2 || frame.linesize[1] < frame.width * 2)
        throw std::runtime_error("Unsupported P010 input dimensions, stride, or slice height");
    const uint64_t yBytes = uint64_t(strideBytes) * sliceHeight;
    const uint64_t bytes = yBytes + yBytes / 2;
    if (!destination || bytes > capacity)
        throw std::runtime_error("P010 hardware input buffer is too small");
    std::memset(destination, 0, static_cast<size_t>(yBytes));
    // Full-range luma padding is black; chroma padding is neutral 512 << 6.
    for (size_t i = static_cast<size_t>(yBytes); i < bytes; i += 2) {
        destination[i] = 0; destination[i + 1] = 128;
    }
    const size_t rowBytes = static_cast<size_t>(frame.width) * 2;
    for (int y = 0; y < frame.height; ++y)
        std::memcpy(destination + static_cast<size_t>(y) * strideBytes,
                    frame.data[0] + static_cast<size_t>(y) * frame.linesize[0], rowBytes);
    for (int y = 0; y < frame.height / 2; ++y)
        std::memcpy(destination + static_cast<size_t>(yBytes) + static_cast<size_t>(y) * strideBytes,
                    frame.data[1] + static_cast<size_t>(y) * frame.linesize[1], rowBytes);
    return static_cast<size_t>(bytes);
}
}
