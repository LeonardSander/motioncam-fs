#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <iomanip>
#include <limits>
#include <locale>
#include <sstream>
#include <string>
#include <vector>

#include "Types.h"

namespace motioncam {

// LLOG60 transfer: scaled LOG60 through 0.96, quadratic
// transition to slope 1 at 0.99, then identity through white. C1 continuous.
struct LLog60 {
    static constexpr double start = 0.96, end = 0.99, width = end - start;
    static double scale() {
        return (end - width / 2) / (std::log1p(60 * start) / std::log(61.) +
            width / 2 * 60 / ((1 + 60 * start) * std::log(61.)));
    }
    static double slope() { return scale() * 60 / ((1 + 60 * start) * std::log(61.)); }
    static double join() { return scale() * std::log1p(60 * start) / std::log(61.); }
    static double encode(double x) {
        x = std::clamp(x, 0., 1.);
        if (x >= end) return x;
        if (x <= start) return scale() * std::log1p(60 * x) / std::log(61.);
        const double t = x - start;
        return join() + slope() * t + (1 - slope()) * t * t / (2 * width);
    }
    static double decode(double y) {
        y = std::clamp(y, 0., 1.);
        if (y >= end) return y;
        if (y <= join()) return std::expm1(y / scale() * std::log(61.)) / 60;
        const double d = y - join(), m = slope(), k = (1 - m) / (2 * width);
        return start + 2 * d / (m + std::sqrt(m * m + 4 * k * d));
    }
    static std::string ffmpegLut() {
        // Decimal precision exceeds RGB48 lookup precision.
        const auto number = [](double v) {
            std::ostringstream out;
            out.imbue(std::locale::classic());
            out << std::setprecision(std::numeric_limits<double>::max_digits10) << v;
            return out.str();
        };
        const std::string x = "val/maxval";
        const std::string startCode = number(start);
        const std::string endCode = number(end);
        const std::string t = "(" + x + "-" + startCode + ")";
        const std::string expression = "if(gte(" + x + "," + endCode + ")," + x +
            ",if(lte(" + x + "," + startCode + ")," + number(scale()) +
            "*log(1+60*" + x + ")/log(61)," + number(join()) + "+" +
            number(slope()) + "*" + t + "+" + number((1 - slope()) / (2 * width)) +
            "*" + t + "*" + t + "))*maxval";
        return "lutrgb=r='" + expression + "':g='" + expression + "':b='" + expression + "'";
    }
};

template <typename Value>
inline Value encodeLog60Normalized(Value normalized, Value encodedWhite,
                                   LogCurve curve = LogCurve::LOG60) {
    if (curve == LogCurve::LLOG60)
        return static_cast<Value>(LLog60::encode(normalized) * encodedWhite);
    return std::log2(Value(1) + Value(60) * std::max(Value(0), normalized)) /
           std::log2(Value(61)) * encodedWhite;
}

// CFA and RGB bake paths share the selected LOG60 or LLOG60 curve. The output bake indexes this
// table with a quantized linear code; the gallery can interpolate between
// codes so its floating point gain result retains sub-code precision.
inline std::vector<float> makeLog60EncodeLookup(
        double black, double white, uint16_t encodedWhite,
        LogCurve curve = LogCurve::LOG60) {
    std::vector<float> lookup(65536);
    const double range = std::max(1.0, white - black);
    for (uint32_t sample = 0; sample < lookup.size(); ++sample) {
        const double normalized = std::clamp(
            (sample - black) / range, 0.0, 1.0);
        lookup[sample] = static_cast<float>(
            encodeLog60Normalized(normalized, static_cast<double>(encodedWhite), curve));
    }
    return lookup;
}

// A frame worker normally uses the same curve for every frame in a clip.
// Keep the 65,536-entry table on that worker instead of rebuilding it.
inline const std::vector<float>& cachedLog60EncodeLookup(
        double black, double white, uint16_t encodedWhite,
        LogCurve curve = LogCurve::LOG60) {
    struct Cache {
        double black = 0.0;
        double white = 0.0;
        uint16_t encodedWhite = 0;
        LogCurve curve = LogCurve::LOG60;
        std::vector<float> lookup;
    };
    static thread_local Cache cache;
    if (cache.lookup.empty() || cache.black != black || cache.white != white ||
        cache.encodedWhite != encodedWhite || cache.curve != curve) {
        cache.lookup = makeLog60EncodeLookup(black, white, encodedWhite, curve);
        cache.curve = curve;
        cache.black = black;
        cache.white = white;
        cache.encodedWhite = encodedWhite;
    }
    return cache.lookup;
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
        uint32_t storedWhiteLevel, LogCurve curve = LogCurve::LOG60) {
    if (!storedWhiteLevel || storedWhiteLevel > 65535u) return {};
    std::vector<uint16_t> table(storedWhiteLevel + 1u);
    for (uint32_t code = 0; code <= storedWhiteLevel; ++code) {
        const double encoded = static_cast<double>(code) / storedWhiteLevel;
        const double linear = curve == LogCurve::LLOG60 ? LLog60::decode(encoded)
            : (std::pow(2.0, encoded * std::log2(61.0)) - 1.0) / 60.0;
        table[code] = static_cast<uint16_t>(std::lround(
            std::clamp(linear, 0.0, 1.0) * 65535.0));
    }
    return table;
}

inline void applyLinearizationTable(uint16_t* samples, size_t count,
                                    const std::vector<uint16_t>& table) {
    if (!samples || table.empty()) return;
    if (table.size() >= 65536) {
        for (size_t i = 0; i < count; ++i)
            samples[i] = table[samples[i]];
        return;
    }
    for (size_t i = 0; i < count; ++i)
        samples[i] = table[std::min<size_t>(samples[i], table.size() - 1)];
}

} // namespace motioncam
