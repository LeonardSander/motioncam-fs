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

// ============================================================================
// vectorbuf and vector_ostream implementations
// ============================================================================

vectorbuf::vectorbuf(std::vector<char>& vec) : vec_(vec) {
    if (!vec_.empty()) {
        setp(vec_.data(), vec_.data() + vec_.size());
    }
}

vectorbuf::int_type vectorbuf::overflow(int_type c) {
    if (c != traits_type::eof()) {
        size_t old_size = vec_.size();
        vec_.resize(old_size + 1);
        vec_[old_size] = static_cast<char>(c);

        setp(vec_.data(), vec_.data() + vec_.size());
        pbump(static_cast<int>(old_size + 1));
    }
    return c;
}

std::streamsize vectorbuf::xsputn(const char* s, std::streamsize count) {
    size_t old_size = vec_.size();
    size_t available = epptr() - pptr();

    if (static_cast<size_t>(count) > available) {
        vec_.resize(old_size + count);
        setp(vec_.data(), vec_.data() + vec_.size());
        pbump(static_cast<int>(old_size));
    }

    std::copy(s, s + count, pptr());
    pbump(static_cast<int>(count));

    return count;
}

vectorbuf::pos_type vectorbuf::seekoff(off_type off, std::ios_base::seekdir way,
                         std::ios_base::openmode which) {
    if (which & std::ios_base::out) {
        pos_type pos;

        switch (way) {
        case std::ios_base::beg:
            pos = off;
            break;
        case std::ios_base::cur:
            pos = (pptr() - pbase()) + off;
            break;
        case std::ios_base::end:
            pos = vec_.size() + off;
            break;
        default:
            return pos_type(off_type(-1));
        }

        return seekpos(pos, which);
    }

    return pos_type(off_type(-1));
}

vectorbuf::pos_type vectorbuf::seekpos(pos_type sp, std::ios_base::openmode which) {
    if (which & std::ios_base::out) {
        off_type pos = sp;

        if (pos < 0) {
            return pos_type(off_type(-1));
        }

        if (static_cast<size_t>(pos) > vec_.size()) {
            vec_.resize(static_cast<size_t>(pos));
        }

        setp(vec_.data(), vec_.data() + vec_.size());
        pbump(static_cast<int>(pos));

        return sp;
    }

    return pos_type(off_type(-1));
}

vector_ostream::vector_ostream(std::vector<char>& vec)
    : std::ostream(&buf_), buf_(vec) {}

std::vector<char>& vector_ostream::vector() {
    return buf_.vec_;
}

const std::vector<char>& vector_ostream::vector() const {
    return buf_.vec_;
}

std::streampos vector_ostream::tell() {
    return tellp();
}

vector_ostream& vector_ostream::seek(std::streampos pos) {
    seekp(pos);
    return *this;
}

vector_ostream& vector_ostream::seek_relative(std::streamoff off) {
    seekp(off, std::ios_base::cur);
    return *this;
}

