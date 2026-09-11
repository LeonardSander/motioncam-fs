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
    struct GainMapParams;
}

namespace motioncam {

struct CameraFrameMetadata;
struct CameraConfiguration;
struct GainMap;

struct DngFrameProcessingPlan {
    int cfaRepeatSize = 2;
    int draftScale = 1;
    int preprocessScale = 1;
    bool higherCfa = false;
    bool explicitBinning = false;
    bool hqProxy = false;
    bool demosaic = false;
    bool remosaic = false;
    LogTransformMode logTransform = LogTransformMode::Disabled;
};
struct PreviewFrame;

namespace utils {

void overrideLensShadingMap(
    CameraFrameMetadata& metadata, const std::vector<GainMap>& gainMaps);

// Parse WIDTHxHEIGHT or WIDTHxHEIGHT_STRIDE. Returns zero values for invalid
// components; STRIDE is a pixel count, not a byte count.
void parseCropTarget(const std::string& target, uint32_t& width,
                     uint32_t& height, uint32_t& stride);

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
struct DngOutputLevels {
    double white = 65535.0;
    std::array<double, 4> black{};
};
DngOutputLevels planDngOutputLevels(
    double sourceWhite, const std::array<double, 4>& sourceBlack,
    bool applyShadingMap, bool normalizeShadingMap, bool debugShadingMap,
    LogTransformMode logTransform);
DngFrameProcessingPlan planDngFrameProcessing(
    const RenderSettings& settings, const CameraFrameMetadata& metadata,
    const std::optional<CalibrationData>& calibration);
uint32_t dngPackedBits(uint16_t whiteLevel, bool rgb, bool cameraNativeStaging);

void addSinglePlaneGainMaps(tinydngwriter::OpcodeList& opcodeList,
                            const tinydngwriter::GainMapParams& params,
                            bool cfaPhases);

// ============================================================================
// Bit Encoding Functions (RGB/Multi-Channel)
// ============================================================================

void encodeRGBTo4Bit(std::vector<uint8_t>& data, uint32_t& width, uint32_t& height);
void encodeRGBTo6Bit(std::vector<uint8_t>& data, uint32_t& width, uint32_t& height);
void encodeRGBTo8Bit(std::vector<uint8_t>& data, uint32_t& width, uint32_t& height);
void encodeRGBTo10Bit(std::vector<uint8_t>& data, uint32_t& width, uint32_t& height);
void encodeRGBTo12Bit(std::vector<uint8_t>& data, uint32_t& width, uint32_t& height);
void encodeRGBTo14Bit(std::vector<uint8_t>& data, uint32_t& width, uint32_t& height);

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
    int top = 0,
    unsigned int targetPlanes = 1);

std::tuple<std::vector<uint8_t>, std::array<unsigned short, 4>, unsigned short,
           tinydngwriter::OpcodeList, tinydngwriter::OpcodeList>
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
    bool optimizeGainMaps,
    float& gainMapExposureOffset,
    uint32_t cfaRepeatSize,
    bool higherCfaHq,
    bool interpretAsQuadBayer,
    std::string cropTarget,
    std::string levels,
    LogTransformMode logTransform,
    QuadBayerMode quadBayerOption,
    bool includeOpcode);

std::shared_ptr<std::vector<char>> generateDng(
    std::vector<uint8_t>& data,
    const CameraFrameMetadata& metadata,
    const CameraConfiguration& cameraConfiguration,
    float recordingFps,
    int frameNumber,
    double baselineExpValue,
    const RenderSettings& settings,
    const std::optional<CalibrationData>& calibration = std::nullopt,
    bool compressionEnabled = false,
    const std::optional<float>& baselineExposureOverride = std::nullopt,
    const std::optional<std::array<float, 3>>& asShotNeutralOverride = std::nullopt,
    PreviewFrame* previewFrame = nullptr);

// Draws a centered, outlined ISO label into unpacked 16-bit image samples.
void bakeIsoOverlay(uint16_t* samples, uint32_t width, uint32_t height,
                    uint32_t channels, double iso, uint16_t black, uint16_t white);

