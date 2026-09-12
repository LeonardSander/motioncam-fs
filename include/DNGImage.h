#pragma once

#include "Types.h"

#include <array>
#include <cstdint>
#include <vector>

namespace motioncam {

struct DNGFrameMetadata {
    size_t metadataBytes = 0;
    std::string uniqueCameraModel;
    double exposureTime = 0.0;
    double iso = 0.0;
    double baselineExposure = 0.0;
    std::array<float, 3> asShotNeutral = {1.0f, 1.0f, 1.0f};
    std::array<float, 4> blackLevel{};
    std::array<float, 4> whiteLevel{};
    uint32_t blackLevelCount = 0;
    uint32_t whiteLevelCount = 0;
    uint32_t inputBitDepth = 0;
    int orientation = -1;
    // Exact TIFF Orientation value (1..8). Unlike orientation, this retains
    // reflection and transpose information for Camera Native round-trips.
    uint16_t tiffOrientation = 0;
    std::array<float, 9> colorMatrix1{};
    std::array<float, 9> colorMatrix2{};
    std::array<float, 9> forwardMatrix1{};
    std::array<float, 9> forwardMatrix2{};
    std::array<float, 9> cameraCalibration1{};
    std::array<float, 9> cameraCalibration2{};
    uint16_t calibrationIlluminant1 = 0;
    uint16_t calibrationIlluminant2 = 0;
    bool hasExposure = false;
    bool hasBaselineExposure = false;
    bool hasAsShotNeutral = false;
    bool hasColorMatrix1 = false;
    bool hasColorMatrix2 = false;
    bool hasForwardMatrix1 = false;
    bool hasForwardMatrix2 = false;
    bool hasCameraCalibration1 = false;
    bool hasCameraCalibration2 = false;
};

struct PreviewFrame {
    std::vector<uint8_t> rgb;
    uint32_t width = 0;
    uint32_t height = 0;
    DNGFrameMetadata metadata;
    Timestamp timestamp = 0;
};

enum class DNGPixelLayout { CFA, LinearRGB, RGB };
enum class DNGStorageLayout { Strips, Tiles };

struct DNGImageLayout {
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t bitsPerSample = 0;
    uint32_t samplesPerPixel = 0;
    uint32_t compression = 0;
    uint32_t planarConfiguration = 1;
    DNGPixelLayout pixels = DNGPixelLayout::CFA;
    DNGStorageLayout storage = DNGStorageLayout::Strips;
    int cfaRepeatSize = 0;
    std::array<uint8_t, 4> cfaPhase{};
};

struct GainMap {
    uint32_t top, left, bottom, right;
    uint32_t coordinateWidth = 0, coordinateHeight = 0;
    uint32_t plane, planes;
    uint32_t rowPitch, colPitch;
    uint32_t width, height, channels;
    double spacingV, spacingH, originV, originH;
    std::vector<float> data;
};

// Storage-independent, unpacked image samples shared by DNG decoding,
// source processing, preview rendering, and DNG serialization.
struct DecodedDNGImage {
    std::vector<uint16_t> samples;
    DNGImageLayout layout;
    DNGFrameMetadata metadata;
    std::vector<GainMap> opcodeList2;
    std::vector<GainMap> opcodeList3;
    Timestamp timestamp = 0;
    bool linearizationApplied = false;
};

} // namespace motioncam
