#include "VulkanGainBake.h"

#include <array>
#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <vector>

int main() {
    constexpr uint32_t width = 32, height = 32;
    const std::vector<float> cfaPhaseGrid(4, 1.25f);
    const std::vector<float> rightHalfGrid(4, 1.5f);
    const std::vector<float> rgbGrid{
        1.25f, 1.0f, 0.75f, 1.25f, 1.0f, 0.75f,
        1.25f, 1.0f, 0.75f, 1.25f, 1.0f, 0.75f};
    auto gainMap = [](const std::vector<float>& data, uint32_t channels,
                      uint32_t left, uint32_t right, uint32_t pitch) {
        motioncam::VulkanRgbGainMap map;
        map.width = map.height = 2;
        map.channels = channels;
        map.left = left;
        map.top = 0;
        map.right = right;
        map.bottom = height;
        map.rowPitch = map.colPitch = pitch;
        map.spacingH = map.spacingV = 1.0f;
        map.data = &data;
        return map;
    };
    auto params = [&](uint32_t channels) {
        motioncam::VulkanGainBakeParams result;
        result.width = width;
        result.height = height;
        result.channels = channels;
        result.outputBits = 16;
        result.destinationWhite = 65535.0f;
        result.scale.fill(1.0f);
        return result;
    };
    auto run = [](const std::vector<uint16_t>& source,
                  const motioncam::VulkanGainBakeParams& settings,
                  bool cfa, std::vector<uint16_t>& result,
                  bool allowSkip = false) {
        result.resize(source.size());
        const bool ok = cfa
            ? motioncam::bakeCfaGainVulkan(source.data(), settings,
                reinterpret_cast<uint8_t*>(result.data()), result.size() * 2)
            : motioncam::bakeRgbGainVulkan(source.data(), settings,
                reinterpret_cast<uint8_t*>(result.data()), result.size() * 2);
        if (!ok) {
            std::fprintf(stderr, "Vulkan gain bake %s\n",
                allowSkip ? "unavailable; test skipped" : "failed");
            std::exit(allowSkip ? 77 : 1);
        }
        return ok;
    };

    std::vector<uint16_t> cfaSource(width * height, 1000), output;
    auto cfa = params(1);
    cfa.maps = {gainMap(cfaPhaseGrid, 1, 0, width, 2),
                gainMap(rightHalfGrid, 1, 16, width, 1)};
    run(cfaSource, cfa, true, output, true);
    if (output[0] != 1250 || output[1] != 1000 ||
        output[16] != 1875 || output[17] != 1500 ||
        output[width] != 1000) {
        std::fprintf(stderr, "CFA gain layout mismatch\n");
        return 1;
    }

    std::vector<uint16_t> rgbSource(width * height * 3, 1000);
    auto rgb = params(3);
    rgb.maps = {gainMap(rgbGrid, 3, 0, width, 1),
                gainMap(rightHalfGrid, 1, 16, width, 1)};
    run(rgbSource, rgb, false, output);
    if ((std::array<uint16_t, 3>{output[0], output[1], output[2]} !=
         std::array<uint16_t, 3>{1250, 1000, 750})) {
        std::fprintf(stderr, "RGB gain layout mismatch\n");
        return 1;
    }
    const size_t right = 16u * 3u;
    if ((std::array<uint16_t, 3>{output[right], output[right + 1],
                                 output[right + 2]} !=
         std::array<uint16_t, 3>{1875, 1500, 1125})) {
        std::fprintf(stderr, "RGB second-map layout mismatch\n");
        return 1;
    }
    const std::vector<float> interpolatedGrid{1.0f, 2.0f, 3.0f, 4.0f};
    auto interpolated = params(1);
    auto interpolatedMap = gainMap(interpolatedGrid, 1, 0, width, 1);
    interpolatedMap.spacingH = interpolatedMap.spacingV = 0.5f;
    interpolated.maps = {interpolatedMap};
    run(cfaSource, interpolated, true, output);
    if (output[7u * width + 7u] != 2406 ||
        output[15u * width + 15u] != 3906) {
        std::fprintf(stderr, "CFA gain interpolation mismatch\n");
        return 1;
    }
    const std::vector<float> colorGrid{
        1.1f, 1.3f, 1.7f, 1.1f, 1.3f, 1.7f,
        1.1f, 1.3f, 1.7f, 1.1f, 1.3f, 1.7f};
    auto phased = params(1);
    phased.cfaPhase = {2, 1, 1, 0};
    phased.maps = {gainMap(colorGrid, 3, 0, width, 1)};
    run(cfaSource, phased, true, output);
    if (output[0] != 1700 || output[1] != 1300 ||
        output[width] != 1300 || output[width + 1] != 1100) {
        std::fprintf(stderr, "CFA color phase mismatch\n");
        return 1;
    }
    return 0;
}
