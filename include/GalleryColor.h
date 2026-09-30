#pragma once

#include "DNGImage.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>

namespace motioncam::gallery {

inline constexpr std::array<float, 9> identityMatrix{
    1.0f, 0.0f, 0.0f,
    0.0f, 1.0f, 0.0f,
    0.0f, 0.0f, 1.0f};

inline float srgbEncode(float value) {
    value = std::max(0.0f, value);
    return value <= 0.0031308f ? 12.92f * value
        : 1.055f * std::pow(value, 1.0f / 2.4f) - 0.055f;
}

inline float srgbDecode(float value) {
    value = std::max(0.0f, value);
    return value <= 0.04045f ? value / 12.92f
        : std::pow((value + 0.055f) / 1.055f, 2.4f);
}

inline std::array<float, 3> sampleProfileTable(const DNGProfileTable& table,
                                                 float hue, float saturation,
                                                 float value) {
    if (table.values.empty()) return {0.0f, 1.0f, 1.0f};
    if (hue < 0.0f || hue >= 360.0f) {
        hue = std::fmod(hue, 360.0f);
        if (hue < 0.0f) hue += 360.0f;
    }
    const float h = hue * table.hueDivisions / 360.0f;
    const float s = std::clamp(saturation, 0.0f, 1.0f) *
        (table.saturationDivisions - 1);
    const float v = table.valueDivisions > 1
        ? std::clamp(value, 0.0f, 1.0f) * (table.valueDivisions - 1) : 0.0f;
    const uint32_t h0 = static_cast<uint32_t>(h) % table.hueDivisions;
    const uint32_t h1 = (h0 + 1) % table.hueDivisions;
    const uint32_t s0 = static_cast<uint32_t>(s);
    const uint32_t s1 = std::min(s0 + 1, table.saturationDivisions - 1);
    const uint32_t v0 = static_cast<uint32_t>(v);
    const uint32_t v1 = std::min(v0 + 1, table.valueDivisions - 1);
    const float hf = h - std::floor(h), sf = s - s0, vf = v - v0;
    auto sample = [&](uint32_t vi, uint32_t hi, uint32_t si, int channel) {
        return table.values[(((static_cast<size_t>(vi) * table.hueDivisions + hi) *
            table.saturationDivisions + si) * 3) + channel];
    };
    auto lerp = [](float a, float b, float t) { return a + (b - a) * t; };
    std::array<float, 3> result{};
    for (int channel = 0; channel < 3; ++channel) {
        const float lower = lerp(
            lerp(sample(v0, h0, s0, channel), sample(v0, h0, s1, channel), sf),
            lerp(sample(v0, h1, s0, channel), sample(v0, h1, s1, channel), sf), hf);
        if (v0 == v1) {
            result[channel] = lower;
            continue;
        }
        const float upper = lerp(
            lerp(sample(v1, h0, s0, channel), sample(v1, h0, s1, channel), sf),
            lerp(sample(v1, h1, s0, channel), sample(v1, h1, s1, channel), sf), hf);
        result[channel] = lerp(lower, upper, vf);
    }
    return result;
}

inline void applyProfileTable(std::array<float, 3>& rgb,
                              const DNGProfileTable& first,
                              const DNGProfileTable* second = nullptr,
                              float firstWeight = 1.0f) {
    if (first.values.empty()) return;
    const float maximum = std::max({rgb[0], rgb[1], rgb[2], 0.0f});
    const float minimum = std::max(0.0f, std::min({rgb[0], rgb[1], rgb[2]}));
    const float delta = maximum - minimum;
    float hue = 0.0f;
    if (delta > 1e-8f) {
        if (maximum == rgb[0]) hue = 60.0f * (rgb[1] - rgb[2]) / delta;
        else if (maximum == rgb[1]) hue = 60.0f * (2.0f + (rgb[2] - rgb[0]) / delta);
        else hue = 60.0f * (4.0f + (rgb[0] - rgb[1]) / delta);
    }
    if (hue < 0.0f) hue += 360.0f;
    else if (hue >= 360.0f) hue -= 360.0f;
    const float saturation = maximum > 1e-8f ? delta / maximum : 0.0f;
    const bool encoded = first.valueDivisions > 1 && first.encoding == 1;
    const float indexedValue = encoded ? srgbEncode(maximum) : maximum;
    auto adjustment = sampleProfileTable(first, hue, saturation, indexedValue);
    if (second && !second->values.empty() &&
        second->hueDivisions == first.hueDivisions &&
        second->saturationDivisions == first.saturationDivisions &&
        second->valueDivisions == first.valueDivisions &&
        second->encoding == first.encoding) {
        const auto other = sampleProfileTable(*second, hue, saturation, indexedValue);
        for (int i = 0; i < 3; ++i)
            adjustment[i] = adjustment[i] * firstWeight + other[i] * (1.0f - firstWeight);
    }
    hue += adjustment[0];
    if (hue < 0.0f || hue >= 360.0f) {
        hue = std::fmod(hue, 360.0f);
        if (hue < 0.0f) hue += 360.0f;
    }
    const float newSaturation = std::clamp(saturation * adjustment[1], 0.0f, 1.0f);
    const float newValue = std::clamp(indexedValue * adjustment[2], 0.0f, 1.0f);
    const float chroma = newValue * newSaturation;
    const float sector = hue / 60.0f;
    const float x = chroma * (1.0f - std::abs(
        sector - 2.0f * std::floor(sector * 0.5f) - 1.0f));
    std::array<float, 3> result{};
    if (sector < 1.0f) result = {chroma, x, 0.0f};
    else if (sector < 2.0f) result = {x, chroma, 0.0f};
    else if (sector < 3.0f) result = {0.0f, chroma, x};
    else if (sector < 4.0f) result = {0.0f, x, chroma};
    else if (sector < 5.0f) result = {x, 0.0f, chroma};
    else result = {chroma, 0.0f, x};
    const float offset = newValue - chroma;
    for (int i = 0; i < 3; ++i)
        rgb[i] = encoded ? srgbDecode(result[i] + offset) : result[i] + offset;
}

inline std::array<float, 9> interpolateMatrix(const std::array<float, 9>& first,
                                               const std::array<float, 9>& second,
                                               float firstWeight) {
    std::array<float, 9> result{};
    for (size_t index = 0; index < result.size(); ++index)
        result[index] = first[index] * firstWeight +
                        second[index] * (1.0f - firstWeight);
    return result;
}

// DNG ColorMatrix values map XYZ into the reference camera space, while
// CameraCalibration maps that reference space into this individual camera's
// space.  Compose the pair before interpolation/inversion for display.
inline std::array<float, 9> calibratedColorMatrix(const DNGFrameMetadata& metadata,
                                                   bool first) {
    const auto& color = first ? metadata.colorMatrix1 : metadata.colorMatrix2;
    const bool hasCalibration = first ? metadata.hasCameraCalibration1
                                      : metadata.hasCameraCalibration2;
    if (!hasCalibration) return color;

    const auto& calibration = first ? metadata.cameraCalibration1
                                    : metadata.cameraCalibration2;
    std::array<float, 9> result{};
    for (int row = 0; row < 3; ++row)
        for (int column = 0; column < 3; ++column)
            for (int inner = 0; inner < 3; ++inner)
                result[row * 3 + column] +=
                    calibration[row * 3 + inner] * color[inner * 3 + column];
    return result;
}

inline std::array<float, 9> cameraCalibrationMatrix(const DNGFrameMetadata& metadata,
                                                     bool first) {
    const bool present = first ? metadata.hasCameraCalibration1
                               : metadata.hasCameraCalibration2;
    if (!present) return identityMatrix;
    return first ? metadata.cameraCalibration1 : metadata.cameraCalibration2;
}

inline bool invertMatrix(const std::array<float, 9>& input,
                         std::array<float, 9>& inverse) {
    const float determinant = input[0] * (input[4] * input[8] - input[5] * input[7]) -
        input[1] * (input[3] * input[8] - input[5] * input[6]) +
        input[2] * (input[3] * input[7] - input[4] * input[6]);
    if (std::abs(determinant) < 1.0e-8f) return false;
    const float scale = 1.0f / determinant;
    inverse = {(input[4] * input[8] - input[5] * input[7]) * scale,
        (input[2] * input[7] - input[1] * input[8]) * scale,
        (input[1] * input[5] - input[2] * input[4]) * scale,
        (input[5] * input[6] - input[3] * input[8]) * scale,
        (input[0] * input[8] - input[2] * input[6]) * scale,
        (input[2] * input[3] - input[0] * input[5]) * scale,
        (input[3] * input[7] - input[4] * input[6]) * scale,
        (input[1] * input[6] - input[0] * input[7]) * scale,
        (input[0] * input[4] - input[1] * input[3]) * scale};
    return true;
}

// Builds the DNG forward transform from individual camera values to XYZ D50:
// ForwardMatrix * inverse(reference neutral diagonal) *
// inverse(CameraCalibration). AnalogBalance is not represented by the preview
// metadata and therefore remains the DNG-default identity.
inline bool forwardCameraToXyz(const DNGFrameMetadata& metadata,
                               float firstIlluminantWeight,
                               float exposure,
                               std::array<float, 9>& result) {
    std::array<float, 9> forward{};
    if (metadata.hasForwardMatrix1 && metadata.hasForwardMatrix2)
        forward = interpolateMatrix(metadata.forwardMatrix1, metadata.forwardMatrix2,
                                    firstIlluminantWeight);
    else if (metadata.hasForwardMatrix1)
        forward = metadata.forwardMatrix1;
    else if (metadata.hasForwardMatrix2)
        forward = metadata.forwardMatrix2;
    else
        return false;

    const auto calibration = interpolateMatrix(
        cameraCalibrationMatrix(metadata, true),
        cameraCalibrationMatrix(metadata, false), firstIlluminantWeight);
    std::array<float, 9> individualToReference{};
    if (!invertMatrix(calibration, individualToReference)) return false;

    std::array<float, 3> referenceNeutral{};
    for (int row = 0; row < 3; ++row)
        for (int column = 0; column < 3; ++column)
            referenceNeutral[row] += individualToReference[row * 3 + column] *
                                     metadata.asShotNeutral[column];
    if (std::any_of(referenceNeutral.begin(), referenceNeutral.end(),
                    [](float value) { return !std::isfinite(value) || value <= 1.0e-8f; }))
        return false;

    std::array<float, 9> balancedReference{};
    for (int row = 0; row < 3; ++row)
        for (int column = 0; column < 3; ++column)
            balancedReference[row * 3 + column] =
                individualToReference[row * 3 + column] /
                referenceNeutral[row];

    result = {};
    for (int row = 0; row < 3; ++row)
        for (int column = 0; column < 3; ++column)
            for (int inner = 0; inner < 3; ++inner)
                result[row * 3 + column] += exposure *
                    forward[row * 3 + inner] *
                    balancedReference[inner * 3 + column];
    return true;
}

} // namespace motioncam::gallery
