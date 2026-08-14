#include "DNGDecoder.h"
#include "DataLevels.h"
#include "Utils.h"
#include <spdlog/spdlog.h>
#include <boost/filesystem.hpp>
#include <boost/algorithm/string.hpp>
#include <boost/regex.hpp>
#include <fstream>
#include <algorithm>
#include <cstring>
#include <cmath>
#include <limits>
#include <set>
#include "liblj92/lj92.h"
#include <jxl/decode.h>
#include <jxl/encode.h>

#ifdef _MSC_VER
#include <stdlib.h>
#define __builtin_bswap16(x) _byteswap_ushort(x)
#define __builtin_bswap32(x) _byteswap_ulong(x)
#endif

namespace motioncam {

namespace {
    // DNG tag constants
    constexpr uint16_t TIFF_TAG_OPCODE_LIST_2 = 51009;
    constexpr uint16_t TIFF_TAG_OPCODE_LIST_3 = 51022;
    constexpr uint32_t OPCODE_GAIN_MAP = 9;
    
    // TIFF header constants
    constexpr uint16_t TIFF_LITTLE_ENDIAN = 0x4949;
    constexpr uint16_t TIFF_BIG_ENDIAN = 0x4D4D;
    constexpr uint16_t TIFF_MAGIC = 42;

    constexpr uint16_t TIFF_TAG_EXPOSURE_TIME = 33434;
    constexpr uint16_t TIFF_TAG_EXIF_IFD = 34665;
    constexpr uint16_t TIFF_TAG_ISO = 34855;
    constexpr uint16_t TIFF_TAG_SUB_IFDS = 330;
    constexpr uint16_t TIFF_TAG_AS_SHOT_NEUTRAL = 50728;
    constexpr uint16_t TIFF_TAG_COLOR_MATRIX_1 = 50721;
    constexpr uint16_t TIFF_TAG_COLOR_MATRIX_2 = 50722;
    constexpr uint16_t TIFF_TAG_FORWARD_MATRIX_1 = 50964;
    constexpr uint16_t TIFF_TAG_FORWARD_MATRIX_2 = 50965;
    constexpr uint16_t TIFF_TAG_BASELINE_EXPOSURE = 50730;
    constexpr uint16_t TIFF_TYPE_BYTE = 1;
    constexpr uint16_t TIFF_TYPE_SHORT = 3;
    constexpr uint16_t TIFF_TYPE_LONG = 4;
    constexpr uint16_t TIFF_TYPE_RATIONAL = 5;
    constexpr uint16_t TIFF_TYPE_SRATIONAL = 10;
    constexpr uint16_t TIFF_TAG_IMAGE_WIDTH = 256;
    constexpr uint16_t TIFF_TAG_IMAGE_HEIGHT = 257;
    constexpr uint16_t TIFF_TAG_BITS_PER_SAMPLE = 258;
    constexpr uint16_t TIFF_TAG_COMPRESSION = 259;
    constexpr uint16_t TIFF_TAG_PHOTOMETRIC = 262;
    constexpr uint16_t TIFF_TAG_SAMPLES_PER_PIXEL = 277;
    constexpr uint16_t TIFF_TAG_SAMPLE_FORMAT = 339;
    constexpr uint16_t TIFF_TAG_CFA_REPEAT_PATTERN_DIM = 33421;
    constexpr uint16_t TIFF_TAG_CFA_PATTERN = 33422;
    constexpr uint16_t TIFF_TAG_STRIP_OFFSETS = 273;
    constexpr uint16_t TIFF_TAG_ROWS_PER_STRIP = 278;
    constexpr uint16_t TIFF_TAG_STRIP_BYTE_COUNTS = 279;
    constexpr uint16_t TIFF_TAG_BLACK_LEVEL_REPEAT_DIM = 50713;
    constexpr uint16_t TIFF_TAG_BLACK_LEVEL = 50714;
    constexpr uint16_t TIFF_TAG_WHITE_LEVEL = 50717;
    constexpr uint16_t TIFF_TAG_DNG_VERSION = 50706;
    constexpr uint16_t TIFF_TAG_DNG_BACKWARD_VERSION = 50707;
    constexpr uint16_t TIFF_TAG_LINEARIZATION_TABLE = 50712;
    constexpr uint16_t TIFF_PHOTOMETRIC_CFA = 32803;
    constexpr uint16_t TIFF_COMPRESSION_NONE = 1;
    constexpr uint16_t TIFF_COMPRESSION_JPEG = 7;
    constexpr uint16_t TIFF_COMPRESSION_JPEG_XL = 52546;

    bool decodeJPEGXL(const uint8_t* encoded, size_t encodedSize,
                      uint32_t width, uint32_t height, uint32_t components,
                      std::vector<uint16_t>& pixels) {
        if (!encoded || !encodedSize || !width || !height ||
            (components != 1 && components != 3)) return false;
        const size_t sampleCount = static_cast<size_t>(width) * height * components;
        pixels.resize(sampleCount);
        JxlDecoder* decoder = JxlDecoderCreate(nullptr);
        if (!decoder) return false;
        auto finish = [&](bool result) {
            JxlDecoderDestroy(decoder);
            return result;
        };
        if (JxlDecoderSubscribeEvents(decoder, JXL_DEC_BASIC_INFO | JXL_DEC_FULL_IMAGE) != JXL_DEC_SUCCESS ||
            JxlDecoderSetInput(decoder, encoded, encodedSize) != JXL_DEC_SUCCESS)
            return finish(false);
        JxlDecoderCloseInput(decoder);
        const JxlPixelFormat format = {
            components, JXL_TYPE_UINT16, JXL_NATIVE_ENDIAN, 0};
        bool basicInfoSeen = false, imageSeen = false;
        for (;;) {
            const JxlDecoderStatus status = JxlDecoderProcessInput(decoder);
            if (status == JXL_DEC_BASIC_INFO) {
                JxlBasicInfo info;
                if (JxlDecoderGetBasicInfo(decoder, &info) != JXL_DEC_SUCCESS ||
                    info.xsize != width || info.ysize != height ||
                    info.num_color_channels != components || info.bits_per_sample != 16)
                    return finish(false);
                basicInfoSeen = true;
            } else if (status == JXL_DEC_NEED_IMAGE_OUT_BUFFER) {
                size_t required = 0;
                if (JxlDecoderImageOutBufferSize(decoder, &format, &required) != JXL_DEC_SUCCESS ||
                    required != pixels.size() * sizeof(uint16_t) ||
                    JxlDecoderSetImageOutBuffer(decoder, &format, pixels.data(), required) != JXL_DEC_SUCCESS)
                    return finish(false);
            } else if (status == JXL_DEC_FULL_IMAGE) {
                imageSeen = true;
            } else if (status == JXL_DEC_SUCCESS) {
                return finish(basicInfoSeen && imageSeen);
            } else {
                return finish(false);
            }
        }
    }

    bool encodeJPEGXL(const std::vector<uint16_t>& pixels, uint32_t width,
                      uint32_t height, uint32_t components, float distance,
                      std::vector<uint8_t>& output) {
        if (!width || !height || (components != 1 && components != 3) || distance < 0.0f ||
            pixels.size() != static_cast<size_t>(width) * height * components) return false;
        JxlEncoder* encoder = JxlEncoderCreate(nullptr);
        if (!encoder) return false;
        auto finish = [&](bool result) { JxlEncoderDestroy(encoder); return result; };
        JxlBasicInfo info;
        JxlEncoderInitBasicInfo(&info);
        info.xsize = width; info.ysize = height; info.bits_per_sample = 16;
        info.num_color_channels = components; info.uses_original_profile = JXL_TRUE;
        JxlColorEncoding color;
        JxlColorEncodingSetToLinearSRGB(&color, components == 1 ? JXL_TRUE : JXL_FALSE);
        if (JxlEncoderSetBasicInfo(encoder, &info) != JXL_ENC_SUCCESS ||
            JxlEncoderSetColorEncoding(encoder, &color) != JXL_ENC_SUCCESS) return finish(false);
        auto* frame = JxlEncoderFrameSettingsCreate(encoder, nullptr);
        if (!frame || JxlEncoderFrameSettingsSetOption(
                frame, JXL_ENC_FRAME_SETTING_EFFORT, 7) != JXL_ENC_SUCCESS ||
            JxlEncoderFrameSettingsSetOption(
                frame, JXL_ENC_FRAME_SETTING_MODULAR, 1) != JXL_ENC_SUCCESS) return finish(false);
        if ((distance == 0.0f && JxlEncoderSetFrameLossless(frame, JXL_TRUE) != JXL_ENC_SUCCESS) ||
            (distance > 0.0f && JxlEncoderSetFrameDistance(frame, distance) != JXL_ENC_SUCCESS))
            return finish(false);
        const JxlPixelFormat format = {components, JXL_TYPE_UINT16, JXL_NATIVE_ENDIAN, 0};
        if (JxlEncoderAddImageFrame(frame, &format, pixels.data(),
                pixels.size() * sizeof(uint16_t)) != JXL_ENC_SUCCESS) return finish(false);
        JxlEncoderCloseInput(encoder);
        output.resize(std::max<size_t>(4096, pixels.size()));
        uint8_t* next = output.data(); size_t available = output.size();
        for (;;) {
            const auto status = JxlEncoderProcessOutput(encoder, &next, &available);
            if (status == JXL_ENC_SUCCESS) break;
            if (status != JXL_ENC_NEED_MORE_OUTPUT) return finish(false);
            const size_t used = static_cast<size_t>(next - output.data());
            output.resize(output.size() * 2); next = output.data() + used;
            available = output.size() - used;
        }
        output.resize(static_cast<size_t>(next - output.data()));
        return finish(!output.empty());
    }

    uint16_t read16(const uint8_t* p, bool little) {
        return little ? static_cast<uint16_t>(p[0] | (p[1] << 8))
                      : static_cast<uint16_t>((p[0] << 8) | p[1]);
    }
    uint32_t read32(const uint8_t* p, bool little) {
        return little
            ? static_cast<uint32_t>(p[0] | (p[1] << 8) | (p[2] << 16) | (p[3] << 24))
            : static_cast<uint32_t>((p[0] << 24) | (p[1] << 16) | (p[2] << 8) | p[3]);
    }
    uint32_t readBE32(const uint8_t* p) { return read32(p, false); }
    float readBEFloat(const uint8_t* p) {
        const uint32_t bits = readBE32(p);
        float value;
        std::memcpy(&value, &bits, sizeof(value));
        return value;
    }
    double readBEDouble(const uint8_t* p) {
        uint64_t bits = 0;
        for (int i = 0; i < 8; ++i) bits = (bits << 8) | p[i];
        double value;
        std::memcpy(&value, &bits, sizeof(value));
        return value;
    }
    void appendBE32(std::vector<uint8_t>& out, uint32_t value) {
        out.push_back(static_cast<uint8_t>(value >> 24));
        out.push_back(static_cast<uint8_t>(value >> 16));
        out.push_back(static_cast<uint8_t>(value >> 8));
        out.push_back(static_cast<uint8_t>(value));
    }
    void appendBEFloat(std::vector<uint8_t>& out, float value) {
        uint32_t bits;
        std::memcpy(&bits, &value, sizeof(bits));
        appendBE32(out, bits);
    }
    void appendBEDouble(std::vector<uint8_t>& out, double value) {
        uint64_t bits;
        std::memcpy(&bits, &value, sizeof(bits));
        for (int shift = 56; shift >= 0; shift -= 8)
            out.push_back(static_cast<uint8_t>(bits >> shift));
    }
    std::vector<uint8_t> serializeGainMap(const GainMap& map) {
        std::vector<uint8_t> payload;
        payload.reserve(96 + map.data.size() * 4);
        appendBE32(payload, 1);               // opcode count
        appendBE32(payload, OPCODE_GAIN_MAP);
        appendBE32(payload, 0x01030000);      // minimum DNG version
        appendBE32(payload, 0);               // flags
        appendBE32(payload, static_cast<uint32_t>(76 + map.data.size() * 4));
        appendBE32(payload, map.top); appendBE32(payload, map.left);
        appendBE32(payload, map.bottom); appendBE32(payload, map.right);
        appendBE32(payload, 0); appendBE32(payload, 1);
        appendBE32(payload, map.rowPitch); appendBE32(payload, map.colPitch);
        appendBE32(payload, map.height); appendBE32(payload, map.width);
        appendBEDouble(payload, map.spacingV); appendBEDouble(payload, map.spacingH);
        appendBEDouble(payload, map.originV); appendBEDouble(payload, map.originH);
        appendBE32(payload, 1);
        for (float gain : map.data) appendBEFloat(payload, gain);
        return payload;
    }
    void write32(uint8_t* p, uint32_t value, bool little) {
        for (int i = 0; i < 4; ++i) {
            const int shift = little ? i * 8 : (3 - i) * 8;
            p[i] = static_cast<uint8_t>((value >> shift) & 0xff);
        }
    }
    void write16(uint8_t* p, uint16_t value, bool little) {
        if (little) {
            p[0] = static_cast<uint8_t>(value & 0xff);
            p[1] = static_cast<uint8_t>(value >> 8);
        } else {
            p[0] = static_cast<uint8_t>(value >> 8);
            p[1] = static_cast<uint8_t>(value & 0xff);
        }
    }

