#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cctype>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace motioncam {

inline std::array<uint8_t, 4> cfaColorsFromPhase(std::string phase) {
    std::transform(phase.begin(), phase.end(), phase.begin(), [](unsigned char value) {
        return static_cast<char>(std::tolower(value));
    });
    if (phase == "rggb") return {0, 1, 1, 2};
    if (phase == "grbg") return {1, 0, 2, 1};
    if (phase == "gbrg") return {1, 2, 0, 1};
    return {2, 1, 1, 0};
}

constexpr uint8_t cfaColorAt(const std::array<uint8_t, 4>& cfa,
                             uint32_t x, uint32_t y) {
    return cfa[((y & 1u) << 1u) | (x & 1u)];
}

constexpr size_t gainMapPhaseChannel(uint32_t x, uint32_t y,
                                     uint32_t left, uint32_t top,
                                     uint32_t phaseGroup) {
    return (((y - top) / phaseGroup) & 1u) * 2u +
           (((x - left) / phaseGroup) & 1u);
}

struct GainMapAxisSample {
    uint32_t first = 0, second = 0;
    float fraction = 0.0f;
};

inline GainMapAxisSample sampleGainMapAxis(double coordinate, uint32_t points) {
    if (!points) return {};
    const double base = std::floor(coordinate);
    const uint32_t first = static_cast<uint32_t>(std::min<double>(
        points - 1, std::max(0.0, base)));
    return {first, std::min(first + 1, points - 1),
            static_cast<float>(std::clamp(coordinate - base, 0.0, 1.0))};
}

template<typename Map>
inline bool validGainMap(const Map& map) {
    return map.width && map.height && map.channels && map.rowPitch && map.colPitch &&
           map.top < map.bottom && map.left < map.right &&
           map.data.size() == static_cast<size_t>(map.width) * map.height * map.channels;
}

template<typename ValueAt>
inline float sampleGainMapBilinear(const GainMapAxisSample& x,
                                   const GainMapAxisSample& y,
                                   ValueAt&& valueAt) {
    const float upper = valueAt(x.first, y.first) * (1.0f - x.fraction) +
                        valueAt(x.second, y.first) * x.fraction;
    const float lower = valueAt(x.first, y.second) * (1.0f - x.fraction) +
                        valueAt(x.second, y.second) * x.fraction;
    const float value = upper * (1.0f - y.fraction) + lower * y.fraction;
    return std::isfinite(value) && value > 0.0f ? value : 1.0f;
}

struct LinearGainBakeLevels {
    uint32_t sourceBits = 1;
    uint32_t destinationBits = 1;
    double sourceWhite = 1.0;
    double destinationWhite = 1.0;
    std::array<double, 4> sourceBlack{};
    std::array<double, 4> destinationBlack{};
};

inline LinearGainBakeLevels planLinearGainBake(
        double sourceWhite, const std::array<double, 4>& sourceBlack,
        bool normalizeGainMaps) {
    LinearGainBakeLevels result;
    result.sourceWhite = sourceWhite;
    result.sourceBlack = sourceBlack;
    while (result.sourceBits < 16 &&
           (static_cast<double>((uint32_t{1} << result.sourceBits) - 1) < sourceWhite))
        ++result.sourceBits;
    result.destinationBits = std::min<uint32_t>(
        16, result.sourceBits + (normalizeGainMaps ? 4u : 2u));
    result.destinationWhite = static_cast<double>(
        (uint32_t{1} << result.destinationBits) - 1);
    const double scale = std::ldexp(
        1.0, static_cast<int>(result.destinationBits - result.sourceBits));
    for (size_t channel = 0; channel < result.destinationBlack.size(); ++channel)
        result.destinationBlack[channel] = sourceBlack[channel] * scale;
    return result;
}

inline double applyLinearGain(double sample, double gain,
                              double sourceBlack, double sourceWhite,
                              double destinationBlack, double destinationWhite) {
    return destinationBlack + gain * (sample - sourceBlack) /
        std::max(1.0, sourceWhite - sourceBlack) *
        (destinationWhite - destinationBlack);
}