vector_ostream& vector_ostream::seek_from_end(std::streamoff off) {
    seekp(off, std::ios_base::end);
    return *this;
}

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

    void colorOnlyShadingMapInternal(std::vector<std::vector<float>>& shadingMap, int lensShadingMapWidth, int lensShadingMapHeight, const std::array<uint8_t, 4> cfa) {
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

    inline float getShadingMapValueInternal(
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

// Pack RGB data to 12-bit (2 pixels = 6 samples * 12 bits = 72 bits = 9 bytes)
void encodeRGBTo12Bit(std::vector<uint8_t>& data, uint32_t& width, uint32_t& height) {
    uint16_t* srcPtr = reinterpret_cast<uint16_t*>(data.data());
    uint8_t* dstPtr = data.data();
    
    for (uint32_t y = 0; y < height; y++) {
        for (uint32_t x = 0; x < width; x += 2) {
            // Read 6 samples (2 RGB pixels)
            uint16_t r0 = srcPtr[0];
            uint16_t g0 = srcPtr[1];
            uint16_t b0 = srcPtr[2];
            uint16_t r1 = srcPtr[3];
            uint16_t g1 = srcPtr[4];
            uint16_t b1 = srcPtr[5];
            
            // Pack into 9 bytes
            dstPtr[0] = r0 >> 4;
            dstPtr[1] = ((r0 & 0x0F) << 4) | (g0 >> 8);
            dstPtr[2] = g0 & 0xFF;
            dstPtr[3] = b0 >> 4;
            dstPtr[4] = ((b0 & 0x0F) << 4) | (r1 >> 8);
            dstPtr[5] = r1 & 0xFF;
            dstPtr[6] = g1 >> 4;
            dstPtr[7] = ((g1 & 0x0F) << 4) | (b1 >> 8);
            dstPtr[8] = b1 & 0xFF;
            
            srcPtr += 6;
            dstPtr += 9;
        }
    }
    
    auto newSize = dstPtr - data.data();
    data.resize(newSize);
}

// Pack RGB data to 10-bit (4 pixels = 12 samples * 10 bits = 120 bits = 15 bytes)
void encodeRGBTo10Bit(std::vector<uint8_t>& data, uint32_t& width, uint32_t& height) {
    uint16_t* srcPtr = reinterpret_cast<uint16_t*>(data.data());
    uint8_t* dstPtr = data.data();
    
    for (uint32_t y = 0; y < height; y++) {
        for (uint32_t x = 0; x < width; x += 4) {
            // Read 12 samples (4 RGB pixels)
            uint16_t s[12];
            for (int i = 0; i < 12; i++) {
                s[i] = srcPtr[i];
            }
            
            // Pack 12 samples * 10 bits = 120 bits = 15 bytes
            dstPtr[0] = s[0] >> 2;
            dstPtr[1] = ((s[0] & 0x03) << 6) | (s[1] >> 4);
            dstPtr[2] = ((s[1] & 0x0F) << 4) | (s[2] >> 6);
            dstPtr[3] = ((s[2] & 0x3F) << 2) | (s[3] >> 8);
            dstPtr[4] = s[3] & 0xFF;
            
            dstPtr[5] = s[4] >> 2;
            dstPtr[6] = ((s[4] & 0x03) << 6) | (s[5] >> 4);
            dstPtr[7] = ((s[5] & 0x0F) << 4) | (s[6] >> 6);
            dstPtr[8] = ((s[6] & 0x3F) << 2) | (s[7] >> 8);
            dstPtr[9] = s[7] & 0xFF;
            
            dstPtr[10] = s[8] >> 2;
            dstPtr[11] = ((s[8] & 0x03) << 6) | (s[9] >> 4);
            dstPtr[12] = ((s[9] & 0x0F) << 4) | (s[10] >> 6);
            dstPtr[13] = ((s[10] & 0x3F) << 2) | (s[11] >> 8);
            dstPtr[14] = s[11] & 0xFF;
            
            srcPtr += 12;
            dstPtr += 15;
        }
    }
    
    auto newSize = dstPtr - data.data();
    data.resize(newSize);
}

// Pack RGB data to 8-bit (1 pixel = 3 samples * 8 bits = 24 bits = 3 bytes)
void encodeRGBTo8Bit(std::vector<uint8_t>& data, uint32_t& width, uint32_t& height) {
    uint16_t* srcPtr = reinterpret_cast<uint16_t*>(data.data());
    uint8_t* dstPtr = data.data();
    
    for (uint32_t y = 0; y < height; y++) {
        for (uint32_t x = 0; x < width; x++) {
            // Read 3 samples (1 RGB pixel)
            dstPtr[0] = srcPtr[0] & 0xFF;
            dstPtr[1] = srcPtr[1] & 0xFF;
            dstPtr[2] = srcPtr[2] & 0xFF;
            
            srcPtr += 3;
            dstPtr += 3;
        }
    }
    
    auto newSize = dstPtr - data.data();
    data.resize(newSize);
}

// Pack RGB data to 6-bit (4 pixels = 12 samples * 6 bits = 72 bits = 9 bytes)
void encodeRGBTo6Bit(std::vector<uint8_t>& data, uint32_t& width, uint32_t& height) {
    uint16_t* srcPtr = reinterpret_cast<uint16_t*>(data.data());
    uint8_t* dstPtr = data.data();
    
    for (uint32_t y = 0; y < height; y++) {
        for (uint32_t x = 0; x < width; x += 4) {
            // Read 12 samples (4 RGB pixels)
            uint8_t v[12];
            for (int i = 0; i < 12; i++) {
                v[i] = srcPtr[i] & 0x3F;
            }
            
            // Pack 12 samples * 6 bits = 72 bits = 9 bytes
            dstPtr[0] = (v[0] << 2) | (v[1] >> 4);
            dstPtr[1] = ((v[1] & 0x0F) << 4) | (v[2] >> 2);
            dstPtr[2] = ((v[2] & 0x03) << 6) | v[3];
            
            dstPtr[3] = (v[4] << 2) | (v[5] >> 4);
            dstPtr[4] = ((v[5] & 0x0F) << 4) | (v[6] >> 2);
            dstPtr[5] = ((v[6] & 0x03) << 6) | v[7];
            
            dstPtr[6] = (v[8] << 2) | (v[9] >> 4);
            dstPtr[7] = ((v[9] & 0x0F) << 4) | (v[10] >> 2);
            dstPtr[8] = ((v[10] & 0x03) << 6) | v[11];
            
            srcPtr += 12;
            dstPtr += 9;
        }
    }
    
    auto newSize = dstPtr - data.data();
    data.resize(newSize);
}

// Pack RGB data to 4-bit (2 pixels = 6 samples * 4 bits = 24 bits = 3 bytes)
void encodeRGBTo4Bit(std::vector<uint8_t>& data, uint32_t& width, uint32_t& height) {
    uint16_t* srcPtr = reinterpret_cast<uint16_t*>(data.data());
    uint8_t* dstPtr = data.data();
    
    for (uint32_t y = 0; y < height; y++) {
        for (uint32_t x = 0; x < width; x += 2) {
            // Read 6 samples (2 RGB pixels)
            uint8_t v[6];
            for (int i = 0; i < 6; i++) {
                v[i] = srcPtr[i] & 0x0F;
            }
            
            // Pack 6 samples * 4 bits = 24 bits = 3 bytes
            dstPtr[0] = (v[0] << 4) | v[1];
            dstPtr[1] = (v[2] << 4) | v[3];
            dstPtr[2] = (v[4] << 4) | v[5];
            
            srcPtr += 6;
            dstPtr += 3;
        }
    }
    
    auto newSize = dstPtr - data.data();
    data.resize(newSize);
}


tinydngwriter::OpcodeList createLensShadingOpcodeList(
    const CameraFrameMetadata& metadata,
    uint32_t imageWidth,
    uint32_t imageHeight,
    int left,
    int top)
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
    // A CFA image has one stored image plane; the map itself carries the four
    // Bayer-phase gain planes.
    unsigned int availablePlanes = static_cast<unsigned int>(metadata.lensShadingMap.size());
    if (availablePlanes == 0) availablePlanes = 1;
    gainParams.planes = 1;
    
    // Grid size in the gain map
    const unsigned int mapPointsV = static_cast<unsigned int>(metadata.lensShadingMapHeight);
    const unsigned int mapPointsH = static_cast<unsigned int>(metadata.lensShadingMapWidth);
    gainParams.map_points_v = mapPointsV;
    gainParams.map_points_h = mapPointsH;
    
    // Compute pixel pitch between adjacent map points in rows/cols (in pixels)
    // If only a single point along a dimension, pitch covers the full extent
    const unsigned int imageRows = imageHeight;
    const unsigned int imageCols = imageWidth;
    gainParams.row_pitch = 1;
    gainParams.col_pitch = 1;
    
    // Map spacing and origin in relative coordinates
    // Spacing is relative pitch to image size; origin is relative to active area
    gainParams.map_spacing_v = mapPointsV > 1 ? 1.0 / (mapPointsV - 1) : 1.0;
    gainParams.map_spacing_h = mapPointsH > 1 ? 1.0 / (mapPointsH - 1) : 1.0;
    gainParams.map_origin_v = 0.0;
    gainParams.map_origin_h = 0.0;
    
    // Number of planes in the gain map payload (match planes when available)
    gainParams.map_planes = std::min(4u, availablePlanes);
    
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

std::tuple<std::vector<uint8_t>, std::array<unsigned short, 4>, unsigned short,
           tinydngwriter::OpcodeList, tinydngwriter::OpcodeList> preprocessData(
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
    LogTransformMode logTransform,
    QuadBayerMode quadBayerOption,
    bool includeOpcode)
{
    scale = (scale > 1 ? (scale / 2) * 2 : 1); // Ensure even scale for downscaling

    if(!(includeOpcode))// || width != metadata.originalWidth || height != metadata.originalHeight)
        cropTarget = "0x0";

    uint32_t cfaSize = (interpretAsQuadBayer ? 2 : 1);  //assume quadbayer for now

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

    tinydngwriter::OpcodeList opcodeList3;

    // When applying shading map, increase precision
    if(applyShadingMap) {
        if(vignetteOnlyColor) {
            CameraFrameMetadata luminanceMetadata = metadata;
            const size_t points = static_cast<size_t>(metadata.lensShadingMapWidth) *
                                  metadata.lensShadingMapHeight;
            luminanceMetadata.lensShadingMap.assign(1, std::vector<float>(points, 1.0f));
            for (size_t point = 0; point < points; ++point) {
                float minimum = std::numeric_limits<float>::max();
                for (const auto& channel : lensShadingMap)
                    if (point < channel.size()) minimum = std::min(minimum, channel[point]);
                if (std::isfinite(minimum) && minimum > 0.0f)
                    luminanceMetadata.lensShadingMap[0][point] = minimum;
            }
            opcodeList3 = createLensShadingOpcodeList(
                luminanceMetadata, inOutWidth, inOutHeight, left, top);
            utils::colorOnlyShadingMap(lensShadingMap, metadata.lensShadingMapWidth, metadata.lensShadingMapHeight, cfa);
        }
        if(normaliseShadingMap) {
            utils::normalizeShadingMap(lensShadingMap);
            useBits = std::min(16, utils::bitsNeeded(static_cast<unsigned short>(dstWhiteLevel)) + 4);
        } else {
            if (debugShadingMap) 
                utils::invertShadingMap(lensShadingMap);
            else if (logTransform != LogTransformMode::Disabled) {                 
                if (logTransform == LogTransformMode::KeepInput) 
                    useBits = std::min(16, utils::bitsNeeded(static_cast<unsigned short>(dstWhiteLevel)) + 0);
                else if (logTransform == LogTransformMode::ReduceBy2Bit) 
                    useBits = std::min(16, std::max(1, utils::bitsNeeded(static_cast<unsigned short>(dstWhiteLevel)) - 2));
                else if (logTransform == LogTransformMode::ReduceBy4Bit) 
                    useBits = std::min(16, std::max(1, utils::bitsNeeded(static_cast<unsigned short>(dstWhiteLevel)) - 4));
                else if (logTransform == LogTransformMode::ReduceBy6Bit) 
                    useBits = std::min(16, std::max(1, utils::bitsNeeded(static_cast<unsigned short>(dstWhiteLevel)) - 6));
                else if (logTransform == LogTransformMode::ReduceBy8Bit) 
                    useBits = std::min(16, std::max(1, utils::bitsNeeded(static_cast<unsigned short>(dstWhiteLevel)) - 8));
                else 
                    useBits = std::min(16, utils::bitsNeeded(static_cast<unsigned short>(dstWhiteLevel)) + 2);
                useBits = std::max(1, useBits); // Ensure at least 1 bit
                dstWhiteLevel = std::pow(2.0f, useBits) - 1; 
            } else {
                useBits = std::min(16, utils::bitsNeeded(static_cast<unsigned short>(dstWhiteLevel)) + 2);
                dstWhiteLevel = std::pow(2.0f, useBits) - 1;  
            }
        }
        for(auto& v : dstBlackLevel)
            v = 0;
    } else if (logTransform != LogTransformMode::Disabled) {
        if (logTransform == LogTransformMode::ReduceBy2Bit) {
            useBits = std::min(16, std::max(1, bitsNeeded(static_cast<unsigned short>(dstWhiteLevel)) - 2));
            dstWhiteLevel = std::pow(2.0f, useBits) - 1;
        } else if (logTransform == LogTransformMode::ReduceBy4Bit) {
            useBits = std::min(16, std::max(1, bitsNeeded(static_cast<unsigned short>(dstWhiteLevel)) - 4));
            dstWhiteLevel = std::pow(2.0f, useBits) - 1;
        } else if (logTransform == LogTransformMode::ReduceBy6Bit) {
            useBits = std::min(16, std::max(1, bitsNeeded(static_cast<unsigned short>(dstWhiteLevel)) - 6));
            dstWhiteLevel = std::pow(2.0f, useBits) - 1;
        } else if (logTransform == LogTransformMode::ReduceBy8Bit) {
            useBits = std::min(16, std::max(1, bitsNeeded(static_cast<unsigned short>(dstWhiteLevel)) - 8));
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

    // Validate parameters
    if (dstWhiteLevel <= 0 || dstWhiteLevel > 65535) {
        spdlog::error("Invalid dstWhiteLevel: {}", dstWhiteLevel);
        throw std::runtime_error("Invalid white level in preprocessData");
    }
    
    spdlog::debug("preprocessData: newWidth={}, newHeight={}, originalWidth={}, dstWhiteLevel={}, applyShadingMap={}, logTransform='{}'",
                  newWidth, newHeight, originalWidth, dstWhiteLevel, applyShadingMap, logTransformModeToString(logTransform));

    // Reinterpret the input data as uint16_t for reading
    uint16_t* srcData = reinterpret_cast<uint16_t*>(data.data());
    
    if (data.size() < sizeof(uint16_t) * originalWidth * inOutHeight) {
        spdlog::error("Input data buffer too small: {} bytes, need at least {}", 
                      data.size(), sizeof(uint16_t) * originalWidth * inOutHeight);
        throw std::runtime_error("Input buffer too small");
    }

    // Dithering is always enabled for log transforms
    const bool disableDither = false;
    
    // Process the image by copying and packing 2x2 Bayer blocks
    std::array<float, 16> shadingMapVals;
    shadingMapVals.fill(1.0f);
    std::vector<uint8_t> dst;
    dst.resize(sizeof(uint16_t) * newWidth * newHeight);
    uint16_t* dstData = reinterpret_cast<uint16_t*>(dst.data());
    
    if (dst.empty() || dstData == nullptr) {
        spdlog::error("Failed to allocate destination buffer");
        throw std::runtime_error("Destination buffer allocation failed");
    }

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
                    shadingMapVals[0] = getShadingMapValueInternal((srcX + left) * shadingMapScaleX, (srcY + top) * shadingMapScaleY, cfa[0], lensShadingMap, metadata.lensShadingMapWidth, metadata.lensShadingMapHeight);
                    shadingMapVals[1] = getShadingMapValueInternal((srcX + left + scale) * shadingMapScaleX, (srcY + top) * shadingMapScaleY, cfa[1], lensShadingMap, metadata.lensShadingMapWidth, metadata.lensShadingMapHeight);
                    shadingMapVals[2] = getShadingMapValueInternal((srcX + left) * shadingMapScaleX, (srcY + top + scale) * shadingMapScaleY, cfa[2], lensShadingMap, metadata.lensShadingMapWidth, metadata.lensShadingMapHeight);
                    shadingMapVals[3] = getShadingMapValueInternal((srcX + left + scale) * shadingMapScaleX, (srcY + top + scale) * shadingMapScaleY, cfa[3], lensShadingMap, metadata.lensShadingMapWidth, metadata.lensShadingMapHeight);
                }

                std::array<float, 4> p;

                if(debugShadingMap) {
                    for (int i = 0; i < 4; i++)
                        p[i] = std::max(0.0f, linear[i] * (srcWhiteLevel - srcBlackLevel[i]) * shadingMapVals[i]) * (dstWhiteLevel - dstBlackLevel[i]);
                } else if (logTransform == LogTransformMode::Disabled) {               // Linearize and (maybe) apply shading map
                    for (int i = 0; i < 4; i++)
                        p[i] = std::max(0.0f, linear[i] * (s[i] - srcBlackLevel[i]) * shadingMapVals[i]) * (dstWhiteLevel - dstBlackLevel[i]);
                } else {
                    std::array<float, 4> dither; // Apply logarithmic tone mapping with triangular dithering. Generate improved triangular dither with better randomization                                    
                    for (int i = 0; i < 4; i++) { // Use different seeds for each pixel in the 2x2 block to avoid correlation
                        if (!disableDither) {
                            uint32_t seed = ((x + (i & 1)) * 1664525 + (y + (i >> 1)) * 1013904223) ^ 0xdeadbeef; // Create unique seed for each pixel using position and pixel index
                            // Apply multiple hash iterations to improve randomness
                            seed ^= seed >> 16; seed *= 0x85ebca6b; seed ^= seed >> 13; seed *= 0xc2b2ae35; seed ^= seed >> 16;                    
                            // Generate triangular dither: sum of two uniform random values
                            float r1 = (seed & 0xffff) / 65535.0f; float r2 = ((seed >> 16) & 0xffff) / 65535.0f;                    
                            // Triangular distribution: r1 + r2 - 1, range [-1, 1] Scale down for subtle dithering appropriate for log encoding
                            dither[i] = (r1 + r2 - 1.0f) * 0.5f;
                        } else {
                            dither[i] = 0.0f;
                        }
                        // Apply log2 transform that preserves black and white levels as identity points
                        float logValue = std::log2(1.0f + 60.0f * std::max(0.0f, linear[i] * (s[i] - srcBlackLevel[i]) * shadingMapVals[i])) / std::log2(61.0f);                  
                        p[i] = (logValue) * dstWhiteLevel + dither[i]; // Scale by dstWhiteLevel to match what the linearization table expects
                    }
                }            
                
                for (int i = 0; i < 4; i++)
                    s[i] = std::clamp(std::round((p[i] + dstBlackLevel[i])), 0.f, dstWhiteLevel);

                // Bounds check before writing
                if (dstOffset + newWidth + 1 >= newWidth * newHeight) {
                    spdlog::error("Buffer overflow detected: dstOffset={}, newWidth={}, newHeight={}, x={}, y={}", 
                                  dstOffset, newWidth, newHeight, x, y);
                    throw std::runtime_error("Destination buffer overflow");
                }

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
                    shadingMapVals[0] = getShadingMapValueInternal((srcX + left) * shadingMapScaleX, (srcY + top) * shadingMapScaleY, 0, lensShadingMap, metadata.lensShadingMapWidth, metadata.lensShadingMapHeight);
                    shadingMapVals[1] = getShadingMapValueInternal((srcX + left + 1) * shadingMapScaleX, (srcY + top) * shadingMapScaleY, 0, lensShadingMap, metadata.lensShadingMapWidth, metadata.lensShadingMapHeight);
                    shadingMapVals[2] = getShadingMapValueInternal((srcX + left) * shadingMapScaleX, (srcY + top + 1) * shadingMapScaleY, 0, lensShadingMap, metadata.lensShadingMapWidth, metadata.lensShadingMapHeight);
                    shadingMapVals[3] = getShadingMapValueInternal((srcX + left + 1) * shadingMapScaleX, (srcY + top + 1) * shadingMapScaleY, 0, lensShadingMap, metadata.lensShadingMapWidth, metadata.lensShadingMapHeight);
                    shadingMapVals[4] = getShadingMapValueInternal((srcX + left + cfaSize * 2) * shadingMapScaleX, (srcY + top) * shadingMapScaleY, 1, lensShadingMap, metadata.lensShadingMapWidth, metadata.lensShadingMapHeight);
                    shadingMapVals[5] = getShadingMapValueInternal((srcX + left + cfaSize * 2 + 1) * shadingMapScaleX, (srcY + top) * shadingMapScaleY, 1, lensShadingMap, metadata.lensShadingMapWidth, metadata.lensShadingMapHeight);
                    shadingMapVals[6] = getShadingMapValueInternal((srcX + left + cfaSize * 2) * shadingMapScaleX, (srcY + top + 1) * shadingMapScaleY, 1, lensShadingMap, metadata.lensShadingMapWidth, metadata.lensShadingMapHeight);
                    shadingMapVals[7] = getShadingMapValueInternal((srcX + left + cfaSize * 2 + 1) * shadingMapScaleX, (srcY + top + 1) * shadingMapScaleY, 1, lensShadingMap, metadata.lensShadingMapWidth, metadata.lensShadingMapHeight);
                    shadingMapVals[8] = getShadingMapValueInternal((srcX + left) * shadingMapScaleX, (srcY + top + cfaSize * 2) * shadingMapScaleY, 2, lensShadingMap, metadata.lensShadingMapWidth, metadata.lensShadingMapHeight);
                    shadingMapVals[9] = getShadingMapValueInternal((srcX + left + 1) * shadingMapScaleX, (srcY + top + cfaSize * 2) * shadingMapScaleY, 2, lensShadingMap, metadata.lensShadingMapWidth, metadata.lensShadingMapHeight);
                    shadingMapVals[10] = getShadingMapValueInternal((srcX + left) * shadingMapScaleX, (srcY + top + cfaSize * 2 + 1) * shadingMapScaleY, 2, lensShadingMap, metadata.lensShadingMapWidth, metadata.lensShadingMapHeight);
                    shadingMapVals[11] = getShadingMapValueInternal((srcX + left + 1) * shadingMapScaleX, (srcY + top + cfaSize * 2 + 1) * shadingMapScaleY, 2, lensShadingMap, metadata.lensShadingMapWidth, metadata.lensShadingMapHeight);
                    shadingMapVals[12] = getShadingMapValueInternal((srcX + left + cfaSize * 2) * shadingMapScaleX, (srcY + top + cfaSize * 2) * shadingMapScaleY, 3, lensShadingMap, metadata.lensShadingMapWidth, metadata.lensShadingMapHeight);
                    shadingMapVals[13] = getShadingMapValueInternal((srcX + left + cfaSize * 2 + 1) * shadingMapScaleX, (srcY + top + cfaSize * 2) * shadingMapScaleY, 3, lensShadingMap, metadata.lensShadingMapWidth, metadata.lensShadingMapHeight);
                    shadingMapVals[14] = getShadingMapValueInternal((srcX + left + cfaSize * 2) * shadingMapScaleX, (srcY + top + cfaSize * 2 + 1) * shadingMapScaleY, 3, lensShadingMap, metadata.lensShadingMapWidth, metadata.lensShadingMapHeight);
                    shadingMapVals[15] = getShadingMapValueInternal((srcX + left + cfaSize * 2 + 1) * shadingMapScaleX, (srcY + top + cfaSize * 2 + 1) * shadingMapScaleY, 3, lensShadingMap, metadata.lensShadingMapWidth, metadata.lensShadingMapHeight);
                }

                std::array<float, 16> p;

                for (int i = 0; i < 16; i++)
                    p[i] = linear[i%4] * (s[i] - srcBlackLevel[i%4]) * shadingMapVals[i];

                std::array<float, 48> d;

                std::array<float, 16> r;

                /*if(cfaSize > 1 && (quadBayerOption == "Remosaic" || quadBayerOption == "Demosaic only")) {
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
                            
                            //if(bayerColor == 0) { // Red position in normal Bayer
                                r[idx] = red;
                            //}
                            //else if(bayerColor == 1) { // Green position in normal Bayer
                                //r[idx] = green;
                            //}
                            //else { // Blue position in normal Bayer
                              //  r[idx] = blue;
                            //}
                        }
                    }
                    p = r;
                }*/


                if (logTransform == LogTransformMode::Disabled) {               // Linearize and (maybe) apply shading map
                    for (int i = 0; i < 16; i++)
                        p[i] = std::max(0.0f, p[i] * (dstWhiteLevel - dstBlackLevel[i%4]));
                } else {
                    std::array<float, 16> dither; // Apply logarithmic tone mapping with triangular dithering. Generate improved triangular dither with better randomization                                    
                    for (int i = 0; i < 16; i++) { // Use different seeds for each pixel in the 2x2 block to avoid correlation
                        if (!disableDither) {
                            uint32_t seed = ((x + (i & 1)) * 1664525 + (y + (i >> 1)) * 1013904223) ^ 0xdeadbeef; // Create unique seed for each pixel using position and pixel index
                            // Apply multiple hash iterations to improve randomness
                            seed ^= seed >> 16; seed *= 0x85ebca6b; seed ^= seed >> 13; seed *= 0xc2b2ae35; seed ^= seed >> 16;                    
                            // Generate triangular dither: sum of two uniform random values
                            float r1 = (seed & 0xffff) / 65535.0f; float r2 = ((seed >> 16) & 0xffff) / 65535.0f;                    
                            // Triangular distribution: r1 + r2 - 1, range [-1, 1] Scale down for subtle dithering appropriate for log encoding
                            dither[i] = (r1 + r2 - 1.0f) * 0.5f;
                        } else {
                            dither[i] = 0.0f;
                        }
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

    return std::make_tuple(dst, blackLevelResult, static_cast<unsigned short>(dstWhiteLevel),
                           opcodeList2, opcodeList3);
}

std::shared_ptr<std::vector<char>> generateDng(
    std::vector<uint8_t>& data,
    const CameraFrameMetadata& metadata,
    const CameraConfiguration& cameraConfiguration,
    float recordingFps,
    int frameNumber,
    double baselineExpValue,
    const RenderSettings& settings,
    const std::optional<CalibrationData>& calibration,
    bool compressionEnabled,
    const std::optional<float>& baselineExposureOverride,
    const std::optional<std::array<float, 3>>& asShotNeutralOverride)
{
    Measure m("generateDng");

    unsigned int width = metadata.width;
    unsigned int height = metadata.height;

    std::array<uint8_t, 4> cfa;
    std::array<uint8_t, 16> qcfa;

    // Determine CFA pattern with priority: cfaPhase > calibration > metadata
    std::string sensorArrangement = cameraConfiguration.sensorArrangement;
    
    // Apply cfaPhase override if specified
    if (!settings.cfaPhase.empty() && settings.cfaPhase != "Don't override CFA") {
        std::string phase = settings.cfaPhase;
        std::transform(phase.begin(), phase.end(), phase.begin(), ::tolower);
        if (phase == "bggr" || phase == "rggb" || phase == "grbg" || phase == "gbrg") {
            sensorArrangement = phase;
            spdlog::debug("CFA phase override applied: {}", sensorArrangement);
        }
    }
    // Apply calibration cfaPhase if no UI override and calibration exists
    else if (calibration && !calibration->cfaPhase.empty()) {
        std::string phase = calibration->cfaPhase;
        std::transform(phase.begin(), phase.end(), phase.begin(), ::tolower);
        if (phase == "bggr" || phase == "rggb" || phase == "grbg" || phase == "gbrg") {
            sensorArrangement = phase;
            spdlog::debug("CFA phase from calibration: {}", sensorArrangement);
        }
    }

    if(sensorArrangement == "rggb")
        cfa = { 0, 1, 1, 2 };
    else if(sensorArrangement == "bggr")
        cfa = { 2, 1, 1, 0 };
    else if(sensorArrangement == "grbg")
        cfa = { 1, 0, 2, 1 };
    else if(sensorArrangement == "gbrg")
        cfa = { 1, 2, 0, 1 };
    else
        throw std::runtime_error("Invalid sensor arrangement");

    // The quality combo retains its selected scale while proxy mode is disabled.
    const int draftScale =
        settings.options & RENDER_OPT_DRAFT ? settings.draftScale : 1;

    // Extract options from settings
    bool applyShadingMap = settings.options & RENDER_OPT_APPLY_VIGNETTE_CORRECTION;
    bool vignetteOnlyColor = settings.options & RENDER_OPT_VIGNETTE_ONLY_COLOR;
    bool normalizeShadingMap = settings.options & RENDER_OPT_NORMALIZE_SHADING_MAP;
    bool debugShadingMap = settings.options & RENDER_OPT_DEBUG_SHADING_MAP;
    bool normalizeExposure = settings.options & RENDER_OPT_NORMALIZE_EXPOSURE;
    bool useLogCurve = settings.options & RENDER_OPT_LOG_TRANSFORM;
    bool interpretAsQuadBayer = metadata.needRemosaic || settings.options & RENDER_OPT_INTERPRET_AS_QUAD_BAYER;

    std::string cropTarget = settings.cropTarget;
    if(!(settings.options & RENDER_OPT_CROPPING))
        cropTarget = "0x0";

    auto [processedData, dstBlackLevel, dstWhiteLevel, opcodeList2, opcodeList3] = utils::preprocessData(
        data,
        width, height,
        metadata,
        cameraConfiguration,
        cfa,
        draftScale,
        applyShadingMap, vignetteOnlyColor, normalizeShadingMap, debugShadingMap, interpretAsQuadBayer,
        cropTarget,
        settings.levels,
        settings.logTransform,
        settings.quadBayerOption,
        true  // includeOpcode = true to generate lens shading opcode when not applied to image
    );

    spdlog::debug("New black level {},{},{},{} and white level {}",
                  dstBlackLevel[0], dstBlackLevel[1], dstBlackLevel[2], dstBlackLevel[3], dstWhiteLevel);

    // Encode to reduce size in container
    auto actualBits = utils::bitsNeeded(dstWhiteLevel);
    auto encodeBits = actualBits;

    // Skip packing if compression is enabled - lj92 needs unpacked 16-bit data
    // The compression will handle the redundancy
    if (!compressionEnabled) {
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
    }
    // For compressed data, keep as unpacked 16-bit but use actualBits for encoding

    // Create first frame
    tinydngwriter::DNGImage dng;

    dng.SetBigEndian(false);
    dng.SetDNGVersion(1, 4, 0, 0);
    dng.SetDNGBackwardVersion(1, 1, 0, 0);
    
    // Set image dimensions and format FIRST (before image data)
    dng.SetImageWidth(width);
    dng.SetImageLength(height);
    dng.SetPlanarConfig(tinydngwriter::PLANARCONFIG_CONTIG);
    dng.SetPhotometric(tinydngwriter::PHOTOMETRIC_CFA);
    dng.SetRowsPerStrip(height);
    dng.SetSamplesPerPixel(1);                                                
    dng.SetXResolution(300);
    dng.SetYResolution(300);

    dng.SetBlackLevelRepeatDim(2, 2);
        
    // Set compression based on user preference (BEFORE SetImageData)
    if (compressionEnabled) {
        if (!dng.SetCompression(tinydngwriter::COMPRESSION_JPEG)) {
            throw std::runtime_error("Failed to enable lossless JPEG compression");
        }
    } else {
        dng.SetCompression(tinydngwriter::COMPRESSION_NONE);
    }

    dng.SetIso(metadata.iso);
    dng.SetExposureTime(metadata.exposureTime / 1e9);

    float exposureOffset = (settings.cameraModel == "Panasonic" ? -2.0f : 0.0f);

    // Parse float from exposureCompensation string and add to exposureOffset
    if (!settings.exposureCompensation.empty()) {
        try {
            exposureOffset += std::stof(settings.exposureCompensation);
        } catch (const std::exception&) {
            // If parsing fails, keep the original exposureOffset value
        }
    }

    float normalizedExposureOffset = 0.0f;
    if (baselineExposureOverride.has_value()) {
        normalizedExposureOffset = *baselineExposureOverride;
    } else if (normalizeExposure) {
        normalizedExposureOffset = std::log2(baselineExpValue / (metadata.iso * metadata.exposureTime));
    }
    dng.SetBaselineExposure(normalizedExposureOffset + exposureOffset);

    if(interpretAsQuadBayer && draftScale == 1 && settings.quadBayerOption == QuadBayerMode::CorrectQBCFAMetadata) {
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

    // For compressed: use actualBits (tells lj92 the real bit depth)
    // For uncompressed: use encodeBits (the packed bit depth)
    const uint16_t bps[1] = { compressionEnabled ? actualBits : encodeBits };
    dng.SetBitsPerSample(1, bps);

    // Apply calibration data to override, otherwise use camera configuration
    // Calibration only overrides fields that are explicitly set
    if (calibration.has_value() && calibration->hasColorMatrix1) {
        dng.SetColorMatrix1(3, calibration->colorMatrix1.data());
    } else if (!isZeroMatrix(cameraConfiguration.colorMatrix1)) {
        dng.SetColorMatrix1(3, cameraConfiguration.colorMatrix1.data());
    }
    
    if (calibration.has_value() && calibration->hasColorMatrix2) {
        dng.SetColorMatrix2(3, calibration->colorMatrix2.data());
    } else if (!isZeroMatrix(cameraConfiguration.colorMatrix2)) {
        dng.SetColorMatrix2(3, cameraConfiguration.colorMatrix2.data());
    }

    if (calibration.has_value() && calibration->hasForwardMatrix1) {
        dng.SetForwardMatrix1(3, calibration->forwardMatrix1.data());
    } else if (!isZeroMatrix(cameraConfiguration.forwardMatrix1)) {
        dng.SetForwardMatrix1(3, cameraConfiguration.forwardMatrix1.data());
    }
    
    if (calibration.has_value() && calibration->hasForwardMatrix2) {
        dng.SetForwardMatrix2(3, calibration->forwardMatrix2.data());
    } else if (!isZeroMatrix(cameraConfiguration.forwardMatrix2)) {
        dng.SetForwardMatrix2(3, cameraConfiguration.forwardMatrix2.data());
    }

    dng.SetCameraCalibration1(3, IDENTITY_MATRIX);
    dng.SetCameraCalibration2(3, IDENTITY_MATRIX);

    // Apply asShotNeutral from calibration if available, otherwise from metadata
    if (calibration.has_value() && calibration->hasAsShotNeutral) {
        dng.SetAsShotNeutral(3, calibration->asShotNeutral.data());
    } else if (asShotNeutralOverride.has_value()) {
        dng.SetAsShotNeutral(3, asShotNeutralOverride->data());
    } else {
        dng.SetAsShotNeutral(3, metadata.asShotNeutral.data());
    }

    dng.SetCalibrationIlluminant1(getColorIlluminant(cameraConfiguration.colorIlluminant1));
    dng.SetCalibrationIlluminant2(getColorIlluminant(cameraConfiguration.colorIlluminant2));

    // Additional information
    const auto software = "MotionCam Tools";

    dng.SetSoftware(software);


    if(settings.cameraModel != ""){
        if (settings.cameraModel == "Blackmagic") {
            dng.SetUniqueCameraModel("Blackmagic Pocket Cinema Camera 4K");
        } else if (settings.cameraModel == "Panasonic") {
            dng.SetUniqueCameraModel("Panasonic Varicam RAW");
        } else if (settings.cameraModel == "Fujifilm" || settings.cameraModel == "Fujifilm X-T5") {
            dng.SetUniqueCameraModel("Fujifilm X-T5");
            dng.SetMake("Fujifilm");
            dng.SetCameraModelName("X-T5");
        } else {
            // Generic camera model
            dng.SetUniqueCameraModel(settings.cameraModel);
        }
    } else {
        dng.SetUniqueCameraModel(cameraConfiguration.extraData.postProcessSettings.metadata.buildModel);
    }

    // Add lens shading map as opcode list 2 if not applied to image data
    if (!opcodeList2.IsEmpty()) {
        dng.SetOpcodeList2(opcodeList2);
        spdlog::debug("Added OpcodeList2 (lens shading map)");
    } else {
        spdlog::debug("No OpcodeList2 lens shading map generated");
    }
    if (!opcodeList3.IsEmpty()) {
        dng.SetOpcodeList3(opcodeList3);
        spdlog::debug("Added OpcodeList3 (deferred luminance vignette correction)");
    }


    // Set data
    dng.SetSubfileType();

    const uint32_t activeArea[4] = { 0, 0, height, width };
    dng.SetActiveArea(&activeArea[0]);

    // Add linearization table based on actual bit depth
    bool needsLinearization = (settings.logTransform != LogTransformMode::Disabled && 
                               !(settings.logTransform == LogTransformMode::KeepInput && !applyShadingMap));
    
    if (needsLinearization && dstWhiteLevel > 0) {
        spdlog::debug("Adding linearization table: logTransform='{}', applyShadingMap={}, dstWhiteLevel={}", 
                     logTransformModeToString(settings.logTransform), applyShadingMap, dstWhiteLevel);
        // Create linearization table sized for the actual stored range
        // The stored values range from 0 to dstWhiteLevel, so we need dstWhiteLevel+1 entries
        const int tableSize = static_cast<int>(dstWhiteLevel) + 1;
        
        if (tableSize <= 0 || tableSize > 65536) {
            spdlog::error("Invalid linearization table size: {}", tableSize);
        } else {
            std::vector<unsigned short> linearizationTable(tableSize);
        
        for (int i = 0; i < tableSize; i++) {
            // Convert stored log value back to linear
            // Must match the aggressive log curve: logValue = log2(1 + k*clampedValue) / log2(1 + k)
            // Inverse: clampedValue = (2^(logValue * log2(1 + k)) - 1) / k
            
            float logValue = static_cast<float>(i);
            float normalizedLogValue = logValue / dstWhiteLevel;  // Normalize by dstWhiteLevel to match forward transform
            
            // Reverse the k=60 curve with guaranteed identity preservation
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
            spdlog::debug("Added linearization table with {} entries for log transform", tableSize);
            std::array<unsigned short, 4> linearBlackLevel = {0, 0, 0, 0};  // Linear black is 0
            dng.SetBlackLevel(4, linearBlackLevel.data());
            dng.SetWhiteLevel(static_cast<unsigned short>(65534));
        }
    } else {           
        dng.SetBlackLevel(4, dstBlackLevel.data());
        dng.SetWhiteLevel(dstWhiteLevel);
    }    

    // Set image data AFTER all metadata is configured (including BitsPerSample and Compression)
    spdlog::debug("Calling SetImageData with {} bytes, compression={}", processedData.size(), compressionEnabled);
    if (!dng.SetImageData(reinterpret_cast<const unsigned char*>(processedData.data()), processedData.size())) {
        spdlog::error("SetImageData failed: {}", dng.Error());
        throw std::runtime_error("Failed to set image data: " + dng.Error());
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

// Public implementations of shading map and bit depth functions
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

void normalizeShadingMap(std::vector<std::vector<float>>& shadingMap) {
    if (shadingMap.empty() || shadingMap[0].empty()) {
        return;
    }

    float maxValue = 0.0f;
    for (const auto& row : shadingMap) {
        for (float value : row) {
            maxValue = std::max(maxValue, value);
        }
    }

    if (maxValue == 0.0f) {
        return;
    }

    for (auto& row : shadingMap) {
        for (float& value : row) {
            value /= maxValue;
        }
    }
}

void invertShadingMap(std::vector<std::vector<float>>& shadingMap) {
    if (shadingMap.empty() || shadingMap[0].empty()) 
        return;
    
    for (const auto& row : shadingMap) 
        for (float value : row) 
            if (value <= 0.0f) 
                return;
              
    for (auto& row : shadingMap) 
        for (float& value : row) 
            value = 1 / value;
}

void colorOnlyShadingMap(
    std::vector<std::vector<float>>& shadingMap,
    int lensShadingMapWidth,
    int lensShadingMapHeight,
    const std::array<uint8_t, 4> cfa)
{
    if (shadingMap.empty() || shadingMap[0].empty())
        return;

    float maxValue = 0.0f;
    for (const auto& row : shadingMap) 
        for (float value : row) 
            maxValue = std::max(maxValue, value);
    
    if (maxValue == 0.0f)
        return;

    bool aggressive = false;

    auto minValue00 = 10.0f;
    auto minValue01 = 10.0f;
    auto minValue10 = 10.0f;
    auto minValue11 = 10.0f;

    for (int j = 0; j < lensShadingMapHeight; j++) {
        for (int i = 0; i < lensShadingMapWidth; i++) {
            if (shadingMap[0][j*lensShadingMapWidth+i] < minValue00)
                minValue00 = shadingMap[0][j*lensShadingMapWidth+i];
            if (shadingMap[1][j*lensShadingMapWidth+i] < minValue01)
                minValue01 = shadingMap[1][j*lensShadingMapWidth+i];
            if (shadingMap[2][j*lensShadingMapWidth+i] < minValue10)
                minValue10 = shadingMap[2][j*lensShadingMapWidth+i];
            if (shadingMap[3][j*lensShadingMapWidth+i] < minValue11)
                minValue11 = shadingMap[3][j*lensShadingMapWidth+i];
        }
    }

    if (cfa == std::array<uint8_t, 4>{0, 1, 1, 2} || cfa == std::array<uint8_t, 4>{2, 1, 1, 0}) {
        minValue01 = std::min(minValue01, minValue10);
        minValue01 = minValue10;
    } else if (cfa == std::array<uint8_t, 4>{1, 0, 2, 1} || cfa == std::array<uint8_t, 4>{1, 2, 0, 1}) {
        minValue00 = std::min(minValue00, minValue11);
        minValue00 = minValue11;
    }
    
    for (int j = 0; j < lensShadingMapHeight; j++) {
        for (int i = 0; i < lensShadingMapWidth; i++) {
            if (aggressive) {
                shadingMap[0][j*lensShadingMapWidth+i] = shadingMap[0][j*lensShadingMapWidth+i] / minValue00;
                shadingMap[1][j*lensShadingMapWidth+i] = shadingMap[1][j*lensShadingMapWidth+i] / minValue01;
                shadingMap[2][j*lensShadingMapWidth+i] = shadingMap[2][j*lensShadingMapWidth+i] / minValue10;
                shadingMap[3][j*lensShadingMapWidth+i] = shadingMap[3][j*lensShadingMapWidth+i] / minValue11;
            }
            auto localMinValue = std::min(
                shadingMap[0][j*lensShadingMapWidth+i],
                std::min(shadingMap[1][j*lensShadingMapWidth+i],
                std::min(shadingMap[2][j*lensShadingMapWidth+i],
                shadingMap[3][j*lensShadingMapWidth+i])));
            for (int channel = 0; channel < 4; channel++) {
                shadingMap[channel][j*lensShadingMapWidth+i] = 
                    shadingMap[channel][j*lensShadingMapWidth+i] / localMinValue;
            }
        }
    }
}

float getShadingMapValue(
    float x, float y,
    int channel,
    const std::vector<std::vector<float>>& lensShadingMap,
    int lensShadingMapWidth,
    int lensShadingMapHeight)
{
    x = std::max(0.0f, std::min(1.0f, x));
    y = std::max(0.0f, std::min(1.0f, y));

    const float mapX = x * (lensShadingMapWidth - 1);
    const float mapY = y * (lensShadingMapHeight - 1);

    const int x0 = static_cast<int>(std::floor(mapX));
    const int y0 = static_cast<int>(std::floor(mapY));
    const int x1 = std::min(x0 + 1, lensShadingMapWidth - 1);
    const int y1 = std::min(y0 + 1, lensShadingMapHeight - 1);

    const float wx = mapX - x0;
    const float wy = mapY - y0;

    const float val00 = lensShadingMap[channel][y0*lensShadingMapWidth+x0];
    const float val01 = lensShadingMap[channel][y0*lensShadingMapWidth+x1];
    const float val10 = lensShadingMap[channel][y1*lensShadingMapWidth+x0];
    const float val11 = lensShadingMap[channel][y1*lensShadingMapWidth+x1];

    const float valTop = val00 * (1.0f - wx) + val01 * wx;
    const float valBottom = val10 * (1.0f - wx) + val11 * wx;

    return valTop * (1.0f - wy) + valBottom * wy;
}

void remosaicRGBToBayer(const std::vector<uint16_t>& rgbData, std::vector<uint16_t>& bayerData, 
                        int width, int height, const std::string& cfaPhase) {
    // Determine CFA pattern (default to BGGR if not specified or invalid)
    std::string pattern = cfaPhase.empty() ? "bggr" : cfaPhase;
    std::transform(pattern.begin(), pattern.end(), pattern.begin(), ::tolower);
    
    // Validate pattern
    if (pattern != "bggr" && pattern != "rggb" && pattern != "grbg" && pattern != "gbrg") {
        pattern = "bggr";
    }
    
    // Allocate output buffer (single channel)
    bayerData.resize(width * height);
    
    // Map pattern to channel indices at each position
    // Pattern format: [0,0] [0,1] [1,0] [1,1]
    int channelMap[2][2]; // [row%2][col%2] -> channel (0=R, 1=G, 2=B)
    
    if (pattern == "bggr") {
        // B G
        // G R
        channelMap[0][0] = 2; channelMap[0][1] = 1;
        channelMap[1][0] = 1; channelMap[1][1] = 0;
    } else if (pattern == "rggb") {
        // R G
        // G B
        channelMap[0][0] = 0; channelMap[0][1] = 1;
        channelMap[1][0] = 1; channelMap[1][1] = 2;
    } else if (pattern == "grbg") {
        // G R
        // B G
        channelMap[0][0] = 1; channelMap[0][1] = 0;
        channelMap[1][0] = 2; channelMap[1][1] = 1;
    } else { // gbrg
        // G B
        // R G
        channelMap[0][0] = 1; channelMap[0][1] = 2;
        channelMap[1][0] = 0; channelMap[1][1] = 1;
    }
    
    // Convert RGB to Bayer by selecting appropriate channel for each pixel
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            int channel = channelMap[y % 2][x % 2];
            int rgbIdx = (y * width + x) * 3 + channel;
            int bayerIdx = y * width + x;
            bayerData[bayerIdx] = rgbData[rgbIdx];
        }
    }
}

// Simple 5x7 bitmap font for digits and common characters
namespace {
    // Each character is 5 pixels wide, 7 pixels tall
    // Stored as 7 bytes, each byte represents a row (5 LSBs used)
    const uint8_t FONT_5x7[][7] = {
        // '0'
        {0b01110, 0b10001, 0b10011, 0b10101, 0b11001, 0b10001, 0b01110},
        // '1'
        {0b00100, 0b01100, 0b00100, 0b00100, 0b00100, 0b00100, 0b01110},
        // '2'
        {0b01110, 0b10001, 0b00001, 0b00010, 0b00100, 0b01000, 0b11111},
        // '3'
        {0b01110, 0b10001, 0b00001, 0b00110, 0b00001, 0b10001, 0b01110},
        // '4'
        {0b00010, 0b00110, 0b01010, 0b10010, 0b11111, 0b00010, 0b00010},
        // '5'
        {0b11111, 0b10000, 0b11110, 0b00001, 0b00001, 0b10001, 0b01110},
        // '6'
        {0b00110, 0b01000, 0b10000, 0b11110, 0b10001, 0b10001, 0b01110},
        // '7'
        {0b11111, 0b00001, 0b00010, 0b00100, 0b01000, 0b01000, 0b01000},
        // '8'
        {0b01110, 0b10001, 0b10001, 0b01110, 0b10001, 0b10001, 0b01110},
        // '9'
        {0b01110, 0b10001, 0b10001, 0b01111, 0b00001, 0b00010, 0b01100},
        // 'I'
        {0b01110, 0b00100, 0b00100, 0b00100, 0b00100, 0b00100, 0b01110},
        // 'S'
        {0b01110, 0b10001, 0b10000, 0b01110, 0b00001, 0b10001, 0b01110},
        // 'O'
        {0b01110, 0b10001, 0b10001, 0b10001, 0b10001, 0b10001, 0b01110},
        // ' ' (space)
        {0b00000, 0b00000, 0b00000, 0b00000, 0b00000, 0b00000, 0b00000},
    };
    
    int getCharIndex(char c) {
        if (c >= '0' && c <= '9') return c - '0';
        if (c == 'I') return 10;
        if (c == 'S') return 11;
        if (c == 'O') return 12;
        if (c == ' ') return 13;
        return 13; // Default to space
    }
}

void burnInText(
    std::vector<uint8_t>& data,
    uint32_t width,
    uint32_t height,
    const std::string& text,
    uint16_t whiteLevel)
{
    if (text.empty() || data.size() < width * height * sizeof(uint16_t)) {
        return;
    }
    
    uint16_t* pixels = reinterpret_cast<uint16_t*>(data.data());
    
    // Scale font size based on image width (roughly 1% of width per character)
    const int baseCharWidth = 5;
    const int baseCharHeight = 7;
    const int scale = std::max(1, static_cast<int>(width / 800)); // Scale factor
    const int charWidth = baseCharWidth * scale;
    const int charHeight = baseCharHeight * scale;
    const int charSpacing = 2 * scale;
    
    // Calculate text dimensions
    const int textWidth = text.length() * (charWidth + charSpacing) - charSpacing;
    const int textHeight = charHeight;
    
    // Position: lower middle of screen
    const int startX = (width - textWidth) / 2;
    const int startY = height - textHeight - (height / 20); // 5% from bottom
    
    // Ensure we don't go out of bounds
    if (startY < 0 || startY + textHeight >= height || startX < 0) {
        return;
    }
    
    // Render each character
    int xOffset = startX;
    for (char c : text) {
        int charIdx = getCharIndex(c);
        
        // Draw character
        for (int row = 0; row < baseCharHeight; ++row) {
            uint8_t rowData = FONT_5x7[charIdx][row];
            for (int col = 0; col < baseCharWidth; ++col) {
                if (rowData & (1 << (baseCharWidth - 1 - col))) {
                    // Draw scaled pixel
                    for (int sy = 0; sy < scale; ++sy) {
                        for (int sx = 0; sx < scale; ++sx) {
                            int px = xOffset + col * scale + sx;
                            int py = startY + row * scale + sy;
                            
                            if (px >= 0 && px < width && py >= 0 && py < height) {
                                pixels[py * width + px] = whiteLevel;
                            }
                        }
                    }
                }
            }
        }
        
        xOffset += charWidth + charSpacing;
    }
}

} // namespace utils
} // namespace motioncam
