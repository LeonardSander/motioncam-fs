#include "Utils.h"
#include "GainMapBake.h"
#include "DNGImage.h"

#include <array>
#include <cassert>
#include <cmath>
#include <cstdint>
#include <vector>

int main() {
    const auto rggb = motioncam::cfaColorsFromPhase("rggb");
    assert(motioncam::cfaColorAt(rggb, 0, 0) == 0);
    assert(motioncam::cfaColorAt(rggb, 1, 0) == 1);
    assert(motioncam::cfaColorAt(rggb, 0, 1) == 1);
    assert(motioncam::cfaColorAt(rggb, 1, 1) == 2);

    std::vector<motioncam::GainMap> cfaMaps(4);
    const std::array<float, 4> phaseGains{2.0f, 3.0f, 5.0f, 7.0f};
    for (size_t phaseIndex = 0; phaseIndex < cfaMaps.size(); ++phaseIndex) {
        auto& map = cfaMaps[phaseIndex];
        map.top = phaseIndex / 2;
        map.left = phaseIndex % 2;
        map.bottom = 8;
        map.right = 8;
        map.coordinateWidth = 8;
        map.coordinateHeight = 8;
        map.plane = 0;
        map.planes = 1;
        map.rowPitch = map.colPitch = 2;
        map.width = map.height = map.channels = 1;
        map.spacingV = map.spacingH = 1.0;
        map.originV = map.originH = 0.0;
        map.data = {phaseGains[phaseIndex]};
    }
    auto roundTrippedCfaMaps = cfaMaps;
    roundTrippedCfaMaps[2].originV += 2e-17;
    roundTrippedCfaMaps[3].originV += 2e-17;
    roundTrippedCfaMaps[2].spacingV += 2e-17;
    roundTrippedCfaMaps[3].spacingV += 2e-17;
    const auto roundTrippedSeparation =
        motioncam::separateGainMapLuminance(roundTrippedCfaMaps);
    assert(roundTrippedSeparation.valid);

    motioncam::GainMap luminance = cfaMaps.front();
    luminance.top = luminance.left = 0;
    luminance.rowPitch = luminance.colPitch = 1;
    luminance.data = {11.0f};
    cfaMaps.push_back(luminance);
    const auto rgbMaps = motioncam::collapseCfaGainMapsForRgb(cfaMaps, rggb);
    assert(rgbMaps.size() == 2);
    assert(rgbMaps[0].channels == 3);
    assert((rgbMaps[0].data == std::vector<float>{2.0f, 4.0f, 7.0f}));
    assert(rgbMaps[1].channels == 1 && rgbMaps[1].data[0] == 11.0f);

    auto shiftedCfaMaps = std::vector<motioncam::GainMap>(cfaMaps.begin(), cfaMaps.begin() + 4);
    shiftedCfaMaps[1].originH += shiftedCfaMaps[1].spacingH * 0.5;
    shiftedCfaMaps[2].originV += shiftedCfaMaps[2].spacingV * 0.5;
    shiftedCfaMaps[3].originH += shiftedCfaMaps[3].spacingH * 0.5;
    shiftedCfaMaps[3].originV += shiftedCfaMaps[3].spacingV * 0.5;
    const auto shiftedRgbMaps = motioncam::collapseCfaGainMapsForRgb(
        shiftedCfaMaps, rggb);
    assert(shiftedRgbMaps.size() == 1 && shiftedRgbMaps[0].channels == 3);
    assert((shiftedRgbMaps[0].data == std::vector<float>{2.0f, 4.0f, 7.0f}));
    assert(shiftedRgbMaps[0].originH == 0.0);
    assert(shiftedRgbMaps[0].originV == 0.0);

    motioncam::GainMap rgbMap = cfaMaps.front();
    rgbMap.top = rgbMap.left = 0;
    rgbMap.rowPitch = rgbMap.colPitch = 1;
    rgbMap.channels = 3;
    rgbMap.data = {2.0f, 4.0f, 7.0f};
    auto separatedRgbMaps = std::vector<motioncam::GainMap>{rgbMap};
    const auto rgbSeparation =
        motioncam::separateGainMapLuminance(separatedRgbMaps);
    assert(rgbSeparation.valid && rgbSeparation.colorSeparated);
    assert(rgbSeparation.luminance.channels == 1);
    assert(rgbSeparation.luminance.data[0] == 2.0f);
    assert((separatedRgbMaps[0].data == std::vector<float>{1.0f, 2.0f, 3.5f}));
    const auto cfaRgbPlanes = motioncam::expandGainMapsForCfa(
        std::vector<motioncam::GainMap>{rgbMap}, rggb);
    assert(cfaRgbPlanes.size() == 4);
    assert(cfaRgbPlanes[0][0] == 2.0f);
    assert(cfaRgbPlanes[1][0] == 4.0f);
    assert(cfaRgbPlanes[2][0] == 4.0f);
    assert(cfaRgbPlanes[3][0] == 7.0f);
    const auto cfaLumaPlanes = motioncam::expandGainMapsForCfa(
        std::vector<motioncam::GainMap>{luminance}, rggb);
    for (const auto& plane : cfaLumaPlanes)
        assert(plane[0] == 11.0f);
    motioncam::GainMap lonePhase = luminance;
    lonePhase.top = 1;
    lonePhase.left = 0;
    lonePhase.rowPitch = lonePhase.colPitch = 2;
    const auto lonePhasePlanes = motioncam::expandGainMapsForCfa(
        std::vector<motioncam::GainMap>{lonePhase}, rggb);
    assert(lonePhasePlanes[0][0] == 1.0f);
    assert(lonePhasePlanes[1][0] == 1.0f);
    assert(lonePhasePlanes[2][0] == 11.0f);
    assert(lonePhasePlanes[3][0] == 1.0f);

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

    // Both DNG and native-source output paths use this boundary. HQ output
    // delegates to the regular demosaic; fast non-HQ previews request
    // nearest-colour reconstruction explicitly.
    std::vector<uint16_t> sharedRgb;
    motioncam::utils::demosaicCfaForOutput(
        mosaic, sharedRgb, width, height, 2, phase,
        motioncam::QuadBayerMode::Demosaic, {}, false);
    assert(sharedRgb == rgb);
    motioncam::utils::demosaicCfaForOutput(
        mosaic, sharedRgb, width, height, 2, phase,
        motioncam::QuadBayerMode::Demosaic, {}, true);
    assert(sharedRgb.size() == mosaic.size() * 3);
    for (int y = 1; y + 1 < height; ++y) for (int x = 1; x + 1 < width; ++x) {
        const size_t pixel = static_cast<size_t>(y) * width + x;
        for (int channel = 0; channel < 3; ++channel)
            assert(sharedRgb[pixel * 3 + channel] == flatColor[channel]);
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