// ============================================================================
// Utility Functions
// ============================================================================

std::pair<int, int> toFraction(float frameRate, int base = 1000);

std::vector<unsigned short> makeLogLinearizationTable(unsigned int storedWhiteLevel);

void remosaicRGBToBayer(
    const std::vector<uint16_t>& rgbData,
    std::vector<uint16_t>& bayerData,
    int width,
    int height,
    const std::string& cfaPhase = "bggr");

// Reduces interleaved RGB by an integer factor. HQ box-averages every source
// pixel; LQ retains one representative pixel per block.
void reduceRGB(
    const std::vector<uint16_t>& input,
    std::vector<uint16_t>& output,
    uint32_t width,
    uint32_t height,
    uint32_t scale,
    bool highQuality,
    uint32_t& outputWidth,
    uint32_t& outputHeight,
    uint16_t logWhiteLevel = 0);

// Center-crops interleaved image samples. Returns false when the requested
// crop is empty or exceeds the source image.
bool cropInterleaved(
    const std::vector<uint16_t>& input,
    std::vector<uint16_t>& output,
    uint32_t width,
    uint32_t height,
    uint32_t channels,
    uint32_t cropWidth,
    uint32_t cropHeight);

void encodeLog60(
    std::vector<uint16_t>& samples,
    uint32_t width,
    uint32_t height,
    uint32_t channels,
    const std::array<double, 4>& blackLevel,
    double whiteLevel,
    uint16_t encodedWhite);

// Averages each 2x2 same-colour block of a 4x4 quad-Bayer image into one
// sample, producing an ordinary 2x2 Bayer mosaic at half resolution.
void binQuadBayer(
    const std::vector<uint16_t>& input,
    std::vector<uint16_t>& output,
    uint32_t width,
    uint32_t height,
    uint32_t& outputWidth,
    uint32_t& outputHeight,
    uint16_t logWhiteLevel = 0);

// Averages contiguous same-colour blocks in a higher CFA. A factor equal to
// half the CFA repeat produces ordinary Bayer; factor 2 turns 8x8 into 4x4.
void binHigherCFA(
    const std::vector<uint16_t>& input,
    std::vector<uint16_t>& output,
    uint32_t width,
    uint32_t height,
    uint32_t factor,
    uint32_t& outputWidth,
    uint32_t& outputHeight,
    uint16_t logWhiteLevel = 0);

// Edge- and luma-guided Bayer/higher-CFA demosaic. Color reconstructs green
// first, interpolates R-G/B-G, and removes coherent 2x2 detail-gain errors;
// OCL compensates pixels sharing an on-sensor lens.
void demosaicHigherCFA(
    const std::vector<uint16_t>& cfaData,
    std::vector<uint16_t>& rgbData,
    int width,
    int height,
    int cfaRepeatSize,
    const std::array<uint8_t, 4>& bayerPhase,
    QuadBayerMode mode,
    const std::array<float, 3>& channelBlack = {});

// Shared CFA-to-RGB boundary used by DNG and native-source previews. Callers
// select nearest-colour reconstruction whenever gallery HQ is disabled.
void demosaicCfaForOutput(
    const std::vector<uint16_t>& cfaData,
    std::vector<uint16_t>& rgbData,
    int width,
    int height,
    int cfaRepeatSize,
    const std::array<uint8_t, 4>& bayerPhase,
    QuadBayerMode mode,
    const std::array<float, 3>& channelBlack,
    bool nearestColour);

bool normalizeRgb16(
    const std::vector<uint16_t>& input,
    std::vector<uint16_t>& output,
    const std::array<double, 3>& black,
    const std::array<double, 3>& white);

bool normalizeRgb16Bytes(
    const std::vector<uint16_t>& input,
    std::vector<uint8_t>& output,
    const std::array<double, 3>& black,
    const std::array<double, 3>& white);

} // namespace utils
} // namespace motioncam
