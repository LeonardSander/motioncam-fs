#include "Utils.h"
#include "Measure.h"

#include "CameraFrameMetadata.h"
#include "CameraMetadata.h"

#include <algorithm>

#include <boost/iostreams/stream.hpp>
#include <boost/iostreams/device/back_inserter.hpp>

#define TINY_DNG_WRITER_IMPLEMENTATION 1

#include <tinydng/tiny_dng_writer.h>

namespace motioncam {
namespace utils {

namespace {
    enum DngIlluminant {
        lsUnknown					=  0,
        lsDaylight					=  1,
        lsFluorescent				=  2,
        lsTungsten					=  3,
        lsFlash						=  4,
        lsFineWeather				=  9,
        lsCloudyWeather				= 10,
        lsShade						= 11,
        lsDaylightFluorescent		= 12,		// D  5700 - 7100K
        lsDayWhiteFluorescent		= 13,		// N  4600 - 5500K
        lsCoolWhiteFluorescent		= 14,		// W  3800 - 4500K
        lsWhiteFluorescent			= 15,		// WW 3250 - 3800K
        lsWarmWhiteFluorescent		= 16,		// L  2600 - 3250K
        lsStandardLightA			= 17,
        lsStandardLightB			= 18,
        lsStandardLightC			= 19,
        lsD55						= 20,
        lsD65						= 21,
        lsD75						= 22,
        lsD50						= 23,
        lsISOStudioTungsten			= 24,

        lsOther						= 255
    };

    inline uint8_t ToTimecodeByte(int value)
    {
        return (((value / 10) << 4) | (value % 10));
    }

    unsigned short bitsNeeded(unsigned short value) {
        if (value == 0)
            return 1;

        unsigned short bits = 0;

        while (value > 0) {
            value >>= 1;
            bits++;
        }

        return bits;
    }

    int getColorIlluminant(const std::string& value) {
        if(value == "standarda")
            return lsStandardLightA;
        else if(value == "standardb")
            return lsStandardLightB;
        else if(value == "standardc")
            return lsStandardLightC;
        else if(value == "d50")
            return lsD50;
        else if(value == "d55")
            return lsD55;
        else if(value == "d65")
            return lsD65;
        else if(value == "d75")
            return lsD75;
        else
            return lsUnknown;
    }

    void normalizeShadingMap(std::vector<std::vector<float>>& shadingMap) {
        if (shadingMap.empty() || shadingMap[0].empty()) {
            return; // Handle empty case
        }

        // Find the maximum value
        float maxValue = 0.0f;
        for (const auto& row : shadingMap) {
            for (float value : row) {
                maxValue = std::max(maxValue, value);
            }
        }

        // Avoid division by zero
        if (maxValue == 0.0f) {
            return;
        }

        // Normalize all values
        for (auto& row : shadingMap) {
            for (float& value : row) {
                value /= maxValue;
            }
        }
    }