    void remosaicCFA(const std::vector<uint16_t>& rgb, std::vector<uint16_t>& bayer,
                  int width, int height, const std::array<uint8_t, 4>& phase) {
        bayer.resize(static_cast<size_t>(width) * height);
        for (int y = 0; y < height; ++y) for (int x = 0; x < width; ++x) {
            const int c = phase[(y & 1) * 2 + (x & 1)];
            const size_t i = static_cast<size_t>(y) * width + x;
            bayer[i] = rgb[i * 3 + c];
        }
    }

    struct TiffEntry {
        uint16_t tag;
        uint16_t type;
        uint32_t count;
        size_t valueOffset;
        uint32_t ifdOffset;
        size_t entryOffset;
    };

    std::vector<TiffEntry> findTiffEntries(const std::vector<uint8_t>& data, bool& little) {
        std::vector<TiffEntry> result;
        if (data.size() < 8) return result;
        const uint16_t order = read16(data.data(), true);
        if (order != TIFF_LITTLE_ENDIAN && order != TIFF_BIG_ENDIAN) return result;
        little = order == TIFF_LITTLE_ENDIAN;
        if (read16(data.data() + 2, little) != TIFF_MAGIC) return result;
        std::vector<uint32_t> pending = {read32(data.data() + 4, little)};
        std::set<uint32_t> visited;
        while (!pending.empty()) {
            const uint32_t ifd = pending.back();
            pending.pop_back();
            if (!ifd || !visited.insert(ifd).second || ifd + 2 > data.size()) continue;
            const uint16_t count = read16(data.data() + ifd, little);
            const size_t entriesEnd = static_cast<size_t>(ifd) + 2 + static_cast<size_t>(count) * 12;
            if (entriesEnd + 4 > data.size()) continue;
            for (uint16_t i = 0; i < count; ++i) {
                const size_t pos = static_cast<size_t>(ifd) + 2 + i * 12;
                const uint16_t tag = read16(data.data() + pos, little);
                const uint16_t type = read16(data.data() + pos + 2, little);
                const uint32_t itemCount = read32(data.data() + pos + 4, little);
                const uint32_t rawOffset = read32(data.data() + pos + 8, little);
                size_t typeSize = (type == TIFF_TYPE_SHORT) ? 2 :
                                  (type == TIFF_TYPE_LONG ? 4 :
                                  ((type == TIFF_TYPE_RATIONAL || type == TIFF_TYPE_SRATIONAL) ? 8 : 1));
                const size_t bytes = typeSize * static_cast<size_t>(itemCount);
                const size_t valueOffset = bytes <= 4 ? pos + 8 : rawOffset;
                if (valueOffset <= data.size() && bytes <= data.size() - valueOffset)
                    result.push_back({tag, type, itemCount, valueOffset, ifd, pos});
                if (tag == TIFF_TAG_EXIF_IFD && type == TIFF_TYPE_LONG && itemCount == 1)
                    pending.push_back(rawOffset);
                if (tag == TIFF_TAG_SUB_IFDS && (type == TIFF_TYPE_LONG)) {
                    for (uint32_t j = 0; j < itemCount && valueOffset + j * 4 + 4 <= data.size(); ++j)
                        pending.push_back(read32(data.data() + valueOffset + j * 4, little));
                }
            }
            pending.push_back(read32(data.data() + entriesEnd, little));
        }
        return result;
    }

    bool replaceTiffStrip(std::vector<uint8_t>& data, uint32_t stripOffset,
                          uint32_t stripBytes, const std::vector<uint8_t>& replacement,
                          bool little) {
        const size_t oldEnd = static_cast<size_t>(stripOffset) + stripBytes;
        if (stripOffset > data.size() || oldEnd > data.size()) return false;
        const auto entries = findTiffEntries(data, little);
        const uint32_t oldRoot = read32(data.data() + 4, little);
        struct PointerPatch { size_t entryOffset; uint32_t rawOffset; };
        std::vector<PointerPatch> externalPointers;
        std::vector<PointerPatch> inlineIfdPointers;
        std::vector<PointerPatch> subIfdArrays;
        std::map<uint32_t, uint32_t> nextIfds;
        for (const auto& entry : entries) {
            const size_t typeSize = entry.type == TIFF_TYPE_SHORT ? 2 :
                entry.type == TIFF_TYPE_LONG ? 4 :
                (entry.type == TIFF_TYPE_RATIONAL || entry.type == TIFF_TYPE_SRATIONAL) ? 8 : 1;
            if (typeSize * static_cast<size_t>(entry.count) > 4)
                externalPointers.push_back({entry.entryOffset,
                    read32(data.data() + entry.entryOffset + 8, little)});
            if ((entry.tag == TIFF_TAG_EXIF_IFD || entry.tag == TIFF_TAG_SUB_IFDS) &&
                entry.type == TIFF_TYPE_LONG) {
                const uint32_t rawOffset = read32(data.data() + entry.entryOffset + 8, little);
                if (entry.count == 1)
                    inlineIfdPointers.push_back({entry.entryOffset, rawOffset});
                else
                    subIfdArrays.push_back({entry.entryOffset, rawOffset});
            }
            if (!nextIfds.count(entry.ifdOffset)) {
                const uint16_t count = read16(data.data() + entry.ifdOffset, little);
                const size_t nextPos = static_cast<size_t>(entry.ifdOffset) + 2 +
                    static_cast<size_t>(count) * 12;
                if (nextPos + 4 <= data.size())
                    nextIfds[entry.ifdOffset] = read32(data.data() + nextPos, little);
            }
        }
        const int64_t delta = static_cast<int64_t>(replacement.size()) - stripBytes;
        auto relocated = [&](uint32_t offset) -> uint32_t {
            if (offset < oldEnd) return offset;
            const int64_t value = static_cast<int64_t>(offset) + delta;
            return value >= 0 && value <= std::numeric_limits<uint32_t>::max()
                ? static_cast<uint32_t>(value) : 0;
        };
        std::vector<uint8_t> rebuilt;
        rebuilt.reserve(data.size() - stripBytes + replacement.size());
        rebuilt.insert(rebuilt.end(), data.begin(), data.begin() + stripOffset);
        rebuilt.insert(rebuilt.end(), replacement.begin(), replacement.end());
        rebuilt.insert(rebuilt.end(), data.begin() + oldEnd, data.end());
        data = std::move(rebuilt);
        write32(data.data() + 4, relocated(oldRoot), little);
        for (const auto& pointer : externalPointers) {
            const size_t entryOffset = relocated(static_cast<uint32_t>(pointer.entryOffset));
            write32(data.data() + entryOffset + 8, relocated(pointer.rawOffset), little);
        }
        for (const auto& pointer : inlineIfdPointers) {
            const size_t entryOffset = relocated(static_cast<uint32_t>(pointer.entryOffset));
            write32(data.data() + entryOffset + 8, relocated(pointer.rawOffset), little);
        }
        for (const auto& pointer : subIfdArrays) {
            const size_t entryOffset = relocated(static_cast<uint32_t>(pointer.entryOffset));
            const uint32_t count = read32(data.data() + entryOffset + 4, little);
            const uint32_t arrayOffset = read32(data.data() + entryOffset + 8, little);
            for (uint32_t i = 0; i < count; ++i) {
                const size_t valueOffset = static_cast<size_t>(arrayOffset) + i * 4;
                if (valueOffset + 4 > data.size()) return false;
                write32(data.data() + valueOffset,
                        relocated(read32(data.data() + valueOffset, little)), little);
            }
        }
        for (const auto& [oldIfd, oldNext] : nextIfds) {
            const uint32_t newIfd = relocated(oldIfd);
            const uint16_t count = read16(data.data() + newIfd, little);
            write32(data.data() + static_cast<size_t>(newIfd) + 2 +
                    static_cast<size_t>(count) * 12, relocated(oldNext), little);
        }
        return true;
    }

    double readRational(const std::vector<uint8_t>& data, const TiffEntry& entry,
                        uint32_t index, bool little) {
        const size_t pos = entry.valueOffset + static_cast<size_t>(index) * 8;
        const uint32_t numerator = read32(data.data() + pos, little);
        const uint32_t denominator = read32(data.data() + pos + 4, little);
        // MotionCam Fuse builds before the decimal-rational writer fix could
        // overflow an exact 2^32 TIFF denominator to zero. Recover that one
        // known legacy encoding so already-finalized sequences can be reopened.
        if (!denominator)
            return numerator ? static_cast<double>(numerator) / 4294967296.0 : 0.0;
        if (entry.type == TIFF_TYPE_SRATIONAL)
            return static_cast<double>(static_cast<int32_t>(numerator)) /
                   static_cast<int32_t>(denominator);
        return static_cast<double>(numerator) / denominator;
    }

    void writeRational(std::vector<uint8_t>& data, const TiffEntry& entry,
                       uint32_t index, double value, bool little) {
        constexpr int32_t denominator = 1000000;
        const double scaled = std::round(value * denominator);
        const int32_t numerator = static_cast<int32_t>(std::clamp(
            scaled, static_cast<double>(std::numeric_limits<int32_t>::min()),
            static_cast<double>(std::numeric_limits<int32_t>::max())));
        const size_t pos = entry.valueOffset + static_cast<size_t>(index) * 8;
        write32(data.data() + pos, static_cast<uint32_t>(numerator), little);
        write32(data.data() + pos + 4, denominator, little);
    }

    bool addMissingMetadataEntries(std::vector<uint8_t>& data,
                                   bool addBaseline, bool addNeutral, bool little) {
        if (!addBaseline && !addNeutral) return true;
        const uint32_t oldIfd = read32(data.data() + 4, little);
        if (oldIfd + 2 > data.size()) return false;
        const uint16_t oldCount = read16(data.data() + oldIfd, little);
        const size_t oldEntriesEnd = static_cast<size_t>(oldIfd) + 2 + static_cast<size_t>(oldCount) * 12;
        if (oldEntriesEnd + 4 > data.size() || data.size() > std::numeric_limits<uint32_t>::max())
            return false;

        if (data.size() & 1u) data.push_back(0);
        const uint32_t newIfd = static_cast<uint32_t>(data.size());
        const uint16_t newCount = static_cast<uint16_t>(oldCount + addBaseline + addNeutral);
        const size_t tableSize = 2 + static_cast<size_t>(newCount) * 12 + 4;
        const size_t externalSize = (addBaseline ? 8 : 0) + (addNeutral ? 24 : 0);
        data.resize(data.size() + tableSize + externalSize, 0);
        std::vector<std::array<uint8_t, 12>> newEntries;
        newEntries.reserve(newCount);
        for (uint16_t i = 0; i < oldCount; ++i) {
            std::array<uint8_t, 12> entry{};
            std::memcpy(entry.data(), data.data() + oldIfd + 2 + static_cast<size_t>(i) * 12, 12);
            newEntries.push_back(entry);
        }
        uint32_t valuePos = static_cast<uint32_t>(static_cast<size_t>(newIfd) + tableSize);
        auto addEntry = [&](uint16_t tag, uint16_t type, uint32_t count, uint32_t bytes) {
            std::array<uint8_t, 12> entry{};
            write16(entry.data(), tag, little);
            write16(entry.data() + 2, type, little);
            write32(entry.data() + 4, count, little);
            write32(entry.data() + 8, valuePos, little);
            newEntries.push_back(entry);
            valuePos += bytes;
        };
        if (addBaseline) addEntry(TIFF_TAG_BASELINE_EXPOSURE, TIFF_TYPE_SRATIONAL, 1, 8);
        if (addNeutral) addEntry(TIFF_TAG_AS_SHOT_NEUTRAL, TIFF_TYPE_RATIONAL, 3, 24);
        std::sort(newEntries.begin(), newEntries.end(), [little](const auto& a, const auto& b) {
            return read16(a.data(), little) < read16(b.data(), little);
        });
        write16(data.data() + newIfd, newCount, little);
        size_t entryPos = static_cast<size_t>(newIfd) + 2;
        for (const auto& entry : newEntries) {
            std::memcpy(data.data() + entryPos, entry.data(), entry.size());
            entryPos += entry.size();
        }
        std::memcpy(data.data() + entryPos, data.data() + oldEntriesEnd, 4);
        write32(data.data() + 4, newIfd, little);
        return true;
    }

