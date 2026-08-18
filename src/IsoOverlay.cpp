#include "Utils.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <string>

namespace motioncam::utils {

void bakeIsoOverlay(uint16_t* samples, uint32_t width, uint32_t height,
                    uint32_t channels, double iso, uint16_t black, uint16_t white) {
    if (!samples || width < 40 || height < 30 || channels == 0 || channels > 4 ||
        !(iso > 0.0) || !std::isfinite(iso)) return;
    static const std::array<std::array<uint8_t, 7>, 13> glyphs{{
        {{14,17,19,21,25,17,14}}, {{4,12,4,4,4,4,14}},
        {{14,17,1,2,4,8,31}}, {{30,1,1,14,1,1,30}},
        {{2,6,10,18,31,2,2}}, {{31,16,16,30,1,1,30}},
        {{14,16,16,30,17,17,14}}, {{31,1,2,4,8,8,8}},
        {{14,17,17,14,17,17,14}}, {{14,17,17,15,1,1,14}},
        {{14,4,4,4,4,4,14}}, {{15,16,16,14,1,1,30}},
        {{14,17,17,17,17,17,14}}
    }};
    const std::string text = "ISO " + std::to_string(static_cast<long long>(std::llround(iso)));
    const uint32_t scale = std::max<uint32_t>(2, height / 270);
    const uint32_t advance = 6 * scale;
    const uint32_t textWidth = static_cast<uint32_t>(text.size()) * advance - scale;
    if (textWidth + 4 * scale > width) return;
    const int originX = static_cast<int>((width - textWidth) / 2);
    const int originY = static_cast<int>(height * 7 / 8) - static_cast<int>(7 * scale / 2);
    auto glyph = [&](char c) -> const std::array<uint8_t, 7>* {
        if (c >= '0' && c <= '9') return &glyphs[c - '0'];
        if (c == 'I') return &glyphs[10];
        if (c == 'S') return &glyphs[11];
        if (c == 'O') return &glyphs[12];
        return nullptr;
    };
    auto paint = [&](int x, int y, uint16_t value) {
        if (x < 0 || y < 0 || x >= static_cast<int>(width) || y >= static_cast<int>(height)) return;
        const size_t first = (static_cast<size_t>(y) * width + x) * channels;
        for (uint32_t c = 0; c < channels; ++c) samples[first + c] = value;
    };
    for (int pass = 0; pass < 2; ++pass) {
        for (size_t i = 0; i < text.size(); ++i) {
            const auto* rows = glyph(text[i]);
            if (!rows) continue;
            for (int row = 0; row < 7; ++row) for (int col = 0; col < 5; ++col) {
                if (((*rows)[row] & (1u << (4 - col))) == 0) continue;
                const int x0 = originX + static_cast<int>(i * advance + col * scale);
                const int y0 = originY + row * static_cast<int>(scale);
                const int radius = pass == 0 ? static_cast<int>(scale) : 0;
                for (int y = y0 - radius; y < y0 + static_cast<int>(scale) + radius; ++y)
                    for (int x = x0 - radius; x < x0 + static_cast<int>(scale) + radius; ++x)
                        paint(x, y, pass == 0 ? black : white);
            }
        }
    }
}

} // namespace motioncam::utils