    void colorOnlyShadingMap(std::vector<std::vector<float>>& shadingMap, int lensShadingMapWidth, int lensShadingMapHeight) {
        if (shadingMap.empty() || shadingMap[0].empty())
            return; // Handle empty case

        // Find the maximum value
        float maxValue = 0.0f;
        for (const auto& row : shadingMap) 
            for (float value : row) 
                maxValue = std::max(maxValue, value);

        // Avoid division by zero
        if (maxValue == 0.0f) 
            return;

        bool aggressive = false;

        auto minValue00 = 10.0f;
        auto minValue01 = 10.0f;
        auto minValue10 = 10.0f;
        auto minValue11 = 10.0f;

        for(int j = 0; j < lensShadingMapHeight; j++) {
            for(int i = 0; i < lensShadingMapWidth; i++) {
                if(shadingMap[0][j*lensShadingMapWidth+i] < minValue00)
                    minValue00 = shadingMap[0][j*lensShadingMapWidth+i];
                if(shadingMap[1][j*lensShadingMapWidth+i] < minValue01)
                    minValue01 = shadingMap[1][j*lensShadingMapWidth+i];
                if(shadingMap[2][j*lensShadingMapWidth+i] < minValue10)
                    minValue10 = shadingMap[2][j*lensShadingMapWidth+i];
                if(shadingMap[3][j*lensShadingMapWidth+i] < minValue11)
                    minValue11 = shadingMap[3][j*lensShadingMapWidth+i];
        }}       // detect image-global white balance adjustment in shadingMap     
        
        for(int j = 0; j < lensShadingMapHeight; j++) {
            for(int i = 0; i < lensShadingMapWidth; i++) {
                if (aggressive) {
                    shadingMap[0][j*lensShadingMapWidth+i] = shadingMap[0][j*lensShadingMapWidth+i] / minValue00;   //cfa unaware
                    shadingMap[1][j*lensShadingMapWidth+i] = shadingMap[1][j*lensShadingMapWidth+i] / minValue01;
                    shadingMap[2][j*lensShadingMapWidth+i] = shadingMap[2][j*lensShadingMapWidth+i] / minValue10;
                    shadingMap[3][j*lensShadingMapWidth+i] = shadingMap[3][j*lensShadingMapWidth+i] / minValue11;
                }
                auto localMinValue = std::min(shadingMap[0][j*lensShadingMapWidth+i], std::min(shadingMap[1][j*lensShadingMapWidth+i], std::min(shadingMap[2][j*lensShadingMapWidth+i], shadingMap[3][j*lensShadingMapWidth+i])));
                for(int channel = 0; channel < 4; channel++) {
                    shadingMap[channel][j*lensShadingMapWidth+i] = shadingMap[channel][j*lensShadingMapWidth+i] / localMinValue;
                }
            }
        }       // For every position in the shading map, divide gain by the minimum value of the four channels
    }       

