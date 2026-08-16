#include "Utils.h"

#include <array>
#include <cassert>
#include <cstdint>
#include <vector>

int main() {
    constexpr int width = 12;
    constexpr int height = 10;
    const std::array<uint8_t, 4> phase = {0, 1, 1, 2}; // RGGB
    const std::array<uint16_t, 3> flatColor = {1000, 2000, 3000};
    std::vector<uint16_t> mosaic(width * height);
    for (int y = 0; y < height; ++y) for (int x = 0; x < width; ++x)
        mosaic[static_cast<size_t>(y) * width + x] = flatColor[phase[(y & 1) * 2 + (x & 1)]];

    std::vector<uint16_t> rgb;
    motioncam::utils::demosaicHigherCFA(mosaic, rgb, width, height, 2, phase, false);
    assert(rgb.size() == mosaic.size() * 3);
    for (int y = 1; y + 1 < height; ++y) for (int x = 1; x + 1 < width; ++x) {
        const size_t pixel = static_cast<size_t>(y) * width + x;
        for (int channel = 0; channel < 3; ++channel)
            assert(rgb[pixel * 3 + channel] == flatColor[channel]);
        const int native = phase[(y & 1) * 2 + (x & 1)];
        assert(rgb[pixel * 3 + native] == mosaic[pixel]);
    }

    // A high-frequency native sample must survive as brightness detail.
    const size_t detailPixel = static_cast<size_t>(4) * width + 4;
    mosaic[detailPixel] = 4095;
    motioncam::utils::demosaicHigherCFA(mosaic, rgb, width, height, 2, phase, false);
    assert(rgb[detailPixel * 3] == 4095);
    // Detail is reconstructed through the brightness estimate, not left only
    // in the native red plane as colour-difference demosaic would do.
    assert(rgb[detailPixel * 3 + 1] > flatColor[1] * 3);
    assert(rgb[detailPixel * 3 + 2] > flatColor[2] * 3);
    return 0;
}