inline uint16_t bakeLinearGainSample(
        uint16_t sample, float gain,
        double sourceBlack, double sourceWhite,
        double destinationBlack, double destinationWhite,
        bool debugGainMap = false) {
    const double value = debugGainMap
        ? (gain > 0.0f ? destinationWhite / gain : 0.0)
        : applyLinearGain(sample, gain, sourceBlack, sourceWhite,
                          destinationBlack, destinationWhite);
    return static_cast<uint16_t>(std::clamp(std::lround(value), 0l, 65535l));
}

struct GainMapOptimization {
    std::array<float, 3> minima{1.0f, 1.0f, 1.0f};
    float common = 1.0f;
};

struct GainMapMetadataAdjustment {
    double exposureOffset = 0.0;
    std::array<float, 3> neutralScale{1.0f, 1.0f, 1.0f};
};

// Convert the two OpcodeList2 representations accepted by DNG readers into
// the representation emitted by MotionCam: one scalar GainMap per CFA phase.
// Existing scalar maps are retained so multiple spatial regions remain valid.
template<typename Map>
inline bool canonicalizeCfaGainMaps(std::vector<Map>& maps) {
    std::vector<Map> canonical;
    for (const auto& map : maps) {
        if (map.channels == 1) {
            canonical.push_back(map);
            continue;
        }
        if (map.channels != 4 ||
            map.data.size() != static_cast<size_t>(map.width) * map.height * 4)
            return false;
        const size_t points = static_cast<size_t>(map.width) * map.height;
        for (uint32_t phase = 0; phase < 4; ++phase) {
            Map scalar = map;
            scalar.channels = 1;
            scalar.top = map.top + phase / 2;
            scalar.left = map.left + phase % 2;
            scalar.rowPitch = 2;
            scalar.colPitch = 2;
            scalar.data.resize(points);
            for (size_t point = 0; point < points; ++point)
                scalar.data[point] = map.data[point * 4 + phase];
            canonical.push_back(std::move(scalar));
        }
    }
    maps = std::move(canonical);
    return true;
}

template<typename Map>
struct GainMapLuminanceSeparation {
    Map luminance{};
    bool colorSeparated = false;
    bool valid = false;
};

template<typename Map>
inline GainMapLuminanceSeparation<Map> separateGainMapLuminance(
        std::vector<Map>& maps) {
    GainMapLuminanceSeparation<Map> result;
    if (maps.empty() || !maps.front().width || !maps.front().height) return result;
    // Color/luminance separation is defined only when the samples represent
    // the four CFA phases of one grid.  Arbitrary scalar maps may instead be
    // independent spatial regions or processing layers and must not be
    // combined by matching their array indices.
    const bool interleavedCfa = maps.size() == 1 && maps.front().channels == 4;
    bool scalarCfa = maps.size() == 4;
    std::array<bool, 4> phases{};
    uint32_t baseTop = maps.front().top;
    uint32_t baseLeft = maps.front().left;
    for (const auto& map : maps) {
        baseTop = std::min(baseTop, map.top);
        baseLeft = std::min(baseLeft, map.left);
    }
    for (const auto& map : maps) {
        scalarCfa &= map.channels == 1 && map.rowPitch == 2 && map.colPitch == 2 &&
            map.width == maps.front().width && map.height == maps.front().height &&
            map.bottom == maps.front().bottom && map.right == maps.front().right &&
            map.plane == maps.front().plane && map.planes == maps.front().planes &&
            map.spacingV == maps.front().spacingV && map.spacingH == maps.front().spacingH &&
            map.originV == maps.front().originV && map.originH == maps.front().originH &&
            map.top >= baseTop && map.top < baseTop + 2 &&
            map.left >= baseLeft && map.left < baseLeft + 2;
        if (scalarCfa) {
            const size_t phase = ((map.top - baseTop) << 1u) | (map.left - baseLeft);
            if (phases[phase]) scalarCfa = false;
            else phases[phase] = true;
        }
    }
    if (!interleavedCfa && (!scalarCfa ||
        !std::all_of(phases.begin(), phases.end(), [](bool present) { return present; })))
        return result;
    const size_t expectedPoints = static_cast<size_t>(maps.front().width) *
                                  maps.front().height;
    for (const auto& map : maps)
        if (map.width != maps.front().width || map.height != maps.front().height ||
            !map.channels || map.data.size() != expectedPoints * map.channels)
            return result;
    result.luminance = maps.front();
    result.luminance.channels = 1;
    const size_t points = static_cast<size_t>(result.luminance.width) *
                          result.luminance.height;
    result.luminance.data.assign(points, 1.0f);
    for (size_t point = 0; point < points; ++point) {
        float minimum = std::numeric_limits<float>::max();
        for (const auto& map : maps) {
            if (map.width != result.luminance.width ||
                map.height != result.luminance.height || !map.channels ||
                map.data.size() < (point + 1) * map.channels)
                continue;
            for (uint32_t channel = 0; channel < map.channels; ++channel)
                minimum = std::min(minimum,
                    map.data[point * map.channels + channel]);
        }
        if (!std::isfinite(minimum) || minimum <= 0.0f) minimum = 1.0f;
        result.luminance.data[point] = minimum;
        for (auto& map : maps) {
            if (map.width != result.luminance.width ||
                map.height != result.luminance.height || !map.channels ||
                map.data.size() < (point + 1) * map.channels)
                continue;
            result.colorSeparated |= maps.size() > 1 || map.channels > 1;
            for (uint32_t channel = 0; channel < map.channels; ++channel)
                map.data[point * map.channels + channel] /= minimum;
        }
    }
    result.valid = true;
    return result;
}

