#include "Utils.h"
#include "Measure.h"

#include "CameraFrameMetadata.h"
#include "CameraMetadata.h"

#include <algorithm>
#include <cmath>

#include <boost/iostreams/stream.hpp>
#include <boost/iostreams/device/back_inserter.hpp>

#define TINY_DNG_WRITER_IMPLEMENTATION 1

#include <tinydng/tiny_dng_writer.h>

namespace motioncam {
namespace utils {

namespace {
    const float IDENTITY_MATRIX[9] = {
        1.0f, 0.0f, 0.0f,
        0.0f, 1.0f, 0.0f,
        0.0f, 0.0f, 1.0f
    };

    bool isZeroMatrix(const std::array<float, 9>& matrix) {
        for (const auto& value : matrix) 
            if (value != 0.0f) 
                return false;
        return true;
    }

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

    enum DngOrientation
    {
        kNormal		 = 1,
        kMirror		 = 2,
        kRotate180	 = 3,
        kMirror180	 = 4,
        kMirror90CCW = 5,
        kRotate90CW	 = 6,
        kMirror90CW	 = 7,
        kRotate90CCW = 8,
        kUnknown	 = 9
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

    void invertShadingMap(std::vector<std::vector<float>>& shadingMap) {
        if (shadingMap.empty() || shadingMap[0].empty()) 
            return;                                 // Handle empty case
        
        for (const auto& row : shadingMap) 
            for (float value : row) 
                if (value <= 0.0f) 
                    return;                             // Avoid division by zero
                  
        for (auto& row : shadingMap) 
            for (float& value : row) 
                value = 1 / value;          // Normalize all values
    }

    void colorOnlyShadingMap(std::vector<std::vector<float>>& shadingMap, int lensShadingMapWidth, int lensShadingMapHeight, const std::array<uint8_t, 4> cfa) {
        if (shadingMap.empty() || shadingMap[0].empty())
            return; // Handle empty case

        float maxValue = 0.0f;

        for (const auto& row : shadingMap) 
            for (float value : row) 
                maxValue = std::max(maxValue, value);
        
        if (maxValue == 0.0f)   // Avoid division by zero
            return;

        bool aggressive = false;            //TODO: add ui option for aggressive color fix reduction that if effective breaks awb and might not improve highlight reconstruction

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
        }}       

        if (cfa == std::array<uint8_t, 4>{0, 1, 1, 2} || cfa == std::array<uint8_t, 4>{2, 1, 1, 0}) {
            minValue01 = std::min(minValue01, minValue10);
            minValue01 = minValue10;
        } else if (cfa == std::array<uint8_t, 4>{1, 0, 2, 1} || cfa == std::array<uint8_t, 4>{1, 2, 0, 1}) {
            minValue00 = std::min(minValue00, minValue11);
            minValue00 = minValue11;
        }   
        
