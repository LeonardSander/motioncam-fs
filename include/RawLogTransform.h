#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace motioncam {

template <typename Value>
inline Value encodeLog60Normalized(Value normalized, Value encodedWhite) {
    return std::log2(Value(1) + Value(60) * std::max(Value(0), normalized)) /
           std::log2(Value(61)) * encodedWhite;
}

// The CFA bake paths use the same LOG60 curve. The output bake indexes this
// table with a quantized linear code; the gallery can interpolate between
// codes so its floating point gain result retains sub-code precision.
inline std::vector<float> makeLog60EncodeLookup(
        double black, double white, uint16_t encodedWhite) {
    std::vector<float> lookup(65536);
    const double range = std::max(1.0, white - black);
    for (uint32_t sample = 0; sample < lookup.size(); ++sample) {
        const double normalized = std::clamp(
            (sample - black) / range, 0.0, 1.0);
        lookup[sample] = static_cast<float>(
            encodeLog60Normalized(normalized, static_cast<double>(encodedWhite)));
    }
    return lookup;
}

inline float sampleLog60EncodeLookup(const std::vector<float>& lookup,
                                     float normalized) {
    const float position = std::clamp(normalized, 0.0f, 1.0f) * 65535.0f;
    const uint32_t low = static_cast<uint32_t>(position);
    const uint32_t high = std::min(low + 1u, 65535u);
    return lookup[low] + (lookup[high] - lookup[low]) * (position - low);
}

template <typename Encoded>
inline uint16_t quantizeDitheredLog60(Encoded encoded, float dither,
                                      uint16_t encodedWhite) {
    return static_cast<uint16_t>(std::clamp(
        std::lround(encoded + dither), 0l, static_cast<long>(encodedWhite)));
}

inline uint16_t encodeDitheredLog60(const std::vector<float>& lookup,
                                    uint16_t linearSample, float dither,
                                    uint16_t encodedWhite) {
    return quantizeDitheredLog60(lookup[linearSample], dither, encodedWhite);
}

inline std::vector<uint16_t> makeLog60LinearizationTable(
        uint32_t storedWhiteLevel) {
    if (!storedWhiteLevel || storedWhiteLevel > 65535u) return {};
    std::vector<uint16_t> table(storedWhiteLevel + 1u);
    for (uint32_t code = 0; code <= storedWhiteLevel; ++code) {
        const double encoded = static_cast<double>(code) / storedWhiteLevel;
        const double linear = (std::pow(2.0, encoded * std::log2(61.0)) - 1.0) / 60.0;
        table[code] = static_cast<uint16_t>(std::lround(
            std::clamp(linear, 0.0, 1.0) * 65535.0));
    }
    return table;
}

inline void applyLinearizationTable(uint16_t* samples, size_t count,
                                    const std::vector<uint16_t>& table) {
    if (!samples || table.empty()) return;
    for (size_t i = 0; i < count; ++i)
        samples[i] = table[std::min<size_t>(samples[i], table.size() - 1)];
}

} // namespace motioncam