template<typename Range>
inline float normalizePositiveGainMinimum(Range& gains) {
    float minimum = std::numeric_limits<float>::max();
    for (float gain : gains)
        if (std::isfinite(gain) && gain > 0.0f)
            minimum = std::min(minimum, gain);
    if (!std::isfinite(minimum) || minimum <= 0.0f) return 1.0f;
    for (float& gain : gains)
        if (std::isfinite(gain) && gain > 0.0f) gain /= minimum;
    return minimum;
}

template<typename Map>
inline std::array<bool, 3> gainMapAffectedColors(
        const Map& map, const std::array<uint8_t, 4>& cfa) {
    std::array<bool, 3> colors{};
    if (map.channels != 1 || !map.rowPitch || !map.colPitch) return colors;
    for (uint32_t y = 0; y < 2; ++y)
        for (uint32_t x = 0; x < 2; ++x)
            if (y % map.rowPitch == 0 && x % map.colPitch == 0)
                colors[std::min<size_t>(2, cfa[
                    (((map.top + y) & 1u) << 1u) | ((map.left + x) & 1u)])] = true;
    return colors;
}

template<typename Map>
inline size_t gainMapSampleColor(const Map& map, size_t sample,
                                 const std::array<uint8_t, 4>& cfa) {
    if (map.channels >= 4)
        return std::min<size_t>(2, cfa[sample % map.channels % 4]);
    if (map.channels == 3) return sample % 3;
    const auto colors = gainMapAffectedColors(map, cfa);
    for (size_t color = 0; color < colors.size(); ++color)
        if (colors[color]) return color;
    return 1;
}

template<typename Map>
inline float gainMapColorValueAt(const Map& map, uint32_t x, uint32_t y,
                                 uint32_t color,
                                 const std::array<uint8_t, 4>& cfa) {
    if (map.channels == 1)
        return map.data[static_cast<size_t>(y) * map.width + x];
    if (map.channels == 3)
        return map.data[(static_cast<size_t>(y) * map.width + x) * 3 +
                        std::min<uint32_t>(2, color)];
    float sum = 0.0f;
    uint32_t count = 0;
    for (uint32_t channel = 0; channel < std::min<uint32_t>(4, map.channels); ++channel)
        if (cfa[channel] == color) {
            sum += map.data[(static_cast<size_t>(y) * map.width + x) *
                            map.channels + channel];
            ++count;
        }
    return count ? sum / count : 1.0f;
}