        for(int j = 0; j < lensShadingMapHeight; j++) {
            for(int i = 0; i < lensShadingMapWidth; i++) {
                if (aggressive) {                               // remove image-global white balance adjustment in shadingMap     
                    shadingMap[0][j*lensShadingMapWidth+i] = shadingMap[0][j*lensShadingMapWidth+i] / minValue00;   
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

void encodeTo8Bit(
    std::vector<uint8_t>& data,
    uint32_t& width,
    uint32_t& height)
{
    Measure m("encodeTo8Bit");

    uint16_t* srcPtr = reinterpret_cast<uint16_t*>(data.data());
    uint8_t* dstPtr = data.data();

    for(int y = 0; y < height; y++) {
        for(int x = 0; x < width; x++) {
            const uint16_t p0 = srcPtr[0];
            // Store lower 8 bits directly
            dstPtr[0] = p0 & 0xFF;

            srcPtr += 1;
            dstPtr += 1;
        }
    }

    // Resize to fit new data
    auto newSize = dstPtr - data.data();

    data.resize(newSize);
}

void encodeTo6Bit(
    std::vector<uint8_t>& data,
    uint32_t& width,
    uint32_t& height)
{
    Measure m("encodeTo6Bit");

    uint16_t* srcPtr = reinterpret_cast<uint16_t*>(data.data());
    uint8_t* dstPtr = data.data();

    for(int y = 0; y < height; y++) {
        for(int x = 0; x < width; x+=4) {
            const uint16_t p0 = srcPtr[0];
            const uint16_t p1 = srcPtr[1];
            const uint16_t p2 = srcPtr[2];
            const uint16_t p3 = srcPtr[3];

            // Pack 4 pixels (6 bits each) into 3 bytes - use lower 6 bits
            const uint8_t v0 = p0 & 0x3F;
            const uint8_t v1 = p1 & 0x3F;
            const uint8_t v2 = p2 & 0x3F;
            const uint8_t v3 = p3 & 0x3F;

            dstPtr[0] = (v0 << 2) | (v1 >> 4);
            dstPtr[1] = ((v1 & 0x0F) << 4) | (v2 >> 2);
            dstPtr[2] = ((v2 & 0x03) << 6) | v3;

            srcPtr += 4;
            dstPtr += 3;
        }
    }

    // Resize to fit new data
    auto newSize = dstPtr - data.data();

    data.resize(newSize);
}

void encodeTo4Bit(
    std::vector<uint8_t>& data,
    uint32_t& width,
    uint32_t& height)
{
    Measure m("encodeTo4Bit");

    uint16_t* srcPtr = reinterpret_cast<uint16_t*>(data.data());
    uint8_t* dstPtr = data.data();

    for(int y = 0; y < height; y++) {
        for(int x = 0; x < width; x+=2) {
            const uint16_t p0 = srcPtr[0];
            const uint16_t p1 = srcPtr[1];

            // Pack 2 pixels (4 bits each) into 1 byte - use lower 4 bits
            const uint8_t v0 = p0 & 0x0F;
            const uint8_t v1 = p1 & 0x0F;

            dstPtr[0] = (v0 << 4) | v1;

            srcPtr += 2;
            dstPtr += 1;
        }
    }

    // Resize to fit new data
    auto newSize = dstPtr - data.data();

    data.resize(newSize);
}

void encodeTo2Bit(
    std::vector<uint8_t>& data,
    uint32_t& width,
    uint32_t& height)
{
    Measure m("encodeTo2Bit");

    uint16_t* srcPtr = reinterpret_cast<uint16_t*>(data.data());
    uint8_t* dstPtr = data.data();

    for(int y = 0; y < height; y++) {
        for(int x = 0; x < width; x+=4) {
            const uint16_t p0 = srcPtr[0];
            const uint16_t p1 = srcPtr[1];
            const uint16_t p2 = srcPtr[2];
            const uint16_t p3 = srcPtr[3];

            // Try different bit order: p3 in bits 1-0, p2 in bits 3-2, p1 in bits 5-4, p0 in bits 7-6
            dstPtr[0] = ((p0 & 0x03) << 6) | 
                       ((p1 & 0x03) << 4) | 
                       ((p2 & 0x03) << 2) | 
                       (p3 & 0x03);

            srcPtr += 4;
            dstPtr += 1;
        }
    }

    // Resize to fit new data
    auto newSize = dstPtr - data.data();

    data.resize(newSize);
}


tinydngwriter::OpcodeList createLensShadingOpcodeList(
    const CameraFrameMetadata& metadata,
    uint32_t imageWidth,
    uint32_t imageHeight,
    int left = 0,
    int top = 0)
{
    tinydngwriter::OpcodeList opcodeList;
    
    if (metadata.lensShadingMap.empty() || 
        metadata.lensShadingMapWidth <= 0 || 
        metadata.lensShadingMapHeight <= 0) {
        return opcodeList; // Return empty list if no shading map
    }
    
    // Build a gain map opcode compatible with DNG OpcodeList2 GainMap
    tinydngwriter::GainMapParams gainParams;
    
    // Set the area to apply the gain map (active image area)
    // Use provided left/top offsets if the active area is a sub-rectangle
    gainParams.top = static_cast<unsigned int>(std::max(0, top));
    gainParams.left = static_cast<unsigned int>(std::max(0, left));
    gainParams.bottom = static_cast<unsigned int>(std::max<int>(0, top) + imageHeight);
    gainParams.right = static_cast<unsigned int>(std::max<int>(0, left) + imageWidth);
    
    // Apply starting from plane 0
    gainParams.plane = 0;
    // Determine number of planes available in the shading map (expect 4 for Bayer)
    unsigned int availablePlanes = static_cast<unsigned int>(metadata.lensShadingMap.size());
    if (availablePlanes == 0) availablePlanes = 1;
    if (availablePlanes >= 4) {
        gainParams.planes = 4;
    } else if (availablePlanes >= 3) {
        gainParams.planes = 3;
    } else {
        gainParams.planes = 1;
    }
    
    // Grid size in the gain map
    const unsigned int mapPointsV = static_cast<unsigned int>(metadata.lensShadingMapHeight);
    const unsigned int mapPointsH = static_cast<unsigned int>(metadata.lensShadingMapWidth);
    gainParams.map_points_v = mapPointsV;
    gainParams.map_points_h = mapPointsH;
    
    // Compute pixel pitch between adjacent map points in rows/cols (in pixels)
    // If only a single point along a dimension, pitch covers the full extent
    const unsigned int imageRows = imageHeight;
    const unsigned int imageCols = imageWidth;
    unsigned int rowPitch = (mapPointsV > 1)
        ? static_cast<unsigned int>(std::max(1u, (imageRows - 1) / (mapPointsV - 1)))
        : imageRows;
    unsigned int colPitch = (mapPointsH > 1)
        ? static_cast<unsigned int>(std::max(1u, (imageCols - 1) / (mapPointsH - 1)))
        : imageCols;
    gainParams.row_pitch = rowPitch;
    gainParams.col_pitch = colPitch;
    
    // Map spacing and origin in relative coordinates
    // Spacing is relative pitch to image size; origin is relative to active area
    gainParams.map_spacing_v = (imageRows > 0) ? static_cast<double>(rowPitch) / static_cast<double>(imageRows) : 0.0;
    gainParams.map_spacing_h = (imageCols > 0) ? static_cast<double>(colPitch) / static_cast<double>(imageCols) : 0.0;
    gainParams.map_origin_v = (imageRows > 0) ? static_cast<double>(std::max(0, top)) / static_cast<double>(imageRows) : 0.0;
    gainParams.map_origin_h = (imageCols > 0) ? static_cast<double>(std::max(0, left)) / static_cast<double>(imageCols) : 0.0;
    
    // Number of planes in the gain map payload (match planes when available)
    gainParams.map_planes = gainParams.planes;
    
    // Fill gain data in plane-major, row-major order
    if (!metadata.lensShadingMap.empty() && !metadata.lensShadingMap[0].empty()) {
        const size_t perPlaneSize = static_cast<size_t>(mapPointsV) * static_cast<size_t>(mapPointsH);
        const size_t expectedSize = perPlaneSize * static_cast<size_t>(gainParams.map_planes);
        gainParams.gain_data.reserve(expectedSize);

        for (unsigned int p = 0; p < gainParams.map_planes; ++p) {
            const unsigned int srcPlane = (p < metadata.lensShadingMap.size()) ? p : 0;
            for (unsigned int v = 0; v < mapPointsV; ++v) {
                for (unsigned int h = 0; h < mapPointsH; ++h) {
                    const size_t index = static_cast<size_t>(v) * mapPointsH + h;
                    float gain = 1.0f;
                    if (index < metadata.lensShadingMap[srcPlane].size()) {
                        gain = metadata.lensShadingMap[srcPlane][index];
                        if (!std::isfinite(gain) || gain <= 0.0f) {
                            gain = 1.0f;
                        } else if (gain > 16.0f) {
                            gain = 16.0f; // broader but safe upper bound
                        }
                    }
                    gainParams.gain_data.push_back(gain);
                }
            }
        }

        // Only add the gain map if we have valid data size
        if (gainParams.gain_data.size() == expectedSize) {
            opcodeList.AddGainMap(gainParams);
        }
    }
    
    return opcodeList;
}

std::tuple<std::vector<uint8_t>, std::array<unsigned short, 4>, unsigned short, tinydngwriter::OpcodeList> preprocessData(
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
    std::string cropTarget,
    std::string levels,
    std::string logTransform,
    bool includeOpcode)
{
    if (scale > 1) {
        // Ensure even scale for downscaling
        scale = (scale / 2) * 2;
    }
    else {
        // No scaling
        scale = 1;
    }

    uint32_t cfaSize = (metadata.needRemosaic ? 2 : 1); //TODO: add ui. assume quadbayer for now

    uint32_t newWidth, newHeight;
    uint32_t cropWidth = 0, cropHeight = 0;

    if (!cropTarget.empty()) {
        const size_t separatorPos = cropTarget.find('x');
        if (separatorPos != std::string::npos) {
            try {
                cropWidth = std::stoul(cropTarget.substr(0, separatorPos));
                cropHeight = std::stoul(cropTarget.substr(separatorPos + 1));
            } catch (const std::exception&) {
                // Ignore invalid crop target
                cropWidth = 0;
                cropHeight = 0;
    }}}

    if (cropWidth > 0 && cropHeight > 0 && cropWidth <= inOutWidth && cropHeight <= inOutHeight) {
        newWidth = cropWidth / scale;
        newHeight = cropHeight / scale;
    } else {
        // Calculate new dimensions
        newWidth = inOutWidth / scale;
        newHeight = inOutHeight / scale;
    }
    
    // Align to 4 for bayer pattern and also because we read 4 bytes at a time when encoding to 10/14 bit
    newWidth = (newWidth / 4) * 4;
    newHeight = (newHeight / 4) * 4;    

    auto srcBlackLevel = metadata.dynamicBlackLevel;
    auto srcWhiteLevel = metadata.dynamicWhiteLevel;

    if (levels == "Static") {
        srcBlackLevel = cameraConfiguration.blackLevel;
        srcWhiteLevel = cameraConfiguration.whiteLevel;
    } else if (!levels.empty()) {
        const size_t separatorPos = levels.find('/');
        if (separatorPos != std::string::npos) {
            try {
                const std::string whiteLevelStr = levels.substr(0, separatorPos);
                const std::string blackLevelStr = levels.substr(separatorPos + 1);
                
                // Parse white level (int or float)
                if (whiteLevelStr.find('.') != std::string::npos) 
                    srcWhiteLevel = std::stof(whiteLevelStr);
                else 
                    srcWhiteLevel = std::stoul(whiteLevelStr);                
                
                // Parse black level (single value or comma-separated values)
                if (blackLevelStr.find(',') != std::string::npos) {
                    // Parse comma-separated values
                    std::array<float, 4> blackValues = {0.0f, 0.0f, 0.0f, 0.0f};
                    size_t start = 0;
                    size_t valueIndex = 0;
                    
                    while (start < blackLevelStr.length() && valueIndex < 4) {
                        size_t commaPos = blackLevelStr.find(',', start);
                        if (commaPos == std::string::npos) commaPos = blackLevelStr.length();
                        
                        std::string valueStr = blackLevelStr.substr(start, commaPos - start);
                        if (valueStr.find('.') != std::string::npos) {
                            blackValues[valueIndex] = std::stof(valueStr);
                        } else {
                            blackValues[valueIndex] = std::stoul(valueStr);
                        }
                        
                        valueIndex++;
                        start = commaPos + 1;
                    }                    
                    srcBlackLevel = blackValues;
                } else {
                    // Parse single value for all channels
                    float blackLevelValue;
                    if (blackLevelStr.find('.') != std::string::npos) 
                        blackLevelValue = std::stof(blackLevelStr);
                    else 
                        blackLevelValue = std::stoul(blackLevelStr);                                
                    srcBlackLevel = {blackLevelValue, blackLevelValue, blackLevelValue, blackLevelValue};
                }
            } catch (const std::exception&) {
                // Handle exception silently
            }
        }
    }

    if(cfaSize > 1 && scale == 2) {
        srcWhiteLevel *= cfaSize * cfaSize;
        for (int i = 0; i < srcBlackLevel.size(); i++) {
            srcBlackLevel[i] *= cfaSize * cfaSize;
        }        
    }

    const std::array<float, 4> linear = {
        1.0f / (srcWhiteLevel - srcBlackLevel[0]),
        1.0f / (srcWhiteLevel - srcBlackLevel[1]),
        1.0f / (srcWhiteLevel - srcBlackLevel[2]),
        1.0f / (srcWhiteLevel - srcBlackLevel[3])
    };

    auto dstBlackLevel = srcBlackLevel;
    auto dstWhiteLevel = srcWhiteLevel;

    // Calculate shading map offsets
    auto lensShadingMap = metadata.lensShadingMap;

    const int fullWidth = metadata.originalWidth;
    const int fullHeight = metadata.originalHeight;

    int left = 0;
    int top = 0;
    if ((!(cropWidth > 0 && cropHeight > 0)) || inOutWidth < cropWidth || inOutHeight < cropHeight) {
        left = (fullWidth - inOutWidth) / 2;
        top = (fullHeight - inOutHeight) / 2;
        cropWidth = 0;
        cropHeight = 0;
    } else {
        left = (fullWidth - cropWidth) / 2;
        top = (fullHeight - cropHeight) / 2;
    }

    const float shadingMapScaleX = 1.0f / static_cast<float>(fullWidth);
    const float shadingMapScaleY = 1.0f / static_cast<float>(fullHeight);

    int useBits = 0;

    // When applying shading map, increase precision
    if(applyShadingMap) {
        if(vignetteOnlyColor)
            colorOnlyShadingMap(lensShadingMap, metadata.lensShadingMapWidth, metadata.lensShadingMapHeight, cfa);
        if(normaliseShadingMap) {
            normalizeShadingMap(lensShadingMap);
            useBits = std::min(16, bitsNeeded(static_cast<unsigned short>(dstWhiteLevel)) + 4);
        } else {
            if (debugShadingMap) 
                invertShadingMap(lensShadingMap);
            else if (logTransform != "") {                 
                if (logTransform == "Keep Input") {
                    useBits = std::min(16, bitsNeeded(static_cast<unsigned short>(dstWhiteLevel)) + 0); //?
                    dstWhiteLevel = std::pow(2.0f, useBits) - 1; 
                } else if (logTransform == "Reduce by 2bit") {
                    useBits = std::min(16, bitsNeeded(static_cast<unsigned short>(dstWhiteLevel)) - 2);
                    dstWhiteLevel = std::pow(2.0f, useBits) - 1;  
                } else if (logTransform == "Reduce by 4bit") {
                    useBits = std::min(16, bitsNeeded(static_cast<unsigned short>(dstWhiteLevel)) - 4);
                    dstWhiteLevel = std::pow(2.0f, useBits) - 1;  
                } else if (logTransform == "Reduce by 6bit") {
                    useBits = std::min(16, bitsNeeded(static_cast<unsigned short>(dstWhiteLevel)) - 6);
                    dstWhiteLevel = std::pow(2.0f, useBits) - 1; 
                } else if (logTransform == "Reduce by 8bit") {
                    useBits = std::min(16, bitsNeeded(static_cast<unsigned short>(dstWhiteLevel)) - 8);
                    dstWhiteLevel = std::pow(2.0f, useBits) - 1; 
                } else {
                    useBits = std::min(16, bitsNeeded(static_cast<unsigned short>(dstWhiteLevel)) + 2);
                    dstWhiteLevel = std::pow(2.0f, useBits) - 1;  
                }
            } else {
                useBits = std::min(16, bitsNeeded(static_cast<unsigned short>(dstWhiteLevel)) + 2);
                dstWhiteLevel = std::pow(2.0f, useBits) - 1;  
            }
        }
        for(auto& v : dstBlackLevel)
            v = 0;                 
    } else if (logTransform != "") {
        if (logTransform == "Reduce by 2bit") {
            useBits = std::min(16, bitsNeeded(static_cast<unsigned short>(dstWhiteLevel)) - 2);
            dstWhiteLevel = std::pow(2.0f, useBits) - 1;
        } else if (logTransform == "Reduce by 4bit") {
            useBits = std::min(16, bitsNeeded(static_cast<unsigned short>(dstWhiteLevel)) - 4);
            dstWhiteLevel = std::pow(2.0f, useBits) - 1;
        } else if (logTransform == "Reduce by 6bit") {
            useBits = std::min(16, bitsNeeded(static_cast<unsigned short>(dstWhiteLevel)) - 6);
            dstWhiteLevel = std::pow(2.0f, useBits) - 1;        
        } else if (logTransform == "Reduce by 8bit") {
            useBits = std::min(16, bitsNeeded(static_cast<unsigned short>(dstWhiteLevel)) - 8);
            dstWhiteLevel = std::pow(2.0f, useBits) - 1;        
        }
        for(auto& v : dstBlackLevel)
            v = 0;       
    }

    // Create opcode list if requested and shading map is not applied to image data
    tinydngwriter::OpcodeList opcodeList2;
    if(includeOpcode && !applyShadingMap) {
        // Create lens shading map as opcode list 2 gain map
        opcodeList2 = createLensShadingOpcodeList(metadata, inOutWidth, inOutHeight, left, top);
    }

    //
    // Preprocess data
    //

    uint32_t originalWidth = inOutWidth;
    uint32_t dstOffset = 0;

    // Reinterpret the input data as uint16_t for reading
    uint16_t* srcData = reinterpret_cast<uint16_t*>(data.data());

    // Process the image by copying and packing 2x2 Bayer blocks
    std::array<float, 16> shadingMapVals;
    shadingMapVals.fill(1.0f);
    std::vector<uint8_t> dst;
    dst.resize(sizeof(uint16_t) * newWidth * newHeight);
    uint16_t* dstData = reinterpret_cast<uint16_t*>(dst.data());

    for (auto y = 0; y < newHeight; y += 2 * (scale < 2 ? cfaSize : 1)) {
        for (auto x = 0; x < newWidth; x += 2 * (scale < 2 ? cfaSize : 1)) {
            // Get the source coordinates (scaled)
            uint32_t srcY = y * scale;
            uint32_t srcX = x * scale;            
 
            if (cfaSize < 2 | scale > 1) {
                std::array<uint16_t, 4> s;
                if (cfaSize == 2 && scale == 2) {                    
                    s[0] = srcData[srcY * originalWidth + srcX] + srcData[srcY * originalWidth + srcX + 1] + srcData[(srcY + 1) * originalWidth + srcX] + srcData[(srcY + 1) * originalWidth + srcX + 1];
                    s[1] = srcData[srcY * originalWidth + srcX + 2] + srcData[srcY * originalWidth + srcX + 3] + srcData[(srcY + 1) * originalWidth + srcX + 2] + srcData[(srcY + 1) * originalWidth + srcX + 3];
                    s[2] = srcData[(srcY + 2) * originalWidth + srcX] + srcData[(srcY + 2) * originalWidth + srcX + 1] + srcData[(srcY + 3) * originalWidth + srcX] + srcData[(srcY + 3) * originalWidth + srcX + 1];
                    s[3] = srcData[(srcY + 2) * originalWidth + srcX + 2] + srcData[(srcY + 2) * originalWidth + srcX + 3] + srcData[(srcY + 3) * originalWidth + srcX + 2] + srcData[(srcY + 3) * originalWidth + srcX + 3];
                } else {
                    s[0] = srcData[srcY * originalWidth + srcX];
                    s[1] = srcData[srcY * originalWidth + srcX + cfaSize];
                    s[2] = srcData[(srcY + cfaSize) * originalWidth + srcX];
                    s[3] = srcData[(srcY + cfaSize) * originalWidth + srcX + cfaSize];
                }                
                
                if(applyShadingMap) {                              
                    // Calculate position in shading map     
                    shadingMapVals[0] = getShadingMapValue((srcX + left) * shadingMapScaleX, (srcY + top) * shadingMapScaleY, 0, lensShadingMap, metadata.lensShadingMapWidth, metadata.lensShadingMapHeight);
                    shadingMapVals[1] = getShadingMapValue((srcX + left + scale) * shadingMapScaleX, (srcY + top) * shadingMapScaleY, 1, lensShadingMap, metadata.lensShadingMapWidth, metadata.lensShadingMapHeight);
                    shadingMapVals[2] = getShadingMapValue((srcX + left) * shadingMapScaleX, (srcY + top + scale) * shadingMapScaleY, 2, lensShadingMap, metadata.lensShadingMapWidth, metadata.lensShadingMapHeight);
                    shadingMapVals[3] = getShadingMapValue((srcX + left + scale) * shadingMapScaleX, (srcY + top + scale) * shadingMapScaleY, 3, lensShadingMap, metadata.lensShadingMapWidth, metadata.lensShadingMapHeight);
                }

                std::array<float, 4> p;

                if(debugShadingMap) {
                    for (int i = 0; i < 4; i++)
                        p[i] = std::max(0.0f, linear[i] * (srcWhiteLevel - srcBlackLevel[i]) * shadingMapVals[i]) * (dstWhiteLevel - dstBlackLevel[i]);
                } else if (logTransform == "") {               // Linearize and (maybe) apply shading map
                    for (int i = 0; i < 4; i++)
                        p[i] = std::max(0.0f, linear[i] * (s[i] - srcBlackLevel[i]) * shadingMapVals[i]) * (dstWhiteLevel - dstBlackLevel[i]);
                } else {                                
                    std::array<float, 4> dither; // Apply logarithmic tone mapping with triangular dithering. Generate improved triangular dither with better randomization                                    
                    for (int i = 0; i < 4; i++) { // Use different seeds for each pixel in the 2x2 block to avoid correlation                    
                        uint32_t seed = ((x + (i & 1)) * 1664525 + (y + (i >> 1)) * 1013904223) ^ 0xdeadbeef; // Create unique seed for each pixel using position and pixel index
                        // Apply multiple hash iterations to improve randomness
                        seed ^= seed >> 16; seed *= 0x85ebca6b; seed ^= seed >> 13; seed *= 0xc2b2ae35; seed ^= seed >> 16;                    
                        // Generate triangular dither: sum of two uniform random values
                        float r1 = (seed & 0xffff) / 65535.0f; float r2 = ((seed >> 16) & 0xffff) / 65535.0f;                    
                        // Triangular distribution: r1 + r2 - 1, range [-1, 1] Scale down for subtle dithering appropriate for log encoding
                        dither[i] = (r1 + r2 - 1.0f) * 0.5f;
                        // Apply log2 transform that preserves black and white levels as identity points
                        float logValue = std::log2(1.0f + 60.0f * std::max(0.0f, linear[i] * (s[i] - srcBlackLevel[i]) * shadingMapVals[i])) / std::log2(61.0f);                  
                        p[i] = (logValue) * dstWhiteLevel + dither[i]; // Scale by dstWhiteLevel to match what the linearization table expects
                    }
                }            
                
                for (int i = 0; i < 4; i++)
                    s[i] = std::clamp(std::round((p[i] + dstBlackLevel[i])), 0.f, dstWhiteLevel);

                // Copy the 2x2 Bayer block
                dstData[dstOffset]                 = static_cast<unsigned short>(s[0]);
                dstData[dstOffset + 1]             = static_cast<unsigned short>(s[1]);
                dstData[dstOffset + newWidth]      = static_cast<unsigned short>(s[2]);
                dstData[dstOffset + newWidth + 1]  = static_cast<unsigned short>(s[3]);

                dstOffset += 2;
            } else {
                std::array<uint16_t, 16> s = {                
                    srcData[srcY * originalWidth + srcX], srcData[srcY * originalWidth + srcX + 1], srcData[(srcY + 1) * originalWidth + srcX], srcData[(srcY + 1) * originalWidth + srcX + 1],
                    srcData[srcY * originalWidth + srcX + 2], srcData[srcY * originalWidth + srcX + 3], srcData[(srcY + 1) * originalWidth + srcX + 2], srcData[(srcY + 1) * originalWidth + srcX + 3],
                    srcData[(srcY + 2) * originalWidth + srcX], srcData[(srcY + 2) * originalWidth + srcX + 1], srcData[(srcY + 3) * originalWidth + srcX], srcData[(srcY + 3) * originalWidth + srcX + 1],
                    srcData[(srcY + 2) * originalWidth + srcX + 2], srcData[(srcY + 2) * originalWidth + srcX + 3], srcData[(srcY + 3) * originalWidth + srcX + 2], srcData[(srcY + 3) * originalWidth + srcX + 3]
                };

                if(applyShadingMap) { 
                    // Calculate position in shading map     
                    shadingMapVals[0] = getShadingMapValue((srcX + left) * shadingMapScaleX, (srcY + top) * shadingMapScaleY, 0, lensShadingMap, metadata.lensShadingMapWidth, metadata.lensShadingMapHeight);
                    shadingMapVals[1] = getShadingMapValue((srcX + left + 1) * shadingMapScaleX, (srcY + top) * shadingMapScaleY, 0, lensShadingMap, metadata.lensShadingMapWidth, metadata.lensShadingMapHeight);
                    shadingMapVals[2] = getShadingMapValue((srcX + left) * shadingMapScaleX, (srcY + top + 1) * shadingMapScaleY, 0, lensShadingMap, metadata.lensShadingMapWidth, metadata.lensShadingMapHeight);
                    shadingMapVals[3] = getShadingMapValue((srcX + left + 1) * shadingMapScaleX, (srcY + top + 1) * shadingMapScaleY, 0, lensShadingMap, metadata.lensShadingMapWidth, metadata.lensShadingMapHeight);
                    shadingMapVals[4] = getShadingMapValue((srcX + left + cfaSize * 2) * shadingMapScaleX, (srcY + top) * shadingMapScaleY, 1, lensShadingMap, metadata.lensShadingMapWidth, metadata.lensShadingMapHeight);
                    shadingMapVals[5] = getShadingMapValue((srcX + left + cfaSize * 2 + 1) * shadingMapScaleX, (srcY + top) * shadingMapScaleY, 1, lensShadingMap, metadata.lensShadingMapWidth, metadata.lensShadingMapHeight);
                    shadingMapVals[6] = getShadingMapValue((srcX + left + cfaSize * 2) * shadingMapScaleX, (srcY + top + 1) * shadingMapScaleY, 1, lensShadingMap, metadata.lensShadingMapWidth, metadata.lensShadingMapHeight);
                    shadingMapVals[7] = getShadingMapValue((srcX + left + cfaSize * 2 + 1) * shadingMapScaleX, (srcY + top + 1) * shadingMapScaleY, 1, lensShadingMap, metadata.lensShadingMapWidth, metadata.lensShadingMapHeight);
                    shadingMapVals[8] = getShadingMapValue((srcX + left) * shadingMapScaleX, (srcY + top + cfaSize * 2) * shadingMapScaleY, 2, lensShadingMap, metadata.lensShadingMapWidth, metadata.lensShadingMapHeight);
                    shadingMapVals[9] = getShadingMapValue((srcX + left + 1) * shadingMapScaleX, (srcY + top + cfaSize * 2) * shadingMapScaleY, 2, lensShadingMap, metadata.lensShadingMapWidth, metadata.lensShadingMapHeight);
                    shadingMapVals[10] = getShadingMapValue((srcX + left) * shadingMapScaleX, (srcY + top + cfaSize * 2 + 1) * shadingMapScaleY, 2, lensShadingMap, metadata.lensShadingMapWidth, metadata.lensShadingMapHeight);
                    shadingMapVals[11] = getShadingMapValue((srcX + left + 1) * shadingMapScaleX, (srcY + top + cfaSize * 2 + 1) * shadingMapScaleY, 2, lensShadingMap, metadata.lensShadingMapWidth, metadata.lensShadingMapHeight);
                    shadingMapVals[12] = getShadingMapValue((srcX + left + cfaSize * 2) * shadingMapScaleX, (srcY + top + cfaSize * 2) * shadingMapScaleY, 3, lensShadingMap, metadata.lensShadingMapWidth, metadata.lensShadingMapHeight);
                    shadingMapVals[13] = getShadingMapValue((srcX + left + cfaSize * 2 + 1) * shadingMapScaleX, (srcY + top + cfaSize * 2) * shadingMapScaleY, 3, lensShadingMap, metadata.lensShadingMapWidth, metadata.lensShadingMapHeight);
                    shadingMapVals[14] = getShadingMapValue((srcX + left + cfaSize * 2) * shadingMapScaleX, (srcY + top + cfaSize * 2 + 1) * shadingMapScaleY, 3, lensShadingMap, metadata.lensShadingMapWidth, metadata.lensShadingMapHeight);
                    shadingMapVals[15] = getShadingMapValue((srcX + left + cfaSize * 2 + 1) * shadingMapScaleX, (srcY + top + cfaSize * 2 + 1) * shadingMapScaleY, 3, lensShadingMap, metadata.lensShadingMapWidth, metadata.lensShadingMapHeight);
                }

                std::array<float, 16> p;

                for (int i = 0; i < 16; i++)
                    p[i] = linear[i%4] * (s[i] - srcBlackLevel[i%4]) * shadingMapVals[i];

                std::array<float, 48> d;

                std::array<float, 16> r;

                if(cfaSize > 1 && false) {
                    // Quad Bayer demosaic - simplified bilinear interpolation
                    // p[16] contains 4x4 Quad Bayer block, d[48] will contain 16 RGB pixels
                    
                    // Simple bilinear interpolation for Quad Bayer and remosaic to normal Bayer
                    for(int py = 0; py < 4; py++) {
                        for(int px = 0; px < 4; px++) {
                            int idx = py * 4 + px;
                            int outIdx = idx * 3;
                            
                            // Determine which color this pixel is based on CFA pattern
                            // For Quad Bayer, each 2x2 block has the same color
                            int cfaIdx = ((py / 2) % 2) * 2 + ((px / 2) % 2);
                            int color = cfa[cfaIdx];
                            
                            float red = 0, green = 0, blue = 0;
                            
                            if(color == 0) { // Red pixel
                                red = p[idx];
                                // Interpolate green from neighbors
                                float gSum = 0; int gCount = 0;
                                if(px > 0 && cfa[(((py / 2) % 2)) * 2 + (((px-1) / 2) % 2)] == 1) { gSum += p[idx-1]; gCount++; }
                                if(px < 3 && cfa[(((py / 2) % 2)) * 2 + (((px+1) / 2) % 2)] == 1) { gSum += p[idx+1]; gCount++; }
                                if(py > 0 && cfa[(((py-1) / 2) % 2) * 2 + ((px / 2) % 2)] == 1) { gSum += p[idx-4]; gCount++; }
                                if(py < 3 && cfa[(((py+1) / 2) % 2) * 2 + ((px / 2) % 2)] == 1) { gSum += p[idx+4]; gCount++; }
                                green = gCount > 0 ? gSum / gCount : p[idx];
                                // Interpolate blue from diagonals
                                float bSum = 0; int bCount = 0;
                                if(px > 0 && py > 0 && cfa[(((py-1) / 2) % 2) * 2 + (((px-1) / 2) % 2)] == 2) { bSum += p[idx-5]; bCount++; }
                                if(px < 3 && py > 0 && cfa[(((py-1) / 2) % 2) * 2 + (((px+1) / 2) % 2)] == 2) { bSum += p[idx-3]; bCount++; }
                                if(px > 0 && py < 3 && cfa[(((py+1) / 2) % 2) * 2 + (((px-1) / 2) % 2)] == 2) { bSum += p[idx+3]; bCount++; }
                                if(px < 3 && py < 3 && cfa[(((py+1) / 2) % 2) * 2 + (((px+1) / 2) % 2)] == 2) { bSum += p[idx+5]; bCount++; }
                                blue = bCount > 0 ? bSum / bCount : p[idx];
                            }
                            else if(color == 1) { // Green pixel
                                green = p[idx];
                                // Interpolate red and blue from neighbors
                                float rSum = 0, bSum = 0; int rCount = 0, bCount = 0;
                                if(px > 0) { 
                                    int c = cfa[(((py / 2) % 2)) * 2 + (((px-1) / 2) % 2)];
                                    if(c == 0) { rSum += p[idx-1]; rCount++; }
                                    else if(c == 2) { bSum += p[idx-1]; bCount++; }
                                }
                                if(px < 3) {
                                    int c = cfa[(((py / 2) % 2)) * 2 + (((px+1) / 2) % 2)];
                                    if(c == 0) { rSum += p[idx+1]; rCount++; }
                                    else if(c == 2) { bSum += p[idx+1]; bCount++; }
                                }
                                if(py > 0) {
                                    int c = cfa[(((py-1) / 2) % 2) * 2 + ((px / 2) % 2)];
                                    if(c == 0) { rSum += p[idx-4]; rCount++; }
                                    else if(c == 2) { bSum += p[idx-4]; bCount++; }
                                }
                                if(py < 3) {
                                    int c = cfa[(((py+1) / 2) % 2) * 2 + ((px / 2) % 2)];
                                    if(c == 0) { rSum += p[idx+4]; rCount++; }
                                    else if(c == 2) { bSum += p[idx+4]; bCount++; }
                                }
                                red = rCount > 0 ? rSum / rCount : p[idx];
                                blue = bCount > 0 ? bSum / bCount : p[idx];
                            }
                            else { // Blue pixel
                                blue = p[idx];
                                // Interpolate green from neighbors
                                float gSum = 0; int gCount = 0;
                                if(px > 0 && cfa[(((py / 2) % 2)) * 2 + (((px-1) / 2) % 2)] == 1) { gSum += p[idx-1]; gCount++; }
                                if(px < 3 && cfa[(((py / 2) % 2)) * 2 + (((px+1) / 2) % 2)] == 1) { gSum += p[idx+1]; gCount++; }
                                if(py > 0 && cfa[(((py-1) / 2) % 2) * 2 + ((px / 2) % 2)] == 1) { gSum += p[idx-4]; gCount++; }
                                if(py < 3 && cfa[(((py+1) / 2) % 2) * 2 + ((px / 2) % 2)] == 1) { gSum += p[idx+4]; gCount++; }
                                green = gCount > 0 ? gSum / gCount : p[idx];
                                // Interpolate red from diagonals
                                float rSum = 0; int rCount = 0;
                                if(px > 0 && py > 0 && cfa[(((py-1) / 2) % 2) * 2 + (((px-1) / 2) % 2)] == 0) { rSum += p[idx-5]; rCount++; }
                                if(px < 3 && py > 0 && cfa[(((py-1) / 2) % 2) * 2 + (((px+1) / 2) % 2)] == 0) { rSum += p[idx-3]; rCount++; }
                                if(px > 0 && py < 3 && cfa[(((py+1) / 2) % 2) * 2 + (((px-1) / 2) % 2)] == 0) { rSum += p[idx+3]; rCount++; }
                                if(px < 3 && py < 3 && cfa[(((py+1) / 2) % 2) * 2 + (((px+1) / 2) % 2)] == 0) { rSum += p[idx+5]; rCount++; }
                                red = rCount > 0 ? rSum / rCount : p[idx];
                            }
                            
                            // Store demosaiced RGB
                            d[outIdx] = red;
                            d[outIdx + 1] = green;
                            d[outIdx + 2] = blue;
                            
                            // Remosaic to normal Bayer - extract appropriate channel based on normal Bayer CFA pattern
                            int bayerCfaIdx = (py % 2) * 2 + (px % 2);
                            int bayerColor = cfa[bayerCfaIdx];
                            
                            if(bayerColor == 0) { // Red position in normal Bayer
                                r[idx] = red;
                            }
                            else if(bayerColor == 1) { // Green position in normal Bayer
                                r[idx] = green;
                            }
                            else { // Blue position in normal Bayer
                                r[idx] = blue;
                            }
                        }
                    }
                    p = r;
                }


                if (logTransform == "") {               // Linearize and (maybe) apply shading map
                    for (int i = 0; i < 16; i++)
                        p[i] = std::max(0.0f, p[i] * (dstWhiteLevel - dstBlackLevel[i%4]));
                } else {                                
                    std::array<float, 16> dither; // Apply logarithmic tone mapping with triangular dithering. Generate improved triangular dither with better randomization                                    
                    for (int i = 0; i < 16; i++) { // Use different seeds for each pixel in the 2x2 block to avoid correlation                    
                        uint32_t seed = ((x + (i & 1)) * 1664525 + (y + (i >> 1)) * 1013904223) ^ 0xdeadbeef; // Create unique seed for each pixel using position and pixel index
                        // Apply multiple hash iterations to improve randomness
                        seed ^= seed >> 16; seed *= 0x85ebca6b; seed ^= seed >> 13; seed *= 0xc2b2ae35; seed ^= seed >> 16;                    
                        // Generate triangular dither: sum of two uniform random values
                        float r1 = (seed & 0xffff) / 65535.0f; float r2 = ((seed >> 16) & 0xffff) / 65535.0f;                    
                        // Triangular distribution: r1 + r2 - 1, range [-1, 1] Scale down for subtle dithering appropriate for log encoding
                        dither[i] = (r1 + r2 - 1.0f) * 0.5f;
                        // Apply log2 transform that preserves black and white levels as identity points
                        float logValue = std::log2(1.0f + 60.0f * std::max(0.0f, p[i])) / std::log2(61.0f);                  
                        p[i] = (logValue) * dstWhiteLevel + dither[i]; // Scale by dstWhiteLevel to match what the linearization table expects
                    }
                }            

                for (int i = 0; i < 16; i++)
                    s[i] = std::clamp(std::round((p[i] + dstBlackLevel[i%4])), 0.f, dstWhiteLevel);
                    
                dstData[dstOffset]                      = static_cast<unsigned short>(s[0]); 
                dstData[dstOffset + 1]                  = static_cast<unsigned short>(s[1]);
                dstData[dstOffset + newWidth]           = static_cast<unsigned short>(s[2]);
                dstData[dstOffset + newWidth + 1]       = static_cast<unsigned short>(s[3]);
                dstData[dstOffset + 2]                  = static_cast<unsigned short>(s[4]); 
                dstData[dstOffset + 3]                  = static_cast<unsigned short>(s[5]);
                dstData[dstOffset + newWidth + 2]       = static_cast<unsigned short>(s[6]);
                dstData[dstOffset + newWidth + 3]       = static_cast<unsigned short>(s[7]);
                dstData[dstOffset + newWidth * 2]       = static_cast<unsigned short>(s[8]); 
                dstData[dstOffset + newWidth * 2 + 1]   = static_cast<unsigned short>(s[9]);
                dstData[dstOffset + newWidth * 3]       = static_cast<unsigned short>(s[10]);
                dstData[dstOffset + newWidth * 3 + 1]   = static_cast<unsigned short>(s[11]);
                dstData[dstOffset + newWidth * 2 + 2]   = static_cast<unsigned short>(s[12]); 
                dstData[dstOffset + newWidth * 2 + 3]   = static_cast<unsigned short>(s[13]);
                dstData[dstOffset + newWidth * 3 + 2]   = static_cast<unsigned short>(s[14]);
                dstData[dstOffset + newWidth * 3 + 3]   = static_cast<unsigned short>(s[15]);
                              
                dstOffset += 2 * cfaSize;
            }            
        }
        dstOffset += newWidth * (cfaSize == 2 && scale == 1 ? 3 : 1);
    }

    // Update dimensions
    inOutWidth = newWidth;
    inOutHeight = newHeight;

    std::array<unsigned short, 4> blackLevelResult;

    for(auto i = 0; i < dstBlackLevel.size(); ++i)
        blackLevelResult[i] = static_cast<unsigned short>(std::round(dstBlackLevel[i]));

    return std::make_tuple(dst, blackLevelResult, static_cast<unsigned short>(dstWhiteLevel), opcodeList2);
}

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
    std::string logTransform)
{
    Measure m("generateDng");

    unsigned int width = metadata.width;
    unsigned int height = metadata.height;

    std::array<uint8_t, 4> cfa;
    std::array<uint8_t, 16> qcfa;

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
    bool vignetteOnlyColor = options & RENDER_OPT_VIGNETTE_ONLY_COLOR;
    bool normalizeShadingMap = options & RENDER_OPT_NORMALIZE_SHADING_MAP;
    bool debugShadingMap = options & RENDER_OPT_DEBUG_SHADING_MAP;
    bool normalizeExposure = options & RENDER_OPT_NORMALIZE_EXPOSURE;
    bool useLogCurve = options & RENDER_OPT_LOG_TRANSFORM;

    if(!(options & RENDER_OPT_CROPPING))// || width != metadata.originalWidth || height != metadata.originalHeight)
        cropTarget = "0x0";

    auto [processedData, dstBlackLevel, dstWhiteLevel, opcodeList2] = utils::preprocessData(
        data,
        width, height,
        metadata,
        cameraConfiguration,
        cfa,
        scale,
        applyShadingMap, vignetteOnlyColor, normalizeShadingMap, debugShadingMap,
        cropTarget,
        levels,
        logTransform,
        true  // includeOpcode = true to generate lens shading opcode when not applied to image
    );

    spdlog::debug("New black level {},{},{},{} and white level {}",
                  dstBlackLevel[0], dstBlackLevel[1], dstBlackLevel[2], dstBlackLevel[3], dstWhiteLevel);

    // Encode to reduce size in container
    auto encodeBits = bitsNeeded(dstWhiteLevel);

    if(encodeBits <= 2) {
        utils::encodeTo2Bit(processedData, width, height);
        encodeBits = 2;
    }
    else if(encodeBits <= 4) {
        utils::encodeTo4Bit(processedData, width, height);
        encodeBits = 4;
    }
    else if(encodeBits <= 6) {
        utils::encodeTo6Bit(processedData, width, height);
        encodeBits = 6;
    }
    else if(encodeBits <= 8) {
        utils::encodeTo8Bit(processedData, width, height);
        encodeBits = 8;
    }
    else if(encodeBits <= 10) {
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
    dng.SetXResolution(300);
    dng.SetYResolution(300);

    dng.SetBlackLevelRepeatDim(2, 2);
        
    dng.SetCompression(tinydngwriter::COMPRESSION_NONE);

    dng.SetIso(metadata.iso);
    dng.SetExposureTime(metadata.exposureTime / 1e9);

    float exposureOffset = (camModel == "Panasonic" ? -2.0f : 0.0f);

    if (normalizeExposure)
        dng.SetBaselineExposure(std::log2(baselineExpValue / (metadata.iso * metadata.exposureTime)) + exposureOffset);
    else
        dng.SetBaselineExposure(exposureOffset);

    if(metadata.needRemosaic && scale == 1 && false) {   //de/remosaic need to be disabled and add ui option. 
        dng.SetCFARepeatPatternDim(4, 4);
        std::array<uint8_t, 4> cfa_pattern_0112 = {0,1,1,2};
        std::array<uint8_t, 4> cfa_pattern_2110 = {2,1,1,0};
        std::array<uint8_t, 4> cfa_pattern_1021 = {1,0,2,1};
        
        if (cfa == cfa_pattern_0112) 
            qcfa = {0,0,1,1,0,0,1,1,1,1,2,2,1,1,2,2};
        else if (cfa == cfa_pattern_2110) 
            qcfa = {2,2,1,1,2,2,1,1,1,1,0,0,1,1,0,0};
        else if (cfa == cfa_pattern_1021) 
            qcfa = {1,1,0,0,1,1,0,0,2,2,1,1,2,2,1,1};
        else 
            qcfa = {1,1,2,2,1,1,2,2,0,0,1,1,0,0,1,1};
        dng.SetCFAPattern(16, qcfa.data());
    } else {
        dng.SetCFARepeatPatternDim(2, 2);
        dng.SetCFAPattern(4, cfa.data());
    }

    // Add orientation tag
    DngOrientation dngOrientation;
    bool isFlipped = cameraConfiguration.extraData.postProcessSettings.flipped;

    switch(metadata.orientation)
    {
    case ScreenOrientation::PORTRAIT:
        dngOrientation = isFlipped ? DngOrientation::kMirror90CW : DngOrientation::kRotate90CW;
        break;

    case ScreenOrientation::REVERSE_PORTRAIT:
        dngOrientation = isFlipped ? DngOrientation::kMirror90CCW : DngOrientation::kRotate90CCW;
        break;

    case ScreenOrientation::REVERSE_LANDSCAPE:
        dngOrientation = isFlipped ? DngOrientation::kMirror180 : DngOrientation::kRotate180;
        break;

    case ScreenOrientation::LANDSCAPE:
        dngOrientation = isFlipped ? DngOrientation::kMirror : DngOrientation::kNormal;
        break;

    default:
        dngOrientation = DngOrientation::kUnknown;
        break;
    }

    dng.SetOrientation(dngOrientation);

    // Time code
    float time = frameNumber / recordingFps;

    int hours = (int) floor(time / 3600);
    int minutes = ((int) floor(time / 60)) % 60;
    int seconds = ((int) floor(time)) % 60;
    int frames = recordingFps > 1 ? (frameNumber % static_cast<int>(std::round(recordingFps))) : 0;

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

    if (!isZeroMatrix(cameraConfiguration.colorMatrix1))
        dng.SetColorMatrix1(3, cameraConfiguration.colorMatrix1.data());
    if (!isZeroMatrix(cameraConfiguration.colorMatrix2))
        dng.SetColorMatrix2(3, cameraConfiguration.colorMatrix2.data());

    if (!isZeroMatrix(cameraConfiguration.forwardMatrix1))
        dng.SetForwardMatrix1(3, cameraConfiguration.forwardMatrix1.data());
    if (!isZeroMatrix(cameraConfiguration.forwardMatrix2))
        dng.SetForwardMatrix2(3, cameraConfiguration.forwardMatrix2.data());

    dng.SetCameraCalibration1(3, IDENTITY_MATRIX);
    dng.SetCameraCalibration2(3, IDENTITY_MATRIX);

    dng.SetAsShotNeutral(3, metadata.asShotNeutral.data());

    dng.SetCalibrationIlluminant1(getColorIlluminant(cameraConfiguration.colorIlluminant1));
    dng.SetCalibrationIlluminant2(getColorIlluminant(cameraConfiguration.colorIlluminant2));

    // Additional information
    const auto software = "MotionCam Tools";

    dng.SetSoftware(software);

    
    if(camModel != ""){
        if (camModel == "Blackmagic") {
            dng.SetUniqueCameraModel("Blackmagic Pocket Cinema Camera 4K");
        } else if (camModel == "Panasonic") {
            dng.SetUniqueCameraModel("Panasonic Varicam RAW");
        } else if (camModel == "Fujifilm" || camModel == "Fujifilm X-T5") {
            dng.SetUniqueCameraModel("Fujifilm X-T5");
            dng.SetMake("Fujifilm");
            dng.SetCameraModelName("X-T5");
        } else {
            // Generic camera model
            dng.SetUniqueCameraModel(camModel);
        }
    } else {
        dng.SetUniqueCameraModel(cameraConfiguration.extraData.postProcessSettings.metadata.buildModel);
    }

    // Add lens shading map as opcode list 2 if not applied to image data
    if (!opcodeList2.IsEmpty()) {
        dng.SetOpcodeList2(opcodeList2);
    }


    // Set data
    dng.SetSubfileType();

    const uint32_t activeArea[4] = { 0, 0, height, width };
    dng.SetActiveArea(&activeArea[0]);

    // Add linearization table based on actual bit depth
    
    if (logTransform != "" && !(logTransform == "Keep Input" && !applyShadingMap)) {
        // Create linearization table sized for the actual stored range
        // The stored values range from 0 to dstWhiteLevel, so we need dstWhiteLevel+1 entries
        const int tableSize = static_cast<int>(dstWhiteLevel) + 1;
        std::vector<unsigned short> linearizationTable(tableSize);
        
        for (int i = 0; i < tableSize; i++) {
            // Convert stored log value back to linear
            // Must match the aggressive log curve: logValue = log2(1 + k*clampedValue) / log2(1 + k)
            // Inverse: clampedValue = (2^(logValue * log2(1 + k)) - 1) / k
            
            float logValue = static_cast<float>(i);
            float normalizedLogValue = logValue / dstWhiteLevel;  // Normalize by dstWhiteLevel to match forward transform
            
            // Reverse the k=30 curve with guaranteed identity preservation
            float linearValue;
            
            if (i == 0) {
                linearValue = 0.0f;  // Exact identity: stored 0 → linear 0
            } else if (i == tableSize - 1) {
                linearValue = 1.0f;  // Force maximum table entry → linear 1 → 65535
            } else {                               
                // Inverse of: logValue = log2(1 + k*clampedValue) / log2(1 + k)
                linearValue = (std::pow(2.0f, normalizedLogValue * std::log2(1.0f + 60.0f)) - 1.0f) / 60.0f;
                linearValue = std::clamp(linearValue, 0.0f, 1.0f);
            }            
            // Scale to 16-bit range            
            linearizationTable[i] = static_cast<unsigned short>(linearValue * 65535.0f);                  
        }        
        dng.SetLinearizationTable(tableSize, linearizationTable.data());
        std::array<unsigned short, 4> linearBlackLevel = {0, 0, 0, 0};  // Linear black is 0
        dng.SetBlackLevel(4, linearBlackLevel.data());
        dng.SetWhiteLevel(65534);  //idk why
    } else {           
        dng.SetBlackLevel(4, dstBlackLevel.data());
        dng.SetWhiteLevel(dstWhiteLevel);    
    }    

    // Write DNG
    std::string err;

    tinydngwriter::DNGWriter writer(false);

    writer.AddImage(&dng);

    // Save to memory
    auto output = std::make_shared<std::vector<char>>();

    // Reserve enough to fit the data
    output->reserve(width*height*sizeof(uint16_t) + 512*1024);

    utils::vector_ostream stream(*output);

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
