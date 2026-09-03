#include "Utils.h"

#include <array>
#include <cassert>
#include <cmath>
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
    motioncam::utils::demosaicHigherCFA(
        mosaic, rgb, width, height, 2, phase, motioncam::QuadBayerMode::Demosaic);
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
    motioncam::utils::demosaicHigherCFA(
        mosaic, rgb, width, height, 2, phase, motioncam::QuadBayerMode::Demosaic);
    assert(rgb[detailPixel * 3] == 4095);
    // Detail is reconstructed through the brightness estimate, not left only
    // in the native red plane as colour-difference demosaic would do.
    assert(rgb[detailPixel * 3 + 1] > flatColor[1] * 3);
    assert(rgb[detailPixel * 3 + 2] > flatColor[2] * 3);

    // Demosaic (Color) uses confidence-limited colour and luma detail on quad
    // CFA instead of forcing every uncertain native residual back into RGB.
    constexpr int quadWidth = 32, quadHeight = 32;
    std::vector<uint16_t> bandMosaic(quadWidth * quadHeight);
    for (int y = 0; y < quadHeight; ++y) for (int x = 0; x < quadWidth; ++x) {
        const double wave = std::sin(2.0 * 3.14159265358979323846 * (x + y) / 6.0);
        bandMosaic[static_cast<size_t>(y) * quadWidth + x] =
            static_cast<uint16_t>(2000.0 + 900.0 * wave);
    }
    std::vector<uint16_t> regularBand, colorBand;
    motioncam::utils::demosaicHigherCFA(
        bandMosaic, regularBand, quadWidth, quadHeight, 4, phase,
        motioncam::QuadBayerMode::Demosaic);
    motioncam::utils::demosaicHigherCFA(
        bandMosaic, colorBand, quadWidth, quadHeight, 4, phase,
        motioncam::QuadBayerMode::DemosaicColor);
    assert(colorBand.size() == regularBand.size());
    bool differsFromRegular = false;
    for (int y = 0; y < quadHeight; ++y) for (int x = 0; x < quadWidth; ++x) {
        const size_t pixel = static_cast<size_t>(y) * quadWidth + x;
        for (int channel = 0; channel < 3; ++channel)
            differsFromRegular |= colorBand[pixel * 3 + channel] !=
                                  regularBand[pixel * 3 + channel];
    }
    assert(differsFromRegular);

    // Color detail gain must not scale the encoded black pedestal. A frame at
    // per-channel black remains at those RGB black levels after demosaic.
    const std::array<float, 3> channelBlack = {64.0f, 96.0f, 128.0f};
    std::vector<uint16_t> blackQuad(quadWidth * quadHeight);
    for (int y = 0; y < quadHeight; ++y) for (int x = 0; x < quadWidth; ++x) {
        const int native = phase[((y / 2) & 1) * 2 + ((x / 2) & 1)];
        blackQuad[static_cast<size_t>(y) * quadWidth + x] =
            static_cast<uint16_t>(channelBlack[native]);
    }
    motioncam::utils::demosaicHigherCFA(
        blackQuad, colorBand, quadWidth, quadHeight, 4, phase,
        motioncam::QuadBayerMode::DemosaicColor, channelBlack);
    for (int y = 4; y < quadHeight - 4; ++y) for (int x = 4; x < quadWidth - 4; ++x) {
        const size_t pixel = static_cast<size_t>(y) * quadWidth + x;
        for (int channel = 0; channel < 3; ++channel)
            assert(colorBand[pixel * 3 + channel] == channelBlack[channel]);
    }

    // Partial groups at the right and bottom edges must not contribute
    // synthetic zero-valued phase errors to neighbouring complete groups.
    constexpr int croppedWidth = 31, croppedHeight = 29;
    std::vector<uint16_t> croppedBlack(croppedWidth * croppedHeight);
    for (int y = 0; y < croppedHeight; ++y) for (int x = 0; x < croppedWidth; ++x) {
        const int native = phase[((y / 2) & 1) * 2 + ((x / 2) & 1)];
        croppedBlack[static_cast<size_t>(y) * croppedWidth + x] =
            static_cast<uint16_t>(channelBlack[native]);
    }
    motioncam::utils::demosaicHigherCFA(
        croppedBlack, colorBand, croppedWidth, croppedHeight, 4, phase,
        motioncam::QuadBayerMode::DemosaicColor, channelBlack);
    assert(colorBand.size() == croppedBlack.size() * 3);
    for (int y = 4; y < croppedHeight; ++y) for (int x = 4; x < croppedWidth; ++x) {
        const size_t pixel = static_cast<size_t>(y) * croppedWidth + x;
        for (int channel = 0; channel < 3; ++channel)
            assert(colorBand[pixel * 3 + channel] == channelBlack[channel]);
    }

    std::vector<uint16_t> gradient(8 * 8 * 3);
    for (size_t i = 0; i < gradient.size(); ++i) gradient[i] = static_cast<uint16_t>(i);
    std::vector<uint16_t> reduced;
    uint32_t reducedWidth = 0, reducedHeight = 0;
    motioncam::utils::reduceRGB(gradient, reduced, 8, 8, 2, true,
                                reducedWidth, reducedHeight);
    assert(reducedWidth == 4 && reducedHeight == 4 && reduced.size() == 4 * 4 * 3);
    assert(reduced[0] == (gradient[0] + gradient[3] + gradient[24] + gradient[27] + 2) / 4);
    motioncam::utils::reduceRGB(gradient, reduced, 8, 8, 2, false,
                                reducedWidth, reducedHeight);
    assert(reduced[0] == gradient[0]);

    std::vector<uint16_t> rectangular(16 * 12 * 3, 100);
    motioncam::utils::reduceRGB(rectangular, reduced, 16, 12, 2, true,
                                reducedWidth, reducedHeight);
    assert(reducedWidth == 8 && reducedHeight == 6);

    // Log-aware HQ reduction must average in linear light and only then
    // encode the averaged result again.
    std::vector<uint16_t> encoded(8 * 8 * 3, 0);
    for (int y = 0; y < 8; ++y) for (int x = 0; x < 8; ++x)
        for (int c = 0; c < 3; ++c)
            encoded[(y * 8 + x) * 3 + c] = (x & 1) ? 4095 : 0;
    motioncam::utils::reduceRGB(encoded, reduced, 8, 8, 2, true,
                                reducedWidth, reducedHeight, 4095);
    const auto expectedLogMidpoint = static_cast<uint16_t>(std::lround(
        std::log2(31.0) / std::log2(61.0) * 4095.0));
    assert(reduced[0] == expectedLogMidpoint);

    std::vector<uint16_t> quad(8 * 8);
    for (int y = 0; y < 8; ++y) for (int x = 0; x < 8; ++x)
        quad[y * 8 + x] = static_cast<uint16_t>((y / 2) * 100 + (x / 2) * 10 +
                                                (y & 1) * 2 + (x & 1));
    std::vector<uint16_t> binned;
    motioncam::utils::binQuadBayer(quad, binned, 8, 8,
                                   reducedWidth, reducedHeight);
    assert(reducedWidth == 4 && reducedHeight == 4);
    assert(binned[0] == 2); // Rounded average of 0, 1, 2, and 3.

    std::vector<uint16_t> sixBySix(12 * 12);
    for (size_t i = 0; i < sixBySix.size(); ++i)
        sixBySix[i] = static_cast<uint16_t>(i);
    motioncam::utils::binHigherCFA(sixBySix, binned, 12, 12, 3,
                                   reducedWidth, reducedHeight);
    assert(reducedWidth == 4 && reducedHeight == 4);
    assert(binned[0] == (0 + 1 + 2 + 12 + 13 + 14 + 24 + 25 + 26 + 4) / 9);

    std::vector<uint16_t> eightByEight(16 * 16, 400);
    motioncam::utils::binHigherCFA(eightByEight, binned, 16, 16, 4,
                                   reducedWidth, reducedHeight);
    assert(reducedWidth == 4 && reducedHeight == 4 && binned[0] == 400);
    motioncam::utils::binHigherCFA(eightByEight, binned, 16, 16, 2,
                                   reducedWidth, reducedHeight);
    assert(reducedWidth == 8 && reducedHeight == 8 && binned[0] == 400);
    return 0;
}