// Convert complete groups of four scalar CFA phase maps into RGB maps. Red and
// blue each come from one phase; the two green phases are averaged. Maps that
// are already RGB, four-channel CFA, or scalar luminance are retained.
template<typename Map>
inline std::vector<Map> collapseCfaGainMapsForRgb(
        const std::vector<Map>& maps, const std::array<uint8_t, 4>& cfa) {
    std::vector<Map> result;
    std::vector<bool> consumed(maps.size(), false);
    auto compatible = [](const Map& a, const Map& b) {
        const auto equivalentOrigin = [](double left, double right, double spacing) {
            const double difference = std::abs(left - right);
            return difference < 1e-9 ||
                (spacing > 0.0 && std::abs(difference - spacing * 0.5) < 1e-9);
        };
        return a.channels == 1 && b.channels == 1 &&
            a.rowPitch == 2 && b.rowPitch == 2 &&
            a.colPitch == 2 && b.colPitch == 2 &&
            a.width == b.width && a.height == b.height &&
            a.bottom == b.bottom && a.right == b.right &&
            a.plane == b.plane && a.planes == b.planes &&
            a.spacingV == b.spacingV && a.spacingH == b.spacingH &&
            equivalentOrigin(a.originV, b.originV, a.spacingV) &&
            equivalentOrigin(a.originH, b.originH, a.spacingH);
    };
    for (size_t seed = 0; seed < maps.size(); ++seed) {
        if (consumed[seed]) continue;
        const auto& first = maps[seed];
        if (first.channels != 1 || first.rowPitch != 2 || first.colPitch != 2) {
            consumed[seed] = true;
            result.push_back(first);
            continue;
        }
        uint32_t baseTop = first.top, baseLeft = first.left;
        for (size_t i = seed; i < maps.size(); ++i)
            if (!consumed[i] && compatible(first, maps[i])) {
                baseTop = std::min(baseTop, maps[i].top);
                baseLeft = std::min(baseLeft, maps[i].left);
            }
        std::array<size_t, 4> phaseIndex{};
        phaseIndex.fill(maps.size());
        for (size_t i = seed; i < maps.size(); ++i) {
            if (consumed[i] || !compatible(first, maps[i]) ||
                maps[i].top < baseTop || maps[i].top >= baseTop + 2 ||
                maps[i].left < baseLeft || maps[i].left >= baseLeft + 2)
                continue;
            const size_t phase = ((maps[i].top - baseTop) << 1u) |
                                 (maps[i].left - baseLeft);
            if (phaseIndex[phase] == maps.size()) phaseIndex[phase] = i;
        }
        if (std::any_of(phaseIndex.begin(), phaseIndex.end(), [&](size_t i) {
                return i == maps.size();
            })) {
            consumed[seed] = true;
            result.push_back(first);
            continue;
        }
        Map rgb = first;
        rgb.top = baseTop;
        rgb.left = baseLeft;
        rgb.rowPitch = 1;
        rgb.colPitch = 1;
        rgb.channels = 3;
        for (const size_t index : phaseIndex) {
            rgb.coordinateWidth = std::max(rgb.coordinateWidth, maps[index].coordinateWidth);
            rgb.coordinateHeight = std::max(rgb.coordinateHeight, maps[index].coordinateHeight);
            rgb.originH = std::min(rgb.originH, maps[index].originH);
            rgb.originV = std::min(rgb.originV, maps[index].originV);
        }
        const size_t points = static_cast<size_t>(rgb.width) * rgb.height;
        rgb.data.assign(points * 3, 0.0f);
        std::array<uint32_t, 3> counts{};
        for (const size_t index : phaseIndex) {
            consumed[index] = true;
            const auto& map = maps[index];
            const size_t color = cfaColorAt(cfa, map.left, map.top);
            ++counts[color];
            for (size_t point = 0; point < points; ++point)
                rgb.data[point * 3 + color] += map.data[point];
        }
        if (std::any_of(counts.begin(), counts.end(), [](uint32_t count) {
                return count == 0;
            })) {
            for (const size_t index : phaseIndex) consumed[index] = false;
            consumed[seed] = true;
            result.push_back(first);
            continue;
        }
        for (size_t point = 0; point < points; ++point)
            for (size_t color = 0; color < 3; ++color)
                rgb.data[point * 3 + color] /= counts[color];
        result.push_back(std::move(rgb));
    }
    return result;
}

