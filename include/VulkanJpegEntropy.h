#pragma once

#include <cstdint>

namespace motioncam {
struct JpegEntropyImageSource {
    const uint16_t* pixels;
    uint32_t width, height, channels, bits, predictor;
    bool* gpuUsed = nullptr;
};

// liblj92 entropy-writer callback: zero means the GPU produced a complete,
// byte-stuffed JPEG scan. Null residuals select the resident image described
// by context; nonzero asks liblj92 to use its CPU writer.
int jpegEntropyVulkan(const int16_t* residuals, int sampleCount,
                      const uint16_t codes[18],
                      const uint16_t codeBits[18],
                      uint8_t* output, int capacity,
                      int* written, void* context);
}
