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