// Expand a gain-map layer for mosaiced pixels. RGB channels are duplicated to
// both CFA green phases, and a scalar luminance map is duplicated to all four.
template<typename Map>
inline std::vector<std::vector<float>> expandGainMapsForCfa(
        const std::vector<Map>& maps, const std::array<uint8_t, 4>& cfa) {
    if (maps.empty()) return {};
    const uint32_t width = maps.front().width;
    const uint32_t height = maps.front().height;
    if (!width || !height) throw std::invalid_argument("Invalid gain-map dimensions");
    const size_t points = static_cast<size_t>(width) * height;
    std::vector<std::vector<float>> planes(4, std::vector<float>(points, 1.0f));
    if (maps.size() == 1) {
        const auto& map = maps.front();
        if (map.channels != 1 && map.channels != 3 && map.channels != 4)
            throw std::invalid_argument("Gain map requires 1, 3, or 4 channels");
        if (map.data.size() != points * map.channels)
            throw std::invalid_argument("Invalid gain-map payload size");
        for (size_t point = 0; point < points; ++point)
            for (uint32_t phase = 0; phase < 4; ++phase) {
                if (map.channels == 1) {
                    const uint32_t phaseY = phase / 2;
                    const uint32_t phaseX = phase % 2;
                    if (!map.rowPitch || !map.colPitch ||
                        (phaseY + map.rowPitch - map.top % map.rowPitch) % map.rowPitch ||
                        (phaseX + map.colPitch - map.left % map.colPitch) % map.colPitch)
                        continue;
                    planes[phase][point] = map.data[point];
                } else {
                    const uint32_t channel = map.channels == 3 ? cfa[phase] : phase;
                    planes[phase][point] = map.data[point * map.channels + channel];
                }
            }
        return planes;
    }
    if (maps.size() != 4)
        throw std::invalid_argument("Gain-map layer requires one map or four CFA phases");
    uint32_t baseTop = maps.front().top, baseLeft = maps.front().left;
    for (const auto& map : maps) {
        baseTop = std::min(baseTop, map.top);
        baseLeft = std::min(baseLeft, map.left);
    }
    std::array<bool, 4> populated{};
    for (const auto& map : maps) {
        if (map.width != width || map.height != height || map.channels != 1 ||
            map.data.size() != points || map.top < baseTop || map.top >= baseTop + 2 ||
            map.left < baseLeft || map.left >= baseLeft + 2)
            throw std::invalid_argument("Gain-map CFA phases are incompatible");
        const size_t phase = ((map.top - baseTop) << 1u) | (map.left - baseLeft);
        if (populated[phase]) throw std::invalid_argument("Gain-map CFA phase is repeated");
        populated[phase] = true;
        planes[phase] = map.data;
    }
    return planes;
}

template<typename Map, typename ValueAt>
inline float sampleGainMapNormalized(const Map& map, double normalizedX,
                                     double normalizedY, ValueAt&& valueAt,
                                     double originH, double originV) {
    const double gridX = map.spacingH > 0.0
        ? (normalizedX - originH) / map.spacingH : 0.0;
    const double gridY = map.spacingV > 0.0
        ? (normalizedY - originV) / map.spacingV : 0.0;
    return sampleGainMapBilinear(
        sampleGainMapAxis(gridX, map.width), sampleGainMapAxis(gridY, map.height),
        std::forward<ValueAt>(valueAt));
}

template<typename Map, typename ValueAt>
inline float sampleGainMapNormalized(const Map& map, double normalizedX,
                                     double normalizedY, ValueAt&& valueAt) {
    return sampleGainMapNormalized(map, normalizedX, normalizedY,
                                   std::forward<ValueAt>(valueAt),
                                   map.originH, map.originV);
}

template<typename Map>
inline float sampleGainMapColorNormalized(
        const Map& map, double normalizedX, double normalizedY, uint32_t color,
        const std::array<uint8_t, 4>& cfa) {
    return sampleGainMapNormalized(map, normalizedX, normalizedY,
        [&](uint32_t x, uint32_t y) {
            return gainMapColorValueAt(map, x, y, color, cfa);
        });
}

