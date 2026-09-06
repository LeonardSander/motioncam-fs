#include "Utils.h"
#include "Measure.h"

#include "CameraFrameMetadata.h"
#include "CameraMetadata.h"
#include "DataLevels.h"
#include "VirtualFileSystemImpl.h"
#include "DNGDecoder.h"
#include "GainMapBake.h"

#include <algorithm>
#include <cmath>
#include <numeric>

#include <boost/iostreams/stream.hpp>
#include <boost/iostreams/device/back_inserter.hpp>

#define TINY_DNG_WRITER_IMPLEMENTATION 1

#include <tinydng/tiny_dng_writer.h>

namespace motioncam {
namespace utils {

namespace {

std::vector<GainMap> canonicalGainMaps(
        const std::vector<std::vector<float>>& planes, uint32_t width,
        uint32_t height) {
    if (planes.empty()) return {};
    GainMap map{};
    map.width = width;
    map.height = height;
    map.channels = static_cast<uint32_t>(planes.size());
    map.rowPitch = map.colPitch = 1;
    const size_t points = static_cast<size_t>(width) * height;
    map.data.resize(points * map.channels);
    for (size_t point = 0; point < points; ++point)
        for (size_t channel = 0; channel < planes.size(); ++channel)
            map.data[point * map.channels + channel] = planes[channel][point];
    return {std::move(map)};
}

void storeCanonicalGainMaps(const std::vector<GainMap>& maps,
                            std::vector<std::vector<float>>& planes) {
    if (maps.empty()) return;
    const auto& map = maps.front();
    const size_t points = static_cast<size_t>(map.width) * map.height;
    for (size_t point = 0; point < points; ++point)
        for (size_t channel = 0; channel < map.channels; ++channel)
            planes[channel][point] = map.data[point * map.channels + channel];
}

} // namespace

void overrideLensShadingMap(
        CameraFrameMetadata& metadata, const std::vector<GainMap>& gainMaps) {
    if (gainMaps.empty()) return;
    const uint32_t width = gainMaps.front().width;
    const uint32_t height = gainMaps.front().height;
    if (!width || !height) throw std::invalid_argument("Invalid gain-map override dimensions");
    std::vector<std::vector<float>> planes;
    if (gainMaps.size() == 1) {
        const auto& map = gainMaps.front();
        if (map.channels != 1 && map.channels != 4)
            throw std::invalid_argument("MCRAW gain-map override requires one or four channels");
        planes.resize(map.channels);
        const size_t points = static_cast<size_t>(width) * height;
        for (auto& plane : planes) plane.resize(points);
        for (size_t point = 0; point < points; ++point)
            for (uint32_t channel = 0; channel < map.channels; ++channel)
                planes[channel][point] = map.data[point * map.channels + channel];
    } else if (gainMaps.size() == 4) {
        planes.reserve(4);
        for (const auto& map : gainMaps) {
            if (map.width != width || map.height != height || map.channels != 1)
                throw std::invalid_argument("MCRAW gain-map override planes must share dimensions");
            planes.push_back(map.data);
        }
    } else {
        throw std::invalid_argument("MCRAW gain-map override requires one map or four planes");
    }
    metadata.lensShadingMap = std::move(planes);
    metadata.lensShadingMapWidth = static_cast<int>(width);
    metadata.lensShadingMapHeight = static_cast<int>(height);
}

std::vector<unsigned short> makeLogLinearizationTable(unsigned int storedWhiteLevel) {
    if (storedWhiteLevel == 0 || storedWhiteLevel >= 65536) return {};
    std::vector<unsigned short> table(storedWhiteLevel + 1);
    for (unsigned int i = 0; i <= storedWhiteLevel; ++i) {
        float linear = 0.0f;
        if (i == storedWhiteLevel) {
            linear = 1.0f;
        } else if (i != 0) {
            const float encoded = static_cast<float>(i) / storedWhiteLevel;
            linear = (std::pow(2.0f, encoded * std::log2(61.0f)) - 1.0f) / 60.0f;
            linear = std::clamp(linear, 0.0f, 1.0f);
        }
        table[i] = static_cast<unsigned short>(linear * 65535.0f);
    }
    return table;
}


void parseCropTarget(const std::string& target, uint32_t& width,
                     uint32_t& height, uint32_t& stride) {
    width = height = stride = 0;
    const size_t separatorPos = target.find('x');
    if (separatorPos == std::string::npos)
        return;
    try {
        size_t heightEnd = 0;
        width = std::stoul(target.substr(0, separatorPos));
        height = std::stoul(target.substr(separatorPos + 1), &heightEnd);
        const size_t suffixPos = separatorPos + 1 + heightEnd;
        if (suffixPos < target.size()) {
            if (target[suffixPos] != '_' || suffixPos + 1 >= target.size())
                throw std::invalid_argument("invalid crop suffix");
            size_t strideEnd = 0;
            stride = std::stoul(target.substr(suffixPos + 1), &strideEnd);
            if (suffixPos + 1 + strideEnd != target.size())
                throw std::invalid_argument("invalid stride");
        }
    } catch (const std::exception&) {
        width = height = stride = 0;
    }
}

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

