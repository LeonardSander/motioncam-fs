#pragma once

#include <iostream>
#include <vector>
#include <streambuf>
#include <ostream>
#include <algorithm>
#include <memory>
#include <optional>
#include <array>
#include <cstdint>

#include "Types.h"
#include "CalibrationData.h"

namespace tinydngwriter {
    class OpcodeList;
}

namespace motioncam {

struct CameraFrameMetadata;
struct CameraConfiguration;

namespace utils {

// ============================================================================
// Stream Utilities
// ============================================================================

class vectorbuf : public std::streambuf {
private:
    std::vector<char>& vec_;
    friend class vector_ostream;

public:
    explicit vectorbuf(std::vector<char>& vec);

protected:
    virtual int_type overflow(int_type c) override;
    virtual std::streamsize xsputn(const char* s, std::streamsize count) override;
    virtual pos_type seekoff(off_type off, std::ios_base::seekdir way,
                             std::ios_base::openmode which = std::ios_base::in | std::ios_base::out) override;
    virtual pos_type seekpos(pos_type sp, std::ios_base::openmode which = std::ios_base::in | std::ios_base::out) override;
};

class vector_ostream : public std::ostream {
private:
    vectorbuf buf_;

public:
    explicit vector_ostream(std::vector<char>& vec);
    std::vector<char>& vector();
    const std::vector<char>& vector() const;
    std::streampos tell();
    vector_ostream& seek(std::streampos pos);
    vector_ostream& seek_relative(std::streamoff off);
    vector_ostream& seek_from_end(std::streamoff off);
};

// ============================================================================
// Bit Depth Utilities
// ============================================================================

unsigned short bitsNeeded(unsigned short value);

// ============================================================================
// Bit Encoding Functions (Bayer/Single-Channel)
// ============================================================================

void encodeTo2Bit(std::vector<uint8_t>& data, uint32_t& width, uint32_t& height);
void encodeTo4Bit(std::vector<uint8_t>& data, uint32_t& width, uint32_t& height);
void encodeTo6Bit(std::vector<uint8_t>& data, uint32_t& width, uint32_t& height);
void encodeTo8Bit(std::vector<uint8_t>& data, uint32_t& width, uint32_t& height);
void encodeTo10Bit(std::vector<uint8_t>& data, uint32_t& width, uint32_t& height);
void encodeTo12Bit(std::vector<uint8_t>& data, uint32_t& width, uint32_t& height);
void encodeTo14Bit(std::vector<uint8_t>& data, uint32_t& width, uint32_t& height);

// ============================================================================
// Shading Map Operations
// ============================================================================

void normalizeShadingMap(std::vector<std::vector<float>>& shadingMap);
void invertShadingMap(std::vector<std::vector<float>>& shadingMap);
void colorOnlyShadingMap(
    std::vector<std::vector<float>>& shadingMap,
    int lensShadingMapWidth,
    int lensShadingMapHeight,
    const std::array<uint8_t, 4> cfa);

float getShadingMapValue(
    float x, float y,
    int channel,
    const std::vector<std::vector<float>>& lensShadingMap,
    int lensShadingMapWidth,
    int lensShadingMapHeight);

// ============================================================================
// DNG Generation
// ============================================================================

tinydngwriter::OpcodeList createLensShadingOpcodeList(
    const CameraFrameMetadata& metadata,
    uint32_t imageWidth,
    uint32_t imageHeight,
    int left = 0,
    int top = 0);

std::tuple<std::vector<uint8_t>, std::array<unsigned short, 4>, unsigned short, tinydngwriter::OpcodeList> 
preprocessData(
    std::vector<uint8_t>& data,
    uint32_t& inOutWidth,
    uint32_t& inOutHeight,
    const CameraFrameMetadata& metadata,
    const CameraConfiguration& cameraConfiguration,
    const std::array<uint8_t, 4>& cfa,
    uint32_t scale,
    bool applyShadingMap,
    bool vignetteOnlyColor,
    bool normaliseShadingMap,
    bool debugShadingMap,
    bool interpretAsQuadBayer,
    std::string cropTarget,
    std::string levels,
    std::string logTransform,
    std::string quadBayerOption,
    bool includeOpcode);

std::shared_ptr<std::vector<char>> generateDng(
    std::vector<uint8_t>& data,
    const CameraFrameMetadata& metadata,
    const CameraConfiguration& cameraConfiguration,
    float recordingFps,
    int frameNumber,
    FileRenderOptions options,
    int scale,
    double baselineExpValue,
    std::string cropTarget,
    std::string camModel,
    std::string levels,
    std::string logTransform,
    std::string exposureCompensation,
    std::string quadBayerOption,
    const std::optional<CalibrationData>& calibration = std::nullopt,
    std::string cfaPhase = ""
);

// ============================================================================
// Utility Functions
// ============================================================================

std::pair<int, int> toFraction(float frameRate, int base = 1000);

void remosaicRGBToBayer(
    const std::vector<uint16_t>& rgbData,
    std::vector<uint16_t>& bayerData,
    int width,
    int height,
    const std::string& cfaPhase = "bggr");

} // namespace utils
} // namespace motioncam