    bool addCfaEntries(std::vector<uint8_t>& data, uint32_t ifdOffset,
                       const std::array<uint8_t, 4>& phase, bool little) {
        if (read32(data.data() + 4, little) != ifdOffset || ifdOffset + 2 > data.size())
            return false;
        const uint16_t oldCount = read16(data.data() + ifdOffset, little);
        const size_t oldEnd = static_cast<size_t>(ifdOffset) + 2 + static_cast<size_t>(oldCount) * 12;
        if (oldEnd + 4 > data.size() || oldCount > std::numeric_limits<uint16_t>::max() - 2)
            return false;
        if (data.size() & 1u) data.push_back(0);
        const uint32_t newIfd = static_cast<uint32_t>(data.size());
        std::vector<std::array<uint8_t, 12>> newEntries;
        newEntries.reserve(oldCount + 2);
        for (uint16_t i = 0; i < oldCount; ++i) {
            std::array<uint8_t, 12> entry{};
            std::memcpy(entry.data(), data.data() + ifdOffset + 2 + static_cast<size_t>(i) * 12, 12);
            newEntries.push_back(entry);
        }
        auto inlineEntry = [&](uint16_t tag, uint16_t type, uint32_t count,
                               const std::array<uint8_t, 4>& value) {
            std::array<uint8_t, 12> entry{};
            write16(entry.data(), tag, little); write16(entry.data() + 2, type, little);
            write32(entry.data() + 4, count, little);
            std::copy(value.begin(), value.end(), entry.begin() + 8);
            newEntries.push_back(entry);
        };
        std::array<uint8_t, 4> dimensions{};
        write16(dimensions.data(), 2, little); write16(dimensions.data() + 2, 2, little);
        inlineEntry(TIFF_TAG_CFA_REPEAT_PATTERN_DIM, TIFF_TYPE_SHORT, 2, dimensions);
        inlineEntry(TIFF_TAG_CFA_PATTERN, TIFF_TYPE_BYTE, 4, phase);
        std::sort(newEntries.begin(), newEntries.end(), [little](const auto& a, const auto& b) {
            return read16(a.data(), little) < read16(b.data(), little);
        });
        const uint16_t newCount = static_cast<uint16_t>(newEntries.size());
        data.resize(data.size() + 2 + static_cast<size_t>(newCount) * 12 + 4, 0);
        write16(data.data() + newIfd, newCount, little);
        size_t position = static_cast<size_t>(newIfd) + 2;
        for (const auto& entry : newEntries) {
            std::memcpy(data.data() + position, entry.data(), entry.size());
            position += entry.size();
        }
        std::memcpy(data.data() + position, data.data() + oldEnd, 4);
        write32(data.data() + 4, newIfd, little);
        return true;
    }
}

DNGDecoder::DNGDecoder(const std::string& sequencePath) 
    : mSequencePath(sequencePath) {
    
    spdlog::info("DNGDecoder: Initializing for {}", sequencePath);
    analyzeSequence();
}

DNGDecoder::~DNGDecoder() {
    spdlog::debug("DNGDecoder: Cleanup completed");
}

bool DNGDecoder::isDNGSequence(const std::string& path) {
    boost::filesystem::path p(path);
    
    // Check if it's a directory containing DNG files
    if (boost::filesystem::is_directory(p)) {
        boost::filesystem::directory_iterator end;
        for (boost::filesystem::directory_iterator it(p); it != end; ++it) {
            if (boost::iequals(it->path().extension().string(), ".dng")) {
                return true;
            }
        }
    }
    
    // Check if it's a single DNG file (part of sequence)
    if (boost::iequals(p.extension().string(), ".dng") && boost::filesystem::exists(p)) {
        return true;
    }
    
    return false;
}

void DNGDecoder::analyzeSequence() {
    boost::filesystem::path sequencePath(mSequencePath);
    
    if (boost::filesystem::is_directory(sequencePath)) {
        mSequenceInfo.basePath = mSequencePath;
    } else {
        mSequenceInfo.basePath = sequencePath.parent_path().string();
    }
    
    findDNGFiles();
    extractTimestampsFromFilenames();
    
    if (!mFrames.empty()) {
        mSequenceInfo.totalFrames = mFrames.size();
        mSequenceInfo.width = mFrames[0].width;
        mSequenceInfo.height = mFrames[0].height;
        
        // Calculate FPS from timestamps if available
        if (mFrames.size() > 1) {
            const double totalDuration =
                (mFrames.back().timestamp - mFrames.front().timestamp) / 1000000000.0;
            mSequenceInfo.fps = totalDuration > 0.0
                ? (mFrames.size() - 1) / totalDuration : 30.0;
        } else {
            mSequenceInfo.fps = 30.0; // Default
        }
    }
    
    spdlog::info("DNGDecoder: Found {} DNG files, {}x{} @ {:.2f}fps", 
                 mSequenceInfo.totalFrames, mSequenceInfo.width, mSequenceInfo.height, mSequenceInfo.fps);
}

void DNGDecoder::findDNGFiles() {
    boost::filesystem::path basePath(mSequenceInfo.basePath);
    
    if (!boost::filesystem::exists(basePath) || !boost::filesystem::is_directory(basePath)) {
        throw std::runtime_error("Invalid DNG sequence path: " + mSequenceInfo.basePath);
    }
    
    std::vector<std::string> dngFiles;
    boost::filesystem::directory_iterator end;
    
    for (boost::filesystem::directory_iterator it(basePath); it != end; ++it) {
        if (boost::iequals(it->path().extension().string(), ".dng")) {
            dngFiles.push_back(it->path().string());
        }
    }
    
    if (dngFiles.empty()) {
        throw std::runtime_error("No DNG files found in: " + mSequenceInfo.basePath);
    }
    
    // Sort files by name
    std::sort(dngFiles.begin(), dngFiles.end());
    
    // Create frame info for each DNG file
    mFrames.clear();
    mFrames.reserve(dngFiles.size());
    
    for (size_t i = 0; i < dngFiles.size(); ++i) {
        DNGFrameInfo frameInfo;
        frameInfo.frameNumber = static_cast<int>(i);
        frameInfo.filePath = dngFiles[i];
        frameInfo.width = 0;
        frameInfo.height = 0;
        frameInfo.hasGainMap = false;
        frameInfo.timestamp = static_cast<Timestamp>(i * 1000000000.0 / 30.0); // Default timing
        
        mFrames.push_back(frameInfo);
    }

    // Image geometry belongs to the raw IFD and is independent of its
    // compression. Read it instead of exposing the old 1920x1080 placeholder.
    for (auto& frame : mFrames) {
        std::vector<uint8_t> bytes;
        if (!readDNGFile(frame.filePath, bytes)) continue;
        bool little = true;
        const auto entries = findTiffEntries(bytes, little);
        const TiffEntry* photo = nullptr;
        for (const auto& entry : entries) {
            if (entry.tag == TIFF_TAG_PHOTOMETRIC) {
                const uint32_t value = entry.type == TIFF_TYPE_SHORT
                    ? read16(bytes.data() + entry.valueOffset, little)
                    : read32(bytes.data() + entry.valueOffset, little);
                if (value == TIFF_PHOTOMETRIC_CFA || value == 34892) {
                    photo = &entry;
                    break;
                }
            }
        }
        if (!photo) continue;
        for (const auto& entry : entries) {
            if (entry.ifdOffset != photo->ifdOffset) continue;
            const uint32_t value = entry.type == TIFF_TYPE_SHORT
                ? read16(bytes.data() + entry.valueOffset, little)
                : read32(bytes.data() + entry.valueOffset, little);
            if (entry.tag == TIFF_TAG_IMAGE_WIDTH) frame.width = static_cast<int>(value);
            if (entry.tag == TIFF_TAG_IMAGE_HEIGHT) frame.height = static_cast<int>(value);
        }
    }
}

void DNGDecoder::extractTimestampsFromFilenames() {
    // Try to extract frame numbers from filenames for better timing
    boost::regex frameNumberRegex(R"((?:^|[-_])(\d{6,})$)");
    boost::smatch match;
    
    for (auto& frame : mFrames) {
        boost::filesystem::path p(frame.filePath);
        std::string filename = p.stem().string();
        
        if (boost::regex_search(filename, match, frameNumberRegex)) {
            int extractedFrameNumber = std::stoi(match[1].str());
            frame.frameNumber = extractedFrameNumber;
            
            // Update timestamp based on extracted frame number
            frame.timestamp = static_cast<Timestamp>(extractedFrameNumber * 1000000000.0 / 30.0);
        }
    }
    
    // Sort frames by timestamp
    std::sort(mFrames.begin(), mFrames.end(), 
              [](const DNGFrameInfo& a, const DNGFrameInfo& b) {
                  return a.timestamp < b.timestamp;
              });
}

bool DNGDecoder::extractFrame(int frameNumber, std::vector<uint8_t>& dngData) {
    if (frameNumber < 0 || frameNumber >= static_cast<int>(mFrames.size())) {
        return false;
    }
    
    const DNGFrameInfo& frameInfo = mFrames[frameNumber];
    return readDNGFile(frameInfo.filePath, dngData);
}

bool DNGDecoder::extractFrameByTimestamp(Timestamp timestamp, std::vector<uint8_t>& dngData) {
    // Find frame with closest timestamp
    auto it = std::lower_bound(mFrames.begin(), mFrames.end(), timestamp,
                              [](const DNGFrameInfo& frame, Timestamp ts) {
                                  return frame.timestamp < ts;
                              });
    
    if (it == mFrames.end()) {
        it = mFrames.end() - 1;
    }
    
    int frameNumber = static_cast<int>(std::distance(mFrames.begin(), it));
    return extractFrame(frameNumber, dngData);
}

bool DNGDecoder::getGainMap(int frameNumber, GainMap& gainMap) {
    if (frameNumber < 0 || frameNumber >= static_cast<int>(mFrames.size())) {
        return false;
    }
    
    // Check cache first
    auto cacheIt = mGainMapCache.find(frameNumber);
    if (cacheIt != mGainMapCache.end()) {
        gainMap = cacheIt->second;
        return true;
    }
    
    const DNGFrameInfo& frameInfo = mFrames[frameNumber];
    
    if (readDNGGainMap(frameInfo.filePath, gainMap)) {
        mGainMapCache[frameNumber] = gainMap;
        return true;
    }
    
    return false;
}

bool DNGDecoder::getGainMaps(int frameNumber, std::vector<GainMap>& gainMaps) {
    std::vector<uint8_t> data;
    if (!extractFrame(frameNumber, data)) return false;
    bool little = true;
    for (const auto& entry : findTiffEntries(data, little)) {
        if (entry.tag == TIFF_TAG_OPCODE_LIST_2 && entry.count &&
            parseOpcodeGainMaps(data.data() + entry.valueOffset, entry.count, gainMaps))
            return true;
    }
    return false;
}

bool DNGDecoder::getFrameMetadata(int frameNumber, DNGFrameMetadata& metadata) {
    std::vector<uint8_t> data;
    if (!extractFrame(frameNumber, data)) return false;
    if (!getColorMetadata(data, metadata)) return false;
    metadata.hasExposure = metadata.iso > 0.0 && metadata.exposureTime > 0.0;
    return true;
}

bool DNGDecoder::getColorMetadata(const std::vector<uint8_t>& data,
                                  DNGFrameMetadata& metadata) {
    bool little = true;
    const auto entries = findTiffEntries(data, little);
    if (entries.empty()) return false;
    auto readMatrix = [&](const TiffEntry& entry, std::array<float, 9>& matrix,
                          bool& present) {
        if (entry.type != TIFF_TYPE_SRATIONAL || entry.count < matrix.size()) return;
        for (uint32_t i = 0; i < matrix.size(); ++i)
            matrix[i] = static_cast<float>(readRational(data, entry, i, little));
        present = true;
    };
    for (const auto& entry : entries) {
        if (entry.tag == TIFF_TAG_EXPOSURE_TIME && entry.type == TIFF_TYPE_RATIONAL && entry.count)
            metadata.exposureTime = readRational(data, entry, 0, little);
        else if (entry.tag == TIFF_TAG_ISO && entry.count) {
            metadata.iso = entry.type == TIFF_TYPE_SHORT
                ? read16(data.data() + entry.valueOffset, little)
                : read32(data.data() + entry.valueOffset, little);
        } else if (entry.tag == TIFF_TAG_BASELINE_EXPOSURE &&
                   entry.type == TIFF_TYPE_SRATIONAL && entry.count) {
            metadata.baselineExposure = readRational(data, entry, 0, little);
            metadata.hasBaselineExposure = true;
        } else if (entry.tag == TIFF_TAG_AS_SHOT_NEUTRAL &&
                   entry.type == TIFF_TYPE_RATIONAL && entry.count >= 3) {
            for (uint32_t c = 0; c < 3; ++c)
                metadata.asShotNeutral[c] = static_cast<float>(readRational(data, entry, c, little));
            metadata.hasAsShotNeutral = true;
        } else if (entry.tag == TIFF_TAG_COLOR_MATRIX_1)
            readMatrix(entry, metadata.colorMatrix1, metadata.hasColorMatrix1);
        else if (entry.tag == TIFF_TAG_COLOR_MATRIX_2)
            readMatrix(entry, metadata.colorMatrix2, metadata.hasColorMatrix2);
        else if (entry.tag == TIFF_TAG_FORWARD_MATRIX_1)
            readMatrix(entry, metadata.forwardMatrix1, metadata.hasForwardMatrix1);
        else if (entry.tag == TIFF_TAG_FORWARD_MATRIX_2)
            readMatrix(entry, metadata.forwardMatrix2, metadata.hasForwardMatrix2);
    }
    return true;
}

bool DNGDecoder::getCFAMetadata(int frameNumber, int& repeatSize,
                                std::array<uint8_t, 4>& phase) {
    std::vector<uint8_t> data;
    if (!extractFrame(frameNumber, data)) return false;
    bool little = true;
    const auto entries = findTiffEntries(data, little);
    for (const auto& dim : entries) {
        if ((dim.tag != TIFF_TAG_CFA_REPEAT_PATTERN_DIM && dim.tag != 65000) ||
            dim.type != TIFF_TYPE_SHORT || dim.count < 2)
            continue;
        const int width = read16(data.data() + dim.valueOffset, little);
        const int height = read16(data.data() + dim.valueOffset + 2, little);
        if (width != height || width < 2 || (width % 2)) continue;
        for (const auto& pattern : entries) {
            if (pattern.ifdOffset != dim.ifdOffset ||
                (pattern.tag != TIFF_TAG_CFA_PATTERN && pattern.tag != 65001) ||
                pattern.count < static_cast<uint32_t>(width * height)) continue;
            repeatSize = width;
            const int group = width / 2;
            phase = {data[pattern.valueOffset], data[pattern.valueOffset + group],
                     data[pattern.valueOffset + static_cast<size_t>(group) * width],
                     data[pattern.valueOffset + static_cast<size_t>(group) * width + group]};
            return true;
        }
    }
    return false;
}

bool DNGDecoder::updateMetadata(std::vector<uint8_t>& data,
                                const double* baselineExposure,
                                const std::array<float, 3>* asShotNeutral) {
    bool little = true;
    auto entries = findTiffEntries(data, little);
    bool hasBaseline = false;
    bool hasNeutral = false;
    for (const auto& entry : entries) {
        hasBaseline |= entry.tag == TIFF_TAG_BASELINE_EXPOSURE &&
                       entry.type == TIFF_TYPE_SRATIONAL && entry.count;
        hasNeutral |= entry.tag == TIFF_TAG_AS_SHOT_NEUTRAL &&
                      entry.type == TIFF_TYPE_RATIONAL && entry.count >= 3;
    }
    if (!addMissingMetadataEntries(data, baselineExposure && !hasBaseline,
                                   asShotNeutral && !hasNeutral, little))
        return false;
    if ((baselineExposure && !hasBaseline) || (asShotNeutral && !hasNeutral))
        entries = findTiffEntries(data, little);
    bool baselineWritten = baselineExposure == nullptr;
    bool neutralWritten = asShotNeutral == nullptr;
    for (const auto& entry : entries) {
        if (!baselineWritten && entry.tag == TIFF_TAG_BASELINE_EXPOSURE &&
            entry.type == TIFF_TYPE_SRATIONAL && entry.count) {
            writeRational(data, entry, 0, *baselineExposure, little);
            baselineWritten = true;
        } else if (!neutralWritten && entry.tag == TIFF_TAG_AS_SHOT_NEUTRAL &&
                   entry.type == TIFF_TYPE_RATIONAL && entry.count >= 3) {
            for (uint32_t c = 0; c < 3; ++c)
                writeRational(data, entry, c, (*asShotNeutral)[c], little);
            neutralWritten = true;
        }
    }
    return baselineWritten && neutralWritten;
}

bool DNGDecoder::repairExposureTime(std::vector<uint8_t>& data, double exposureTime) {
    if (!(exposureTime > 0.0) || !std::isfinite(exposureTime)) return false;
    bool little = true;
    for (const auto& entry : findTiffEntries(data, little)) {
        if (entry.tag != TIFF_TAG_EXPOSURE_TIME || entry.type != TIFF_TYPE_RATIONAL || !entry.count)
            continue;
        const uint32_t denominator = read32(data.data() + entry.valueOffset + 4, little);
        const double current = readRational(data, entry, 0, little);
        if (!denominator || !(current > 0.0) || !std::isfinite(current))
            writeRational(data, entry, 0, exposureTime, little);
        return true;
    }
    return false;
}

bool DNGDecoder::processHigherCFA(std::vector<uint8_t>& data,
                                  int repeatSize,
                                  const std::array<uint8_t, 4>& phase,
                                  QuadBayerMode mode,
                                  bool remosaic,
                                  int proxyScale,
                                  bool higherCfaHq) {
    const bool proxy = proxyScale > 1;
    bool little = true;
    const auto entries = findTiffEntries(data, little);
    auto scalar = [&](const TiffEntry& e) -> uint32_t {
        return e.type == TIFF_TYPE_SHORT ? read16(data.data() + e.valueOffset, little)
                                         : read32(data.data() + e.valueOffset, little);
    };
    const TiffEntry* photo = nullptr;
    bool sourceIsRgb = false;
    for (const auto& e : entries) {
        if (e.tag != TIFF_TAG_PHOTOMETRIC) continue;
        const uint32_t value = scalar(e);
        if (value == TIFF_PHOTOMETRIC_CFA || (remosaic && value == 34892)) {
            photo = &e;
            sourceIsRgb = value == 34892;
            break;
        }
    }
    if (!photo) return false;
    if (!sourceIsRgb && repeatSize <= 2) return true;
    auto find = [&](uint16_t tag) -> const TiffEntry* {
        for (const auto& e : entries) if (e.ifdOffset == photo->ifdOffset && e.tag == tag) return &e;
        return nullptr;
    };
    const TiffEntry* dimE = find(sourceIsRgb ? 65000 : TIFF_TAG_CFA_REPEAT_PATTERN_DIM);
    const TiffEntry* patternE = find(sourceIsRgb ? 65001 : TIFF_TAG_CFA_PATTERN);
    if (sourceIsRgb) {
        if (!dimE) dimE = find(TIFF_TAG_CFA_REPEAT_PATTERN_DIM);
        if (!patternE) patternE = find(TIFF_TAG_CFA_PATTERN);
        const bool addMissingCfa = !dimE || !patternE;
        const auto widthE = find(TIFF_TAG_IMAGE_WIDTH), heightE = find(TIFF_TAG_IMAGE_HEIGHT);
        const auto bitsE = find(TIFF_TAG_BITS_PER_SAMPLE), compressionE = find(TIFF_TAG_COMPRESSION);
        const auto offsetsE = find(TIFF_TAG_STRIP_OFFSETS), countsE = find(TIFF_TAG_STRIP_BYTE_COUNTS);
        const auto sppE = find(TIFF_TAG_SAMPLES_PER_PIXEL);
        if (!widthE || !heightE || !bitsE || !compressionE ||
            !offsetsE || !countsE || !sppE || offsetsE->count != 1 || countsE->count != 1 ||
            scalar(*compressionE) != TIFF_COMPRESSION_NONE || scalar(*sppE) != 3 ||
            scalar(*bitsE) != 16) return false;
        const uint32_t width = scalar(*widthE), height = scalar(*heightE);
        const uint32_t stripOffset = scalar(*offsetsE), stripBytes = scalar(*countsE);
        const size_t samples = static_cast<size_t>(width) * height * 3;
        if (!width || !height || stripOffset > data.size() ||
            stripBytes < samples * sizeof(uint16_t) || stripBytes > data.size() - stripOffset)
            return false;
        std::vector<uint16_t> rgb(samples);
        for (size_t i = 0; i < samples; ++i)
            rgb[i] = read16(data.data() + stripOffset + i * 2, little);
        std::vector<uint16_t> bayer;
        remosaicCFA(rgb, bayer, width, height, phase);
        const uint32_t newBytes = static_cast<uint32_t>(bayer.size() * sizeof(uint16_t));
        std::vector<uint8_t> bayerBytes(newBytes);
        for (size_t i = 0; i < bayer.size(); ++i) {
            bayerBytes[i * 2] = little ? bayer[i] & 0xff : bayer[i] >> 8;
            bayerBytes[i * 2 + 1] = little ? bayer[i] >> 8 : bayer[i] & 0xff;
        }
        auto setScalar = [&](const TiffEntry& e, uint32_t value) {
            if (e.type == TIFF_TYPE_SHORT) write16(data.data() + e.valueOffset, value, little);
            else write32(data.data() + e.valueOffset, value, little);
        };
        setScalar(*compressionE, TIFF_COMPRESSION_NONE);
        setScalar(*offsetsE, stripOffset);
        setScalar(*countsE, newBytes);
        setScalar(*sppE, 1);
        setScalar(*photo, TIFF_PHOTOMETRIC_CFA);
        write32(data.data() + bitsE->entryOffset + 4, 1, little);
        write16(data.data() + bitsE->entryOffset + 8, 16, little);
        if (const auto sampleFormat = find(TIFF_TAG_SAMPLE_FORMAT)) {
            write32(data.data() + sampleFormat->entryOffset + 4, 1, little);
            write16(data.data() + sampleFormat->entryOffset + 8, 1, little);
        }
        if (const auto black = find(TIFF_TAG_BLACK_LEVEL)) {
            uint32_t blackValue = 0;
            if (black->count) {
                if (black->type == TIFF_TYPE_RATIONAL)
                    blackValue = static_cast<uint32_t>(std::lround(readRational(data, *black, 0, little)));
                else
                    blackValue = scalar(*black);
            }
            write32(data.data() + black->entryOffset + 4, 1, little);
            if (black->type == TIFF_TYPE_SHORT) {
                write16(data.data() + black->entryOffset + 8, static_cast<uint16_t>(blackValue), little);
                write16(data.data() + black->entryOffset + 10, 0, little);
            } else if (black->type != TIFF_TYPE_RATIONAL) {
                write32(data.data() + black->entryOffset + 8, blackValue, little);
            }
        }
        if (const auto blackRepeat = find(TIFF_TAG_BLACK_LEVEL_REPEAT_DIM)) {
            write16(data.data() + blackRepeat->valueOffset, 1, little);
            write16(data.data() + blackRepeat->valueOffset + 2, 1, little);
        }
        if (addMissingCfa) {
            if (!replaceTiffStrip(data, stripOffset, stripBytes, bayerBytes, little)) return false;
            // The replacement IFD is appended after the image. Pack first so
            // the temporary 16-bit RGB/Bayer strips can still be removed.
            if (!packUncompressedToWhiteLevel(data)) return false;
            const auto relocatedEntries = findTiffEntries(data, little);
            const TiffEntry* relocatedPhoto = nullptr;
            for (const auto& entry : relocatedEntries)
                if (entry.tag == TIFF_TAG_PHOTOMETRIC &&
                    (entry.type == TIFF_TYPE_SHORT
                        ? read16(data.data() + entry.valueOffset, little)
                        : read32(data.data() + entry.valueOffset, little)) == TIFF_PHOTOMETRIC_CFA) {
                    relocatedPhoto = &entry;
                    break;
                }
            if (!relocatedPhoto || !addCfaEntries(data, relocatedPhoto->ifdOffset, phase, little))
                return false;
        } else {
            write16(data.data() + dimE->entryOffset, TIFF_TAG_CFA_REPEAT_PATTERN_DIM, little);
            write32(data.data() + dimE->entryOffset + 4, 2, little);
            write16(data.data() + dimE->entryOffset + 8, 2, little);
            write16(data.data() + dimE->entryOffset + 10, 2, little);
            write16(data.data() + patternE->entryOffset, TIFF_TAG_CFA_PATTERN, little);
            write32(data.data() + patternE->entryOffset + 4, 4, little);
            for (size_t i = 0; i < 4; ++i) data[patternE->entryOffset + 8 + i] = phase[i];
            if (!replaceTiffStrip(data, stripOffset, stripBytes, bayerBytes, little)) return false;
        }
        return true;
    }
    if (!proxy && mode == QuadBayerMode::CorrectQBCFAMetadata) return true;
    if (!proxy && mode == QuadBayerMode::WrongCFAMetadata) {
        if (!dimE || !patternE) return false;
        write32(data.data() + dimE->entryOffset + 4, 2, little);
        write16(data.data() + dimE->entryOffset + 8, 2, little);
        write16(data.data() + dimE->entryOffset + 10, 2, little);
        write32(data.data() + patternE->entryOffset + 4, 4, little);
        for (size_t i = 0; i < 4; ++i) data[patternE->entryOffset + 8 + i] = phase[i];
        return true;
    }

    const auto widthE = find(TIFF_TAG_IMAGE_WIDTH), heightE = find(TIFF_TAG_IMAGE_HEIGHT);
    const auto bitsE = find(TIFF_TAG_BITS_PER_SAMPLE), compressionE = find(TIFF_TAG_COMPRESSION);
    const auto offsetsE = find(TIFF_TAG_STRIP_OFFSETS), countsE = find(TIFF_TAG_STRIP_BYTE_COUNTS);
    const auto sppE = find(TIFF_TAG_SAMPLES_PER_PIXEL);
    if (!widthE || !heightE || !bitsE || !compressionE || !offsetsE || !countsE || !sppE ||
        offsetsE->count != 1 || countsE->count != 1) return false;
    const uint32_t width = scalar(*widthE), height = scalar(*heightE), bits = scalar(*bitsE);
    const uint32_t compression = scalar(*compressionE), stripOffset = scalar(*offsetsE);
    const uint32_t stripBytes = scalar(*countsE);
    if (!width || !height || bits < 8 || bits > 16 || stripOffset > data.size() ||
        stripBytes > data.size() - stripOffset || find(TIFF_TAG_LINEARIZATION_TABLE)) return false;
    std::vector<uint16_t> pixels(static_cast<size_t>(width) * height);
    if (compression == TIFF_COMPRESSION_JPEG) {
        lj92 decoder = nullptr;
        int dw = 0, dh = 0, db = 0, components = 0;
        if (lj92_open(&decoder, data.data() + stripOffset, stripBytes, &dw, &dh, &db, &components) != LJ92_ERROR_NONE)
            return false;
        const bool valid = dw == static_cast<int>(width) && dh == static_cast<int>(height) && components == 1 &&
            lj92_decode(decoder, pixels.data(), width, 0, nullptr, 0) == LJ92_ERROR_NONE;
        lj92_close(decoder);
        if (!valid) return false;
    } else if (compression == TIFF_COMPRESSION_JPEG_XL) {
        if (bits != 16 || !decodeJPEGXL(data.data() + stripOffset, stripBytes,
                                        width, height, 1, pixels)) return false;
    } else if (compression == TIFF_COMPRESSION_NONE) {
        const size_t rowBytes = (static_cast<size_t>(width) * bits + 7) / 8;
        if (rowBytes * height > stripBytes) return false;
        for (uint32_t y = 0; y < height; ++y) {
            size_t bit = static_cast<size_t>(y) * rowBytes * 8;
            for (uint32_t x = 0; x < width; ++x) {
                uint16_t value = 0;
                for (uint32_t b = 0; b < bits; ++b, ++bit)
                    value = static_cast<uint16_t>((value << 1) |
                        ((data[stripOffset + bit / 8] >> (7 - bit % 8)) & 1));
                pixels[static_cast<size_t>(y) * width + x] = value;
            }
        }
    } else return false;

    std::array<double, 4> sourceBlack = {0.0, 0.0, 0.0, 0.0};
    const auto blackLevelEntry = find(TIFF_TAG_BLACK_LEVEL);
    if (blackLevelEntry && blackLevelEntry->count) {
        for (uint32_t i = 0; i < 4; ++i) {
            const uint32_t source = std::min(i, blackLevelEntry->count - 1);
            if (blackLevelEntry->type == TIFF_TYPE_RATIONAL) {
                sourceBlack[i] = readRational(data, *blackLevelEntry, source, little);
            } else {
                const size_t pos = blackLevelEntry->valueOffset + source *
                    (blackLevelEntry->type == TIFF_TYPE_SHORT ? 2 : 4);
                sourceBlack[i] = blackLevelEntry->type == TIFF_TYPE_SHORT
                    ? read16(data.data() + pos, little)
                    : read32(data.data() + pos, little);
            }
        }
    }

    std::vector<uint16_t> output;
    uint32_t outputWidth = width, outputHeight = height;
    uint32_t hqReductionShift = 0;
    uint32_t hqReductionArea = 1;
    std::array<double, 3> outputChannelBlack = {0.0, 0.0, 0.0};
    bool rgbOutput = false;
    auto demosaicWithRgbBlackMetadata = [&](const std::vector<uint16_t>& input,
                                             std::vector<uint16_t>& rgb,
                                             uint32_t imageWidth,
                                             uint32_t imageHeight,
                                             int imageRepeatSize,
                                             double levelScale) {
        std::array<double, 4> phaseBlack{};
        for (int i = 0; i < 4; ++i) phaseBlack[i] = sourceBlack[i] * levelScale;
        std::array<double, 3> sums = {0.0, 0.0, 0.0};
        std::array<int, 3> counts = {0, 0, 0};
        for (int i = 0; i < 4; ++i) {
            sums[phase[i]] += phaseBlack[i];
            ++counts[phase[i]];
        }
        for (int channel = 0; channel < 3; ++channel)
            outputChannelBlack[channel] = counts[channel] ? sums[channel] / counts[channel] : 0.0;

        utils::demosaicHigherCFA(
            input, rgb, imageWidth, imageHeight, imageRepeatSize, phase,
            mode == QuadBayerMode::DemosaicOCL);
        rgbOutput = !remosaic;
    };
    if (proxy) {
        const uint32_t group = repeatSize / 2;
        const bool staged8x8Demosaic = repeatSize == 8 && proxyScale == 2 &&
            (mode == QuadBayerMode::Demosaic || mode == QuadBayerMode::DemosaicOCL);
        const uint32_t reductionGroup = staged8x8Demosaic ? 2u : group;
        hqReductionArea = reductionGroup * reductionGroup;
        if (higherCfaHq) {
            double largestLevel = 0.0;
            if (const auto white = find(TIFF_TAG_WHITE_LEVEL))
                largestLevel = scalar(*white) * static_cast<double>(hqReductionArea);
            if (const auto black = find(TIFF_TAG_BLACK_LEVEL)) {
                for (uint32_t i = 0; i < black->count; ++i) {
                    double level = 0.0;
                    if (black->type == TIFF_TYPE_RATIONAL) {
                        level = readRational(data, *black, i, little);
                    } else {
                        const size_t pos = black->valueOffset + i *
                            (black->type == TIFF_TYPE_SHORT ? 2 : 4);
                        level = black->type == TIFF_TYPE_SHORT
                            ? read16(data.data() + pos, little)
                            : read32(data.data() + pos, little);
                    }
                    largestLevel = std::max(
                        largestLevel, level * static_cast<double>(hqReductionArea));
                }
            }
            while (largestLevel > std::numeric_limits<uint16_t>::max()) {
                largestLevel *= 0.5;
                ++hqReductionShift;
            }
        }
        const uint32_t sourceScale = staged8x8Demosaic
            ? 2u
            : reductionGroup * std::max(1u,
                (static_cast<uint32_t>(proxyScale) + reductionGroup - 1) / reductionGroup);
        outputWidth = (width / sourceScale) & ~3u;
        outputHeight = (height / sourceScale) & ~3u;
        if (!outputWidth || !outputHeight) return false;
        output.resize(static_cast<size_t>(outputWidth) * outputHeight);
        const uint32_t selection = (reductionGroup - 1) / 2;
        for (uint32_t y = 0; y < outputHeight; y += 2) for (uint32_t x = 0; x < outputWidth; x += 2) {
            const uint32_t srcX = x * sourceScale, srcY = y * sourceScale;
            for (uint32_t by = 0; by < 2; ++by) for (uint32_t bx = 0; bx < 2; ++bx) {
                const uint32_t anchorX = srcX + bx * reductionGroup;
                const uint32_t anchorY = srcY + by * reductionGroup;
                uint32_t value = 0;
                if (higherCfaHq) {
                    for (uint32_t gy = 0; gy < reductionGroup; ++gy)
                        for (uint32_t gx = 0; gx < reductionGroup; ++gx)
                            value += pixels[static_cast<size_t>(anchorY + gy) * width + anchorX + gx];
                } else {
                    value = pixels[static_cast<size_t>(anchorY + selection) * width + anchorX + selection];
                }
                value >>= hqReductionShift;
                output[static_cast<size_t>(y + by) * outputWidth + x + bx] =
                    static_cast<uint16_t>(value);
            }
        }
        if (staged8x8Demosaic) {
            std::vector<uint16_t> rgb;
            const double levelScale = static_cast<double>(hqReductionArea) /
                static_cast<double>(uint32_t{1} << hqReductionShift);
            demosaicWithRgbBlackMetadata(
                output, rgb, outputWidth, outputHeight, 4, levelScale);
            if (remosaic) remosaicCFA(rgb, output, outputWidth, outputHeight, phase);
            else output = std::move(rgb);
        } else {
            remosaic = true; // Full block reduction is already ordinary 2x2 Bayer.
        }
    } else {
        std::vector<uint16_t> rgb;
        demosaicWithRgbBlackMetadata(pixels, rgb, width, height, repeatSize, 1.0);
        if (remosaic) remosaicCFA(rgb, output, width, height, phase);
        else output = std::move(rgb);
    }

    const bool metadataBeforeStrip = std::all_of(entries.begin(), entries.end(),
        [stripOffset](const TiffEntry& entry) { return entry.entryOffset < stripOffset; });
    if (metadataBeforeStrip && static_cast<size_t>(stripOffset) + stripBytes <= data.size() &&
        data.size() - (static_cast<size_t>(stripOffset) + stripBytes) < 4096)
        data.resize(stripOffset);
    if (data.size() & 1u) data.push_back(0);
    const uint32_t newOffset = static_cast<uint32_t>(data.size());
    const uint32_t newBytes = static_cast<uint32_t>(output.size() * sizeof(uint16_t));
    data.resize(data.size() + newBytes);
    for (size_t i = 0; i < output.size(); ++i) {
        data[newOffset + i * 2] = little ? output[i] & 0xff : output[i] >> 8;
        data[newOffset + i * 2 + 1] = little ? output[i] >> 8 : output[i] & 0xff;
    }
    auto setScalar = [&](const TiffEntry& e, uint32_t value) {
        if (e.type == TIFF_TYPE_SHORT) write16(data.data() + e.valueOffset, value, little);
        else write32(data.data() + e.valueOffset, value, little);
    };
    setScalar(*compressionE, TIFF_COMPRESSION_NONE); setScalar(*offsetsE, newOffset);
    setScalar(*countsE, newBytes); setScalar(*sppE, remosaic ? 1 : 3);
    setScalar(*photo, remosaic ? TIFF_PHOTOMETRIC_CFA : 34892); // LinearRaw
    setScalar(*widthE, outputWidth); setScalar(*heightE, outputHeight);
    if (proxy && higherCfaHq) {
        const double levelScale = static_cast<double>(hqReductionArea) /
            static_cast<double>(uint32_t{1} << hqReductionShift);
        if (const auto white = find(TIFF_TAG_WHITE_LEVEL))
            setScalar(*white, static_cast<uint32_t>(std::lround(scalar(*white) * levelScale)));
        if (const auto black = find(TIFF_TAG_BLACK_LEVEL)) {
            if (black->type == TIFF_TYPE_RATIONAL) {
                for (uint32_t i = 0; i < black->count; ++i)
                    writeRational(data, *black, i,
                                  readRational(data, *black, i, little) * levelScale, little);
            } else {
                for (uint32_t i = 0; i < black->count; ++i) {
                    const size_t pos = black->valueOffset + i * (black->type == TIFF_TYPE_SHORT ? 2 : 4);
                    const uint32_t old = black->type == TIFF_TYPE_SHORT
                        ? read16(data.data() + pos, little) : read32(data.data() + pos, little);
                    const uint32_t scaled = static_cast<uint32_t>(std::lround(old * levelScale));
                    if (black->type == TIFF_TYPE_SHORT) write16(data.data() + pos, scaled, little);
                    else write32(data.data() + pos, scaled, little);
                }
            }
        }
    }
    if (rgbOutput && blackLevelEntry && blackLevelEntry->count >= 3) {
        write32(data.data() + blackLevelEntry->entryOffset + 4, 3, little);
        for (uint32_t channel = 0; channel < 3; ++channel) {
            if (blackLevelEntry->type == TIFF_TYPE_RATIONAL) {
                writeRational(data, *blackLevelEntry, channel,
                              outputChannelBlack[channel], little);
            } else {
                const size_t pos = blackLevelEntry->valueOffset + channel *
                    (blackLevelEntry->type == TIFF_TYPE_SHORT ? 2 : 4);
                const uint32_t value = static_cast<uint32_t>(
                    std::lround(outputChannelBlack[channel]));
                if (blackLevelEntry->type == TIFF_TYPE_SHORT) write16(data.data() + pos, value, little);
                else write32(data.data() + pos, value, little);
            }
        }
        if (const auto repeat = find(TIFF_TAG_BLACK_LEVEL_REPEAT_DIM)) {
            if (repeat->type == TIFF_TYPE_SHORT && repeat->count >= 2) {
                write16(data.data() + repeat->valueOffset, 1, little);
                write16(data.data() + repeat->valueOffset + 2, 1, little);
            }
        }
    }
    if (!remosaic) {
        const uint32_t bitsOffset = static_cast<uint32_t>(data.size());
        data.resize(data.size() + 6);
        for (int c = 0; c < 3; ++c) write16(data.data() + bitsOffset + c * 2, 16, little);
        write32(data.data() + bitsE->entryOffset + 4, 3, little);
        write32(data.data() + bitsE->entryOffset + 8, bitsOffset, little);
        if (const auto sampleFormat = find(TIFF_TAG_SAMPLE_FORMAT)) {
            const uint32_t formatOffset = static_cast<uint32_t>(data.size());
            data.resize(data.size() + 6);
            for (int c = 0; c < 3; ++c) write16(data.data() + formatOffset + c * 2, 1, little);
            write32(data.data() + sampleFormat->entryOffset + 4, 3, little);
            write32(data.data() + sampleFormat->entryOffset + 8, formatOffset, little);
        }
        if (dimE) write16(data.data() + dimE->entryOffset, 65000, little);
        if (patternE) write16(data.data() + patternE->entryOffset, 65001, little);
    } else if (dimE && patternE) {
        write32(data.data() + bitsE->entryOffset + 4, 1, little);
        write16(data.data() + bitsE->entryOffset + 8, 16, little);
        write16(data.data() + bitsE->entryOffset + 10, 0, little);
        write32(data.data() + dimE->entryOffset + 4, 2, little);
        write16(data.data() + dimE->entryOffset + 8, 2, little);
        write16(data.data() + dimE->entryOffset + 10, 2, little);
        write32(data.data() + patternE->entryOffset + 4, 4, little);
        for (size_t i = 0; i < 4; ++i) data[patternE->entryOffset + 8 + i] = phase[i];
    }
    return true;
}

bool DNGDecoder::ensureUncompressed(std::vector<uint8_t>& data) {
    bool little = true;
    const auto entries = findTiffEntries(data, little);
    if (entries.empty()) return false;
    auto scalar = [&](const TiffEntry& e) -> uint32_t {
        return e.type == TIFF_TYPE_SHORT ? read16(data.data() + e.valueOffset, little)
                                         : read32(data.data() + e.valueOffset, little);
    };
    const TiffEntry* photo = nullptr;
    for (const auto& entry : entries) {
        if (entry.tag == TIFF_TAG_PHOTOMETRIC) {
            const uint32_t value = scalar(entry);
            if (value == TIFF_PHOTOMETRIC_CFA || value == 34892) {
                photo = &entry;
                break;
            }
        }
    }
    if (!photo) return false;
    auto find = [&](uint16_t tag) -> const TiffEntry* {
        for (const auto& entry : entries)
            if (entry.ifdOffset == photo->ifdOffset && entry.tag == tag) return &entry;
        return nullptr;
    };
    const auto widthE = find(TIFF_TAG_IMAGE_WIDTH), heightE = find(TIFF_TAG_IMAGE_HEIGHT);
    const auto bitsE = find(TIFF_TAG_BITS_PER_SAMPLE), compressionE = find(TIFF_TAG_COMPRESSION);
    const auto offsetsE = find(TIFF_TAG_STRIP_OFFSETS), countsE = find(TIFF_TAG_STRIP_BYTE_COUNTS);
    const auto sppE = find(TIFF_TAG_SAMPLES_PER_PIXEL);
    if (!widthE || !heightE || !bitsE || !compressionE || !offsetsE || !countsE ||
        !sppE || offsetsE->count != 1 || countsE->count != 1) return false;

    const uint32_t compression = scalar(*compressionE);
    if (compression == TIFF_COMPRESSION_NONE) return true;
    const uint32_t width = scalar(*widthE), height = scalar(*heightE);
    const uint32_t bits = scalar(*bitsE), channels = scalar(*sppE);
    const uint32_t stripOffset = scalar(*offsetsE), stripBytes = scalar(*countsE);
    if (!width || !height || !channels || channels > 4 || bits < 8 || bits > 16 ||
        stripOffset > data.size() || stripBytes > data.size() - stripOffset ||
        find(TIFF_TAG_LINEARIZATION_TABLE)) return false;

    std::vector<uint16_t> pixels(static_cast<size_t>(width) * height * channels);
    if (compression == TIFF_COMPRESSION_JPEG_XL) {
        if (bits != 16 || !decodeJPEGXL(data.data() + stripOffset, stripBytes,
                                        width, height, channels, pixels)) return false;
    } else if (compression == TIFF_COMPRESSION_JPEG) {
        lj92 decoder = nullptr;
        int decodedWidth = 0, decodedHeight = 0, decodedBits = 0, components = 0;
        if (lj92_open(&decoder, data.data() + stripOffset, stripBytes, &decodedWidth,
                      &decodedHeight, &decodedBits, &components) != LJ92_ERROR_NONE)
            return false;
        const bool valid = decodedWidth == static_cast<int>(width) &&
            decodedHeight == static_cast<int>(height) &&
            components == static_cast<int>(channels) &&
            lj92_decode(decoder, pixels.data(), width * channels, 0, nullptr, 0) == LJ92_ERROR_NONE;
        lj92_close(decoder);
        if (!valid) return false;
    } else {
        return false;
    }

    const size_t byteCount = pixels.size() * sizeof(uint16_t);
    if (byteCount > std::numeric_limits<uint32_t>::max()) return false;
    const uint32_t newBytes = static_cast<uint32_t>(byteCount);
    std::vector<uint8_t> decoded(newBytes);
    for (size_t i = 0; i < pixels.size(); ++i) {
        decoded[i * 2] = little ? pixels[i] & 0xff : pixels[i] >> 8;
        decoded[i * 2 + 1] = little ? pixels[i] >> 8 : pixels[i] & 0xff;
    }
    auto setScalar = [&](const TiffEntry& entry, uint32_t value) {
        if (entry.type == TIFF_TYPE_SHORT)
            write16(data.data() + entry.valueOffset, static_cast<uint16_t>(value), little);
        else
            write32(data.data() + entry.valueOffset, value, little);
    };
    setScalar(*bitsE, 16);
    setScalar(*compressionE, TIFF_COMPRESSION_NONE);
    setScalar(*offsetsE, stripOffset);
    setScalar(*countsE, newBytes);
    return replaceTiffStrip(data, stripOffset, stripBytes, decoded, little);
}

bool DNGDecoder::overrideDataLevels(std::vector<uint8_t>& data, const std::string& levels) {
    // Dynamic and Static deliberately mean the same thing for DNG input: the
    // levels stored in this particular source frame.
    if (levels.empty() || levels == "Dynamic" || levels == "Static" ||
        levels == "Dynamic/Dynamic" || levels == "Dynamic/Static" ||
        levels == "Static/Dynamic" || levels == "Static/Static")
        return true;

    bool little = true;
    const auto entries = findTiffEntries(data, little);
    auto scalar = [&](const TiffEntry& entry, uint32_t index = 0) -> float {
        index = std::min(index, entry.count - 1);
        if (entry.type == TIFF_TYPE_RATIONAL)
            return static_cast<float>(readRational(data, entry, index, little));
        const size_t offset = entry.valueOffset + static_cast<size_t>(index) *
            (entry.type == TIFF_TYPE_SHORT ? 2 : 4);
        return entry.type == TIFF_TYPE_SHORT
            ? read16(data.data() + offset, little)
            : read32(data.data() + offset, little);
    };
    const TiffEntry* photo = nullptr;
    for (const auto& entry : entries) {
        if (entry.tag == TIFF_TAG_PHOTOMETRIC && entry.count &&
            (scalar(entry) == TIFF_PHOTOMETRIC_CFA || scalar(entry) == 34892)) {
            photo = &entry;
            break;
        }
    }
    if (!photo) return false;
    auto find = [&](uint16_t tag) -> const TiffEntry* {
        for (const auto& entry : entries)
            if (entry.ifdOffset == photo->ifdOffset && entry.tag == tag && entry.count)
                return &entry;
        return nullptr;
    };
    const auto white = find(TIFF_TAG_WHITE_LEVEL);
    const auto black = find(TIFF_TAG_BLACK_LEVEL);
    if (!white) return false;

    std::array<float, 4> sourceBlack{0, 0, 0, 0};
    if (black) for (uint32_t i = 0; i < sourceBlack.size(); ++i)
        sourceBlack[i] = scalar(*black, i);
    const float sourceWhite = scalar(*white);
    const auto resolved = resolveDataLevels(
        levels, sourceWhite, sourceBlack, sourceWhite, sourceBlack);

    const bool overridesWhite = levels.substr(0, levels.find('/')) != "Dynamic" &&
                                levels.substr(0, levels.find('/')) != "Static";
    const auto separator = levels.find('/');
    const std::string blackSelection = separator == std::string::npos
        ? std::string{} : levels.substr(separator + 1);
    const bool overridesBlack = !blackSelection.empty() &&
        blackSelection != "Dynamic" && blackSelection != "Static";
    if (overridesBlack && !black) return false;

    auto writeLevel = [&](const TiffEntry& entry, uint32_t index, float value) {
        index = std::min(index, entry.count - 1);
        if (entry.type == TIFF_TYPE_RATIONAL) {
            writeRational(data, entry, index, value, little);
        } else {
            const uint32_t rounded = static_cast<uint32_t>(std::clamp(
                std::round(static_cast<double>(value)), 0.0,
                static_cast<double>(std::numeric_limits<uint32_t>::max())));
            const size_t offset = entry.valueOffset + static_cast<size_t>(index) *
                (entry.type == TIFF_TYPE_SHORT ? 2 : 4);
            if (entry.type == TIFF_TYPE_SHORT)
                write16(data.data() + offset, static_cast<uint16_t>(std::min<uint32_t>(rounded, 65535)), little);
            else
                write32(data.data() + offset, rounded, little);
        }
    };
    if (overridesWhite)
        for (uint32_t i = 0; i < white->count; ++i) writeLevel(*white, i, resolved.white);
    if (overridesBlack)
        for (uint32_t i = 0; i < black->count; ++i)
            writeLevel(*black, i, resolved.black[std::min<uint32_t>(i, 3)]);
    return true;
}

bool DNGDecoder::packUncompressedToWhiteLevel(std::vector<uint8_t>& data) {
    bool little = true;
    const auto entries = findTiffEntries(data, little);
    auto scalar = [&](const TiffEntry& entry) -> uint32_t {
        return entry.type == TIFF_TYPE_SHORT ? read16(data.data() + entry.valueOffset, little)
                                             : read32(data.data() + entry.valueOffset, little);
    };
    const TiffEntry* photo = nullptr;
    for (const auto& entry : entries)
        if (entry.tag == TIFF_TAG_PHOTOMETRIC &&
            (scalar(entry) == TIFF_PHOTOMETRIC_CFA || scalar(entry) == 34892)) {
            photo = &entry;
            break;
        }
    if (!photo) return false;
    auto find = [&](uint16_t tag) -> const TiffEntry* {
        for (const auto& entry : entries)
            if (entry.ifdOffset == photo->ifdOffset && entry.tag == tag) return &entry;
        return nullptr;
    };
    const auto widthE = find(TIFF_TAG_IMAGE_WIDTH), heightE = find(TIFF_TAG_IMAGE_HEIGHT);
    const auto bitsE = find(TIFF_TAG_BITS_PER_SAMPLE), compressionE = find(TIFF_TAG_COMPRESSION);
    const auto offsetsE = find(TIFF_TAG_STRIP_OFFSETS), countsE = find(TIFF_TAG_STRIP_BYTE_COUNTS);
    const auto sppE = find(TIFF_TAG_SAMPLES_PER_PIXEL), whiteE = find(TIFF_TAG_WHITE_LEVEL);
    if (!widthE || !heightE || !bitsE || !compressionE || !offsetsE || !countsE ||
        !sppE || !whiteE || offsetsE->count != 1 || countsE->count != 1 ||
        scalar(*compressionE) != TIFF_COMPRESSION_NONE) return false;
    const uint32_t sourceBits = scalar(*bitsE);
    const uint32_t white = scalar(*whiteE);
    uint32_t packedBits = 1;
    while (packedBits < 16 && ((uint32_t{1} << packedBits) - 1) < white) ++packedBits;
    if (sourceBits != 16 || packedBits >= 16) return true;
    const uint32_t width = scalar(*widthE), height = scalar(*heightE), channels = scalar(*sppE);
    const uint32_t oldOffset = scalar(*offsetsE), oldBytes = scalar(*countsE);
    const size_t sampleCount = static_cast<size_t>(width) * height * channels;
    if (!width || !height || !channels || channels > 4 || oldOffset > data.size() ||
        oldBytes < sampleCount * sizeof(uint16_t) || oldBytes > data.size() - oldOffset)
        return false;
    const size_t rowBytes = (static_cast<size_t>(width) * channels * packedBits + 7) / 8;
    const size_t packedBytes = rowBytes * height;
    if (packedBytes > std::numeric_limits<uint32_t>::max()) return false;
    std::vector<uint8_t> packed(packedBytes, 0);
    for (uint32_t y = 0; y < height; ++y) {
        size_t bit = static_cast<size_t>(y) * rowBytes * 8;
        for (uint32_t x = 0; x < width * channels; ++x) {
            const size_t source = (static_cast<size_t>(y) * width * channels + x) * 2;
            const uint16_t value = std::min<uint16_t>(
                read16(data.data() + oldOffset + source, little),
                static_cast<uint16_t>((uint32_t{1} << packedBits) - 1));
            for (int b = static_cast<int>(packedBits) - 1; b >= 0; --b, ++bit)
                packed[bit / 8] |= static_cast<uint8_t>(((value >> b) & 1u) << (7 - bit % 8));
        }
    }
    for (uint32_t i = 0; i < bitsE->count; ++i)
        write16(data.data() + bitsE->valueOffset + static_cast<size_t>(i) * 2,
                static_cast<uint16_t>(packedBits), little);
    if (offsetsE->type == TIFF_TYPE_SHORT) write16(data.data() + offsetsE->valueOffset, oldOffset, little);
    else write32(data.data() + offsetsE->valueOffset, oldOffset, little);
    if (countsE->type == TIFF_TYPE_SHORT) write16(data.data() + countsE->valueOffset, packedBytes, little);
    else write32(data.data() + countsE->valueOffset, static_cast<uint32_t>(packedBytes), little);
    return replaceTiffStrip(data, oldOffset, oldBytes, packed, little);
}

bool DNGDecoder::extractUncompressedRGB16(const std::vector<uint8_t>& data,
                                          std::vector<uint8_t>& rgbData,
                                          uint32_t& width,
                                          uint32_t& height) {
    bool little = true;
    const auto entries = findTiffEntries(data, little);
    auto scalar = [&](const TiffEntry& entry) -> uint32_t {
        return entry.type == TIFF_TYPE_SHORT ? read16(data.data() + entry.valueOffset, little)
                                             : read32(data.data() + entry.valueOffset, little);
    };
    const TiffEntry* photo = nullptr;
    for (const auto& entry : entries) {
        if (entry.tag != TIFF_TAG_PHOTOMETRIC) continue;
        const uint32_t value = scalar(entry);
        if (value == 2 || value == 34892) { photo = &entry; break; }
    }
    if (!photo) return false;
    auto find = [&](uint16_t tag) -> const TiffEntry* {
        for (const auto& entry : entries)
            if (entry.ifdOffset == photo->ifdOffset && entry.tag == tag) return &entry;
        return nullptr;
    };
    const auto widthE = find(TIFF_TAG_IMAGE_WIDTH), heightE = find(TIFF_TAG_IMAGE_HEIGHT);
    const auto bitsE = find(TIFF_TAG_BITS_PER_SAMPLE), compressionE = find(TIFF_TAG_COMPRESSION);
    const auto offsetsE = find(TIFF_TAG_STRIP_OFFSETS), countsE = find(TIFF_TAG_STRIP_BYTE_COUNTS);
    const auto sppE = find(TIFF_TAG_SAMPLES_PER_PIXEL);
    if (!widthE || !heightE || !bitsE || !compressionE || !offsetsE || !countsE ||
        !sppE || offsetsE->count != 1 || countsE->count != 1 ||
        scalar(*compressionE) != TIFF_COMPRESSION_NONE || scalar(*sppE) != 3)
        return false;
    width = scalar(*widthE);
    height = scalar(*heightE);
    const uint32_t bits = scalar(*bitsE);
    const uint32_t offset = scalar(*offsetsE), bytes = scalar(*countsE);
    if (!width || !height || bits < 8 || bits > 16 || offset > data.size() ||
        bytes > data.size() - offset) return false;

    const size_t samplesPerRow = static_cast<size_t>(width) * 3;
    const size_t rowBytes = (samplesPerRow * bits + 7) / 8;
    if (rowBytes > bytes / height) return false;

    auto level = [&](const TiffEntry* entry, uint32_t channel,
                     double fallback) -> double {
        if (!entry || !entry->count) return fallback;
        const uint32_t index = std::min(channel, entry->count - 1);
        if (entry->type == TIFF_TYPE_RATIONAL || entry->type == TIFF_TYPE_SRATIONAL)
            return readRational(data, *entry, index, little);
        const size_t pos = entry->valueOffset + static_cast<size_t>(index) *
            (entry->type == TIFF_TYPE_SHORT ? 2 : 4);
        return entry->type == TIFF_TYPE_SHORT
            ? read16(data.data() + pos, little) : read32(data.data() + pos, little);
    };
    const auto blackE = find(TIFF_TAG_BLACK_LEVEL);
    const auto whiteE = find(TIFF_TAG_WHITE_LEVEL);
    const double defaultWhite = static_cast<double>((uint32_t{1} << bits) - 1);
    std::array<double, 3> black{}, white{};
    for (uint32_t channel = 0; channel < 3; ++channel) {
        black[channel] = level(blackE, channel, 0.0);
        white[channel] = level(whiteE, channel, defaultWhite);
        if (!(white[channel] > black[channel])) return false;
    }

    const size_t sampleCount = samplesPerRow * height;
    if (sampleCount > std::numeric_limits<size_t>::max() / sizeof(uint16_t)) return false;
    rgbData.resize(sampleCount * sizeof(uint16_t));
    for (uint32_t y = 0; y < height; ++y) {
        size_t bit = static_cast<size_t>(y) * rowBytes * 8;
        for (size_t x = 0; x < samplesPerRow; ++x) {
            uint16_t value = 0;
            if (bits == 16) {
                value = read16(data.data() + offset + static_cast<size_t>(y) * rowBytes + x * 2,
                               little);
            } else {
                for (uint32_t b = 0; b < bits; ++b, ++bit)
                    value = static_cast<uint16_t>((value << 1) |
                        ((data[offset + bit / 8] >> (7 - bit % 8)) & 1));
            }
            const uint32_t channel = static_cast<uint32_t>(x % 3);
            const double normalized = std::clamp(
                (static_cast<double>(value) - black[channel]) /
                    (white[channel] - black[channel]),
                0.0, 1.0);
            const uint16_t output = static_cast<uint16_t>(std::lround(normalized * 65535.0));
            const size_t outputOffset =
                (static_cast<size_t>(y) * samplesPerRow + x) * sizeof(uint16_t);
            // FFmpeg is explicitly configured for rgb48le regardless of the DNG byte order.
            rgbData[outputOffset] = static_cast<uint8_t>(output & 0xff);
            rgbData[outputOffset + 1] = static_cast<uint8_t>(output >> 8);
        }
    }
    return true;
}

bool DNGDecoder::compressJPEGXL(std::vector<uint8_t>& data, float distance) {
    bool little = true;
    const auto entries = findTiffEntries(data, little);
    auto scalar = [&](const TiffEntry& entry) -> uint32_t {
        return entry.type == TIFF_TYPE_SHORT ? read16(data.data() + entry.valueOffset, little)
                                             : read32(data.data() + entry.valueOffset, little);
    };
    const TiffEntry* photo = nullptr;
    for (const auto& entry : entries)
        if (entry.tag == TIFF_TAG_PHOTOMETRIC &&
            (scalar(entry) == TIFF_PHOTOMETRIC_CFA || scalar(entry) == 34892)) {
            photo = &entry; break;
        }
    if (!photo) return false;
    auto find = [&](uint16_t tag) -> const TiffEntry* {
        for (const auto& entry : entries)
            if (entry.ifdOffset == photo->ifdOffset && entry.tag == tag) return &entry;
        return nullptr;
    };
    const auto widthE = find(TIFF_TAG_IMAGE_WIDTH), heightE = find(TIFF_TAG_IMAGE_HEIGHT);
    const auto bitsE = find(TIFF_TAG_BITS_PER_SAMPLE), compressionE = find(TIFF_TAG_COMPRESSION);
    const auto offsetsE = find(TIFF_TAG_STRIP_OFFSETS), countsE = find(TIFF_TAG_STRIP_BYTE_COUNTS);
    const auto sppE = find(TIFF_TAG_SAMPLES_PER_PIXEL);
    if (!widthE || !heightE || !bitsE || !compressionE || !offsetsE || !countsE ||
        !sppE || offsetsE->count != 1 || countsE->count != 1 ||
        scalar(*compressionE) != TIFF_COMPRESSION_NONE) return false;
    const uint32_t width = scalar(*widthE), height = scalar(*heightE);
    const uint32_t bits = scalar(*bitsE), channels = scalar(*sppE);
    const uint32_t stripOffset = scalar(*offsetsE), stripBytes = scalar(*countsE);
    if (!width || !height || bits < 8 || bits > 16 || (channels != 1 && channels != 3) ||
        stripOffset > data.size() || stripBytes > data.size() - stripOffset) return false;
    const size_t rowBytes = (static_cast<size_t>(width) * channels * bits + 7) / 8;
    if (rowBytes * height > stripBytes) return false;
    std::vector<uint16_t> pixels(static_cast<size_t>(width) * height * channels);
    for (uint32_t y = 0; y < height; ++y) {
        size_t bit = static_cast<size_t>(y) * rowBytes * 8;
        for (uint32_t x = 0; x < width * channels; ++x) {
            uint16_t value = 0;
            for (uint32_t b = 0; b < bits; ++b, ++bit)
                value = static_cast<uint16_t>((value << 1) |
                    ((data[stripOffset + bit / 8] >> (7 - bit % 8)) & 1));
            pixels[static_cast<size_t>(y) * width * channels + x] = value;
        }
    }
    std::vector<uint8_t> encoded;
    if (!encodeJPEGXL(pixels, width, height, channels, distance, encoded)) return false;
    bool dngVersionWritten = false, backwardVersionWritten = false;
    for (const auto& entry : entries) {
        if (entry.type != TIFF_TYPE_BYTE || entry.count < 4) continue;
        if (entry.tag != TIFF_TAG_DNG_VERSION &&
            entry.tag != TIFF_TAG_DNG_BACKWARD_VERSION) continue;
        data[entry.valueOffset] = 1;
        data[entry.valueOffset + 1] = 7;
        data[entry.valueOffset + 2] = 0;
        data[entry.valueOffset + 3] = 0;
        dngVersionWritten |= entry.tag == TIFF_TAG_DNG_VERSION;
        backwardVersionWritten |= entry.tag == TIFF_TAG_DNG_BACKWARD_VERSION;
    }
    if (!dngVersionWritten || !backwardVersionWritten) return false;
    for (uint32_t i = 0; i < bitsE->count; ++i)
        write16(data.data() + bitsE->valueOffset + static_cast<size_t>(i) * 2, 16, little);
    if (compressionE->type == TIFF_TYPE_SHORT)
        write16(data.data() + compressionE->valueOffset, TIFF_COMPRESSION_JPEG_XL, little);
    else write32(data.data() + compressionE->valueOffset, TIFF_COMPRESSION_JPEG_XL, little);
    if (offsetsE->type == TIFF_TYPE_SHORT) write16(data.data() + offsetsE->valueOffset, stripOffset, little);
    else write32(data.data() + offsetsE->valueOffset, stripOffset, little);
    if (countsE->type == TIFF_TYPE_SHORT) write16(data.data() + countsE->valueOffset, encoded.size(), little);
    else write32(data.data() + countsE->valueOffset, static_cast<uint32_t>(encoded.size()), little);
    return replaceTiffStrip(data, stripOffset, stripBytes, encoded, little);
}

bool DNGDecoder::compressLosslessJPEG(std::vector<uint8_t>& data) {
    bool little = true;
    const auto entries = findTiffEntries(data, little);
    auto scalar = [&](const TiffEntry& entry) -> uint32_t {
        return entry.type == TIFF_TYPE_SHORT ? read16(data.data() + entry.valueOffset, little)
                                             : read32(data.data() + entry.valueOffset, little);
    };
    const TiffEntry* photo = nullptr;
    for (const auto& entry : entries)
        if (entry.tag == TIFF_TAG_PHOTOMETRIC &&
            (scalar(entry) == TIFF_PHOTOMETRIC_CFA || scalar(entry) == 34892)) {
            photo = &entry; break;
        }
    if (!photo) return false;
    auto find = [&](uint16_t tag) -> const TiffEntry* {
        for (const auto& entry : entries)
            if (entry.ifdOffset == photo->ifdOffset && entry.tag == tag) return &entry;
        return nullptr;
    };
    const auto widthE = find(TIFF_TAG_IMAGE_WIDTH), heightE = find(TIFF_TAG_IMAGE_HEIGHT);
    const auto bitsE = find(TIFF_TAG_BITS_PER_SAMPLE), compressionE = find(TIFF_TAG_COMPRESSION);
    const auto offsetsE = find(TIFF_TAG_STRIP_OFFSETS), countsE = find(TIFF_TAG_STRIP_BYTE_COUNTS);
    const auto sppE = find(TIFF_TAG_SAMPLES_PER_PIXEL);
    if (!widthE || !heightE || !bitsE || !compressionE || !offsetsE || !countsE ||
        !sppE || offsetsE->count != 1 || countsE->count != 1 ||
        scalar(*compressionE) != TIFF_COMPRESSION_NONE) return false;
    const uint32_t width = scalar(*widthE), height = scalar(*heightE);
    const uint32_t bits = scalar(*bitsE), channels = scalar(*sppE);
    const uint32_t stripOffset = scalar(*offsetsE), stripBytes = scalar(*countsE);
    if (!width || !height || bits < 8 || bits > 16 || (channels != 1 && channels != 3) ||
        stripOffset > data.size() || stripBytes > data.size() - stripOffset) return false;
    const size_t rowBytes = (static_cast<size_t>(width) * channels * bits + 7) / 8;
    if (rowBytes * height > stripBytes) return false;
    std::vector<uint16_t> pixels(static_cast<size_t>(width) * height * channels);
    for (uint32_t y = 0; y < height; ++y) {
        size_t bit = static_cast<size_t>(y) * rowBytes * 8;
        for (uint32_t x = 0; x < width * channels; ++x) {
            uint16_t value = 0;
            for (uint32_t b = 0; b < bits; ++b, ++bit)
                value = static_cast<uint16_t>((value << 1) |
                    ((data[stripOffset + bit / 8] >> (7 - bit % 8)) & 1));
            pixels[static_cast<size_t>(y) * width * channels + x] = value;
        }
    }
    uint8_t* encodedBuffer = nullptr;
    int encodedLength = 0;
    if (lj92_encode(pixels.data(), width, height, bits, channels, width * channels, 0,
                    nullptr, 0, &encodedBuffer, &encodedLength) != LJ92_ERROR_NONE ||
        !encodedBuffer || encodedLength <= 0) {
        free(encodedBuffer);
        return false;
    }
    std::vector<uint8_t> encoded(encodedBuffer, encodedBuffer + encodedLength);
    free(encodedBuffer);
    if (compressionE->type == TIFF_TYPE_SHORT)
        write16(data.data() + compressionE->valueOffset, TIFF_COMPRESSION_JPEG, little);
    else write32(data.data() + compressionE->valueOffset, TIFF_COMPRESSION_JPEG, little);
    if (countsE->type == TIFF_TYPE_SHORT) write16(data.data() + countsE->valueOffset, encoded.size(), little);
    else write32(data.data() + countsE->valueOffset, static_cast<uint32_t>(encoded.size()), little);
    return replaceTiffStrip(data, stripOffset, stripBytes, encoded, little);
}

bool DNGDecoder::bakeGainMaps(std::vector<uint8_t>& data,
                              bool normalizeGainMaps,
                              bool colorOnly) {
    bool little = true;
    const auto entries = findTiffEntries(data, little);
    if (entries.empty()) return false;
    auto scalar = [&](const TiffEntry& e, uint32_t index = 0) -> uint32_t {
        const size_t pos = e.valueOffset + index * (e.type == TIFF_TYPE_SHORT ? 2 : 4);
        return e.type == TIFF_TYPE_SHORT ? read16(data.data() + pos, little)
                                         : read32(data.data() + pos, little);
    };
    const TiffEntry* photo = nullptr;
    for (const auto& e : entries)
        if (e.tag == TIFF_TAG_PHOTOMETRIC && scalar(e) == TIFF_PHOTOMETRIC_CFA) {
            photo = &e;
            break;
        }
    if (!photo) return false;
    const uint32_t rawIfd = photo->ifdOffset;
    auto find = [&](uint16_t tag) -> const TiffEntry* {
        for (const auto& e : entries) if (e.ifdOffset == rawIfd && e.tag == tag) return &e;
        return nullptr;
    };
    const auto widthE = find(TIFF_TAG_IMAGE_WIDTH), heightE = find(TIFF_TAG_IMAGE_HEIGHT);
    const auto bitsE = find(TIFF_TAG_BITS_PER_SAMPLE), compressionE = find(TIFF_TAG_COMPRESSION);
    const auto offsetsE = find(TIFF_TAG_STRIP_OFFSETS), countsE = find(TIFF_TAG_STRIP_BYTE_COUNTS);
    const auto opcodeE = find(TIFF_TAG_OPCODE_LIST_2);
    if (!widthE || !heightE || !bitsE || !compressionE || !offsetsE || !countsE ||
        !opcodeE || offsetsE->count != 1 || countsE->count != 1)
        return false;
    if (find(TIFF_TAG_LINEARIZATION_TABLE)) return false;
    const uint32_t width = scalar(*widthE), height = scalar(*heightE);
    const uint32_t bits = scalar(*bitsE), compression = scalar(*compressionE);
    const uint32_t stripOffset = scalar(*offsetsE), stripBytes = scalar(*countsE);
    if (!width || !height || bits < 8 || bits > 16 || stripOffset > data.size() ||
        stripBytes > data.size() - stripOffset) return false;

    std::vector<GainMap> maps;
    if (!parseOpcodeGainMaps(data.data() + opcodeE->valueOffset, opcodeE->count, maps))
        return false;
    std::vector<uint8_t> luminanceOpcode;
    bool gridColorSeparated = false;
    if (colorOnly && !maps.empty()) {
        GainMap luminance = maps.front();
        luminance.plane = 0; luminance.planes = 1; luminance.channels = 1;
        luminance.data.assign(static_cast<size_t>(luminance.width) * luminance.height, 1.0f);
        for (size_t point = 0; point < luminance.data.size(); ++point) {
            float minimum = std::numeric_limits<float>::max();
            for (auto& map : maps) {
                if (map.width != luminance.width || map.height != luminance.height) continue;
                for (uint32_t channel = 0; channel < map.channels; ++channel)
                    minimum = std::min(minimum, map.data[point * map.channels + channel]);
            }
            if (!std::isfinite(minimum) || minimum <= 0.0f) minimum = 1.0f;
            luminance.data[point] = minimum;
            for (auto& map : maps) {
                if (map.width != luminance.width || map.height != luminance.height ||
                    map.channels < 2) continue;
                gridColorSeparated = true;
                for (uint32_t channel = 0; channel < map.channels; ++channel)
                    map.data[point * map.channels + channel] /= minimum;
            }
        }
        luminanceOpcode = serializeGainMap(luminance);
        if (luminanceOpcode.size() > opcodeE->count) return false;
    }
    std::vector<uint16_t> pixels(static_cast<size_t>(width) * height);
    if (compression == TIFF_COMPRESSION_JPEG) {
        lj92 decoder = nullptr;
        int decodedWidth = 0, decodedHeight = 0, decodedBits = 0, components = 0;
        if (lj92_open(&decoder, data.data() + stripOffset, stripBytes, &decodedWidth,
                      &decodedHeight, &decodedBits, &components) != LJ92_ERROR_NONE)
            return false;
        const bool valid = decodedWidth == static_cast<int>(width) &&
                           decodedHeight == static_cast<int>(height) && components == 1 &&
                           lj92_decode(decoder, pixels.data(), width, 0, nullptr, 0) == LJ92_ERROR_NONE;
        lj92_close(decoder);
        if (!valid) return false;
    } else if (compression == TIFF_COMPRESSION_JPEG_XL) {
        if (bits != 16 || !decodeJPEGXL(data.data() + stripOffset, stripBytes,
                                        width, height, 1, pixels)) return false;
    } else if (compression == TIFF_COMPRESSION_NONE) {
        const size_t rowBytes = (static_cast<size_t>(width) * bits + 7) / 8;
        if (rowBytes * height > stripBytes) return false;
        for (uint32_t y = 0; y < height; ++y) {
            size_t bitOffset = static_cast<size_t>(y) * rowBytes * 8;
            for (uint32_t x = 0; x < width; ++x) {
                uint16_t value = 0;
                for (uint32_t b = 0; b < bits; ++b, ++bitOffset)
                    value = static_cast<uint16_t>((value << 1) |
                        ((data[stripOffset + bitOffset / 8] >> (7 - bitOffset % 8)) & 1));
                pixels[static_cast<size_t>(y) * width + x] = value;
            }
        }
    } else return false;

    auto mapGain = [&](const GainMap& map, uint32_t x, uint32_t y) {
        if (x < map.left || x >= map.right || y < map.top || y >= map.bottom ||
            !map.rowPitch || !map.colPitch || (y - map.top) % map.rowPitch ||
            (x - map.left) % map.colPitch) return 1.0f;
        const double nx = static_cast<double>(x) / std::max(1u, width);
        const double ny = static_cast<double>(y) / std::max(1u, height);
        const double gx = map.spacingH > 0 ? (nx - map.originH) / map.spacingH : 0;
        const double gy = map.spacingV > 0 ? (ny - map.originV) / map.spacingV : 0;
        const size_t x0 = std::min<size_t>(map.width - 1, static_cast<size_t>(std::max(0.0, std::floor(gx))));
        const size_t y0 = std::min<size_t>(map.height - 1, static_cast<size_t>(std::max(0.0, std::floor(gy))));
        const size_t x1 = std::min<size_t>(map.width - 1, x0 + 1), y1 = std::min<size_t>(map.height - 1, y0 + 1);
        const float fx = static_cast<float>(std::clamp(gx - std::floor(gx), 0.0, 1.0));
        const float fy = static_cast<float>(std::clamp(gy - std::floor(gy), 0.0, 1.0));
        const size_t channel = map.channels >= 4
            ? std::min<size_t>(map.channels - 1, (y & 1u) * 2 + (x & 1u))
            : 0;
        auto at = [&](size_t xx, size_t yy) { return map.data[(yy * map.width + xx) * map.channels + channel]; };
        return (at(x0,y0) * (1-fx) + at(x1,y0) * fx) * (1-fy) +
               (at(x0,y1) * (1-fx) + at(x1,y1) * fx) * fy;
    };
    float maximumGain = 1.0f;
    for (const auto& map : maps)
        for (float gain : map.data) if (std::isfinite(gain)) maximumGain = std::max(maximumGain, gain);
    const auto blackE = find(TIFF_TAG_BLACK_LEVEL), whiteE = find(TIFF_TAG_WHITE_LEVEL);
    double black = 0.0, white = static_cast<double>((1u << std::min(16u, bits)) - 1);
    if (blackE && blackE->count) black = blackE->type == TIFF_TYPE_RATIONAL
        ? readRational(data, *blackE, 0, little) : scalar(*blackE);
    if (whiteE && whiteE->count) white = whiteE->type == TIFF_TYPE_RATIONAL
        ? readRational(data, *whiteE, 0, little) : scalar(*whiteE);
    const double destinationWhite = normalizeGainMaps ? white : 65535.0;
    for (uint32_t y = 0; y < height; ++y) for (uint32_t x = 0; x < width; ++x) {
        float gain = 1.0f;
        for (const auto& map : maps) gain *= mapGain(map, x, y);
        if (colorOnly && !gridColorSeparated) {
            float localMinimum = gain;
            for (uint32_t phaseY = 0; phaseY < 2; ++phaseY)
                for (uint32_t phaseX = 0; phaseX < 2; ++phaseX) {
                    float phaseGain = 1.0f;
                    const uint32_t px = std::min(width - 1, (x & ~1u) + phaseX);
                    const uint32_t py = std::min(height - 1, (y & ~1u) + phaseY);
                    for (const auto& map : maps) phaseGain *= mapGain(map, px, py);
                    localMinimum = std::min(localMinimum, phaseGain);
                }
            if (localMinimum > 0) gain /= localMinimum;
        }
        if (normalizeGainMaps) gain /= maximumGain;
        const size_t index = static_cast<size_t>(y) * width + x;
        const double linear = std::max(0.0, (pixels[index] - black) / std::max(1.0, white - black));
        pixels[index] = static_cast<uint16_t>(std::clamp(std::lround(linear * gain * destinationWhite), 0l, 65535l));
    }

    if (data.size() & 1u) data.push_back(0);
    const uint32_t newOffset = static_cast<uint32_t>(data.size());
    const uint32_t newBytes = width * height * 2;
    data.resize(data.size() + newBytes);
    for (size_t i = 0; i < pixels.size(); ++i) {
        if (little) { data[newOffset + i*2] = pixels[i] & 0xff; data[newOffset + i*2+1] = pixels[i] >> 8; }
        else { data[newOffset + i*2] = pixels[i] >> 8; data[newOffset + i*2+1] = pixels[i] & 0xff; }
    }
    auto writeScalar = [&](const TiffEntry& e, uint32_t value, uint32_t index = 0) {
        const size_t pos = e.valueOffset + index * (e.type == TIFF_TYPE_SHORT ? 2 : 4);
        if (e.type == TIFF_TYPE_SHORT) write16(data.data() + pos, static_cast<uint16_t>(value), little);
        else write32(data.data() + pos, value, little);
    };
    writeScalar(*bitsE, 16); writeScalar(*compressionE, TIFF_COMPRESSION_NONE);
    writeScalar(*offsetsE, newOffset); writeScalar(*countsE, newBytes);
    if (whiteE) writeScalar(*whiteE, static_cast<uint32_t>(destinationWhite));
    if (blackE) {
        if (blackE->type == TIFF_TYPE_RATIONAL)
            for (uint32_t i = 0; i < blackE->count; ++i) writeRational(data, *blackE, i, 0.0, little);
        else for (uint32_t i = 0; i < blackE->count; ++i) writeScalar(*blackE, 0, i);
    }
    if (colorOnly) {
        std::copy(luminanceOpcode.begin(), luminanceOpcode.end(),
                  data.begin() + opcodeE->valueOffset);
        write16(data.data() + opcodeE->entryOffset, TIFF_TAG_OPCODE_LIST_3, little);
        write32(data.data() + opcodeE->entryOffset + 4,
                static_cast<uint32_t>(luminanceOpcode.size()), little);
    } else {
        // Keep a valid OpcodeList2 tag, but make its list empty to prevent double application.
        write32(data.data() + opcodeE->valueOffset, 0, false);
        write32(data.data() + opcodeE->entryOffset + 4, 4, little);
    }
    return true;
}

bool DNGDecoder::readDNGFile(const std::string& filePath, std::vector<uint8_t>& data) {
    std::ifstream file(filePath, std::ios::binary);
    if (!file.is_open()) {
        spdlog::error("Could not open DNG file: {}", filePath);
        return false;
    }
    
    // Get file size
    file.seekg(0, std::ios::end);
    size_t fileSize = file.tellg();
    file.seekg(0, std::ios::beg);
    
    // Read entire file
    data.resize(fileSize);
    file.read(reinterpret_cast<char*>(data.data()), fileSize);
    
    if (!file.good()) {
        spdlog::error("Error reading DNG file: {}", filePath);
        return false;
    }
    
    spdlog::debug("DNGDecoder: Read DNG file {} ({} bytes)", filePath, fileSize);
    return true;
}

bool DNGDecoder::readDNGGainMap(const std::string& dngPath, GainMap& gainMap) {
    std::vector<uint8_t> dngData;
    if (!readDNGFile(dngPath, dngData)) {
        return false;
    }
    
    bool little = true;
    std::vector<GainMap> maps;
    for (const auto& entry : findTiffEntries(dngData, little)) {
        if (entry.tag == TIFF_TAG_OPCODE_LIST_2 && entry.count &&
            parseOpcodeGainMaps(dngData.data() + entry.valueOffset, entry.count, maps) &&
            !maps.empty()) {
            gainMap = maps.front();
            return true;
        }
    }
    return false;
}

bool DNGDecoder::parseOpcodeGainMaps(const uint8_t* opcodeData, size_t opcodeSize,
                                    std::vector<GainMap>& gainMaps) {
    if (opcodeSize < 4) return false;
    const uint32_t numOpcodes = readBE32(opcodeData);
    size_t offset = 4;
    for (uint32_t i = 0; i < numOpcodes; ++i) {
        if (offset + 16 > opcodeSize) return false;
        const uint32_t id = readBE32(opcodeData + offset);
        const uint32_t bytes = readBE32(opcodeData + offset + 12);
        offset += 16;
        if (bytes > opcodeSize - offset) return false;
        if (id == OPCODE_GAIN_MAP && bytes >= 76) {
            const uint8_t* p = opcodeData + offset;
            GainMap map{};
            map.top = readBE32(p); map.left = readBE32(p + 4);
            map.bottom = readBE32(p + 8); map.right = readBE32(p + 12);
            map.plane = readBE32(p + 16); map.planes = readBE32(p + 20);
            map.rowPitch = readBE32(p + 24); map.colPitch = readBE32(p + 28);
            map.height = readBE32(p + 32); map.width = readBE32(p + 36);
            map.spacingV = readBEDouble(p + 40); map.spacingH = readBEDouble(p + 48);
            map.originV = readBEDouble(p + 56); map.originH = readBEDouble(p + 64);
            map.channels = readBE32(p + 72);
            const uint64_t samples = static_cast<uint64_t>(map.width) * map.height * map.channels;
            if (!map.width || !map.height || !map.channels || samples > (bytes - 76) / 4)
                return false;
            map.data.resize(static_cast<size_t>(samples));
            for (size_t sample = 0; sample < map.data.size(); ++sample)
                map.data[sample] = readBEFloat(p + 76 + sample * 4);
            gainMaps.push_back(std::move(map));
        }
        offset += bytes;
    }
    return !gainMaps.empty();
}

} // namespace motioncam