    inline float getShadingMapValueInternal(
        float x, float y, int channel, const std::vector<std::vector<float>>& lensShadingMap, int lensShadingMapWidth, int lensShadingMapHeight)
    {
        return getShadingMapValue(x, y, channel, lensShadingMap,
                                  lensShadingMapWidth, lensShadingMapHeight);
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


void addSinglePlaneGainMaps(tinydngwriter::OpcodeList& opcodeList,
                            const tinydngwriter::GainMapParams& params,
                            bool cfaPhases)
{
    if (params.map_planes <= 1) {
        opcodeList.AddGainMap(params);
        return;
    }
    const size_t planeSize =
        static_cast<size_t>(params.map_points_v) * params.map_points_h;
    if (params.gain_data.size() != planeSize * params.map_planes ||
        (cfaPhases && params.map_planes != 4)) {
        return;
    }
    for (unsigned int channel = 0; channel < params.map_planes; ++channel) {
        auto single = params;
        single.map_planes = 1;
        single.gain_data.assign(params.gain_data.begin() + channel * planeSize,
                                params.gain_data.begin() + (channel + 1) * planeSize);
        if (cfaPhases) {
            single.top = channel / 2;
            single.left = channel % 2;
            single.row_pitch = 2;
            single.col_pitch = 2;
        } else {
            single.plane = params.plane + channel;
            single.planes = 1;
        }
        opcodeList.AddGainMap(single);
    }
}

tinydngwriter::OpcodeList createLensShadingOpcodeList(
    const CameraFrameMetadata& metadata,
    uint32_t imageWidth,
    uint32_t imageHeight,
    int left,
    int top,
    unsigned int targetPlanes)
{
    tinydngwriter::OpcodeList opcodeList;
    
    if (metadata.lensShadingMap.empty() || 
        metadata.lensShadingMapWidth <= 0 || 
        metadata.lensShadingMapHeight <= 0) {
        return opcodeList; // Return empty list if no shading map
    }
    
    // Build GainMap opcodes compatible with DNG readers which represent a
    // Bayer shading map as one opcode per CFA phase (not as four MapPlanes in
    // a single-plane raw image).
    tinydngwriter::GainMapParams gainParams{};
    
    // The emitted DNG's raw IFD and ActiveArea both start at (0, 0).
    gainParams.top = 0;
    gainParams.left = 0;
    gainParams.bottom = imageHeight;
    gainParams.right = imageWidth;
    
    // Apply starting from plane 0
    gainParams.plane = 0;
    // A CFA image has one stored image plane; the map itself carries the four
    // Bayer-phase gain planes.
    unsigned int availablePlanes = static_cast<unsigned int>(metadata.lensShadingMap.size());
    if (availablePlanes == 0) availablePlanes = 1;
    gainParams.planes = targetPlanes;
    
    // Grid size in the gain map
    const unsigned int mapPointsV = static_cast<unsigned int>(metadata.lensShadingMapHeight);
    const unsigned int mapPointsH = static_cast<unsigned int>(metadata.lensShadingMapWidth);
    gainParams.map_points_v = mapPointsV;
    gainParams.map_points_h = mapPointsH;
    
    // A multi-channel CFA map addresses one of the four 2x2 phases per opcode.
    gainParams.row_pitch = 1;
    gainParams.col_pitch = 1;
    
    // The shading grid is defined over the original sensor image. Express its
    // grid locations in the coordinate system of the cropped output image so
    // deferred opcode correction samples the same values as the baked path.
    const double coordinateHeight = metadata.originalHeight > 0
        ? metadata.originalHeight : imageHeight;
    const double coordinateWidth = metadata.originalWidth > 0
        ? metadata.originalWidth : imageWidth;
    gainParams.map_spacing_v = mapPointsV > 1
        ? coordinateHeight / imageHeight / (mapPointsV - 1) : 1.0;
    gainParams.map_spacing_h = mapPointsH > 1
        ? coordinateWidth / imageWidth / (mapPointsH - 1) : 1.0;
    gainParams.map_origin_v = -static_cast<double>(std::max(0, top)) / imageHeight;
    gainParams.map_origin_h = -static_cast<double>(std::max(0, left)) / imageWidth;
    
    gainParams.map_planes = std::min(4u, availablePlanes);
    
    // Fill gain data in plane-major, row-major order
    if (!metadata.lensShadingMap.empty() && !metadata.lensShadingMap[0].empty()) {
        const size_t perPlaneSize = static_cast<size_t>(mapPointsV) * static_cast<size_t>(mapPointsH);
        gainParams.gain_data.reserve(perPlaneSize * gainParams.map_planes);
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
        addSinglePlaneGainMaps(opcodeList, gainParams, gainParams.map_planes > 1);
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
    bool optimizeGainMaps,
    float& gainMapExposureOffset,
    uint32_t cfaRepeatSize,
    bool higherCfaHq,
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

    const uint32_t cfaGroupSize = std::max(1u, cfaRepeatSize / 2);
    const uint32_t proxyGroupSize = cfaGroupSize;
    uint32_t cfaSize = (interpretAsQuadBayer ? 2 : 1);
    // Keep proxy sampling aligned to complete same-colour CFA blocks. The
    // requested UI scale is rounded up to the next valid block multiple.
    const uint32_t sourceScale = cfaRepeatSize > 2 && scale > 1
        ? proxyGroupSize * std::max(1u, (scale + proxyGroupSize - 1) / proxyGroupSize)
        : scale;

    uint32_t newWidth, newHeight;
    uint32_t cropWidth = 0, cropHeight = 0;

    uint32_t ignoredStride = 0;
    parseCropTarget(cropTarget, cropWidth, cropHeight, ignoredStride);

    if (cropWidth > 0 && cropHeight > 0 && cropWidth <= inOutWidth && cropHeight <= inOutHeight) {
        newWidth = cropWidth / sourceScale;
        newHeight = cropHeight / sourceScale;
    } else {
        // Calculate new dimensions
        newWidth = inOutWidth / sourceScale;
        newHeight = inOutHeight / sourceScale;
    }
    
    // Align to 4 for bayer pattern and also because we read 4 bytes at a time when encoding to 10/14 bit
    newWidth = (newWidth / 4) * 4;
    newHeight = (newHeight / 4) * 4;    

    const auto resolvedLevels = resolveDataLevels(
        levels, metadata.dynamicWhiteLevel, metadata.dynamicBlackLevel,
        cameraConfiguration.whiteLevel, cameraConfiguration.blackLevel);
    auto srcBlackLevel = resolvedLevels.black;
    auto srcWhiteLevel = resolvedLevels.white;

    uint32_t hqReductionShift = 0;
    if(cfaRepeatSize > 2 && scale > 1 && higherCfaHq) {
        const float area = static_cast<float>(proxyGroupSize * proxyGroupSize);
        srcWhiteLevel *= area;
        for (int i = 0; i < srcBlackLevel.size(); i++) {
            srcBlackLevel[i] *= area;
        }
        float largestLevel = srcWhiteLevel;
        for (float black : srcBlackLevel)
            largestLevel = std::max(largestLevel, black);
        while (largestLevel > std::numeric_limits<uint16_t>::max()) {
            largestLevel *= 0.5f;
            ++hqReductionShift;
        }
        const float divisor = static_cast<float>(uint32_t{1} << hqReductionShift);
        srcWhiteLevel /= divisor;
        for (float& black : srcBlackLevel)
            black /= divisor;
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

    tinydngwriter::OpcodeList opcodeList2;
    tinydngwriter::OpcodeList opcodeList3;

    if (vignetteOnlyColor) {
        auto maps = canonicalGainMaps(lensShadingMap,
            metadata.lensShadingMapWidth, metadata.lensShadingMapHeight);
        auto separation = lensShadingMap.size() >= 4
            ? separateGainMapLuminance(maps)
            : GainMapLuminanceSeparation<GainMap>{};
        if (applyShadingMap && separation.valid) {
            CameraFrameMetadata luminanceMetadata = metadata;
            if (optimizeGainMaps) {
                const float luminanceMinimum =
                    normalizePositiveGainMinimum(separation.luminance.data);
                gainMapExposureOffset += std::log2(luminanceMinimum);
            }
            luminanceMetadata.lensShadingMap.assign(
                1, std::move(separation.luminance.data));
            // OpcodeList3 runs on demosaiced RGB. Its single map plane is
            // reused for all three target image planes.
            opcodeList3 = createLensShadingOpcodeList(
                luminanceMetadata, inOutWidth, inOutHeight, left, top, 3);
        }
        // Without baking, intentionally discard luminance and retain only the
        // local color ratios in OpcodeList2.
        if (separation.valid)
            storeCanonicalGainMaps(maps, lensShadingMap);
        else
            for (auto& plane : lensShadingMap)
                std::fill(plane.begin(), plane.end(), 1.0f);
    }
    if (applyShadingMap) {
        auto maps = canonicalGainMaps(lensShadingMap,
            metadata.lensShadingMapWidth, metadata.lensShadingMapHeight);
        transformGainMapLayersForBake<GainMap>(
            std::array<std::vector<GainMap>*, 1>{&maps},
            normaliseShadingMap, debugShadingMap);
        storeCanonicalGainMaps(maps, lensShadingMap);
    }
    // Linear vignette baking expands the resolved levels by an exact bit shift.
    // Retaining the correspondingly scaled destination black level gives
    // unsigned DNG samples room for negative, black-subtracted excursions.
    // Log encoding remains black-subtracted and uses a zero destination black.
    if(applyShadingMap) {
        if (logTransform != LogTransformMode::Disabled) {
            if (normaliseShadingMap) {
                useBits = std::min(16, utils::bitsNeeded(static_cast<unsigned short>(dstWhiteLevel)) + 4);
            } else if (!debugShadingMap) {
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
            }
            for(auto& v : dstBlackLevel)
                v = 0;
        } else {
            std::array<double, 4> sourceBlack{};
            std::copy(srcBlackLevel.begin(), srcBlackLevel.end(), sourceBlack.begin());
            const auto bakeLevels = planLinearGainBake(
                srcWhiteLevel, sourceBlack, normaliseShadingMap);
            useBits = static_cast<int>(bakeLevels.destinationBits);
            dstWhiteLevel = static_cast<float>(bakeLevels.destinationWhite);
            for (size_t channel = 0; channel < dstBlackLevel.size(); ++channel)
                dstBlackLevel[channel] = static_cast<float>(bakeLevels.destinationBlack[channel]);
        }
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
    if(includeOpcode && !applyShadingMap) {
        // Create lens shading map as opcode list 2 gain map
        CameraFrameMetadata opcodeMetadata = metadata;
        opcodeMetadata.lensShadingMap = lensShadingMap;
        opcodeList2 = createLensShadingOpcodeList(opcodeMetadata, inOutWidth, inOutHeight, left, top);
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

    // Dithering is always enabled for DNG log transforms.
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
            uint32_t srcY = y * sourceScale;
            uint32_t srcX = x * sourceScale;
 
            if (cfaSize < 2 | scale > 1) {
                std::array<uint16_t, 4> s;
                if (cfaRepeatSize > 2 && scale > 1) {
                    const uint32_t selection = (proxyGroupSize - 1) / 2;
                    for (uint32_t by = 0; by < 2; ++by) {
                        for (uint32_t bx = 0; bx < 2; ++bx) {
                            const uint32_t anchorX = srcX + bx * proxyGroupSize;
                            const uint32_t anchorY = srcY + by * proxyGroupSize;
                            uint32_t value = 0;
                            if (higherCfaHq) {
                                for (uint32_t gy = 0; gy < proxyGroupSize; ++gy)
                                    for (uint32_t gx = 0; gx < proxyGroupSize; ++gx)
                                        value += srcData[(anchorY + gy) * originalWidth + anchorX + gx];
                            } else {
                                value = srcData[(anchorY + selection) * originalWidth + anchorX + selection];
                            }
                            value >>= hqReductionShift;
                            s[by * 2 + bx] = static_cast<uint16_t>(value);
                        }
                    }
                } else {
                    s[0] = srcData[srcY * originalWidth + srcX];
                    s[1] = srcData[srcY * originalWidth + srcX + cfaSize];
                    s[2] = srcData[(srcY + cfaSize) * originalWidth + srcX];
                    s[3] = srcData[(srcY + cfaSize) * originalWidth + srcX + cfaSize];
                }                
                
                if(applyShadingMap) {                              
                    // Calculate position in shading map     
                    const int group = cfaGroupSize;
                    auto shadingChannel = [&](uint32_t px, uint32_t py) {
                        return cfa[((py / group) & 1) * 2 + ((px / group) & 1)];
                    };
                    shadingMapVals[0] = getShadingMapValueInternal((srcX + left) * shadingMapScaleX, (srcY + top) * shadingMapScaleY, shadingChannel(srcX, srcY), lensShadingMap, metadata.lensShadingMapWidth, metadata.lensShadingMapHeight);
                    shadingMapVals[1] = getShadingMapValueInternal((srcX + left + proxyGroupSize) * shadingMapScaleX, (srcY + top) * shadingMapScaleY, shadingChannel(srcX + proxyGroupSize, srcY), lensShadingMap, metadata.lensShadingMapWidth, metadata.lensShadingMapHeight);
                    shadingMapVals[2] = getShadingMapValueInternal((srcX + left) * shadingMapScaleX, (srcY + top + proxyGroupSize) * shadingMapScaleY, shadingChannel(srcX, srcY + proxyGroupSize), lensShadingMap, metadata.lensShadingMapWidth, metadata.lensShadingMapHeight);
                    shadingMapVals[3] = getShadingMapValueInternal((srcX + left + proxyGroupSize) * shadingMapScaleX, (srcY + top + proxyGroupSize) * shadingMapScaleY, shadingChannel(srcX + proxyGroupSize, srcY + proxyGroupSize), lensShadingMap, metadata.lensShadingMapWidth, metadata.lensShadingMapHeight);
                }

                std::array<float, 4> p;

                if(debugShadingMap) {
                    for (int i = 0; i < 4; i++)
                        p[i] = std::max(0.0f, linear[i] * (srcWhiteLevel - srcBlackLevel[i]) * shadingMapVals[i]) * (dstWhiteLevel - dstBlackLevel[i]);
                } else if (logTransform == LogTransformMode::Disabled) {               // Linearize and (maybe) apply shading map
                    for (int i = 0; i < 4; i++)
                        p[i] = static_cast<float>(applyLinearGain(
                            s[i], shadingMapVals[i], srcBlackLevel[i], srcWhiteLevel,
                            dstBlackLevel[i], dstWhiteLevel) - dstBlackLevel[i]);
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
                    shadingMapVals[4] = getShadingMapValueInternal((srcX + left + 2) * shadingMapScaleX, (srcY + top) * shadingMapScaleY, 1, lensShadingMap, metadata.lensShadingMapWidth, metadata.lensShadingMapHeight);
                    shadingMapVals[5] = getShadingMapValueInternal((srcX + left + 3) * shadingMapScaleX, (srcY + top) * shadingMapScaleY, 1, lensShadingMap, metadata.lensShadingMapWidth, metadata.lensShadingMapHeight);
                    shadingMapVals[6] = getShadingMapValueInternal((srcX + left + 2) * shadingMapScaleX, (srcY + top + 1) * shadingMapScaleY, 1, lensShadingMap, metadata.lensShadingMapWidth, metadata.lensShadingMapHeight);
                    shadingMapVals[7] = getShadingMapValueInternal((srcX + left + 3) * shadingMapScaleX, (srcY + top + 1) * shadingMapScaleY, 1, lensShadingMap, metadata.lensShadingMapWidth, metadata.lensShadingMapHeight);
                    shadingMapVals[8] = getShadingMapValueInternal((srcX + left) * shadingMapScaleX, (srcY + top + 2) * shadingMapScaleY, 2, lensShadingMap, metadata.lensShadingMapWidth, metadata.lensShadingMapHeight);
                    shadingMapVals[9] = getShadingMapValueInternal((srcX + left + 1) * shadingMapScaleX, (srcY + top + 2) * shadingMapScaleY, 2, lensShadingMap, metadata.lensShadingMapWidth, metadata.lensShadingMapHeight);
                    shadingMapVals[10] = getShadingMapValueInternal((srcX + left) * shadingMapScaleX, (srcY + top + 3) * shadingMapScaleY, 2, lensShadingMap, metadata.lensShadingMapWidth, metadata.lensShadingMapHeight);
                    shadingMapVals[11] = getShadingMapValueInternal((srcX + left + 1) * shadingMapScaleX, (srcY + top + 3) * shadingMapScaleY, 2, lensShadingMap, metadata.lensShadingMapWidth, metadata.lensShadingMapHeight);
                    shadingMapVals[12] = getShadingMapValueInternal((srcX + left + 2) * shadingMapScaleX, (srcY + top + 2) * shadingMapScaleY, 3, lensShadingMap, metadata.lensShadingMapWidth, metadata.lensShadingMapHeight);
                    shadingMapVals[13] = getShadingMapValueInternal((srcX + left + 3) * shadingMapScaleX, (srcY + top + 2) * shadingMapScaleY, 3, lensShadingMap, metadata.lensShadingMapWidth, metadata.lensShadingMapHeight);
                    shadingMapVals[14] = getShadingMapValueInternal((srcX + left + 2) * shadingMapScaleX, (srcY + top + 3) * shadingMapScaleY, 3, lensShadingMap, metadata.lensShadingMapWidth, metadata.lensShadingMapHeight);
                    shadingMapVals[15] = getShadingMapValueInternal((srcX + left + 3) * shadingMapScaleX, (srcY + top + 3) * shadingMapScaleY, 3, lensShadingMap, metadata.lensShadingMapWidth, metadata.lensShadingMapHeight);
                }

                std::array<float, 16> p;

                for (int i = 0; i < 16; i++)
                    p[i] = linear[i/4] * (s[i] - srcBlackLevel[i/4]) * shadingMapVals[i];

                if (logTransform == LogTransformMode::Disabled) {               // Linearize and (maybe) apply shading map
                    for (int i = 0; i < 16; i++)
                        p[i] = static_cast<float>(applyLinearGain(
                            s[i], shadingMapVals[i], srcBlackLevel[i/4], srcWhiteLevel,
                            dstBlackLevel[i/4], dstWhiteLevel) - dstBlackLevel[i/4]);
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
                    s[i] = std::clamp(std::round((p[i] + dstBlackLevel[i/4])), 0.f, dstWhiteLevel);
                    
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

    if(sensorArrangement != "rggb" && sensorArrangement != "bggr" &&
       sensorArrangement != "grbg" && sensorArrangement != "gbrg")
        throw std::runtime_error("Invalid sensor arrangement");
    cfa = cfaColorsFromPhase(sensorArrangement);

    CameraFrameMetadata gainMetadata = metadata;
    float gainMapExposureOffset = 0.0f;
    std::array<float, 3> gainMapNeutralScale{1.0f, 1.0f, 1.0f};
    if ((settings.options & RENDER_OPT_OPTIMIZE_GAIN_MAPS) &&
        !gainMetadata.lensShadingMap.empty()) {
        auto maps = canonicalGainMaps(gainMetadata.lensShadingMap,
            gainMetadata.lensShadingMapWidth, gainMetadata.lensShadingMapHeight);
        const auto adjustment = optimizeGainMapLayers<GainMap>(
            std::array{&maps}, cfa);
        gainMapExposureOffset = static_cast<float>(adjustment.exposureOffset);
        gainMapNeutralScale = adjustment.neutralScale;
        storeCanonicalGainMaps(maps, gainMetadata.lensShadingMap);
    }

    // The quality combo retains its selected scale while proxy mode is disabled.
    const int draftScale =
        settings.options & RENDER_OPT_DRAFT ? settings.draftScale : 1;

    // Extract options from settings
    bool applyShadingMap = settings.options & RENDER_OPT_APPLY_VIGNETTE_CORRECTION;
    bool vignetteOnlyColor = settings.options & RENDER_OPT_VIGNETTE_ONLY_COLOR;
    bool normalizeShadingMap = settings.options & RENDER_OPT_NORMALIZE_SHADING_MAP;
    bool debugShadingMap = settings.options & RENDER_OPT_DEBUG_SHADING_MAP;
    bool normalizeExposure = settings.options & RENDER_OPT_NORMALIZE_EXPOSURE;
    LogTransformMode effectiveLogTransform =
        settings.options & RENDER_OPT_LOG_TRANSFORM
            ? settings.logTransform
            : LogTransformMode::Disabled;
    // Keep Input exists to avoid losing precision when vignette correction is
    // baked into the pixels. Without that processing the input is already
    // linear, so adding a log curve and LinearizationTable would be incorrect.
    if (effectiveLogTransform == LogTransformMode::KeepInput && !applyShadingMap)
        effectiveLogTransform = LogTransformMode::Disabled;
    int cfaRepeatSize = calibration && calibration->hasCfaSize
        ? calibration->cfaSize : metadata.cfaSize;
    if (cfaRepeatSize < 2 || (cfaRepeatSize % 2) != 0)
        cfaRepeatSize = metadata.needRemosaic ? 4 : 2;
    const bool higherCFA = cfaRepeatSize > 2;
    const bool explicitBinning = settings.quadBayerOption == QuadBayerMode::Binning ||
                                 settings.quadBayerOption == QuadBayerMode::Bin8x8To4x4;
    const bool lossyJpegDct = compressionEnabled && isLossyJpegDct(settings.jxlDistance);
    const bool hqProxy = draftScale > 1 &&
        (settings.options & RENDER_OPT_HIGHER_CFA_HQ);
    const int preprocessScale = hqProxy || explicitBinning ? 1 : draftScale;
    const bool demosaic = (higherCFA || settings.cameraNativeStaging || hqProxy) &&
        (hqProxy || draftScale == 1) &&
        (!settings.streamingPreview ||
         (settings.options & RENDER_OPT_HIGHER_CFA_HQ)) &&
        (hqProxy || settings.quadBayerOption == QuadBayerMode::Demosaic ||
         settings.quadBayerOption == QuadBayerMode::DemosaicColor ||
         settings.quadBayerOption == QuadBayerMode::DemosaicOCL);
    const bool remosaic = demosaic && (settings.options & RENDER_OPT_REMOSAIC_TO_BAYER);

    if (settings.streamingPreview && frameNumber == 0) {
        spdlog::info(
            "Gallery MCRAW staging: cfa_repeat={} draft_scale={} hq={} demosaic={} remosaic={} camera_native={}",
            cfaRepeatSize, draftScale, hqProxy, demosaic, remosaic,
            settings.cameraNativeStaging);
    }

    std::string cropTarget = settings.cropTarget;
    if(!(settings.options & RENDER_OPT_CROPPING))
        cropTarget = "0x0";

    struct ActiveBadPixel { uint32_t row; uint32_t column; };
    std::vector<ActiveBadPixel> activeBadPixels;
    if (calibration && calibration->hasBadPixels &&
        settings.badPixelTreatment != BadPixelTreatment::Disabled) {
        if (settings.badPixelTreatment == BadPixelTreatment::OpcodeOnly &&
            std::any_of(calibration->badPixels.begin(), calibration->badPixels.end(), [](const auto& pixel) {
                return pixel.action != CalibrationData::BadPixelAction::Interpolate;
            }))
            spdlog::warn("DNG bad-pixel opcodes can only represent interpolation; brighten/dampen entries are skipped unless the image is demosaiced");
        const auto levelsForDefects = resolveDataLevels(
            settings.levels, metadata.dynamicWhiteLevel, metadata.dynamicBlackLevel,
            cameraConfiguration.whiteLevel, cameraConfiguration.blackLevel);
        auto* samples = reinterpret_cast<uint16_t*>(data.data());
        const std::vector<uint16_t> source(samples, samples + static_cast<size_t>(width) * height);
        const int group = std::max(1, cfaRepeatSize / 2);
        const int sensorLeft = std::max(0, (metadata.originalWidth - static_cast<int>(width)) / 2);
        const int sensorTop = std::max(0, (metadata.originalHeight - static_cast<int>(height)) / 2);
        auto phaseAt = [&](int x, int y) {
            return ((y / group) & 1) * 2 + ((x / group) & 1);
        };
        auto interpolateBad = [&](int x, int y) {
            std::vector<uint16_t> neighbours;
            const int phase = phaseAt(x + sensorLeft, y + sensorTop);
            for (int radius = 1; radius <= std::max(4, group * 2) && neighbours.size() < 4; ++radius)
                for (int dy = -radius; dy <= radius; ++dy)
                    for (int dx = -radius; dx <= radius; ++dx) {
                        if (std::max(std::abs(dx), std::abs(dy)) != radius) continue;
                        const int nx = x + dx, ny = y + dy;
                        if (nx < 0 || ny < 0 || nx >= static_cast<int>(width) || ny >= static_cast<int>(height)) continue;
                        if (phaseAt(nx + sensorLeft, ny + sensorTop) == phase)
                            neighbours.push_back(source[static_cast<size_t>(ny) * width + nx]);
                    }
            if (neighbours.empty()) return source[static_cast<size_t>(y) * width + x];
            const auto middle = neighbours.begin() + neighbours.size() / 2;
            std::nth_element(neighbours.begin(), middle, neighbours.end());
            return *middle;
        };
        const double exposureSeconds = metadata.exposureTime / 1.0e9;
        for (const auto& defect : calibration->badPixels) {
            const int firstX = defect.repeatX ? defect.x : defect.x - sensorLeft;
            const int firstY = defect.repeatY ? defect.y : defect.y - sensorTop;
            const int stepX = defect.repeatX ? defect.repeatX : static_cast<int>(width) + 1;
            const int stepY = defect.repeatY ? defect.repeatY : static_cast<int>(height) + 1;
            int localStartX = defect.repeatX ? firstX - sensorLeft : firstX;
            int localStartY = defect.repeatY ? firstY - sensorTop : firstY;
            if (defect.repeatX) while (localStartX < 0) localStartX += stepX;
            if (defect.repeatY) while (localStartY < 0) localStartY += stepY;
            for (int y = localStartY; y < static_cast<int>(height); y += stepY)
                for (int x = localStartX; x < static_cast<int>(width); x += stepX) {
                    if (x < 0 || y < 0 || metadata.iso < defect.minIso || exposureSeconds < defect.minExposureSeconds) continue;
                    const int phase = phaseAt(x + sensorLeft, y + sensorTop);
                    const float black = levelsForDefects.black[phase];
                    const float range = std::max(1.0f, levelsForDefects.white - black);
                    const float normalized = std::clamp((source[static_cast<size_t>(y) * width + x] - black) / range, 0.0f, 1.0f);
                    if ((defect.thresholdAbove && normalized < *defect.thresholdAbove) ||
                        (defect.thresholdBelow && normalized > *defect.thresholdBelow)) continue;
                    if (defect.action == CalibrationData::BadPixelAction::Interpolate)
                        activeBadPixels.push_back({static_cast<uint32_t>(y), static_cast<uint32_t>(x)});
                    const bool bake = settings.badPixelTreatment == BadPixelTreatment::Bake || demosaic;
                    if (!bake) continue;
                    const size_t index = static_cast<size_t>(y) * width + x;
                    if (defect.action == CalibrationData::BadPixelAction::Interpolate) {
                        samples[index] = interpolateBad(x, y);
                    } else {
                        const float factor = defect.action == CalibrationData::BadPixelAction::Brighten
                            ? 1.0f + defect.amount : 1.0f - defect.amount;
                        samples[index] = static_cast<uint16_t>(std::clamp(
                            std::lround(black + std::max(0.0f, source[index] - black) * factor), 0l, 65535l));
                    }
                }
        }
    }

    auto [processedData, dstBlackLevel, dstWhiteLevel, opcodeList2, opcodeList3] = utils::preprocessData(
        data,
        width, height,
        gainMetadata,
        cameraConfiguration,
        cfa,
        preprocessScale,
        applyShadingMap, vignetteOnlyColor, normalizeShadingMap, debugShadingMap,
        settings.options & RENDER_OPT_OPTIMIZE_GAIN_MAPS,
        gainMapExposureOffset,
        cfaRepeatSize, settings.options & RENDER_OPT_HIGHER_CFA_HQ,
        cfaRepeatSize == 4,
        cropTarget,
        settings.levels,
        effectiveLogTransform,
        settings.quadBayerOption,
        true // includeOpcode
    );

    int processedRepeatSize = cfaRepeatSize;
    if (explicitBinning && processedRepeatSize > 2) {
        std::vector<uint16_t> source(static_cast<size_t>(width) * height);
        std::memcpy(source.data(), processedData.data(), source.size() * sizeof(uint16_t));
        const uint32_t factor = settings.quadBayerOption == QuadBayerMode::Bin8x8To4x4 &&
                                processedRepeatSize == 8
            ? 2u : static_cast<uint32_t>(processedRepeatSize / 2);
        std::vector<uint16_t> binned;
        uint32_t binnedWidth = 0, binnedHeight = 0;
        binHigherCFA(source, binned, width, height, factor,
                     binnedWidth, binnedHeight,
                     effectiveLogTransform != LogTransformMode::Disabled && !debugShadingMap
                         ? dstWhiteLevel : 0);
        if (binned.empty()) throw std::runtime_error("Could not bin higher-CFA image");
        processedData.resize(binned.size() * sizeof(uint16_t));
        std::memcpy(processedData.data(), binned.data(), processedData.size());
        width = binnedWidth;
        height = binnedHeight;
        processedRepeatSize /= static_cast<int>(factor);

        if (draftScale > 1 && !hqProxy) {
            source = std::move(binned);
            const uint32_t group = static_cast<uint32_t>(processedRepeatSize / 2);
            const uint32_t sourceScale = group * std::max(1u,
                (static_cast<uint32_t>(draftScale) + group - 1) / group);
            const uint32_t reducedWidth = (width / sourceScale) & ~3u;
            const uint32_t reducedHeight = (height / sourceScale) & ~3u;
            if (!reducedWidth || !reducedHeight)
                throw std::runtime_error("Proxy scale is too large for the binned image");
            std::vector<uint16_t> reduced(static_cast<size_t>(reducedWidth) * reducedHeight);
            const uint32_t selection = (group - 1) / 2;
            for (uint32_t y = 0; y < reducedHeight; y += 2)
                for (uint32_t x = 0; x < reducedWidth; x += 2)
                    for (uint32_t by = 0; by < 2; ++by)
                        for (uint32_t bx = 0; bx < 2; ++bx)
                            reduced[static_cast<size_t>(y + by) * reducedWidth + x + bx] =
                                source[(static_cast<size_t>(y) * sourceScale + by * group + selection) * width +
                                       x * sourceScale + bx * group + selection];
            processedData.resize(reduced.size() * sizeof(uint16_t));
            std::memcpy(processedData.data(), reduced.data(), processedData.size());
            width = reducedWidth;
            height = reducedHeight;
            processedRepeatSize = 2;
        }
    }

    if (demosaic) {
        std::vector<uint16_t> cfaSamples(static_cast<size_t>(width) * height);
        std::memcpy(cfaSamples.data(), processedData.data(), cfaSamples.size() * sizeof(uint16_t));
        std::array<uint32_t, 3> blackSums = {0, 0, 0};
        std::array<uint32_t, 3> blackCounts = {0, 0, 0};
        for (int phaseIndex = 0; phaseIndex < 4; ++phaseIndex) {
            const int channel = cfa[phaseIndex];
            blackSums[channel] += dstBlackLevel[phaseIndex];
            ++blackCounts[channel];
        }
        std::array<uint16_t, 3> channelBlack = {0, 0, 0};
        for (int channel = 0; channel < 3; ++channel)
            if (blackCounts[channel])
                channelBlack[channel] = static_cast<uint16_t>(
                    std::lround(static_cast<double>(blackSums[channel]) / blackCounts[channel]));

        std::vector<uint16_t> rgbSamples;
        demosaicHigherCFA(cfaSamples, rgbSamples, width, height,
                          processedRepeatSize, cfa, settings.quadBayerOption,
                          {static_cast<float>(channelBlack[0]),
                           static_cast<float>(channelBlack[1]),
                           static_cast<float>(channelBlack[2])});
        // Interpolation and luma-detail restoration may legitimately overshoot
        // the input range. Clamp before packed 2/4/6/8/10/12/14-bit encoding;
        // otherwise the packers retain only the low bits and highlights wrap.
        const uint16_t outputWhite = dstWhiteLevel;
        for (uint16_t& sample : rgbSamples)
            sample = std::min(sample, outputWhite);
        if (hqProxy) {
            std::vector<uint16_t> reduced;
            uint32_t reducedWidth = 0, reducedHeight = 0;
            const uint32_t rgbScale = static_cast<uint32_t>(draftScale);
            reduceRGB(rgbSamples, reduced, width, height,
                      rgbScale, true,
                      reducedWidth, reducedHeight,
                      effectiveLogTransform != LogTransformMode::Disabled && !debugShadingMap
                          ? dstWhiteLevel : 0);
            if (reduced.empty())
                throw std::runtime_error("Proxy scale is too large for the image");
            rgbSamples = std::move(reduced);
            width = reducedWidth;
            height = reducedHeight;
        }
        if (remosaic) {
            std::vector<uint16_t> bayerSamples;
            std::string phase;
            for (uint8_t value : cfa) phase += value == 0 ? 'r' : (value == 2 ? 'b' : 'g');
            remosaicRGBToBayer(rgbSamples, bayerSamples, width, height, phase);
            processedData.resize(bayerSamples.size() * sizeof(uint16_t));
            std::memcpy(processedData.data(), bayerSamples.data(), processedData.size());
        } else {
            processedData.resize(rgbSamples.size() * sizeof(uint16_t));
            std::memcpy(processedData.data(), rgbSamples.data(), processedData.size());
            dstBlackLevel = {channelBlack[0], channelBlack[1], channelBlack[2], 0};

            // FFmpeg's TIFF/DNG decoder rejects packed 10/12-bit RGB. Camera
            // Native staging uses a conventional unpacked 16-bit linear RGB
            // image. LOG60 is applied by FFmpeg immediately before YUV conversion.
            if (settings.cameraNativeStaging) {
                auto* rgb = reinterpret_cast<uint16_t*>(processedData.data());
                for (size_t i = 0; i < rgbSamples.size(); ++i) {
                    const size_t channel = i % 3;
                    const float black = static_cast<float>(dstBlackLevel[channel]);
                    const float range = std::max(1.0f, static_cast<float>(dstWhiteLevel) - black);
                    const float normalized = std::clamp((static_cast<float>(rgb[i]) - black) / range, 0.0f, 1.0f);
                    rgb[i] = static_cast<uint16_t>(std::lround(normalized * 65535.0f));
                }

                dstBlackLevel = {0, 0, 0, 0};
                dstWhiteLevel = 65535;
            }
        }
    }

    if (settings.options & RENDER_OPT_BAKE_ISO) {
        const int overlayChannels = demosaic && !remosaic ? 3 : 1;
        bakeIsoOverlay(reinterpret_cast<uint16_t*>(processedData.data()), width, height,
                       overlayChannels, metadata.iso,
                       *std::min_element(dstBlackLevel.begin(), dstBlackLevel.end()),
                       dstWhiteLevel);
    }

    spdlog::debug("New black level {},{},{},{} and white level {}",
                  dstBlackLevel[0], dstBlackLevel[1], dstBlackLevel[2], dstBlackLevel[3], dstWhiteLevel);

    // Encode to reduce size in container
    auto actualBits = utils::bitsNeeded(dstWhiteLevel);
    auto encodeBits = actualBits;
    const bool writerCompression = compressionEnabled && !lossyJpegDct;
    const bool jpegXlCompression = writerCompression && settings.jxlDistance >= 0.0f;

    // Compressed codecs consume unpacked uint16 samples.
    // The compression will handle the redundancy
    if (!writerCompression && !settings.cameraNativeStaging && !lossyJpegDct) {
        if (demosaic && !remosaic) {
            if (encodeBits <= 4) encodeRGBTo4Bit(processedData, width, height), encodeBits = 4;
            else if (encodeBits <= 6) encodeRGBTo6Bit(processedData, width, height), encodeBits = 6;
            else if (encodeBits <= 8) encodeRGBTo8Bit(processedData, width, height), encodeBits = 8;
            else if (encodeBits <= 10) encodeRGBTo10Bit(processedData, width, height), encodeBits = 10;
            else if (encodeBits <= 12) encodeRGBTo12Bit(processedData, width, height), encodeBits = 12;
            else encodeBits = 16; // RGB has no packed 14-bit encoder.
        }
        else if(encodeBits <= 2) {
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
    // Camera-native/gallery staging intentionally keeps processedData as
    // unpacked uint16_t samples. RGB staging already reaches 16 bits through
    // its normalization above, but a non-HQ proxy may remain CFA at the
    // sensor's 10/12/14-bit white level. Advertising that nominal depth makes
    // readers treat the uint16 buffer as tightly packed rows, producing a
    // resolution-dependent stride mismatch and a scrambled gallery image.
    if (settings.cameraNativeStaging)
        encodeBits = 16;
    // Compressed codecs consume unpacked uint16 samples. Uncompressed output,
    // however, must advertise the bit depth of the buffer after packing. This
    // differs from the sensor precision for RGB sourced from 13/14-bit data:
    // there is no packed RGB encoder for those depths, so that path deliberately
    // leaves uint16 samples in processedData and sets encodeBits to 16.

    // Create first frame
    tinydngwriter::DNGImage dng;

    dng.SetBigEndian(false);
    dng.SetDNGVersion(1, jpegXlCompression ? 7 : 4, 0, 0);
    tinydngwriter::OpcodeList opcodeList1;
    if (settings.badPixelTreatment == BadPixelTreatment::OpcodeOnly && !demosaic &&
        preprocessScale == 1 && cropTarget == "0x0" && !activeBadPixels.empty()) {
        if (cfaRepeatSize != 2)
            spdlog::warn("FixBadPixelsList is Bayer-specific; compatibility depends on the DNG reader for {}x{} CFA data",
                         cfaRepeatSize, cfaRepeatSize);
        tinydngwriter::FixBadPixelsParams params;
        for (const auto& pixel : activeBadPixels)
            params.bad_pixels.push_back({pixel.row, pixel.column});
        const auto phaseName = [&]() {
            std::string value;
            for (uint8_t color : cfa) value += color == 0 ? 'r' : color == 2 ? 'b' : 'g';
            return value;
        }();
        params.bayer_phase = phaseName == "rggb" ? 0 : phaseName == "grbg" ? 1 : phaseName == "gbrg" ? 2 : 3;
        opcodeList1.AddFixBadPixelsList(params);
    }
    const bool hasStageOpcodes = !opcodeList1.IsEmpty() || !opcodeList2.IsEmpty() || !opcodeList3.IsEmpty();
    dng.SetDNGBackwardVersion(
        1, jpegXlCompression ? 7 : (hasStageOpcodes ? 3 : 1), 0, 0);
    
    // Set image dimensions and format FIRST (before image data)
    dng.SetImageWidth(width);
    dng.SetImageLength(height);
    dng.SetPlanarConfig(tinydngwriter::PLANARCONFIG_CONTIG);
    dng.SetPhotometric(demosaic && !remosaic
        ? tinydngwriter::PHOTOMETRIC_LINEARRAW : tinydngwriter::PHOTOMETRIC_CFA);
    dng.SetRowsPerStrip(height);
    const int samplesPerPixel = demosaic && !remosaic ? 3 : 1;
    dng.SetSamplesPerPixel(samplesPerPixel);
    dng.SetXResolution(300);
    dng.SetYResolution(300);

    // BlackLevel count is repeatRows * repeatCols * SamplesPerPixel. RGB
    // LinearRaw therefore uses one spatial cell containing R/G/B levels;
    // mosaiced output keeps the ordinary 2x2 phase grid.
    dng.SetBlackLevelRepeatDim(samplesPerPixel == 3 ? 1 : 2,
                               samplesPerPixel == 3 ? 1 : 2);
        
    // Set compression based on user preference (BEFORE SetImageData)
    if (writerCompression) {
        if (settings.jxlDistance < 0.0f) {
            if (!dng.SetCompression(tinydngwriter::COMPRESSION_JPEG))
                throw std::runtime_error("Failed to enable JPEG 92 compression");
        } else if (!dng.SetCompression(tinydngwriter::COMPRESSION_JPEG_XL) ||
                   !dng.SetJXLDistance(settings.jxlDistance))
            throw std::runtime_error("Failed to enable JPEG XL compression");
    } else {
        dng.SetCompression(tinydngwriter::COMPRESSION_NONE);
    }

    dng.SetIso(metadata.iso);
    dng.SetExposureTime(metadata.exposureTime / 1e9);

    const float exposureOffset = vfs::configuredExposureOffset(settings);

    float normalizedExposureOffset = 0.0f;
    if (baselineExposureOverride.has_value()) {
        normalizedExposureOffset = *baselineExposureOverride;
    } else if (normalizeExposure) {
        normalizedExposureOffset = std::log2(baselineExpValue / (metadata.iso * metadata.exposureTime));
    }
    dng.SetBaselineExposure(normalizedExposureOffset + exposureOffset + gainMapExposureOffset);

    if ((explicitBinning || settings.streamingPreview) && !demosaic &&
        draftScale == 1 && processedRepeatSize > 2) {
        dng.SetCFARepeatPatternDim(processedRepeatSize, processedRepeatSize);
        std::vector<uint8_t> expanded(
            static_cast<size_t>(processedRepeatSize) * processedRepeatSize);
        const int group = processedRepeatSize / 2;
        for (int y = 0; y < processedRepeatSize; ++y)
            for (int x = 0; x < processedRepeatSize; ++x)
                expanded[static_cast<size_t>(y) * processedRepeatSize + x] =
                    cfa[((y / group) & 1) * 2 + ((x / group) & 1)];
        dng.SetCFAPattern(static_cast<unsigned int>(expanded.size()), expanded.data());
    } else if (higherCFA && !demosaic && draftScale == 1 &&
        settings.quadBayerOption == QuadBayerMode::CorrectQBCFAMetadata) {
        dng.SetCFARepeatPatternDim(cfaRepeatSize, cfaRepeatSize);
        std::vector<uint8_t> expanded(static_cast<size_t>(cfaRepeatSize) * cfaRepeatSize);
        const int group = cfaRepeatSize / 2;
        for (int y = 0; y < cfaRepeatSize; ++y)
            for (int x = 0; x < cfaRepeatSize; ++x)
                expanded[static_cast<size_t>(y) * cfaRepeatSize + x] =
                    cfa[((y / group) & 1) * 2 + ((x / group) & 1)];
        dng.SetCFAPattern(static_cast<unsigned int>(expanded.size()), expanded.data());
    } else if (!(demosaic && !remosaic)) {
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

    // DNG 1.7 JPEG XL is decoded through a uint16 pixel buffer. Keep sensor
    // values unchanged and describe their meaningful range with WhiteLevel.
    // For uncompressed: use encodeBits (the packed bit depth). Advertising
    // actualBits here made a 14-bit LinearRaw DNG claim three packed 14-bit
    // samples while its strip actually contained three uint16 samples. Strict
    // readers such as darktable reject that inconsistent strip layout.
    const uint16_t storedBits = (jpegXlCompression || lossyJpegDct) ? 16 : encodeBits;
    const uint16_t bps[3] = { storedBits, storedBits, storedBits };
    dng.SetBitsPerSample(samplesPerPixel, bps);

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
    std::array<float, 3> outputNeutral = metadata.asShotNeutral;
    if (calibration.has_value() && calibration->hasAsShotNeutral)
        outputNeutral = calibration->asShotNeutral;
    else if (asShotNeutralOverride.has_value())
        outputNeutral = *asShotNeutralOverride;
    for (size_t color = 0; color < 3; ++color)
        outputNeutral[color] *= gainMapNeutralScale[color];
    dng.SetAsShotNeutral(3, outputNeutral.data());

    dng.SetCalibrationIlluminant1(getColorIlluminant(cameraConfiguration.colorIlluminant1));
    dng.SetCalibrationIlluminant2(getColorIlluminant(cameraConfiguration.colorIlluminant2));

    // Additional information
    const auto software = "MotionCam Tools";

    dng.SetSoftware(software);


    const auto identity = vfs::resolveCameraIdentity(settings.cameraModel,
        cameraConfiguration.extraData.postProcessSettings.metadata.buildModel);
    dng.SetUniqueCameraModel(identity.uniqueModel);
    if (!identity.make.empty()) dng.SetMake(identity.make);
    if (!identity.model.empty()) dng.SetCameraModelName(identity.model);

    if (!opcodeList1.IsEmpty()) {
        dng.SetOpcodeList1(opcodeList1);
        spdlog::debug("Added OpcodeList1 (bad pixels)");
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
    const bool needsLinearization =
        effectiveLogTransform != LogTransformMode::Disabled;
    
    if (needsLinearization && dstWhiteLevel > 0) {
        spdlog::debug("Adding linearization table: logTransform='{}', applyShadingMap={}, dstWhiteLevel={}", 
                     logTransformModeToString(effectiveLogTransform), applyShadingMap, dstWhiteLevel);
        auto linearizationTable = makeLogLinearizationTable(dstWhiteLevel);
        if (linearizationTable.empty()) {
            spdlog::error("Invalid linearization table white level: {}", dstWhiteLevel);
        } else {
            dng.SetLinearizationTable(static_cast<unsigned int>(linearizationTable.size()),
                                      linearizationTable.data());
            spdlog::debug("Added linearization table with {} entries for log transform",
                          linearizationTable.size());
            std::array<unsigned short, 4> linearBlackLevel = {0, 0, 0, 0};  // Linear black is 0
            dng.SetBlackLevel(samplesPerPixel == 3 ? 3 : 4, linearBlackLevel.data());
            dng.SetWhiteLevel(static_cast<unsigned short>(65534));
        }
    } else {           
        dng.SetBlackLevel(samplesPerPixel == 3 ? 3 : 4, dstBlackLevel.data());
        dng.SetWhiteLevel(dstWhiteLevel);
    }    

    // Set image data AFTER all metadata is configured (including BitsPerSample and Compression)
    spdlog::debug("Calling SetImageData with {} bytes, compression={}", processedData.size(), writerCompression);
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

    if (lossyJpegDct) {
        std::vector<uint8_t> bytes(output->begin(), output->end());
        if (!DNGDecoder::compressLossyJPEG(bytes))
            throw std::runtime_error("Failed to enable lossy JPEG DCT compression");
        output->assign(bytes.begin(), bytes.end());
    }

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

float getShadingMapValue(
    float x, float y,
    int channel,
    const std::vector<std::vector<float>>& lensShadingMap,
    int lensShadingMapWidth,
    int lensShadingMapHeight)
{
    const auto sx = sampleGainMapAxis(
        std::clamp(x, 0.0f, 1.0f) * (lensShadingMapWidth - 1),
        lensShadingMapWidth);
    const auto sy = sampleGainMapAxis(
        std::clamp(y, 0.0f, 1.0f) * (lensShadingMapHeight - 1),
        lensShadingMapHeight);
    return sampleGainMapBilinear(sx, sy, [&](uint32_t px, uint32_t py) {
        return lensShadingMap[channel][py * lensShadingMapWidth + px];
    });
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

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"

bool generateJpegThumbnail(
    std::vector<uint8_t>& data,
    const CameraFrameMetadata& metadata,
    const CameraConfiguration& cameraConfiguration,
    const std::string& outputPath,
    int thumbWidth,
    int thumbHeight) {
    const int width = static_cast<int>(metadata.width);
    const int height = static_cast<int>(metadata.height);
    if (width < 2 || height < 2 || thumbWidth < 1 || thumbHeight < 1 ||
        data.size() < static_cast<size_t>(width) * height * sizeof(uint16_t))
        return false;

    const auto cfa = cfaColorsFromPhase(cameraConfiguration.sensorArrangement);

    const float black = std::accumulate(metadata.dynamicBlackLevel.begin(),
        metadata.dynamicBlackLevel.end(), 0.0f) / 4.0f;
    const float range = std::max(1.0f, metadata.dynamicWhiteLevel - black);
    const std::array<float, 3> gains{
        1.0f / std::max(metadata.asShotNeutral[0], 0.001f),
        1.0f / std::max(metadata.asShotNeutral[1], 0.001f),
        1.0f / std::max(metadata.asShotNeutral[2], 0.001f)};
    const float scale = std::min(static_cast<float>(thumbWidth) / width,
                                 static_cast<float>(thumbHeight) / height);
    const int renderedWidth = std::max(1, static_cast<int>(std::lround(width * scale)));
    const int renderedHeight = std::max(1, static_cast<int>(std::lround(height * scale)));
    const auto* raw = reinterpret_cast<const uint16_t*>(data.data());
    std::vector<uint8_t> rgb(static_cast<size_t>(renderedWidth) * renderedHeight * 3);
    auto srgb = [](float value) {
        value = std::clamp(value, 0.0f, 1.0f);
        return value <= 0.0031308f ? value * 12.92f
                                  : 1.055f * std::pow(value, 1.0f / 2.4f) - 0.055f;
    };
    for (int y = 0; y < renderedHeight; ++y) {
        for (int x = 0; x < renderedWidth; ++x) {
            int sx = std::clamp(static_cast<int>(x / scale), 0, width - 2) & ~1;
            int sy = std::clamp(static_cast<int>(y / scale), 0, height - 2) & ~1;
            const int base = sy * width + sx;
            const uint16_t px[4]{raw[base], raw[base + 1], raw[base + width], raw[base + width + 1]};
            float channels[3]{};
            int counts[3]{};
            for (int i = 0; i < 4; ++i) {
                channels[cfa[i]] += px[i];
                ++counts[cfa[i]];
            }
            const size_t out = (static_cast<size_t>(y) * renderedWidth + x) * 3;
            for (int channel = 0; channel < 3; ++channel) {
                const float sample = counts[channel] ? channels[channel] / counts[channel] : black;
                const float value = ((sample - black) / range) * gains[channel];
                rgb[out + channel] = static_cast<uint8_t>(std::lround(srgb(value) * 255.0f));
            }
        }
    }
    return stbi_write_jpg(outputPath.c_str(), renderedWidth, renderedHeight, 3,
                          rgb.data(), 88) != 0;
}

bool generateJpegThumbnailFromDng(
    std::vector<uint8_t> data,
    const std::string& outputPath,
    int thumbWidth,
    int thumbHeight) {
    if (data.empty() || thumbWidth < 1 || thumbHeight < 1) return false;
    // UI thumbnails are rendered only from the primary raw image. Discard all
    // embedded previews before any decompression or CFA processing.
    if (!DNGDecoder::removeThumbnails(data)) return false;
    int repeatSize = 2;
    std::array<uint8_t, 4> phase{0, 1, 1, 2};
    const bool hasCfa = DNGDecoder::getCFAMetadata(data, repeatSize, phase);
    if (!DNGDecoder::ensureUncompressed(data, true)) return false;
    if (hasCfa) {
        // A UI thumbnail never needs a full-resolution demosaic. First use the
        // existing sparse proxy path to reduce the CFA to a small ordinary
        // Bayer image, then demosaic only that reduced image. Besides avoiding
        // hundreds of MiB of temporary RGB, this keeps thumbnail work from
        // starving mounted-frame reads.
        constexpr int thumbnailProxyScale = 16;
        if (!DNGDecoder::processHigherCFA(
                data, repeatSize, phase, QuadBayerMode::Demosaic, false,
                thumbnailProxyScale, false) ||
            !DNGDecoder::processHigherCFA(
                data, 2, phase, QuadBayerMode::Demosaic, false, 1, false))
            return false;
    }

    std::vector<uint8_t> rgb16;
    uint32_t width = 0, height = 0;
    if (!DNGDecoder::extractUncompressedRGB16(data, rgb16, width, height) ||
        width < 1 || height < 1) return false;

    DNGFrameMetadata metadata;
    if (!DNGDecoder::getColorMetadata(data, metadata)) return false;
    std::vector<uint16_t> samples(rgb16.size() / sizeof(uint16_t));
    for (size_t i = 0; i < samples.size(); ++i)
        samples[i] = static_cast<uint16_t>(
            rgb16[i * 2] | static_cast<uint16_t>(rgb16[i * 2 + 1]) << 8);
    return generateJpegThumbnailFromRgb16(
        samples, width, height, metadata.asShotNeutral,
        outputPath, thumbWidth, thumbHeight);
}

bool generateJpegThumbnailFromRgb16(
    const std::vector<uint16_t>& data,
    uint32_t width,
    uint32_t height,
    const std::array<float, 3>& asShotNeutral,
    const std::string& outputPath,
    int thumbWidth,
    int thumbHeight) {
    if (width < 1 || height < 1 || thumbWidth < 1 || thumbHeight < 1 ||
        data.size() < static_cast<size_t>(width) * height * 3) return false;
    const std::array<float, 3> gains{
        1.0f / std::max(asShotNeutral[0], 0.001f),
        1.0f / std::max(asShotNeutral[1], 0.001f),
        1.0f / std::max(asShotNeutral[2], 0.001f)};
    const float scale = std::min(static_cast<float>(thumbWidth) / width,
                                 static_cast<float>(thumbHeight) / height);
    const int renderedWidth = std::max(1, static_cast<int>(std::lround(width * scale)));
    const int renderedHeight = std::max(1, static_cast<int>(std::lround(height * scale)));
    std::vector<uint8_t> rgb(static_cast<size_t>(renderedWidth) * renderedHeight * 3);
    auto srgb = [](float value) {
        value = std::clamp(value, 0.0f, 1.0f);
        return value <= 0.0031308f ? value * 12.92f
            : 1.055f * std::pow(value, 1.0f / 2.4f) - 0.055f;
    };
    for (int y = 0; y < renderedHeight; ++y) {
        for (int x = 0; x < renderedWidth; ++x) {
            const uint32_t sx = std::min(width - 1,
                static_cast<uint32_t>(x / scale));
            const uint32_t sy = std::min(height - 1,
                static_cast<uint32_t>(y / scale));
            const size_t input = (static_cast<size_t>(sy) * width + sx) * 3;
            const size_t output = (static_cast<size_t>(y) * renderedWidth + x) * 3;
            std::array<float, 3> linear{};
            for (int channel = 0; channel < 3; ++channel)
                linear[channel] = (data[input + channel] / 65535.0f) * gains[channel];
            for (int channel = 0; channel < 3; ++channel) {
                const float value = linear[channel];
                rgb[output + channel] = static_cast<uint8_t>(std::lround(srgb(value) * 255.0f));
            }
        }
    }
    return stbi_write_jpg(outputPath.c_str(), renderedWidth, renderedHeight, 3,
                          rgb.data(), 88) != 0;
}

} // namespace utils
} // namespace motioncam