    inline float getShadingMapValue(
        float x, float y, int channel, const std::vector<std::vector<float>>& lensShadingMap, int lensShadingMapWidth, int lensShadingMapHeight)
    {
        // Clamp input coordinates to [0, 1] range
        x = std::max(0.0f, std::min(1.0f, x));
        y = std::max(0.0f, std::min(1.0f, y));

        // Convert normalized coordinates to map coordinates
        const float mapX = x * (lensShadingMapWidth - 1);
        const float mapY = y * (lensShadingMapHeight - 1);

        // Get integer coordinates for the four surrounding pixels
        const int x0 = static_cast<int>(std::floor(mapX));
        const int y0 = static_cast<int>(std::floor(mapY));
        const int x1 = std::min(x0 + 1, lensShadingMapWidth - 1);
        const int y1 = std::min(y0 + 1, lensShadingMapHeight - 1);

        // Calculate interpolation weights
        const float wx = mapX - x0;  // Weight for x-direction interpolation
        const float wy = mapY - y0;  // Weight for y-direction interpolation

        // Get the four surrounding pixel values
        const float val00 = lensShadingMap[channel][y0*lensShadingMapWidth+x0];  // Top-left
        const float val01 = lensShadingMap[channel][y0*lensShadingMapWidth+x1];  // Top-right
        const float val10 = lensShadingMap[channel][y1*lensShadingMapWidth+x0];  // Bottom-left
        const float val11 = lensShadingMap[channel][y1*lensShadingMapWidth+x1];  // Bottom-right

        // Perform bilinear interpolation
        const float valTop = val00 * (1.0f - wx) + val01 * wx;     // Interpolation at y0
        const float valBottom = val10 * (1.0f - wx) + val11 * wx;  // Interpolation at y1

        // Then interpolate along y-axis
        return valTop * (1.0f - wy) + valBottom * wy;
    }
}

void encodeTo10Bit(
    std::vector<uint8_t>& data,
    uint32_t& width,
    uint32_t& height)
{
    Measure m("encodeTo10Bit");

    uint16_t* srcPtr = reinterpret_cast<uint16_t*>(data.data());
    uint8_t* dstPtr = data.data();

    for(int y = 0; y < height; y++) {
        for(int x = 0; x < width; x+=4) {
            const uint16_t p0 = srcPtr[0];
            const uint16_t p1 = srcPtr[1];
            const uint16_t p2 = srcPtr[2];
            const uint16_t p3 = srcPtr[3];

            dstPtr[0] = p0 >> 2;
            dstPtr[1] = ((p0 & 0x03) << 6) | (p1 >> 4);
            dstPtr[2] = ((p1 & 0x0F) << 4) | (p2 >> 6);
            dstPtr[3] = ((p2 & 0x3F) << 2) | (p3 >> 8);
            dstPtr[4] = p3 & 0xFF;

            srcPtr += 4;
            dstPtr += 5;
        }
    }

    // Resize to fit new data
    auto newSize = dstPtr - data.data();

    data.resize(newSize);
}

void encodeTo12Bit(
    std::vector<uint8_t>& data,
    uint32_t& width,
    uint32_t& height)
{
    Measure m("encodeTo12Bit");

    uint16_t* srcPtr = reinterpret_cast<uint16_t*>(data.data());
    uint8_t* dstPtr = data.data();

    for(int y = 0; y < height; y++) {
        for(int x = 0; x < width; x+=2) {
            const uint16_t p0 = srcPtr[0];
            const uint16_t p1 = srcPtr[1];

            dstPtr[0] = p0 >> 4;
            dstPtr[1] = ((p0 & 0x0F) << 4) | (p1 >> 8);
            dstPtr[2] = p1 & 0xFF;

            srcPtr += 2;
            dstPtr += 3;
        }
    }
    // Resize to fit new data
    auto newSize = dstPtr - data.data();

    data.resize(newSize);
}

void encodeTo14Bit(
    std::vector<uint8_t>& data,
    uint32_t& width,
    uint32_t& height)
{
    Measure m("encodeTo14Bit");

    uint16_t* srcPtr = reinterpret_cast<uint16_t*>(data.data());
    uint8_t* dstPtr = data.data();

    for(int y = 0; y < height; y++) {
        for(int x = 0; x < width; x+=4) {
            const uint16_t p0 = srcPtr[0];
            const uint16_t p1 = srcPtr[1];
            const uint16_t p2 = srcPtr[2];
            const uint16_t p3 = srcPtr[3];

            dstPtr[0] = p0 >> 6;
            dstPtr[1] = ((p0 & 0x3F) << 2) | (p1 >> 12);
            dstPtr[2] = (p1 >> 4) & 0xFF;
            dstPtr[3] = ((p1 & 0x0F) << 4) | (p2 >> 10);
            dstPtr[4] = (p2 >> 2) & 0xFF;
            dstPtr[5] = ((p2 & 0x03) << 6) | (p3 >> 8);
            dstPtr[6] = p3 & 0xFF;

            srcPtr += 4;
            dstPtr += 7;
        }
    }

    // Resize to fit new data
    auto newSize = dstPtr - data.data();

    data.resize(newSize);
}

std::tuple<std::vector<uint8_t>, std::array<unsigned short, 4>, unsigned short> preprocessData(
    std::vector<uint8_t>& data,
    uint32_t& inOutWidth,
    uint32_t& inOutHeight,
    const CameraFrameMetadata& metadata,
    const CameraConfiguration& cameraConfiguration,
    const std::array<uint8_t, 4>& cfa,
    uint32_t scale,
    bool applyShadingMap=true,
    bool vignetteOnlyColor=false,
    bool normaliseShadingMap=false)
{
    if (scale > 1) {
        // Ensure even scale for downscaling
        scale = (scale / 2) * 2;
    }
    else {
        // No scaling
        scale = 1;
    }

    // Calculate new dimensions
    uint32_t newWidth = inOutWidth / scale;
    uint32_t newHeight = inOutHeight / scale;

    // Align to 4 for bayer pattern and also because we read 4 bytes at a time when encoding to 10/14 bit
    newWidth = (newWidth / 4) * 4;
    newHeight = (newHeight / 4) * 4;

    const auto& srcBlackLevel = cameraConfiguration.blackLevel;
    const float srcWhiteLevel = cameraConfiguration.whiteLevel;

    const std::array<float, 4> linear = {
        1.0f / (srcWhiteLevel - srcBlackLevel[0]),
        1.0f / (srcWhiteLevel - srcBlackLevel[1]),
        1.0f / (srcWhiteLevel - srcBlackLevel[2]),
        1.0f / (srcWhiteLevel - srcBlackLevel[3])
    };

    std::array<unsigned short, 4> dstBlackLevel = srcBlackLevel;
    float dstWhiteLevel = srcWhiteLevel;

    // Calculate shading map offsets
    auto lensShadingMap = metadata.lensShadingMap;

    const int fullWidth = metadata.originalWidth;
    const int fullHeight = metadata.originalHeight;

    const int left = (fullWidth - inOutWidth) / 2;
    const int top = (fullHeight - inOutHeight) / 2;

    const float shadingMapScaleX = 1.0f / static_cast<float>(fullWidth);
    const float shadingMapScaleY = 1.0f / static_cast<float>(fullHeight);

    // When applying shading map, increase precision
    if(applyShadingMap) {
        if(vignetteOnlyColor)
            colorOnlyShadingMap(lensShadingMap, metadata.lensShadingMapWidth, metadata.lensShadingMapHeight);
        if(normaliseShadingMap) {
            normalizeShadingMap(lensShadingMap);
            int useBits = std::min(16, bitsNeeded(static_cast<unsigned short>(cameraConfiguration.whiteLevel)) + 4);
            dstWhiteLevel = std::pow(2.0f, useBits) - 1;
            for(auto& v : dstBlackLevel)
                v <<= 4;
        } else {
            int useBits = std::min(16, bitsNeeded(static_cast<unsigned short>(cameraConfiguration.whiteLevel)) + 2);
            dstWhiteLevel = std::pow(2.0f, useBits) - 1;
            for(auto& v : dstBlackLevel)
                v <<= 2;
        }                 
    }

    //
    // Preprocess data
    //

    uint32_t originalWidth = inOutWidth;
    uint32_t dstOffset = 0;

    // Reinterpret the input data as uint16_t for reading
    uint16_t* srcData = reinterpret_cast<uint16_t*>(data.data());

    // Process the image by copying and packing 2x2 Bayer blocks
    std::array<float, 4> shadingMapVals { 1.0f, 1.0f, 1.0f, 1.0f };
    std::vector<uint8_t> dst;

    dst.resize(sizeof(uint16_t) * newWidth * newHeight);
    uint16_t* dstData = reinterpret_cast<uint16_t*>(dst.data());

    for (auto y = 0; y < newHeight; y += 2) {
        for (auto x = 0; x < newWidth; x += 2) {
            // Get the source coordinates (scaled)
            uint32_t srcY = y * scale;
            uint32_t srcX = x * scale;

            auto s0 = srcData[srcY * originalWidth + srcX];
            auto s1 = srcData[srcY * originalWidth + srcX + 1];
            auto s2 = srcData[(srcY + 1) * originalWidth + srcX];
            auto s3 = srcData[(srcY + 1) * originalWidth + srcX + 1];

            if(applyShadingMap) {
                // Calculate position in shading map
                const float sx = (srcX + left) * shadingMapScaleX;
                const float sy = (srcY + top) * shadingMapScaleY;

                // Calculate shading map
                shadingMapVals = {
                    getShadingMapValue(sx, sy, 0, lensShadingMap, metadata.lensShadingMapWidth, metadata.lensShadingMapHeight),
                    getShadingMapValue(sx, sy, 1, lensShadingMap, metadata.lensShadingMapWidth, metadata.lensShadingMapHeight),
                    getShadingMapValue(sx, sy, 2, lensShadingMap, metadata.lensShadingMapWidth, metadata.lensShadingMapHeight),
                    getShadingMapValue(sx, sy, 3, lensShadingMap, metadata.lensShadingMapWidth, metadata.lensShadingMapHeight)
                };
            }

            // Linearize and (maybe) apply shading map
            const float p0 = std::max(0.0f, linear[0] * (s0 - srcBlackLevel[0]) * shadingMapVals[cfa[0]]) * (dstWhiteLevel - dstBlackLevel[0]);
            const float p1 = std::max(0.0f, linear[1] * (s1 - srcBlackLevel[1]) * shadingMapVals[cfa[1]]) * (dstWhiteLevel - dstBlackLevel[1]);
            const float p2 = std::max(0.0f, linear[2] * (s2 - srcBlackLevel[2]) * shadingMapVals[cfa[2]]) * (dstWhiteLevel - dstBlackLevel[2]);
            const float p3 = std::max(0.0f, linear[3] * (s3 - srcBlackLevel[3]) * shadingMapVals[cfa[3]]) * (dstWhiteLevel - dstBlackLevel[3]);

            s0 = std::clamp(std::round((p0 + dstBlackLevel[0])), 0.f, dstWhiteLevel);
            s1 = std::clamp(std::round((p1 + dstBlackLevel[1])), 0.f, dstWhiteLevel);
            s2 = std::clamp(std::round((p2 + dstBlackLevel[2])), 0.f, dstWhiteLevel);
            s3 = std::clamp(std::round((p3 + dstBlackLevel[3])), 0.f, dstWhiteLevel);

            // Copy the 2x2 Bayer block
            dstData[dstOffset]                 = static_cast<unsigned short>(s0);
            dstData[dstOffset + 1]             = static_cast<unsigned short>(s1);
            dstData[dstOffset + newWidth]      = static_cast<unsigned short>(s2);
            dstData[dstOffset + newWidth + 1]  = static_cast<unsigned short>(s3);

            dstOffset += 2;
        }

        dstOffset += newWidth;
    }

    // Update dimensions
    inOutWidth = newWidth;
    inOutHeight = newHeight;

    return std::make_tuple(dst, dstBlackLevel, static_cast<unsigned short>(dstWhiteLevel));
}

std::shared_ptr<std::vector<char>> generateDng(
    std::vector<uint8_t>& data,
    const CameraFrameMetadata& metadata,
    const CameraConfiguration& cameraConfiguration,
    float recordingFps,
    int frameNumber,
    FileRenderOptions options,
    int scale)
{
    Measure m("generateDng");

    unsigned int width = metadata.width;
    unsigned int height = metadata.height;

    std::array<uint8_t, 4> cfa;

    if(cameraConfiguration.sensorArrangement == "rggb")
        cfa = { 0, 1, 1, 2 };
    else if(cameraConfiguration.sensorArrangement == "bggr")
        cfa = { 2, 1, 1, 0 };
    else if(cameraConfiguration.sensorArrangement == "grbg")
        cfa = { 1, 0, 2, 1 };
    else if(cameraConfiguration.sensorArrangement == "gbrg")
        cfa = { 1, 2, 0, 1 };
    else
        throw std::runtime_error("Invalid sensor arrangement");

    // Scale down if requested
    bool applyShadingMap = options & RENDER_OPT_APPLY_VIGNETTE_CORRECTION;
    bool normalizeShadingMap = options & RENDER_OPT_NORMALIZE_SHADING_MAP;
    bool vignetteOnlyColor = options & RENDER_OPT_VIGNETTE_ONLY_COLOR;

    auto [processedData, dstBlackLevel, dstWhiteLevel] = utils::preprocessData(
        data,
        width, height,
        metadata,
        cameraConfiguration,
        cfa,
        scale,
        applyShadingMap, vignetteOnlyColor, normalizeShadingMap);

    spdlog::debug("New black level {},{},{},{} and white level {}",
                  dstBlackLevel[0], dstBlackLevel[1], dstBlackLevel[2], dstBlackLevel[3], dstWhiteLevel);

    // Encode to reduce size in container
    auto encodeBits = bitsNeeded(dstWhiteLevel);

    if(encodeBits <= 10) {
        utils::encodeTo10Bit(processedData, width, height);
        encodeBits = 10;
    }
    else if(encodeBits <= 12) {
        utils::encodeTo12Bit(processedData, width, height);
        encodeBits = 12;
    }
    else if(encodeBits <= 14) {
        utils::encodeTo14Bit(processedData, width, height);
        encodeBits = 14;
    }
    else {
        encodeBits = 16;
    }

    // Create first frame
    tinydngwriter::DNGImage dng;

    dng.SetBigEndian(false);
    dng.SetDNGVersion(1, 4, 0, 0);
    dng.SetDNGBackwardVersion(1, 1, 0, 0);
    dng.SetImageData(reinterpret_cast<const unsigned char*>(processedData.data()), processedData.size());
    dng.SetImageWidth(width);
    dng.SetImageLength(height);
    dng.SetPlanarConfig(tinydngwriter::PLANARCONFIG_CONTIG);
    dng.SetPhotometric(tinydngwriter::PHOTOMETRIC_CFA);
    dng.SetRowsPerStrip(height);
    dng.SetSamplesPerPixel(1);
    dng.SetCFARepeatPatternDim(2, 2);

    dng.SetBlackLevelRepeatDim(2, 2);
    dng.SetBlackLevel(4, dstBlackLevel.data());
    dng.SetWhiteLevel(dstWhiteLevel);
    dng.SetCompression(tinydngwriter::COMPRESSION_NONE);

    dng.SetIso(metadata.iso);
    dng.SetExposureTime(metadata.exposureTime / 1e9);
    dng.SetBaselineExposure(1.5f); 

    dng.SetCFAPattern(4, cfa.data());

    // Time code
    float time = frameNumber / recordingFps;

    int hours = (int) floor(time / 3600);
    int minutes = ((int) floor(time / 60)) % 60;
    int seconds = ((int) floor(time)) % 60;
    int frames = recordingFps > 1 ? (frameNumber % ((int) round(recordingFps))) : 0;

    std::vector<uint8_t> timeCode(8);

    timeCode[0] = ToTimecodeByte(frames) & 0x3F;
    timeCode[1] = ToTimecodeByte(seconds) & 0x7F;
    timeCode[2] = ToTimecodeByte(minutes) & 0x7F;
    timeCode[3] = ToTimecodeByte(hours) & 0x3F;

    dng.SetTimeCode(timeCode.data());
    dng.SetFrameRate(recordingFps);

    // Rectangular
    dng.SetCFALayout(1);

    const uint16_t bps[1] = { encodeBits };
    dng.SetBitsPerSample(1, bps);

    dng.SetColorMatrix1(3, cameraConfiguration.colorMatrix1.data());
    dng.SetColorMatrix2(3, cameraConfiguration.colorMatrix2.data());

    dng.SetForwardMatrix1(3, cameraConfiguration.forwardMatrix1.data());
    dng.SetForwardMatrix2(3, cameraConfiguration.forwardMatrix2.data());

    dng.SetAsShotNeutral(3, metadata.asShotNeutral.data());

    dng.SetCalibrationIlluminant1(getColorIlluminant(cameraConfiguration.colorIlluminant1));
    dng.SetCalibrationIlluminant2(getColorIlluminant(cameraConfiguration.colorIlluminant2));

    // Additional information
    const auto software = "MotionCam Tools";

    dng.SetSoftware(software);
    dng.SetUniqueCameraModel(cameraConfiguration.extraData.postProcessSettings.metadata.buildModel);

    // Set data
    dng.SetSubfileType();

    const uint32_t activeArea[4] = { 0, 0, height, width };
    dng.SetActiveArea(&activeArea[0]);

    // Write DNG
    std::string err;

    tinydngwriter::DNGWriter writer(false);

    writer.AddImage(&dng);

    // Save to memory
    auto output = std::make_shared<std::vector<char>>();

    // Reserve enough to fit the data
    output->reserve(width*height*sizeof(uint16_t) + 512*1024);

    boost::iostreams::back_insert_device<std::vector<char>> sink(*output);
    boost::iostreams::stream<boost::iostreams::back_insert_device<std::vector<char>>> stream(sink);

    writer.WriteToFile(stream, &err);

    return output;
}

int gcd(int a, int b) {
    while (b != 0) {
        int temp = b;
        b = a % b;
        a = temp;
    }
    return a;
}

std::pair<int, int> toFraction(float frameRate, int base) {
    // Handle invalid input
    if (frameRate <= 0) {
        return std::make_pair(0, 1);
    }

    // For frame rates, we want numerator/denominator where denominator is close to base
    // This gives us precise ratios like 30000/1001 for 29.97 fps

    int numerator = static_cast<int>(std::round(frameRate * base));
    int denominator = base;

    // Reduce to lowest terms
    int divisor = gcd(numerator, denominator);
    numerator /= divisor;
    denominator /= divisor;

    return std::make_pair(numerator, denominator);
}

} // namespace utils
} // namespace motioncam