template<typename Map>
inline GainMapOptimization optimizeGainMapStack(
        std::vector<Map>& maps, const std::array<uint8_t, 4>& cfa) {
    GainMapOptimization result;
    result.minima.fill(std::numeric_limits<float>::max());
    std::array<bool, 3> contributed{};
    for (const auto& map : maps)
        for (size_t sample = 0; sample < map.data.size(); ++sample) {
            const float gain = map.data[sample];
            if (!std::isfinite(gain) || gain <= 0.0f) continue;
            if (map.channels == 1) {
                const auto colors = gainMapAffectedColors(map, cfa);
                for (size_t color = 0; color < colors.size(); ++color)
                    if (colors[color]) {
                        result.minima[color] = std::min(result.minima[color], gain);
                        contributed[color] = true;
                    }
            } else {
                const size_t color = gainMapSampleColor(map, sample, cfa);
                result.minima[color] = std::min(result.minima[color], gain);
                contributed[color] = true;
            }
        }
    for (size_t color = 0; color < result.minima.size(); ++color)
        if (!contributed[color] || !std::isfinite(result.minima[color]) ||
            result.minima[color] <= 0.0f)
            result.minima[color] = 1.0f;
    result.common = *std::min_element(result.minima.begin(), result.minima.end());
    for (auto& map : maps)
        for (size_t sample = 0; sample < map.data.size(); ++sample) {
            const size_t color = gainMapSampleColor(map, sample, cfa);
            float& gain = map.data[sample];
            if (std::isfinite(gain) && gain > 0.0f) gain /= result.minima[color];
        }
    return result;
}

// Optimize all independently encoded gain-map layers and return the metadata
// compensation which preserves their combined rendering.  Keeping this here
// makes MCRAW, mounted DNG and DirectLog use the same layer semantics.
template<typename Map, typename LayerRange>
inline GainMapMetadataAdjustment optimizeGainMapLayers(
        LayerRange&& layers, const std::array<uint8_t, 4>& cfa) {
    GainMapMetadataAdjustment adjustment;
    for (auto* layer : layers) {
        if (!layer || layer->empty()) continue;
        const auto optimization = optimizeGainMapStack(*layer, cfa);
        if (!std::isfinite(optimization.common) || optimization.common <= 0.0f)
            continue;
        adjustment.exposureOffset += std::log2(optimization.common);
        for (size_t color = 0; color < adjustment.neutralScale.size(); ++color)
            adjustment.neutralScale[color] *=
                optimization.common / optimization.minima[color];
    }
    return adjustment;
}

template<typename Map, typename LayerRange>
inline void normalizeGainMapLayersLikeMcraw(LayerRange&& layers) {
    float maximum = 0.0f;
    for (const auto* layer : layers)
        if (layer)
            for (const auto& map : *layer)
                for (float gain : map.data)
                    if (std::isfinite(gain)) maximum = std::max(maximum, gain);
    if (!(maximum > 0.0f)) return;
    for (auto* layer : layers)
        if (layer)
            for (auto& map : *layer)
                for (float& gain : map.data)
                    if (std::isfinite(gain) && gain > 0.0f) gain /= maximum;
}

template<typename Map>
inline void normalizeGainMapStack(std::vector<Map>& maps) {
    normalizeGainMapLayersLikeMcraw<Map>(
        std::array<std::vector<Map>*, 1>{&maps});
}

template<typename Map>
inline void invertGainMapStack(std::vector<Map>& maps) {
    for (auto& map : maps)
        for (float& gain : map.data)
            gain = std::isfinite(gain) && gain > 0.0f ? 1.0f / gain : 1.0f;
}

// Apply the mutually exclusive bake-time map transform in one place. Debug
// inversion intentionally loses to normalization, matching the UI semantics.
template<typename Map, typename LayerRange>
inline void transformGainMapLayersForBake(LayerRange&& layers,
                                          bool normalize, bool debug) {
    if (normalize) {
        normalizeGainMapLayersLikeMcraw<Map>(std::forward<LayerRange>(layers));
        return;
    }
    if (!debug) return;
    for (auto* layer : layers)
        if (layer) invertGainMapStack(*layer);
}

template<typename Map>
inline bool reduceGainMapStackToColor(std::vector<Map>& maps) {
    if (maps.empty()) return true;
    return separateGainMapLuminance(maps).valid;
}

} // namespace motioncam
