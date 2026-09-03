#include "DNGDecoder.h"
#include "DataLevels.h"
#include "Utils.h"
#include <spdlog/spdlog.h>
#include <boost/filesystem.hpp>
#include <boost/algorithm/string.hpp>
#include <boost/regex.hpp>
#include <fstream>
#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstring>
#include <cmath>
#include <limits>
#include <set>
#include <sstream>
#include <iomanip>
#include <setjmp.h>
#include <thread>
#include "liblj92/lj92.h"
#include <jpeglib.h>
#include <jxl/decode.h>
#include <jxl/encode.h>

#ifdef _MSC_VER
#include <stdlib.h>
#define __builtin_bswap16(x) _byteswap_ushort(x)
#define __builtin_bswap32(x) _byteswap_ulong(x)
#endif

namespace motioncam {

namespace {
std::atomic_uint32_t foregroundDngWork{0};

constexpr size_t gainMapPhaseChannel(uint32_t x, uint32_t y,
                                     uint32_t left, uint32_t top,
                                     uint32_t phaseGroup) {
    return (((y - top) / phaseGroup) & 1u) * 2u +
           (((x - left) / phaseGroup) & 1u);
}

static_assert(gainMapPhaseChannel(1, 1, 0, 0, 2) == 0); // 4x4 CFA
static_assert(gainMapPhaseChannel(2, 2, 0, 0, 2) == 3);
static_assert(gainMapPhaseChannel(2, 2, 0, 0, 3) == 0); // 6x6 CFA
static_assert(gainMapPhaseChannel(3, 3, 0, 0, 3) == 3);
static_assert(gainMapPhaseChannel(3, 3, 0, 0, 4) == 0); // 8x8 CFA
static_assert(gainMapPhaseChannel(4, 4, 0, 0, 4) == 3);

void yieldToForegroundDngWork() {
    while (foregroundDngWork.load(std::memory_order_acquire) != 0)
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
}
}

void DNGDecoder::beginForegroundWork() {
    foregroundDngWork.fetch_add(1, std::memory_order_acq_rel);
}

void DNGDecoder::endForegroundWork() {
    foregroundDngWork.fetch_sub(1, std::memory_order_acq_rel);
}

namespace {
    using SoftwareVersion = std::array<unsigned int, 4>;

    bool parseSoftwareVersion(const std::string& software, size_t productLength,
                              SoftwareVersion& version) {
        const size_t firstDigit = software.find_first_of("0123456789", productLength);
        if (firstDigit == std::string::npos) return false;
        size_t cursor = firstDigit;
        for (size_t component = 0; component < version.size(); ++component) {
            if (cursor >= software.size() || !std::isdigit(
                    static_cast<unsigned char>(software[cursor]))) return component != 0;
            unsigned int value = 0;
            while (cursor < software.size() && std::isdigit(
                       static_cast<unsigned char>(software[cursor]))) {
                value = value * 10u + static_cast<unsigned int>(software[cursor] - '0');
                ++cursor;
            }
            version[component] = value;
            if (cursor >= software.size() || software[cursor] != '.') return true;
            ++cursor;
        }
        return true;
    }

    bool hasAffectedGainMapOrder(const std::string& rawSoftware) {
        std::string software = rawSoftware.c_str();
        boost::algorithm::trim(software);
        boost::algorithm::to_lower(software);

        constexpr const char* toolsName = "motioncam tools";
        constexpr const char* motionCamName = "motioncam";
        SoftwareVersion version{};
        const auto productMatch = [&](const char* product) {
            const size_t length = std::strlen(product);
            return boost::algorithm::starts_with(software, product) &&
                (software.size() == length || std::isspace(
                    static_cast<unsigned char>(software[length])) ||
                 software[length] == '-' || software[length] == 'v');
        };
        if (productMatch(toolsName)) {
            // Early MotionCam Tools files did not always include a version.
            if (!parseSoftwareVersion(software, std::strlen(toolsName), version)) return true;
            return version <= SoftwareVersion{1, 0, 0, 0};
        }
        if (productMatch(motionCamName)) {
            if (!parseSoftwareVersion(software, std::strlen(motionCamName), version)) return false;
            return version < SoftwareVersion{4, 0, 5, 0};
        }
        return false;
    }

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
    constexpr uint16_t TIFF_TAG_JPEG_OFFSET = 513;
    constexpr uint16_t TIFF_TAG_JPEG_BYTE_COUNT = 514;
    constexpr uint16_t TIFF_TAG_AS_SHOT_NEUTRAL = 50728;
    constexpr uint16_t TIFF_TAG_COLOR_MATRIX_1 = 50721;
    constexpr uint16_t TIFF_TAG_COLOR_MATRIX_2 = 50722;
    constexpr uint16_t TIFF_TAG_FORWARD_MATRIX_1 = 50964;
    constexpr uint16_t TIFF_TAG_FORWARD_MATRIX_2 = 50965;
    constexpr uint16_t TIFF_TAG_BASELINE_EXPOSURE = 50730;
    constexpr uint16_t TIFF_TAG_TIME_CODES = 51043;
    constexpr uint16_t TIFF_TAG_FRAME_RATE = 51044;
    constexpr uint16_t TIFF_TAG_XMP = 700;
    constexpr uint16_t TIFF_TAG_SOFTWARE = 305;
    constexpr uint16_t TIFF_TYPE_BYTE = 1;
    constexpr uint16_t TIFF_TYPE_SHORT = 3;
    constexpr uint16_t TIFF_TYPE_LONG = 4;
    constexpr uint16_t TIFF_TYPE_RATIONAL = 5;
    constexpr uint16_t TIFF_TYPE_UNDEFINED = 7;
    constexpr uint16_t TIFF_TYPE_SRATIONAL = 10;

    size_t tiffTypeSize(uint16_t type) {
        switch (type) {
        case 1:  // BYTE
        case 2:  // ASCII
        case 6:  // SBYTE
        case 7:  // UNDEFINED
            return 1;
        case 3:  // SHORT
        case 8:  // SSHORT
            return 2;
        case 4:  // LONG
        case 9:  // SLONG
        case 11: // FLOAT
        case 13: // IFD
            return 4;
        case 5:  // RATIONAL
        case 10: // SRATIONAL
        case 12: // DOUBLE
            return 8;
        default:
            return 0;
        }
    }
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
    constexpr uint16_t TIFF_TAG_PLANAR_CONFIGURATION = 284;
    constexpr uint16_t TIFF_TAG_TILE_OFFSETS = 324;
    constexpr uint16_t TIFF_TAG_TILE_BYTE_COUNTS = 325;
    constexpr uint16_t TIFF_TAG_TILE_WIDTH = 322;
    constexpr uint16_t TIFF_TAG_TILE_LENGTH = 323;
    constexpr uint16_t TIFF_TAG_UNUSED_TILE_LENGTH = 65010;
    constexpr uint16_t TIFF_TAG_UNUSED_LINEARIZATION_TABLE = 65011;
    constexpr uint16_t TIFF_TAG_BLACK_LEVEL_REPEAT_DIM = 50713;
    constexpr uint16_t TIFF_TAG_BLACK_LEVEL = 50714;
    constexpr uint16_t TIFF_TAG_WHITE_LEVEL = 50717;
    constexpr uint16_t TIFF_TAG_DNG_VERSION = 50706;
    constexpr uint16_t TIFF_TAG_DNG_BACKWARD_VERSION = 50707;
    constexpr uint16_t TIFF_TAG_LINEARIZATION_TABLE = 50712;
    constexpr uint16_t TIFF_TAG_DEFAULT_CROP_ORIGIN = 50719;
    constexpr uint16_t TIFF_TAG_DEFAULT_CROP_SIZE = 50720;
    constexpr uint16_t TIFF_TAG_ACTIVE_AREA = 50829;
    constexpr uint16_t TIFF_PHOTOMETRIC_CFA = 32803;
    constexpr uint16_t TIFF_COMPRESSION_NONE = 1;
    constexpr uint16_t TIFF_COMPRESSION_JPEG = 7;
    constexpr uint16_t TIFF_COMPRESSION_LOSSY_JPEG = 34892;
    constexpr uint16_t TIFF_COMPRESSION_JPEG_XL = 52546;

    struct JpegErrorManager {
        jpeg_error_mgr base;
        jmp_buf jump;
    };

    void jpegErrorExit(j_common_ptr info) {
        longjmp(reinterpret_cast<JpegErrorManager*>(info->err)->jump, 1);
    }

    bool checkedImageSamples(uint32_t width, uint32_t height, uint32_t channels,
                             size_t& samples) {
        if (!width || !height || !channels ||
            static_cast<size_t>(width) > std::numeric_limits<size_t>::max() / height)
            return false;
        samples = static_cast<size_t>(width) * height;
        if (samples > std::numeric_limits<size_t>::max() / channels) return false;
        samples *= channels;
        return samples <= std::vector<uint16_t>().max_size();
    }

    bool decodeDctJPEG(const uint8_t* encoded, size_t encodedSize,
                            uint32_t width, uint32_t height, uint32_t channels,
                            std::vector<uint16_t>& pixels) {
        if (!encoded || !encodedSize || (channels != 1 && channels != 3)) return false;
        jpeg_decompress_struct codec{};
        JpegErrorManager error{};
        // These must outlive the setjmp boundary. A libjpeg error longjmps
        // here, so constructing non-trivial C++ objects below setjmp would be
        // undefined behaviour when their scopes are bypassed.
        std::vector<uint8_t> row8;
        std::vector<J12SAMPLE> row12;
        std::vector<uint16_t> decoded;
        codec.err = jpeg_std_error(&error.base);
        error.base.error_exit = jpegErrorExit;
        if (setjmp(error.jump)) { jpeg_destroy_decompress(&codec); return false; }
        jpeg_create_decompress(&codec);
        jpeg_mem_src(&codec, encoded, static_cast<unsigned long>(encodedSize));
        if (jpeg_read_header(&codec, TRUE) != JPEG_HEADER_OK) {
            jpeg_destroy_decompress(&codec); return false;
        }
        codec.out_color_space = channels == 3 ? JCS_RGB : JCS_GRAYSCALE;
        const int precision = codec.data_precision;
        if (precision != 8 && precision != 12) {
            jpeg_destroy_decompress(&codec); return false;
        }
        const bool blackmagicLayout = channels == 1 && (height & 1u) == 0 &&
            width <= std::numeric_limits<uint32_t>::max() / 2u &&
            codec.image_width == width * 2 && codec.image_height == height / 2;
        // Accept the inverse experimental layout written by early builds so
        // those files can still be mounted and converted.
        const bool legacyLayout = channels == 1 && (width & 1u) == 0 &&
            height <= std::numeric_limits<uint32_t>::max() / 2u &&
            codec.image_width == width / 2 && codec.image_height == height * 2;
        const bool reshapedCfa = blackmagicLayout || legacyLayout;
        const uint32_t decodedWidth = blackmagicLayout ? width * 2
            : legacyLayout ? width / 2 : width;
        const uint32_t decodedHeight = blackmagicLayout ? height / 2
            : legacyLayout ? height * 2 : height;
        if (!jpeg_start_decompress(&codec) || codec.output_width != decodedWidth ||
            codec.output_height != decodedHeight || codec.output_components != channels) {
            jpeg_destroy_decompress(&codec); return false;
        }
        size_t decodedSamples = 0;
        if (!checkedImageSamples(decodedWidth, decodedHeight, channels, decodedSamples)) {
            jpeg_destroy_decompress(&codec); return false;
        }
        if (precision == 12) row12.resize(static_cast<size_t>(decodedWidth) * channels);
        else row8.resize(static_cast<size_t>(decodedWidth) * channels);
        decoded.resize(decodedSamples);
        while (codec.output_scanline < codec.output_height) {
            const uint32_t y = codec.output_scanline;
            if (precision == 12) {
                J12SAMPROW rows[] = {row12.data()};
                if (jpeg12_read_scanlines(&codec, rows, 1) != 1) {
                    jpeg_destroy_decompress(&codec); return false;
                }
                std::copy(row12.begin(), row12.end(),
                          decoded.begin() + static_cast<size_t>(y) * row12.size());
            } else {
                JSAMPROW rows[] = {row8.data()};
                if (jpeg_read_scanlines(&codec, rows, 1) != 1) {
                    jpeg_destroy_decompress(&codec); return false;
                }
                std::copy(row8.begin(), row8.end(),
                          decoded.begin() + static_cast<size_t>(y) * row8.size());
            }
        }
        const bool ok = jpeg_finish_decompress(&codec) == TRUE;
        jpeg_destroy_decompress(&codec);
        if (!ok) return false;
        if (!reshapedCfa || blackmagicLayout) {
            // The Blackmagic transform is a pure reshape: decoded sample order
            // already is ordinary TIFF row-major CFA order.
            pixels = std::move(decoded);
            return true;
        }
        pixels.resize(static_cast<size_t>(width) * height);
        for (uint32_t y = 0; y < height; ++y)
            for (uint32_t x = 0; x < width; ++x)
                pixels[static_cast<size_t>(y) * width + x] =
                    decoded[(static_cast<size_t>(y) * 2 + (x & 1u)) * decodedWidth + x / 2];
        return true;
    }

    bool decodeLosslessJPEG(const uint8_t* encoded, size_t encodedSize,
                            uint32_t width, uint32_t height, uint32_t channels,
                            std::vector<uint16_t>& pixels) {
        if (!encoded || !encodedSize || !width || !height || !channels || channels > 4)
            return false;
        jpeg_decompress_struct codec{};
        JpegErrorManager error{};
        std::vector<uint8_t> row8;
        std::vector<J12SAMPLE> row12;
        std::vector<J16SAMPLE> row16;
        codec.err = jpeg_std_error(&error.base);
        error.base.error_exit = jpegErrorExit;
        if (setjmp(error.jump)) { jpeg_destroy_decompress(&codec); return false; }
        jpeg_create_decompress(&codec);
        jpeg_mem_src(&codec, encoded, static_cast<unsigned long>(encodedSize));
        if (jpeg_read_header(&codec, TRUE) != JPEG_HEADER_OK ||
            codec.image_width != width || codec.image_height != height ||
            codec.num_components != channels || codec.data_precision < 8 ||
            codec.data_precision > 16) {
            jpeg_destroy_decompress(&codec);
            return false;
        }
        codec.out_color_space = channels == 1 ? JCS_GRAYSCALE : JCS_UNKNOWN;
        if (!jpeg_start_decompress(&codec) || codec.output_width != width ||
            codec.output_height != height || codec.output_components != channels) {
            jpeg_destroy_decompress(&codec);
            return false;
        }
        const size_t rowSamples = static_cast<size_t>(width) * channels;
        pixels.resize(rowSamples * height);
        const int precision = codec.data_precision;
        if (precision <= 8) row8.resize(rowSamples);
        else if (precision <= 12) row12.resize(rowSamples);
        else row16.resize(rowSamples);
        while (codec.output_scanline < codec.output_height) {
            const size_t output = static_cast<size_t>(codec.output_scanline) * rowSamples;
            if (precision <= 8) {
                JSAMPROW rows[]{row8.data()};
                if (jpeg_read_scanlines(&codec, rows, 1) != 1) {
                    jpeg_destroy_decompress(&codec); return false;
                }
                std::copy(row8.begin(), row8.end(), pixels.begin() + output);
            } else if (precision <= 12) {
                J12SAMPROW rows[]{row12.data()};
                if (jpeg12_read_scanlines(&codec, rows, 1) != 1) {
                    jpeg_destroy_decompress(&codec); return false;
                }
                std::copy(row12.begin(), row12.end(), pixels.begin() + output);
            } else {
                J16SAMPROW rows[]{row16.data()};
                if (jpeg16_read_scanlines(&codec, rows, 1) != 1) {
                    jpeg_destroy_decompress(&codec); return false;
                }
                std::copy(row16.begin(), row16.end(), pixels.begin() + output);
            }
        }
        const bool ok = jpeg_finish_decompress(&codec) == TRUE;
        jpeg_destroy_decompress(&codec);
        return ok;
    }

    bool encodeDctJPEG12(const std::vector<uint16_t>& pixels, uint32_t width,
                            uint32_t height, uint32_t channels, int quality,
                            std::vector<uint8_t>& encoded) {
        size_t sampleCount = 0;
        if ((channels != 1 && channels != 3) ||
            !checkedImageSamples(width, height, channels, sampleCount) ||
            width > JPEG_MAX_DIMENSION || height > JPEG_MAX_DIMENSION ||
            pixels.size() != sampleCount) return false;
        jpeg_compress_struct codec{};
        JpegErrorManager error{};
        codec.err = jpeg_std_error(&error.base);
        error.base.error_exit = jpegErrorExit;
        unsigned char* output = nullptr;
        unsigned long outputSize = 0;
        if (setjmp(error.jump)) {
            jpeg_destroy_compress(&codec); free(output); return false;
        }
        jpeg_create_compress(&codec);
        jpeg_mem_dest(&codec, &output, &outputSize);
        codec.image_width = width; codec.image_height = height;
        codec.input_components = channels;
        codec.in_color_space = channels == 3 ? JCS_RGB : JCS_GRAYSCALE;
        codec.data_precision = 12;
        jpeg_set_defaults(&codec);
        // Raw linear RGB must not receive JPEG's usual chroma subsampling.
        if (channels == 3)
            for (int i = 0; i < 3; ++i)
                codec.comp_info[i].h_samp_factor = codec.comp_info[i].v_samp_factor = 1;
        // Blackmagic streams use 16-bit DQT entries (precision=1), which is
        // permitted by extended sequential 12-bit JPEG (SOF1).
        jpeg_set_quality(&codec, std::clamp(quality, 1, 100), FALSE);
        jpeg_start_compress(&codec, TRUE);
        const JOCTET blackmagicComment[4] = {0x99, 0x99, 0x99, 0x99};
        jpeg_write_marker(&codec, JPEG_COM, blackmagicComment,
                          sizeof(blackmagicComment));
        while (codec.next_scanline < codec.image_height) {
            J12SAMPROW row = const_cast<J12SAMPROW>(
                reinterpret_cast<const J12SAMPLE*>(pixels.data() +
                    static_cast<size_t>(codec.next_scanline) * width * channels));
            if (jpeg12_write_scanlines(&codec, &row, 1) != 1) {
                jpeg_destroy_compress(&codec); free(output); return false;
            }
        }
        jpeg_finish_compress(&codec);
        encoded.assign(output, output + outputSize);
        jpeg_destroy_compress(&codec); free(output);
        return !encoded.empty();
    }

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
                frame, JXL_ENC_FRAME_SETTING_EFFORT, 7) != JXL_ENC_SUCCESS) return finish(false);
        // Modular is the appropriate bit-exact path for lossless raw samples.
        // Do not force it for lossy output: libjxl's distance setting is tuned
        // for its normal lossy mode and forcing modular can collapse smooth,
        // low-range raw/log data into implausibly small, poorly interoperable
        // codestreams. This also matches the Adobe DNG SDK's mode selection.
        if (distance == 0.0f && JxlEncoderFrameSettingsSetOption(
                frame, JXL_ENC_FRAME_SETTING_MODULAR, 1) != JXL_ENC_SUCCESS)
            return finish(false);
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
        appendBE32(payload, map.plane); appendBE32(payload, map.planes);
        appendBE32(payload, map.rowPitch); appendBE32(payload, map.colPitch);
        appendBE32(payload, map.height); appendBE32(payload, map.width);
        appendBEDouble(payload, map.spacingV); appendBEDouble(payload, map.spacingH);
        appendBEDouble(payload, map.originV); appendBEDouble(payload, map.originH);
        appendBE32(payload, map.channels);
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
                const size_t typeSize = tiffTypeSize(type);
                if (!typeSize || (itemCount && typeSize >
                    std::numeric_limits<size_t>::max() / itemCount)) continue;
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

    bool sortTiffIfdEntries(std::vector<uint8_t>& data, uint32_t ifd, bool little) {
        if (ifd > data.size() || data.size() - ifd < 2) return false;
        const uint16_t count = read16(data.data() + ifd, little);
        const size_t first = static_cast<size_t>(ifd) + 2;
        if (count > (data.size() - first) / 12 ||
            data.size() - first - static_cast<size_t>(count) * 12 < 4) return false;
        std::vector<std::array<uint8_t, 12>> records(count);
        for (uint16_t i = 0; i < count; ++i)
            std::copy_n(data.data() + first + static_cast<size_t>(i) * 12, 12,
                        records[i].data());
        std::stable_sort(records.begin(), records.end(), [&](const auto& a, const auto& b) {
            return read16(a.data(), little) < read16(b.data(), little);
        });
        for (uint16_t i = 0; i < count; ++i)
            std::copy_n(records[i].data(), 12,
                        data.data() + first + static_cast<size_t>(i) * 12);
        return true;
    }

    bool replaceTiffStrip(std::vector<uint8_t>& data, uint32_t stripOffset,
                          uint32_t stripBytes, const std::vector<uint8_t>& replacement,
                          bool little) {
        const size_t oldEnd = static_cast<size_t>(stripOffset) + stripBytes;
        if (stripOffset > data.size() || oldEnd > data.size()) return false;
        const auto entries = findTiffEntries(data, little);
        const uint32_t oldRoot = read32(data.data() + 4, little);
        struct PointerPatch { size_t entryOffset; uint32_t rawOffset; };
        struct ImageOffsetsPatch {
            size_t entryOffset;
            uint16_t type;
            std::vector<uint32_t> offsets;
        };
        std::vector<PointerPatch> externalPointers;
        std::vector<PointerPatch> inlineIfdPointers;
        std::vector<PointerPatch> subIfdArrays;
        std::vector<ImageOffsetsPatch> imageOffsets;
        std::map<uint32_t, uint32_t> nextIfds;
        for (const auto& entry : entries) {
            const size_t typeSize = tiffTypeSize(entry.type);
            if (!typeSize) return false;
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
            if ((entry.tag == TIFF_TAG_STRIP_OFFSETS ||
                 entry.tag == TIFF_TAG_TILE_OFFSETS) &&
                (entry.type == TIFF_TYPE_SHORT || entry.type == TIFF_TYPE_LONG)) {
                ImageOffsetsPatch patch{entry.entryOffset, entry.type, {}};
                patch.offsets.reserve(entry.count);
                const size_t step = entry.type == TIFF_TYPE_SHORT ? 2 : 4;
                for (uint32_t i = 0; i < entry.count; ++i) {
                    const size_t at = entry.valueOffset + static_cast<size_t>(i) * step;
                    patch.offsets.push_back(entry.type == TIFF_TYPE_SHORT
                        ? read16(data.data() + at, little)
                        : read32(data.data() + at, little));
                }
                imageOffsets.push_back(std::move(patch));
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
        for (const auto& patch : imageOffsets) {
            const size_t entryOffset = relocated(static_cast<uint32_t>(patch.entryOffset));
            const uint32_t count = read32(data.data() + entryOffset + 4, little);
            const size_t step = patch.type == TIFF_TYPE_SHORT ? 2 : 4;
            const size_t values = count * step > 4
                ? read32(data.data() + entryOffset + 8, little)
                : entryOffset + 8;
            for (uint32_t i = 0; i < count && i < patch.offsets.size(); ++i) {
                const uint32_t value = relocated(patch.offsets[i]);
                if (patch.type == TIFF_TYPE_SHORT)
                    write16(data.data() + values + static_cast<size_t>(i) * step,
                            static_cast<uint16_t>(value), little);
                else
                    write32(data.data() + values + static_cast<size_t>(i) * step,
                            value, little);
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

    bool removeTiffRanges(std::vector<uint8_t>& data,
                          const std::vector<std::pair<uint32_t, uint32_t>>& ranges,
                          bool little) {
        if (ranges.empty()) return true;
        const auto entries = findTiffEntries(data, little);
        const uint32_t oldRoot = read32(data.data() + 4, little);
        struct PointerPatch { size_t entryOffset; uint32_t rawOffset; };
        struct ImageOffsetsPatch { size_t entryOffset; uint16_t type; std::vector<uint32_t> offsets; };
        std::vector<PointerPatch> externalPointers, inlineIfdPointers, subIfdArrays;
        std::vector<ImageOffsetsPatch> imageOffsets;
        std::map<uint32_t, uint32_t> nextIfds;
        for (const auto& entry : entries) {
            const size_t typeSize = tiffTypeSize(entry.type);
            if (!typeSize) return false;
            if (typeSize * static_cast<size_t>(entry.count) > 4)
                externalPointers.push_back({entry.entryOffset,
                    read32(data.data() + entry.entryOffset + 8, little)});
            if ((entry.tag == TIFF_TAG_EXIF_IFD || entry.tag == TIFF_TAG_SUB_IFDS) &&
                entry.type == TIFF_TYPE_LONG) {
                const uint32_t rawOffset = read32(data.data() + entry.entryOffset + 8, little);
                if (entry.count == 1) inlineIfdPointers.push_back({entry.entryOffset, rawOffset});
                else subIfdArrays.push_back({entry.entryOffset, rawOffset});
            }
            if ((entry.tag == TIFF_TAG_STRIP_OFFSETS || entry.tag == TIFF_TAG_TILE_OFFSETS) &&
                (entry.type == TIFF_TYPE_SHORT || entry.type == TIFF_TYPE_LONG)) {
                ImageOffsetsPatch patch{entry.entryOffset, entry.type, {}};
                const size_t step = entry.type == TIFF_TYPE_SHORT ? 2 : 4;
                for (uint32_t i = 0; i < entry.count; ++i) {
                    const size_t at = entry.valueOffset + static_cast<size_t>(i) * step;
                    patch.offsets.push_back(entry.type == TIFF_TYPE_SHORT
                        ? read16(data.data() + at, little) : read32(data.data() + at, little));
                }
                imageOffsets.push_back(std::move(patch));
            }
            if (!nextIfds.count(entry.ifdOffset)) {
                const uint16_t count = read16(data.data() + entry.ifdOffset, little);
                const size_t nextPos = static_cast<size_t>(entry.ifdOffset) + 2 +
                    static_cast<size_t>(count) * 12;
                if (nextPos + 4 <= data.size())
                    nextIfds[entry.ifdOffset] = read32(data.data() + nextPos, little);
            }
        }
        auto relocated = [&](uint32_t offset) -> uint32_t {
            uint64_t removed = 0;
            for (const auto& [start, bytes] : ranges) {
                const uint64_t end = static_cast<uint64_t>(start) + bytes;
                if (offset < start) break;
                if (offset < end) return 0;
                removed += bytes;
            }
            return static_cast<uint32_t>(offset - removed);
        };
        size_t removedBytes = 0;
        for (const auto& range : ranges) removedBytes += range.second;
        std::vector<uint8_t> rebuilt;
        rebuilt.reserve(data.size() - removedBytes);
        size_t cursor = 0;
        for (const auto& [start, bytes] : ranges) {
            if (start < cursor || start > data.size() || bytes > data.size() - start) return false;
            rebuilt.insert(rebuilt.end(), data.begin() + cursor, data.begin() + start);
            cursor = static_cast<size_t>(start) + bytes;
        }
        rebuilt.insert(rebuilt.end(), data.begin() + cursor, data.end());
        data = std::move(rebuilt);
        write32(data.data() + 4, relocated(oldRoot), little);
        auto patchPointer = [&](const PointerPatch& pointer) {
            const size_t entryOffset = relocated(static_cast<uint32_t>(pointer.entryOffset));
            write32(data.data() + entryOffset + 8, relocated(pointer.rawOffset), little);
        };
        for (const auto& pointer : externalPointers) patchPointer(pointer);
        for (const auto& pointer : inlineIfdPointers) patchPointer(pointer);
        for (const auto& pointer : subIfdArrays) {
            const size_t entryOffset = relocated(static_cast<uint32_t>(pointer.entryOffset));
            const uint32_t count = read32(data.data() + entryOffset + 4, little);
            const uint32_t arrayOffset = read32(data.data() + entryOffset + 8, little);
            for (uint32_t i = 0; i < count; ++i) {
                const size_t at = static_cast<size_t>(arrayOffset) + i * 4;
                if (at + 4 > data.size()) return false;
                write32(data.data() + at, relocated(read32(data.data() + at, little)), little);
            }
        }
        for (const auto& patch : imageOffsets) {
            const size_t entryOffset = relocated(static_cast<uint32_t>(patch.entryOffset));
            const uint32_t count = read32(data.data() + entryOffset + 4, little);
            const size_t step = patch.type == TIFF_TYPE_SHORT ? 2 : 4;
            const size_t values = count * step > 4
                ? read32(data.data() + entryOffset + 8, little) : entryOffset + 8;
            for (uint32_t i = 0; i < count && i < patch.offsets.size(); ++i) {
                const uint32_t value = relocated(patch.offsets[i]);
                if (patch.type == TIFF_TYPE_SHORT)
                    write16(data.data() + values + static_cast<size_t>(i) * step,
                            static_cast<uint16_t>(value), little);
                else write32(data.data() + values + static_cast<size_t>(i) * step, value, little);
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

    bool insertTiffScalarEntry(std::vector<uint8_t>& data, uint32_t targetIfd,
                               uint16_t tag, uint16_t type, uint32_t value, bool little) {
        if (type != TIFF_TYPE_SHORT && type != TIFF_TYPE_LONG) return false;
        if (data.size() > std::numeric_limits<uint32_t>::max() - 12u ||
            targetIfd > data.size() || data.size() - targetIfd < 2) return false;
        const uint16_t targetCount = read16(data.data() + targetIfd, little);
        const size_t targetEnd = static_cast<size_t>(targetIfd) + 2u +
                                 static_cast<size_t>(targetCount) * 12u;
        if (targetCount == std::numeric_limits<uint16_t>::max() ||
            targetEnd + 4u > data.size()) return false;

        size_t insertion = targetEnd;
        for (uint16_t i = 0; i < targetCount; ++i) {
            const size_t at = static_cast<size_t>(targetIfd) + 2u + i * 12u;
            const uint16_t existingTag = read16(data.data() + at, little);
            if (existingTag == tag) return false;
            if (existingTag > tag) { insertion = at; break; }
        }

        const auto entries = findTiffEntries(data, little);
        struct EntryPointer { uint32_t entry; uint32_t target; };
        struct SubIfdArray {
            uint32_t entry;
            std::vector<uint32_t> targets;
            bool inlineValues = false;
        };
        std::vector<EntryPointer> externalPointers, inlineIfdPointers, imagePointers;
        std::vector<SubIfdArray> subIfdArrays, imageOffsetArrays;
        std::map<uint32_t, uint32_t> nextIfds;
        for (const auto& entry : entries) {
            const size_t typeSize = tiffTypeSize(entry.type);
            if (!typeSize) return false;
            const uint32_t raw = read32(data.data() + entry.entryOffset + 8, little);
            if (entry.count && typeSize > 4u / entry.count)
                externalPointers.push_back({static_cast<uint32_t>(entry.entryOffset), raw});
            if ((entry.tag == TIFF_TAG_EXIF_IFD || entry.tag == TIFF_TAG_SUB_IFDS) &&
                entry.type == TIFF_TYPE_LONG) {
                if (entry.count == 1) {
                    inlineIfdPointers.push_back({static_cast<uint32_t>(entry.entryOffset), raw});
                } else {
                    SubIfdArray array{static_cast<uint32_t>(entry.entryOffset), {}, false};
                    array.targets.reserve(entry.count);
                    for (uint32_t i = 0; i < entry.count; ++i)
                        array.targets.push_back(read32(data.data() + entry.valueOffset + i * 4u, little));
                    subIfdArrays.push_back(std::move(array));
                }
            }
            if ((entry.tag == TIFF_TAG_STRIP_OFFSETS || entry.tag == TIFF_TAG_TILE_OFFSETS) &&
                (entry.type == TIFF_TYPE_SHORT || entry.type == TIFF_TYPE_LONG)) {
                if (entry.count == 1) {
                    const uint32_t target = entry.type == TIFF_TYPE_SHORT
                        ? read16(data.data() + entry.valueOffset, little)
                        : read32(data.data() + entry.valueOffset, little);
                    imagePointers.push_back({static_cast<uint32_t>(entry.entryOffset), target});
                } else {
                    SubIfdArray array{static_cast<uint32_t>(entry.entryOffset), {},
                                      entry.type == TIFF_TYPE_SHORT && entry.count <= 2};
                    array.targets.reserve(entry.count);
                    const size_t step = entry.type == TIFF_TYPE_SHORT ? 2u : 4u;
                    for (uint32_t i = 0; i < entry.count; ++i)
                        array.targets.push_back(entry.type == TIFF_TYPE_SHORT
                            ? read16(data.data() + entry.valueOffset + i * step, little)
                            : read32(data.data() + entry.valueOffset + i * step, little));
                    imageOffsetArrays.push_back(std::move(array));
                }
            }
            if (!nextIfds.count(entry.ifdOffset)) {
                const uint16_t count = read16(data.data() + entry.ifdOffset, little);
                const size_t nextAt = static_cast<size_t>(entry.ifdOffset) + 2u +
                                      static_cast<size_t>(count) * 12u;
                if (nextAt + 4u > data.size()) return false;
                nextIfds[entry.ifdOffset] = read32(data.data() + nextAt, little);
            }
        }
        const uint32_t oldRoot = read32(data.data() + 4, little);
        auto relocated = [&](uint32_t offset) -> uint32_t {
            return offset >= insertion ? offset + 12u : offset;
        };

        data.insert(data.begin() + insertion, 12u, 0);
        write32(data.data() + 4, relocated(oldRoot), little);
        for (const auto& pointer : externalPointers)
            write32(data.data() + relocated(pointer.entry) + 8,
                    relocated(pointer.target), little);
        for (const auto& pointer : inlineIfdPointers)
            write32(data.data() + relocated(pointer.entry) + 8,
                    relocated(pointer.target), little);
        for (const auto& pointer : imagePointers) {
            const uint32_t entryAt = relocated(pointer.entry);
            const uint16_t type = read16(data.data() + entryAt + 2, little);
            if (type == TIFF_TYPE_SHORT &&
                relocated(pointer.target) > std::numeric_limits<uint16_t>::max()) {
                write16(data.data() + entryAt + 2, TIFF_TYPE_LONG, little);
                write32(data.data() + entryAt + 8, relocated(pointer.target), little);
            } else if (type == TIFF_TYPE_SHORT) {
                write16(data.data() + entryAt + 8, relocated(pointer.target), little);
            } else {
                write32(data.data() + entryAt + 8, relocated(pointer.target), little);
            }
        }
        for (const auto& array : subIfdArrays) {
            const uint32_t entryAt = relocated(array.entry);
            const uint32_t arrayAt = array.inlineValues
                ? entryAt + 8 : read32(data.data() + entryAt + 8, little);
            for (size_t i = 0; i < array.targets.size(); ++i)
                write32(data.data() + arrayAt + i * 4u, relocated(array.targets[i]), little);
        }
        for (const auto& array : imageOffsetArrays) {
            const uint32_t entryAt = relocated(array.entry);
            uint16_t type = read16(data.data() + entryAt + 2, little);
            const bool promote = type == TIFF_TYPE_SHORT &&
                std::any_of(array.targets.begin(), array.targets.end(), [&](uint32_t target) {
                    return relocated(target) > std::numeric_limits<uint16_t>::max();
                });
            if (promote) {
                const uint64_t bytes = static_cast<uint64_t>(array.targets.size()) * 4u;
                if (bytes > std::numeric_limits<uint32_t>::max() ||
                    data.size() > std::numeric_limits<uint32_t>::max() - bytes)
                    return false;
                const uint32_t longArray = static_cast<uint32_t>(data.size());
                data.resize(data.size() + static_cast<size_t>(bytes));
                write16(data.data() + entryAt + 2, TIFF_TYPE_LONG, little);
                write32(data.data() + entryAt + 8, longArray, little);
                type = TIFF_TYPE_LONG;
            }
            const uint32_t arrayAt = array.inlineValues
                && !promote ? entryAt + 8 : read32(data.data() + entryAt + 8, little);
            const size_t step = type == TIFF_TYPE_SHORT ? 2u : 4u;
            for (size_t i = 0; i < array.targets.size(); ++i) {
                if (type == TIFF_TYPE_SHORT)
                    write16(data.data() + arrayAt + i * step,
                            static_cast<uint16_t>(relocated(array.targets[i])), little);
                else
                    write32(data.data() + arrayAt + i * step,
                            relocated(array.targets[i]), little);
            }
        }
        for (const auto& [oldIfd, oldNext] : nextIfds) {
            const uint32_t newIfd = relocated(oldIfd);
            const uint16_t count = oldIfd == targetIfd ? targetCount + 1u
                                                       : read16(data.data() + newIfd, little);
            write32(data.data() + static_cast<size_t>(newIfd) + 2u +
                    static_cast<size_t>(count) * 12u, relocated(oldNext), little);
        }
        write16(data.data() + relocated(targetIfd), targetCount + 1u, little);
        write16(data.data() + insertion, tag, little);
        write16(data.data() + insertion + 2u, type, little);
        write32(data.data() + insertion + 4u, 1, little);
        if (type == TIFF_TYPE_SHORT)
            write16(data.data() + insertion + 8u, static_cast<uint16_t>(value), little);
        else
            write32(data.data() + insertion + 8u, value, little);
        return sortTiffIfdEntries(data, relocated(targetIfd), little);
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
        constexpr uint32_t preferredDenominator = 1000000;
        const bool signedRational = entry.type == TIFF_TYPE_SRATIONAL;
        const double numeratorLimit = signedRational
            ? static_cast<double>(std::numeric_limits<int32_t>::max())
            : static_cast<double>(std::numeric_limits<uint32_t>::max());
        // Geometry tags such as DefaultCropSize are unsigned rationals and can
        // easily exceed UINT32_MAX when multiplied by a fixed 1e6 denominator
        // (for example a 12032-pixel-wide phone sensor).  Retain micro-unit
        // precision where possible, but reduce the denominator rather than
        // clipping the value.  SRATIONAL values keep their signed range.
        uint32_t denominator = preferredDenominator;
        const double magnitude = std::abs(value);
        if (magnitude > 0.0 && magnitude * denominator > numeratorLimit)
            denominator = std::max<uint32_t>(1, static_cast<uint32_t>(
                std::floor(numeratorLimit / magnitude)));
        const double scaled = std::round(value * denominator);
        const size_t pos = entry.valueOffset + static_cast<size_t>(index) * 8;
        if (signedRational) {
            const auto numerator = static_cast<int32_t>(std::clamp(
                scaled, static_cast<double>(std::numeric_limits<int32_t>::min()),
                static_cast<double>(std::numeric_limits<int32_t>::max())));
            write32(data.data() + pos, static_cast<uint32_t>(numerator), little);
        } else {
            const auto numerator = static_cast<uint32_t>(std::clamp(
                scaled, 0.0, static_cast<double>(std::numeric_limits<uint32_t>::max())));
            write32(data.data() + pos, numerator, little);
        }
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

    bool addMissingTimingEntries(std::vector<uint8_t>& data, bool addFrameRate,
                                 bool addTimeCode, bool addXmp, uint32_t xmpBytes, bool little) {
        const int additions = addFrameRate + addTimeCode + addXmp;
        if (!additions) return true;
        const uint32_t oldIfd = read32(data.data() + 4, little);
        if (oldIfd + 2 > data.size()) return false;
        const uint16_t oldCount = read16(data.data() + oldIfd, little);
        const size_t oldEnd = static_cast<size_t>(oldIfd) + 2 + static_cast<size_t>(oldCount) * 12;
        if (oldEnd + 4 > data.size() || oldCount > std::numeric_limits<uint16_t>::max() - additions)
            return false;
        if (data.size() & 1u) data.push_back(0);
        const uint32_t newIfd = static_cast<uint32_t>(data.size());
        const uint16_t newCount = static_cast<uint16_t>(oldCount + additions);
        const size_t tableSize = 2 + static_cast<size_t>(newCount) * 12 + 4;
        const size_t externalSize = (addFrameRate ? 8 : 0) + (addTimeCode ? 8 : 0) +
                                    (addXmp ? xmpBytes : 0);
        data.resize(data.size() + tableSize + externalSize, 0);
        std::vector<std::array<uint8_t, 12>> entries;
        entries.reserve(newCount);
        for (uint16_t i = 0; i < oldCount; ++i) {
            std::array<uint8_t, 12> entry{};
            std::memcpy(entry.data(), data.data() + oldIfd + 2 + static_cast<size_t>(i) * 12, 12);
            entries.push_back(entry);
        }
        uint32_t valuePos = newIfd + static_cast<uint32_t>(tableSize);
        auto add = [&](uint16_t tag, uint16_t type, uint32_t count, uint32_t bytes) {
            std::array<uint8_t, 12> entry{};
            write16(entry.data(), tag, little);
            write16(entry.data() + 2, type, little);
            write32(entry.data() + 4, count, little);
            write32(entry.data() + 8, valuePos, little);
            entries.push_back(entry);
            valuePos += bytes;
        };
        if (addFrameRate) add(TIFF_TAG_FRAME_RATE, TIFF_TYPE_SRATIONAL, 1, 8);
        if (addTimeCode) add(TIFF_TAG_TIME_CODES, TIFF_TYPE_BYTE, 8, 8);
        if (addXmp) add(TIFF_TAG_XMP, TIFF_TYPE_BYTE, xmpBytes, xmpBytes);
        std::sort(entries.begin(), entries.end(), [little](const auto& a, const auto& b) {
            return read16(a.data(), little) < read16(b.data(), little);
        });
        write16(data.data() + newIfd, newCount, little);
        size_t pos = static_cast<size_t>(newIfd) + 2;
        for (const auto& entry : entries) {
            std::memcpy(data.data() + pos, entry.data(), 12);
            pos += 12;
        }
        std::memcpy(data.data() + pos, data.data() + oldEnd, 4);
        write32(data.data() + 4, newIfd, little);
        return true;
    }

    uint8_t toBcd(int value) {
        return static_cast<uint8_t>(((value / 10) << 4) | (value % 10));
    }

    int fromBcd(uint8_t value) {
        return ((value >> 4) & 0x03) * 10 + (value & 0x0f);
    }

    int64_t dropFrameNumber(int64_t frame, int nominalFps) {
        const int droppedPerMinute = nominalFps == 60 ? 4 : 2;
        const int64_t framesPerMinute = nominalFps * 60 - droppedPerMinute;
        const int64_t framesPerTenMinutes = nominalFps * 600 - droppedPerMinute * 9;
        const int64_t tenMinuteBlocks = frame / framesPerTenMinutes;
        const int64_t remainder = frame % framesPerTenMinutes;
        return frame + droppedPerMinute * 9 * tenMinuteBlocks +
            (remainder >= droppedPerMinute
                ? droppedPerMinute * ((remainder - droppedPerMinute) / framesPerMinute)
                : 0);
    }

    constexpr auto RELATIVE_PRESENTATION_TIMESTAMP_NS =
        "rpt:RelativePresentationTimestampNs=";
    constexpr auto RELATIVE_PRESENTATION_TIMESTAMP_NAMESPACE =
        "https://github.com/motioncam-app/motioncam-fs";

    std::string relativePresentationTimestampXmp(Timestamp timestampNs) {
        std::ostringstream value;
        value << std::setw(20) << std::setfill('0') << timestampNs;
        return "<?xpacket begin=''?>"
               "<x:xmpmeta xmlns:x='adobe:ns:meta/'>"
               "<rdf:RDF xmlns:rdf='http://www.w3.org/1999/02/22-rdf-syntax-ns#'>"
               "<rdf:Description xmlns:rpt='" +
               std::string(RELATIVE_PRESENTATION_TIMESTAMP_NAMESPACE) + "' "
               "rpt:RelativePresentationTimestampNs='" + value.str() + "'/>"
               "</rdf:RDF></x:xmpmeta><?xpacket end='w'?>";
    }

    bool readRelativePresentationTimestamp(const std::vector<uint8_t>& data,
                                           const TiffEntry& entry, Timestamp& timestamp) {
        if (entry.tag != TIFF_TAG_XMP || entry.type != TIFF_TYPE_BYTE || !entry.count) return false;
        const std::string xmp(reinterpret_cast<const char*>(data.data() + entry.valueOffset), entry.count);
        if (xmp.find(RELATIVE_PRESENTATION_TIMESTAMP_NAMESPACE) == std::string::npos) return false;
        const std::string marker = RELATIVE_PRESENTATION_TIMESTAMP_NS;
        const auto markerPos = xmp.find(marker);
        if (markerPos == std::string::npos || markerPos + marker.size() >= xmp.size()) return false;
        const char quote = xmp[markerPos + marker.size()];
        if (quote != '\'' && quote != '"') return false;
        const auto begin = markerPos + marker.size() + 1;
        const auto end = xmp.find(quote, begin);
        if (end == std::string::npos) return false;
        try {
            const auto value = std::stoull(xmp.substr(begin, end - begin));
            if (value > static_cast<uint64_t>(std::numeric_limits<Timestamp>::max())) return false;
            timestamp = static_cast<Timestamp>(value);
            return true;
        } catch (...) { return false; }
    }

    bool writeRelativePresentationTimestamp(std::vector<uint8_t>& data, const TiffEntry& entry,
                                            Timestamp timestamp, bool little) {
        std::string xmp(reinterpret_cast<const char*>(data.data() + entry.valueOffset), entry.count);
        std::ostringstream value;
        value << std::setw(20) << std::setfill('0') << timestamp;
        const bool hasTimingNamespace =
            xmp.find(RELATIVE_PRESENTATION_TIMESTAMP_NAMESPACE) != std::string::npos;
        const std::string marker = RELATIVE_PRESENTATION_TIMESTAMP_NS;
        const auto markerPos = xmp.find(marker);
        if (hasTimingNamespace && markerPos != std::string::npos &&
            markerPos + marker.size() < xmp.size()) {
            const char quote = xmp[markerPos + marker.size()];
            const auto begin = markerPos + marker.size() + 1;
            const auto end = xmp.find(quote, begin);
            if ((quote == '\'' || quote == '"') && end != std::string::npos) {
                xmp.replace(begin, end - begin, value.str());
            }
        } else {
            const auto rdfEnd = xmp.find("</rdf:RDF>");
            if (rdfEnd == std::string::npos) return false;
            xmp.insert(rdfEnd,
                "<rdf:Description xmlns:rpt='" +
                std::string(RELATIVE_PRESENTATION_TIMESTAMP_NAMESPACE) + "' "
                "rpt:RelativePresentationTimestampNs='" +
                value.str() + "'/>");
        }
        if (xmp.size() == entry.count) {
            std::memcpy(data.data() + entry.valueOffset, xmp.data(), xmp.size());
            return true;
        }
        if (data.size() & 1u) data.push_back(0);
        if (data.size() > std::numeric_limits<uint32_t>::max() - xmp.size()) return false;
        const uint32_t offset = static_cast<uint32_t>(data.size());
        data.insert(data.end(), xmp.begin(), xmp.end());
        write32(data.data() + entry.entryOffset + 4, static_cast<uint32_t>(xmp.size()), little);
        write32(data.data() + entry.entryOffset + 8, offset, little);
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
        } else if (!(mSequenceInfo.fps > 0.0)) {
            mSequenceInfo.fps = 24.0;
        }
    }
    
    spdlog::info("DNGDecoder: Found {} DNG files, {}x{} @ {:.2f}fps", 
                 mSequenceInfo.totalFrames, mSequenceInfo.width, mSequenceInfo.height, mSequenceInfo.fps);
}

void DNGDecoder::findDNGFiles() {
    std::vector<std::string> dngFiles;
    const boost::filesystem::path sourcePath(mSequencePath);
    if (boost::filesystem::is_regular_file(sourcePath)) {
        if (!boost::iequals(sourcePath.extension().string(), ".dng"))
            throw std::runtime_error("Not a DNG file: " + mSequencePath);
        dngFiles.push_back(sourcePath.string());
    } else {
        const boost::filesystem::path basePath(mSequenceInfo.basePath);
        if (!boost::filesystem::exists(basePath) || !boost::filesystem::is_directory(basePath))
            throw std::runtime_error("Invalid DNG sequence path: " + mSequenceInfo.basePath);
        boost::filesystem::directory_iterator end;
        for (boost::filesystem::directory_iterator it(basePath); it != end; ++it) {
            if (boost::iequals(it->path().extension().string(), ".dng"))
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
        frameInfo.timestamp = static_cast<Timestamp>(i * 1000000000.0 / 24.0);
        
        mFrames.push_back(frameInfo);
    }

    // Image geometry belongs to the raw IFD and is independent of its
    // compression. Read it instead of exposing the old 1920x1080 placeholder.
    for (auto& frame : mFrames) {
        std::vector<uint8_t> bytes;
        if (!readDNGFile(frame.filePath, bytes)) continue;
        bool little = true;
        const auto entries = findTiffEntries(bytes, little);
        double frameRate = 0.0;
        for (const auto& entry : entries) {
            if (entry.tag == TIFF_TAG_XMP && entry.type == TIFF_TYPE_BYTE && entry.count &&
                entry.valueOffset + entry.count <= bytes.size()) {
                const std::string xmp(reinterpret_cast<const char*>(bytes.data() + entry.valueOffset),
                                      entry.count);
                frame.duplicateFrame = xmp.find("rpt:DuplicateFrame='true'") != std::string::npos ||
                    xmp.find("rpt:DuplicateFrame=\"true\"") != std::string::npos;
                frame.syntheticFrame = xmp.find("rpt:SyntheticFrame='true'") != std::string::npos ||
                    xmp.find("rpt:SyntheticFrame=\"true\"") != std::string::npos;
            }
            Timestamp embeddedTimestamp = 0;
            if (readRelativePresentationTimestamp(bytes, entry, embeddedTimestamp)) {
                frame.timestamp = embeddedTimestamp;
                frame.hasExactPresentationTimestamp = true;
            }
            if (entry.tag == TIFF_TAG_FRAME_RATE &&
                (entry.type == TIFF_TYPE_RATIONAL || entry.type == TIFF_TYPE_SRATIONAL) &&
                entry.count) {
                frameRate = readRational(bytes, entry, 0, little);
                if (!(mSequenceInfo.fps > 0.0)) mSequenceInfo.fps = frameRate;
            }
        }
        if (!frame.hasExactPresentationTimestamp && frameRate > 0.0) {
            for (const auto& entry : entries) {
                if (entry.tag != TIFF_TAG_TIME_CODES || entry.type != TIFF_TYPE_BYTE || entry.count < 8)
                    continue;
                const auto* tc = bytes.data() + entry.valueOffset;
                const int nominal = frameRate >= 47.0 && frameRate <= 61.0
                    ? static_cast<int>(std::lround(frameRate / 2.0))
                    : std::max(1, std::min(30, static_cast<int>(std::lround(frameRate))));
                const int fieldsPerCode = frameRate >= 47.0 && frameRate <= 61.0 ? 2 : 1;
                const int field = fieldsPerCode == 2
                    ? ((nominal == 25 ? tc[3] : tc[1]) & 0x80 ? 1 : 0) : 0;
                const int secondsPart = fromBcd(tc[1] & 0x7f);
                const int minutesPart = fromBcd(tc[2] & 0x7f);
                const int hoursPart = fromBcd(tc[3] & 0x3f);
                const int64_t seconds = secondsPart + 60LL * minutesPart + 3600LL * hoursPart;
                const int timeCodeFrame = fromBcd(tc[0] & 0x3f);
                int64_t addressFrame = seconds * nominal + timeCodeFrame;
                if (tc[0] & 0x40) {
                    const int64_t totalMinutes = hoursPart * 60LL + minutesPart;
                    const int droppedPerMinute = nominal == 30 ? 2 : 4;
                    addressFrame -= droppedPerMinute * (totalMinutes - totalMinutes / 10);
                }
                const int64_t sourceFrame = addressFrame * fieldsPerCode + field;
                frame.timestamp = static_cast<Timestamp>(std::llround(sourceFrame * 1e9 / frameRate));
                frame.hasTimeCodeTimestamp = true;
                break;
            }
        }
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

namespace {
struct ImagePayloadSegment { size_t offset, size; };

bool imagePayloadSegments(const std::vector<uint8_t>& data,
                          std::vector<ImagePayloadSegment>& result) {
        bool little = true;
        const auto entries = findTiffEntries(data, little);
        uint32_t imageIfd = 0;
        for (const auto& entry : entries) {
            if (entry.tag != TIFF_TAG_PHOTOMETRIC) continue;
            const uint32_t value = entry.type == TIFF_TYPE_SHORT
                ? read16(data.data() + entry.valueOffset, little)
                : entry.type == TIFF_TYPE_LONG
                    ? read32(data.data() + entry.valueOffset, little) : 0;
            if (value == TIFF_PHOTOMETRIC_CFA || value == 34892) {
                imageIfd = entry.ifdOffset;
                break;
            }
        }
        if (!imageIfd) return false;
        const TiffEntry* offsets = nullptr;
        const TiffEntry* counts = nullptr;
        for (const auto& entry : entries) {
            if (entry.ifdOffset != imageIfd) continue;
            if (entry.tag == TIFF_TAG_STRIP_OFFSETS || entry.tag == TIFF_TAG_TILE_OFFSETS)
                offsets = &entry;
            if (entry.tag == TIFF_TAG_STRIP_BYTE_COUNTS || entry.tag == TIFF_TAG_TILE_BYTE_COUNTS)
                counts = &entry;
            if (offsets && counts) break;
        }
        if (!offsets || !counts || offsets->ifdOffset != counts->ifdOffset ||
            offsets->count != counts->count || !offsets->count) return false;
        auto value = [&](const TiffEntry& entry, uint32_t i, uint32_t& out) {
            const size_t width = entry.type == TIFF_TYPE_SHORT ? 2 :
                                 entry.type == TIFF_TYPE_LONG ? 4 : 0;
            const size_t pos = entry.valueOffset + static_cast<size_t>(i) * width;
            if (!width || pos + width > data.size()) return false;
            out = width == 2 ? read16(data.data() + pos, little)
                             : read32(data.data() + pos, little);
            return true;
        };
        for (uint32_t i = 0; i < offsets->count; ++i) {
            uint32_t offset = 0, count = 0;
            if (!value(*offsets, i, offset) || !value(*counts, i, count) ||
                static_cast<size_t>(offset) + count > data.size()) return false;
            result.push_back({offset, count});
        }
        return true;
}

uint64_t hashImagePayload(const std::vector<uint8_t>& data,
                          const std::vector<ImagePayloadSegment>& parts) {
    uint64_t value = 1469598103934665603ULL;
    for (const auto& part : parts)
        for (size_t i = 0; i < part.size; ++i) {
            value ^= data[part.offset + i];
            value *= 1099511628211ULL;
        }
    return value;
}
}

bool DNGDecoder::imagePayloadHash(const std::vector<uint8_t>& data, uint64_t& hash) {
    std::vector<ImagePayloadSegment> segments;
    if (!imagePayloadSegments(data, segments)) return false;
    hash = hashImagePayload(data, segments);
    return true;
}

bool DNGDecoder::imagePayloadsEqual(const std::vector<uint8_t>& left,
                                    const std::vector<uint8_t>& right) {
    std::vector<ImagePayloadSegment> a, b;
    if (!imagePayloadSegments(left, a) || !imagePayloadSegments(right, b)) return false;
    size_t aTotal = 0, bTotal = 0;
    for (const auto& segment : a) aTotal += segment.size;
    for (const auto& segment : b) bTotal += segment.size;
    if (aTotal != bTotal) return false;

    // FNV-1a is used as a cheap rejection pass; equality is always confirmed
    // byte-for-byte, so hash collisions cannot create false duplicates.
    if (hashImagePayload(left, a) != hashImagePayload(right, b)) return false;
    size_t ai = 0, bi = 0, ap = 0, bp = 0, remaining = aTotal;
    while (remaining) {
        const size_t count = std::min(a[ai].size - ap, b[bi].size - bp);
        if (std::memcmp(left.data() + a[ai].offset + ap,
                        right.data() + b[bi].offset + bp, count) != 0) return false;
        remaining -= count; ap += count; bp += count;
        if (ap == a[ai].size) { ++ai; ap = 0; }
        if (bp == b[bi].size) { ++bi; bp = 0; }
    }
    return true;
}

void DNGDecoder::extractTimestampsFromFilenames() {
    // Try to extract frame numbers from filenames for better timing
    boost::regex frameNumberRegex(R"((?:^|[-_])(\d+)$)");
    boost::smatch match;

    std::vector<std::optional<int>> extracted(mFrames.size());
    for (size_t i = 0; i < mFrames.size(); ++i) {
        const auto& frame = mFrames[i];
        boost::filesystem::path p(frame.filePath);
        std::string filename = p.stem().string();
        if (boost::regex_search(filename, match, frameNumberRegex)) {
            extracted[i] = std::stoi(match[1].str());
        }
    }

    // Camera filenames commonly end in HHMMSS (for example IMG_..._145306).
    // Treat the suffix as a frame counter only when the whole sequence looks
    // like a plausible monotonically increasing counter. Otherwise retain the
    // default 30 fps timestamps assigned while discovering the files.
    bool plausibleCounter = extracted.size() >= 3;
    for (size_t i = 0; i < extracted.size(); ++i) {
        plausibleCounter &= extracted[i].has_value();
        if (i && plausibleCounter) {
            const int delta = *extracted[i] - *extracted[i - 1];
            plausibleCounter &= delta > 0 && delta <= 100;
        }
    }
    if (plausibleCounter) {
        const double fallbackFps = mSequenceInfo.fps > 0.0 ? mSequenceInfo.fps : 24.0;
        for (size_t i = 0; i < mFrames.size(); ++i) {
            mFrames[i].frameNumber = *extracted[i];
            if (!mFrames[i].hasExactPresentationTimestamp &&
                !mFrames[i].hasTimeCodeTimestamp)
                mFrames[i].timestamp = static_cast<Timestamp>(
                    *extracted[i] * 1000000000.0 / fallbackFps);
        }
        mSequenceInfo.hasFrameNumberSequence = true;
    } else {
        // A folder of independently named stills is not a VFR sequence. Keep
        // discovery order and expose it as a cinema-rate CFR clip. Do not
        // replace timing read from the DNGs; frames without timing already have
        // the discovery-order fallback assigned by findDNGFiles().
        mSequenceInfo.fps = 24.0;
        for (size_t i = 0; i < mFrames.size(); ++i) {
            mFrames[i].frameNumber = static_cast<int>(i);
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
    return getGainMaps(data, 2, gainMaps);
}

bool DNGDecoder::getGainMaps(const std::vector<uint8_t>& data, int opcodeList,
                             std::vector<GainMap>& gainMaps) {
    gainMaps.clear();
    const uint16_t wanted = opcodeList == 3 ? TIFF_TAG_OPCODE_LIST_3 : TIFF_TAG_OPCODE_LIST_2;
    bool little = true;
    for (const auto& entry : findTiffEntries(data, little)) {
        if (entry.tag == wanted && entry.count &&
            parseOpcodeGainMaps(data.data() + entry.valueOffset, entry.count, gainMaps))
            return true;
    }
    return false;
}

bool DNGDecoder::replaceGainMaps(std::vector<uint8_t>& data, int opcodeList,
                                 const std::vector<GainMap>& gainMaps) {
    const uint16_t wanted = opcodeList == 3 ? TIFF_TAG_OPCODE_LIST_3 : TIFF_TAG_OPCODE_LIST_2;
    bool little = true;
    const auto entries = findTiffEntries(data, little);
    const TiffEntry* entry = nullptr;
    for (const auto& candidate : entries)
        if (candidate.tag == wanted) { entry = &candidate; break; }
    std::vector<uint8_t> output(4, 0);
    uint32_t outputCount = 0;
    if (entry && entry->count) {
        const uint8_t* source = data.data() + entry->valueOffset;
        if (entry->count < 4) return false;
        const uint32_t count = readBE32(source);
        size_t offset = 4;
        for (uint32_t i = 0; i < count; ++i) {
            if (offset + 16 > entry->count) return false;
            const uint32_t id = readBE32(source + offset);
            const uint32_t bytes = readBE32(source + offset + 12);
            if (bytes > entry->count - offset - 16) return false;
            const size_t opcodeBytes = 16 + bytes;
            if (id != OPCODE_GAIN_MAP) {
                output.insert(output.end(), source + offset, source + offset + opcodeBytes);
                ++outputCount;
            }
            offset += opcodeBytes;
        }
        if (offset != entry->count) return false;
    }
    for (const auto& map : gainMaps) {
        const auto encoded = serializeGainMap(map);
        if (encoded.size() < 4 || readBE32(encoded.data()) != 1) return false;
        output.insert(output.end(), encoded.begin() + 4, encoded.end());
        ++outputCount;
    }
    output[0] = static_cast<uint8_t>(outputCount >> 24);
    output[1] = static_cast<uint8_t>(outputCount >> 16);
    output[2] = static_cast<uint8_t>(outputCount >> 8);
    output[3] = static_cast<uint8_t>(outputCount);
    if (!entry) {
        if (gainMaps.empty()) return true;
        const uint32_t oldIfd = read32(data.data() + 4, little);
        if (oldIfd + 2 > data.size()) return false;
        const uint16_t oldCount = read16(data.data() + oldIfd, little);
        const size_t oldEnd = static_cast<size_t>(oldIfd) + 2 + static_cast<size_t>(oldCount) * 12;
        if (oldEnd + 4 > data.size() || oldCount == std::numeric_limits<uint16_t>::max())
            return false;
        if (data.size() & 1u) data.push_back(0);
        const uint32_t newIfd = static_cast<uint32_t>(data.size());
        const uint16_t newCount = static_cast<uint16_t>(oldCount + 1);
        const size_t tableBytes = 2 + static_cast<size_t>(newCount) * 12 + 4;
        data.resize(data.size() + tableBytes, 0);
        std::vector<std::array<uint8_t, 12>> rebuilt;
        rebuilt.reserve(newCount);
        for (uint16_t i = 0; i < oldCount; ++i) {
            std::array<uint8_t, 12> existing{};
            std::memcpy(existing.data(), data.data() + oldIfd + 2 + static_cast<size_t>(i) * 12, 12);
            rebuilt.push_back(existing);
        }
        std::array<uint8_t, 12> added{};
        write16(added.data(), wanted, little);
        write16(added.data() + 2, TIFF_TYPE_UNDEFINED, little);
        write32(added.data() + 4, static_cast<uint32_t>(output.size()), little);
        const uint32_t payloadOffset = static_cast<uint32_t>(data.size());
        write32(added.data() + 8, payloadOffset, little);
        rebuilt.push_back(added);
        std::sort(rebuilt.begin(), rebuilt.end(), [little](const auto& a, const auto& b) {
            return read16(a.data(), little) < read16(b.data(), little);
        });
        write16(data.data() + newIfd, newCount, little);
        size_t destination = static_cast<size_t>(newIfd) + 2;
        for (const auto& rebuiltEntry : rebuilt) {
            std::memcpy(data.data() + destination, rebuiltEntry.data(), rebuiltEntry.size());
            destination += rebuiltEntry.size();
        }
        std::memcpy(data.data() + destination, data.data() + oldEnd, 4);
        write32(data.data() + 4, newIfd, little);
        data.insert(data.end(), output.begin(), output.end());
        return true;
    }
    if (output.size() <= 4) {
        write32(data.data() + entry->entryOffset + 4,
                static_cast<uint32_t>(output.size()), little);
        std::fill(data.begin() + entry->entryOffset + 8,
                  data.begin() + entry->entryOffset + 12, 0);
        std::copy(output.begin(), output.end(), data.begin() + entry->entryOffset + 8);
        return true;
    }
    if (data.size() & 1u) data.push_back(0);
    const uint32_t offset = static_cast<uint32_t>(data.size());
    data.insert(data.end(), output.begin(), output.end());
    write32(data.data() + entry->entryOffset + 4, static_cast<uint32_t>(output.size()), little);
    write32(data.data() + entry->entryOffset + 8, offset, little);
    return true;
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
    uint32_t packedBitDepth = 0;
    uint32_t linearizationInputMax = 0;
    uint32_t reportedWhiteLevelMax = 0;
    bool hasLinearization = false;
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
        } else if (entry.tag == TIFF_TAG_BLACK_LEVEL && entry.count) {
            metadata.blackLevelCount = std::min<uint32_t>(4, entry.count);
            for (uint32_t c = 0; c < metadata.blackLevelCount; ++c)
                metadata.blackLevel[c] = entry.type == TIFF_TYPE_RATIONAL
                    ? static_cast<float>(readRational(data, entry, c, little))
                    : static_cast<float>(entry.type == TIFF_TYPE_SHORT
                        ? read16(data.data() + entry.valueOffset + c * 2, little)
                        : read32(data.data() + entry.valueOffset + c * 4, little));
        } else if (entry.tag == TIFF_TAG_WHITE_LEVEL && entry.count) {
            metadata.whiteLevelCount = std::min<uint32_t>(4, entry.count);
            for (uint32_t c = 0; c < metadata.whiteLevelCount; ++c) {
                metadata.whiteLevel[c] = entry.type == TIFF_TYPE_RATIONAL
                    ? static_cast<float>(readRational(data, entry, c, little))
                    : static_cast<float>(entry.type == TIFF_TYPE_SHORT
                        ? read16(data.data() + entry.valueOffset + c * 2, little)
                        : read32(data.data() + entry.valueOffset + c * 4, little));
                reportedWhiteLevelMax = std::max<uint32_t>(reportedWhiteLevelMax,
                    static_cast<uint32_t>(std::clamp(
                        std::ceil(static_cast<double>(metadata.whiteLevel[c])),
                        0.0, 65535.0)));
            }
        } else if (entry.tag == TIFF_TAG_LINEARIZATION_TABLE && entry.count) {
            // The table is indexed by the stored sample code. Its highest
            // possible input is therefore count - 1, independently of the
            // post-linearization WhiteLevel values contained in the table.
            hasLinearization = true;
            linearizationInputMax = std::max(linearizationInputMax, entry.count - 1);
        } else if (entry.tag == TIFF_TAG_BITS_PER_SAMPLE && entry.count &&
                   entry.type == TIFF_TYPE_SHORT) {
            for (uint32_t i = 0; i < entry.count; ++i)
                packedBitDepth = std::max<uint32_t>(packedBitDepth,
                    read16(data.data() + entry.valueOffset + i * 2, little));
        } else if (entry.tag == TIFF_TAG_COLOR_MATRIX_1)
            readMatrix(entry, metadata.colorMatrix1, metadata.hasColorMatrix1);
        else if (entry.tag == TIFF_TAG_COLOR_MATRIX_2)
            readMatrix(entry, metadata.colorMatrix2, metadata.hasColorMatrix2);
        else if (entry.tag == TIFF_TAG_FORWARD_MATRIX_1)
            readMatrix(entry, metadata.forwardMatrix1, metadata.hasForwardMatrix1);
        else if (entry.tag == TIFF_TAG_FORWARD_MATRIX_2)
            readMatrix(entry, metadata.forwardMatrix2, metadata.hasForwardMatrix2);
    }
    auto bitsForMaximum = [](uint32_t maximum) {
        uint32_t bits = 1;
        while (bits < 16 && ((uint32_t{1} << bits) - 1) < maximum) ++bits;
        return bits;
    };
    metadata.inputBitDepth = 0;
    auto includeDepth = [&](uint32_t depth) {
        if (!depth) return;
        depth = std::min<uint32_t>(16, depth);
        metadata.inputBitDepth = metadata.inputBitDepth
            ? std::min(metadata.inputBitDepth, depth) : depth;
    };
    if (reportedWhiteLevelMax) includeDepth(bitsForMaximum(reportedWhiteLevelMax));
    includeDepth(packedBitDepth);
    if (hasLinearization) includeDepth(bitsForMaximum(linearizationInputMax));
    std::vector<std::pair<size_t, size_t>> imageRanges;
    for (const auto& offsetEntry : entries) {
        if (offsetEntry.tag != TIFF_TAG_STRIP_OFFSETS &&
            offsetEntry.tag != TIFF_TAG_TILE_OFFSETS &&
            offsetEntry.tag != TIFF_TAG_JPEG_OFFSET) continue;
        const uint16_t countTag = offsetEntry.tag == TIFF_TAG_STRIP_OFFSETS
            ? TIFF_TAG_STRIP_BYTE_COUNTS
            : offsetEntry.tag == TIFF_TAG_TILE_OFFSETS
                ? TIFF_TAG_TILE_BYTE_COUNTS : TIFF_TAG_JPEG_BYTE_COUNT;
        const TiffEntry* countEntry = nullptr;
        for (const auto& candidate : entries)
            if (candidate.ifdOffset == offsetEntry.ifdOffset && candidate.tag == countTag) {
                countEntry = &candidate;
                break;
            }
        if (!countEntry || countEntry->count != offsetEntry.count) continue;
        auto unsignedValue = [&](const TiffEntry& entry, uint32_t index) -> uint32_t {
            const size_t step = entry.type == TIFF_TYPE_SHORT ? 2 : 4;
            const size_t at = entry.valueOffset + static_cast<size_t>(index) * step;
            return entry.type == TIFF_TYPE_SHORT
                ? read16(data.data() + at, little)
                : entry.type == TIFF_TYPE_LONG ? read32(data.data() + at, little) : 0;
        };
        for (uint32_t i = 0; i < offsetEntry.count; ++i) {
            const size_t offset = unsignedValue(offsetEntry, i);
            const size_t bytes = unsignedValue(*countEntry, i);
            if (offset <= data.size() && bytes <= data.size() - offset && bytes)
                imageRanges.emplace_back(offset, offset + bytes);
        }
    }
    std::sort(imageRanges.begin(), imageRanges.end());
    size_t imageBytes = 0, end = 0;
    for (const auto& range : imageRanges) {
        if (range.second <= end) continue;
        imageBytes += range.second - std::max(end, range.first);
        end = range.second;
    }
    metadata.metadataBytes = data.size() - std::min(data.size(), imageBytes);
    return true;
}

bool DNGDecoder::getCFAMetadata(int frameNumber, int& repeatSize,
                                std::array<uint8_t, 4>& phase) {
    std::vector<uint8_t> data;
    if (!extractFrame(frameNumber, data)) return false;
    return getCFAMetadata(data, repeatSize, phase);
}

bool DNGDecoder::getCFAMetadata(const std::vector<uint8_t>& data, int& repeatSize,
                                std::array<uint8_t, 4>& phase) {
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

bool DNGDecoder::setTimingMetadata(std::vector<uint8_t>& data,
                                   double frameRate,
                                   Timestamp timestampNs) {
    if (!(frameRate > 0.0) || !std::isfinite(frameRate) || timestampNs < 0) {
        spdlog::error("Invalid DNG timing values: fps={}, timestamp={}", frameRate, timestampNs);
        return false;
    }
    bool little = true;
    auto entries = findTiffEntries(data, little);
    bool hasFrameRate = false, hasTimeCode = false, hasXmp = false;
    for (const auto& entry : entries) {
        hasFrameRate |= entry.tag == TIFF_TAG_FRAME_RATE &&
                        (entry.type == TIFF_TYPE_RATIONAL ||
                         entry.type == TIFF_TYPE_SRATIONAL) && entry.count;
        hasTimeCode |= entry.tag == TIFF_TAG_TIME_CODES && entry.type == TIFF_TYPE_BYTE && entry.count >= 8;
        hasXmp |= entry.tag == TIFF_TAG_XMP && entry.type == TIFF_TYPE_BYTE && entry.count;
    }
    const auto newXmp = relativePresentationTimestampXmp(timestampNs);
    if (!addMissingTimingEntries(data, !hasFrameRate, !hasTimeCode, !hasXmp,
                                 static_cast<uint32_t>(newXmp.size()), little)) {
        spdlog::error("Could not append DNG timing tags (bytes={}, rate={}, timecode={}, xmp={})",
                      data.size(), hasFrameRate, hasTimeCode, hasXmp);
        return false;
    }
    entries = findTiffEntries(data, little);
    const bool pairedRate = frameRate >= 47.0 && frameRate <= 61.0;
    const int nominalFps = pairedRate
        ? static_cast<int>(std::lround(frameRate / 2.0))
        : std::max(1, std::min(30, static_cast<int>(std::lround(frameRate))));
    const int64_t sourceFrame = static_cast<int64_t>(std::llround(timestampNs * frameRate / 1e9));
    const int field = pairedRate ? static_cast<int>(sourceFrame & 1) : 0;
    int64_t addressFrame = pairedRate ? sourceFrame / 2 :
        static_cast<int64_t>(std::llround(timestampNs * nominalFps / 1e9));
    const bool dropFrame = std::abs(frameRate - 29.97) < 0.02 ||
                           std::abs(frameRate - 59.94) < 0.02;
    if (dropFrame) addressFrame = dropFrameNumber(addressFrame, nominalFps);
    const int frames = static_cast<int>(addressFrame % nominalFps);
    const int64_t totalSeconds = addressFrame / nominalFps;
    uint8_t timeCode[8] = {
        static_cast<uint8_t>((toBcd(frames) & 0x3f) | (dropFrame ? 0x40 : 0)),
        static_cast<uint8_t>(toBcd(static_cast<int>(totalSeconds % 60)) & 0x7f),
        static_cast<uint8_t>(toBcd(static_cast<int>((totalSeconds / 60) % 60)) & 0x7f),
        static_cast<uint8_t>(toBcd(static_cast<int>((totalSeconds / 3600) % 24)) & 0x3f),
        0, 0, 0, 0
    };
    if (field) {
        if (nominalFps == 25) timeCode[3] |= 0x80;
        else timeCode[1] |= 0x80;
    }
    bool frameRateWritten = false, timeCodeWritten = false, timestampWritten = false;
    for (const auto& entry : entries) {
        if (entry.tag == TIFF_TAG_FRAME_RATE &&
            (entry.type == TIFF_TYPE_RATIONAL || entry.type == TIFF_TYPE_SRATIONAL) &&
            entry.count) {
            writeRational(data, entry, 0, frameRate, little);
            // DNG defines FrameRate as SRATIONAL. Normalize files produced by
            // older builds, which incorrectly emitted the same 8-byte value as
            // unsigned RATIONAL.
            write16(data.data() + entry.entryOffset + 2, TIFF_TYPE_SRATIONAL, little);
            frameRateWritten = true;
        } else if (entry.tag == TIFF_TAG_TIME_CODES && entry.type == TIFF_TYPE_BYTE && entry.count >= 8) {
            std::memcpy(data.data() + entry.valueOffset, timeCode, sizeof(timeCode));
            timeCodeWritten = true;
        } else if (entry.tag == TIFF_TAG_XMP && entry.type == TIFF_TYPE_BYTE && entry.count) {
            if (!hasXmp && entry.count == newXmp.size()) {
                std::memcpy(data.data() + entry.valueOffset, newXmp.data(), newXmp.size());
                timestampWritten = true;
            } else {
                timestampWritten = writeRelativePresentationTimestamp(data, entry, timestampNs, little);
            }
        }
    }
    if (!frameRateWritten || !timeCodeWritten || !timestampWritten)
        spdlog::error("Incomplete DNG timing write (rate={}, timecode={}, timestamp={}, entries={})",
                      frameRateWritten, timeCodeWritten, timestampWritten, entries.size());
    return frameRateWritten && timeCodeWritten && timestampWritten;
}

bool DNGDecoder::getTimingMetadata(const std::vector<uint8_t>& data,
                                   Timestamp& timestampNs) {
    bool little = true;
    bool found = false;
    for (const auto& entry : findTiffEntries(data, little))
        if (readRelativePresentationTimestamp(data, entry, timestampNs)) found = true;
    return found;
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
                                  bool higherCfaHq,
                                  bool nearestNeighborDemosaic) {
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
        if (value == TIFF_PHOTOMETRIC_CFA ||
            ((remosaic || proxy) && (value == 2 || value == 34892))) {
            photo = &e;
            sourceIsRgb = value != TIFF_PHOTOMETRIC_CFA;
            break;
        }
    }
    if (!photo) return false;
    // Ordinary Bayer is normally left untouched, but callers may request this
    // routine when an RGB staging image is required.
    auto find = [&](uint16_t tag) -> const TiffEntry* {
        for (const auto& e : entries) if (e.ifdOffset == photo->ifdOffset && e.tag == tag) return &e;
        return nullptr;
    };
    auto writeEntryValues = [&](const TiffEntry& entry,
                                std::initializer_list<double> values) {
        if (entry.count < values.size()) return false;
        uint32_t index = 0;
        for (double value : values) {
            const size_t elementSize = entry.type == TIFF_TYPE_SHORT ? 2u : 4u;
            const size_t position = entry.valueOffset + static_cast<size_t>(index) * elementSize;
            if (entry.type == TIFF_TYPE_SHORT) {
                if (position + 2 > data.size()) return false;
                write16(data.data() + position, static_cast<uint16_t>(std::lround(value)), little);
            } else if (entry.type == TIFF_TYPE_LONG) {
                if (position + 4 > data.size()) return false;
                write32(data.data() + position, static_cast<uint32_t>(std::llround(value)), little);
            } else if (entry.type == TIFF_TYPE_RATIONAL) {
                writeRational(data, entry, index, value, little);
            } else {
                return false;
            }
            ++index;
        }
        return true;
    };
    auto entryValue = [&](const TiffEntry& entry, uint32_t index) -> double {
        if (index >= entry.count) return 0.0;
        const size_t position = entry.valueOffset + static_cast<size_t>(index) *
            (entry.type == TIFF_TYPE_SHORT ? 2u : entry.type == TIFF_TYPE_LONG ? 4u : 8u);
        if (entry.type == TIFF_TYPE_SHORT) return read16(data.data() + position, little);
        if (entry.type == TIFF_TYPE_LONG) return read32(data.data() + position, little);
        if (entry.type == TIFF_TYPE_RATIONAL) return readRational(data, entry, index, little);
        return 0.0;
    };
    auto updateGeometryMetadata = [&](uint32_t sourceWidth, uint32_t sourceHeight,
                                      uint32_t imageWidth, uint32_t imageHeight) {
        if (const auto rows = find(TIFF_TAG_ROWS_PER_STRIP)) {
            if (!writeEntryValues(*rows, {static_cast<double>(imageHeight)})) return false;
        }
        if (const auto active = find(TIFF_TAG_ACTIVE_AREA)) {
            const double scaleX = static_cast<double>(imageWidth) / sourceWidth;
            const double scaleY = static_cast<double>(imageHeight) / sourceHeight;
            const double top = std::floor(entryValue(*active, 0) * scaleY);
            const double left = std::floor(entryValue(*active, 1) * scaleX);
            const double bottom = std::min<double>(imageHeight,
                std::ceil(entryValue(*active, 2) * scaleY));
            const double right = std::min<double>(imageWidth,
                std::ceil(entryValue(*active, 3) * scaleX));
            if (!writeEntryValues(*active, {top, left, bottom, right})) return false;
        }
        if (const auto origin = find(TIFF_TAG_DEFAULT_CROP_ORIGIN)) {
            const double scaleX = static_cast<double>(imageWidth) / sourceWidth;
            const double scaleY = static_cast<double>(imageHeight) / sourceHeight;
            if (!writeEntryValues(*origin,
                    {entryValue(*origin, 0) * scaleX,
                     entryValue(*origin, 1) * scaleY})) return false;
        }
        if (const auto size = find(TIFF_TAG_DEFAULT_CROP_SIZE)) {
            const double scaleX = static_cast<double>(imageWidth) / sourceWidth;
            const double scaleY = static_cast<double>(imageHeight) / sourceHeight;
            if (!writeEntryValues(*size,
                    {entryValue(*size, 0) * scaleX,
                     entryValue(*size, 1) * scaleY})) return false;
        }
        return true;
    };
    const TiffEntry* dimE = find(sourceIsRgb ? 65000 : TIFF_TAG_CFA_REPEAT_PATTERN_DIM);
    const TiffEntry* patternE = find(sourceIsRgb ? 65001 : TIFF_TAG_CFA_PATTERN);
    auto writeHigherCfaMetadata = [&]() {
        if (!dimE || !patternE || repeatSize < 2 || (repeatSize & 1) ||
            dimE->type != TIFF_TYPE_SHORT || dimE->count < 2 ||
            patternE->type != TIFF_TYPE_BYTE) return false;
        const uint64_t patternBytes64 = static_cast<uint64_t>(repeatSize) * repeatSize;
        if (patternBytes64 > std::numeric_limits<uint32_t>::max()) return false;
        const uint32_t patternBytes = static_cast<uint32_t>(patternBytes64);
        std::vector<uint8_t> pattern(patternBytes);
        const int block = repeatSize / 2;
        for (int y = 0; y < repeatSize; ++y)
            for (int x = 0; x < repeatSize; ++x)
                pattern[static_cast<size_t>(y) * repeatSize + x] =
                    phase[static_cast<size_t>(y / block) * 2 + x / block];

        write16(data.data() + dimE->valueOffset, static_cast<uint16_t>(repeatSize), little);
        write16(data.data() + dimE->valueOffset + 2, static_cast<uint16_t>(repeatSize), little);
        const size_t patternEntry = patternE->entryOffset;
        write32(data.data() + patternEntry + 4, patternBytes, little);
        if (patternBytes <= 4) {
            std::fill_n(data.data() + patternEntry + 8, 4, 0);
            std::copy(pattern.begin(), pattern.end(), data.data() + patternEntry + 8);
        } else {
            if (data.size() & 1u) data.push_back(0);
            if (data.size() > std::numeric_limits<uint32_t>::max() - patternBytes) return false;
            const uint32_t patternOffset = static_cast<uint32_t>(data.size());
            data.insert(data.end(), pattern.begin(), pattern.end());
            write32(data.data() + patternEntry + 8, patternOffset, little);
        }
        return true;
    };
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
        uint32_t width = scalar(*widthE), height = scalar(*heightE);
        const uint32_t sourceWidth = width, sourceHeight = height;
        const uint32_t stripOffset = scalar(*offsetsE), stripBytes = scalar(*countsE);
        const size_t samples = static_cast<size_t>(width) * height * 3;
        if (!width || !height || stripOffset > data.size() ||
            stripBytes < samples * sizeof(uint16_t) || stripBytes > data.size() - stripOffset)
            return false;
        std::vector<uint16_t> rgb(samples);
        for (size_t i = 0; i < samples; ++i)
            rgb[i] = read16(data.data() + stripOffset + i * 2, little);
        if (proxy) {
            std::vector<uint16_t> reduced;
            uint32_t reducedWidth = 0, reducedHeight = 0;
            utils::reduceRGB(rgb, reduced, width, height,
                             static_cast<uint32_t>(proxyScale), higherCfaHq,
                             reducedWidth, reducedHeight);
            if (reduced.empty()) return false;
            rgb = std::move(reduced);
            width = reducedWidth;
            height = reducedHeight;
        }
        auto setScalar = [&](const TiffEntry& e, uint32_t value) {
            if (e.type == TIFF_TYPE_SHORT) write16(data.data() + e.valueOffset, value, little);
            else write32(data.data() + e.valueOffset, value, little);
        };
        setScalar(*widthE, width);
        setScalar(*heightE, height);
        if (!updateGeometryMetadata(sourceWidth, sourceHeight, width, height)) return false;
        if (!remosaic) {
            std::vector<uint8_t> rgbBytes(rgb.size() * sizeof(uint16_t));
            for (size_t i = 0; i < rgb.size(); ++i) {
                rgbBytes[i * 2] = little ? rgb[i] & 0xff : rgb[i] >> 8;
                rgbBytes[i * 2 + 1] = little ? rgb[i] >> 8 : rgb[i] & 0xff;
            }
            setScalar(*countsE, static_cast<uint32_t>(rgbBytes.size()));
            return replaceTiffStrip(data, stripOffset, stripBytes, rgbBytes, little);
        }
        std::vector<uint16_t> bayer;
        remosaicCFA(rgb, bayer, width, height, phase);
        const uint32_t newBytes = static_cast<uint32_t>(bayer.size() * sizeof(uint16_t));
        std::vector<uint8_t> bayerBytes(newBytes);
        for (size_t i = 0; i < bayer.size(); ++i) {
            bayerBytes[i * 2] = little ? bayer[i] & 0xff : bayer[i] >> 8;
            bayerBytes[i * 2 + 1] = little ? bayer[i] >> 8 : bayer[i] & 0xff;
        }
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
    // A 2x2 CFA is already ordinary Bayer. Demosaicing and remosaicing it is
    // both lossy and unnecessary, and previously routed this no-op option
    // through the higher-CFA interpolation path.
    if (repeatSize == 2 && remosaic && !proxy) return true;
    if (!proxy && mode == QuadBayerMode::CorrectQBCFAMetadata)
        return writeHigherCfaMetadata();
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
    uint32_t width = scalar(*widthE), height = scalar(*heightE);
    const uint32_t sourceWidth = width, sourceHeight = height;
    const uint32_t bits = scalar(*bitsE);
    const uint32_t compression = scalar(*compressionE), stripOffset = scalar(*offsetsE);
    const uint32_t stripBytes = scalar(*countsE);
    if (!width || !height || bits < 8 || bits > 16 || stripOffset > data.size() ||
        stripBytes > data.size() - stripOffset) return false;
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
        if (bits == 16) {
            if (stripBytes < pixels.size() * 2) return false;
            for (size_t i = 0; i < pixels.size(); ++i)
                pixels[i] = read16(data.data() + stripOffset + i * 2, little);
        } else {
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

        if (nearestNeighborDemosaic) {
            rgb.resize(static_cast<size_t>(imageWidth) * imageHeight * 3);
            const int block = std::max(1, imageRepeatSize / 2);
            auto colorAt = [&](int x, int y) {
                const int phaseX = (x % imageRepeatSize) / block;
                const int phaseY = (y % imageRepeatSize) / block;
                return phase[static_cast<size_t>(phaseY) * 2 + phaseX];
            };
            using Offset = std::pair<int16_t, int16_t>;
            std::vector<std::vector<Offset>> nearestOffsets(
                static_cast<size_t>(imageRepeatSize) * imageRepeatSize * 3);
            for (int phaseY = 0; phaseY < imageRepeatSize; ++phaseY) {
                for (int phaseX = 0; phaseX < imageRepeatSize; ++phaseX) {
                    for (int channel = 0; channel < 3; ++channel) {
                        auto& candidates = nearestOffsets[
                            (static_cast<size_t>(phaseY) * imageRepeatSize + phaseX) * 3 + channel];
                        for (int dy = -imageRepeatSize; dy <= imageRepeatSize; ++dy) {
                            for (int dx = -imageRepeatSize; dx <= imageRepeatSize; ++dx) {
                                int sampleX = (phaseX + dx) % imageRepeatSize;
                                int sampleY = (phaseY + dy) % imageRepeatSize;
                                if (sampleX < 0) sampleX += imageRepeatSize;
                                if (sampleY < 0) sampleY += imageRepeatSize;
                                if (colorAt(sampleX, sampleY) == channel)
                                    candidates.emplace_back(dx, dy);
                            }
                        }
                        std::stable_sort(candidates.begin(), candidates.end(),
                            [](const Offset& left, const Offset& right) {
                                const int leftDistance = left.first * left.first + left.second * left.second;
                                const int rightDistance = right.first * right.first + right.second * right.second;
                                return leftDistance < rightDistance;
                            });
                    }
                }
            }
            for (int y = 0; y < static_cast<int>(imageHeight); ++y) {
                for (int x = 0; x < static_cast<int>(imageWidth); ++x) {
                    const size_t destination =
                        (static_cast<size_t>(y) * imageWidth + x) * 3;
                    for (int channel = 0; channel < 3; ++channel) {
                        const auto& candidates = nearestOffsets[
                            (static_cast<size_t>(y % imageRepeatSize) * imageRepeatSize +
                             x % imageRepeatSize) * 3 + channel];
                        for (const auto& [dx, dy] : candidates) {
                            const int sourceX = x + dx;
                            const int sourceY = y + dy;
                            if (sourceX < 0 || sourceX >= static_cast<int>(imageWidth) ||
                                sourceY < 0 || sourceY >= static_cast<int>(imageHeight)) continue;
                            rgb[destination + channel] =
                                input[static_cast<size_t>(sourceY) * imageWidth + sourceX];
                            break;
                        }
                    }
                }
            }
        } else {
            utils::demosaicHigherCFA(
                input, rgb, imageWidth, imageHeight, imageRepeatSize, phase, mode,
                {static_cast<float>(outputChannelBlack[0]),
                 static_cast<float>(outputChannelBlack[1]),
                 static_cast<float>(outputChannelBlack[2])});
        }
        rgbOutput = !remosaic;
    };
    const bool explicitBinning = mode == QuadBayerMode::Binning ||
                                 mode == QuadBayerMode::Bin8x8To4x4;
    if (explicitBinning && repeatSize > 2) {
        const uint32_t factor = mode == QuadBayerMode::Bin8x8To4x4 && repeatSize == 8
            ? 2u : static_cast<uint32_t>(repeatSize / 2);
        std::vector<uint16_t> binned;
        utils::binHigherCFA(pixels, binned, width, height, factor,
                            outputWidth, outputHeight);
        if (binned.empty()) return false;
        pixels = std::move(binned);
        width = outputWidth;
        height = outputHeight;
        repeatSize /= static_cast<int>(factor);
    }
    if (explicitBinning && !proxy) {
        output = pixels;
        outputWidth = width;
        outputHeight = height;
        remosaic = true;
    } else if (proxy && higherCfaHq) {
        std::vector<uint16_t> fullRgb;
        demosaicWithRgbBlackMetadata(pixels, fullRgb, width, height, repeatSize, 1.0);
        std::vector<uint16_t> reducedRgb;
        utils::reduceRGB(fullRgb, reducedRgb, width, height,
                         static_cast<uint32_t>(proxyScale), true,
                         outputWidth, outputHeight);
        if (reducedRgb.empty()) return false;
        if (remosaic) remosaicCFA(reducedRgb, output, outputWidth, outputHeight, phase);
        else output = std::move(reducedRgb);
        // Averaging preserves the numeric black/white levels.
        hqReductionArea = 1;
        hqReductionShift = 0;
    } else if (proxy) {
        const uint32_t group = repeatSize / 2;
        const uint32_t reductionGroup = group;
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
        const uint32_t sourceScale = reductionGroup * std::max(1u,
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
        remosaic = true; // Complete CFA-block reduction is ordinary 2x2 Bayer.
    } else {
        std::vector<uint16_t> rgb;
        demosaicWithRgbBlackMetadata(pixels, rgb, width, height, repeatSize, 1.0);
        if (remosaic) remosaicCFA(rgb, output, width, height, phase);
        else output = std::move(rgb);
    }

    const uint32_t newBytes = static_cast<uint32_t>(output.size() * sizeof(uint16_t));
    std::vector<uint8_t> outputBytes(newBytes);
    for (size_t i = 0; i < output.size(); ++i) {
        outputBytes[i * 2] = little ? output[i] & 0xff : output[i] >> 8;
        outputBytes[i * 2 + 1] = little ? output[i] >> 8 : output[i] & 0xff;
    }
    auto setScalar = [&](const TiffEntry& e, uint32_t value) {
        if (e.type == TIFF_TYPE_SHORT) write16(data.data() + e.valueOffset, value, little);
        else write32(data.data() + e.valueOffset, value, little);
    };
    setScalar(*compressionE, TIFF_COMPRESSION_NONE); setScalar(*offsetsE, stripOffset);
    setScalar(*countsE, newBytes); setScalar(*sppE, remosaic ? 1 : 3);
    setScalar(*photo, remosaic ? TIFF_PHOTOMETRIC_CFA : 34892); // LinearRaw
    setScalar(*widthE, outputWidth); setScalar(*heightE, outputHeight);
    if (!updateGeometryMetadata(sourceWidth, sourceHeight, outputWidth, outputHeight)) return false;
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
        if (explicitBinning && !proxy && repeatSize > 2) {
            if (!writeHigherCfaMetadata()) return false;
        } else {
            write32(data.data() + dimE->entryOffset + 4, 2, little);
            write16(data.data() + dimE->entryOffset + 8, 2, little);
            write16(data.data() + dimE->entryOffset + 10, 2, little);
            write32(data.data() + patternE->entryOffset + 4, 4, little);
            for (size_t i = 0; i < 4; ++i) data[patternE->entryOffset + 8 + i] = phase[i];
        }
    }
    return replaceTiffStrip(data, stripOffset, stripBytes, outputBytes, little);
}

bool DNGDecoder::ensureUncompressed(std::vector<uint8_t>& data, bool backgroundWork) {
    const auto ensureStarted = std::chrono::steady_clock::now();
    if (backgroundWork) yieldToForegroundDngWork();
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
    // Compression and SamplesPerPixel have TIFF defaults of 1. Some valid DNG
    // writers omit them; materialize the defaults because later processing
    // stages intentionally operate on an explicit canonical IFD.
    if (!find(TIFF_TAG_COMPRESSION)) {
        if (!insertTiffScalarEntry(data, photo->ifdOffset, TIFF_TAG_COMPRESSION,
                                   TIFF_TYPE_SHORT, TIFF_COMPRESSION_NONE, little))
            return false;
        return ensureUncompressed(data, backgroundWork);
    }
    if (!find(TIFF_TAG_SAMPLES_PER_PIXEL)) {
        if (!insertTiffScalarEntry(data, photo->ifdOffset, TIFF_TAG_SAMPLES_PER_PIXEL,
                                   TIFF_TYPE_SHORT, 1, little))
            return false;
        return ensureUncompressed(data, backgroundWork);
    }
    const auto widthE = find(TIFF_TAG_IMAGE_WIDTH), heightE = find(TIFF_TAG_IMAGE_HEIGHT);
    const auto bitsE = find(TIFF_TAG_BITS_PER_SAMPLE), compressionE = find(TIFF_TAG_COMPRESSION);
    const auto stripOffsetsE = find(TIFF_TAG_STRIP_OFFSETS);
    const auto stripCountsE = find(TIFF_TAG_STRIP_BYTE_COUNTS);
    const auto tileOffsetsE = find(TIFF_TAG_TILE_OFFSETS);
    const auto tileCountsE = find(TIFF_TAG_TILE_BYTE_COUNTS);
    const auto sppE = find(TIFF_TAG_SAMPLES_PER_PIXEL);
    if (!widthE || !heightE || !bitsE || !compressionE || !sppE) return false;
    if ((stripOffsetsE || stripCountsE) && !find(TIFF_TAG_ROWS_PER_STRIP)) {
        if (!insertTiffScalarEntry(data, photo->ifdOffset, TIFF_TAG_ROWS_PER_STRIP,
                                   TIFF_TYPE_LONG, scalar(*heightE), little))
            return false;
        return ensureUncompressed(data, backgroundWork);
    }

    const uint32_t compression = scalar(*compressionE);
    const uint32_t width = scalar(*widthE), height = scalar(*heightE);
    const uint32_t bits = scalar(*bitsE), channels = scalar(*sppE);
    const auto planarE = find(TIFF_TAG_PLANAR_CONFIGURATION);
    const uint32_t planar = planarE ? scalar(*planarE) : 1;
    std::atomic_uint32_t turboAttempts{0}, turboSuccesses{0}, legacySuccesses{0};
    if (!width || !height || !channels || channels > 4 || bits < 8 || bits > 16)
        return false;
    if (planar != 1 && planar != 2) return false;
    size_t imageSamples = 0;
    if (!checkedImageSamples(width, height, channels, imageSamples) ||
        imageSamples > std::numeric_limits<uint32_t>::max() / sizeof(uint16_t) ||
        width > static_cast<uint32_t>(std::numeric_limits<int>::max()) / channels)
        return false;
    auto setScalar = [&](const TiffEntry& entry, uint32_t value) {
        if (entry.type == TIFF_TYPE_SHORT)
            write16(data.data() + entry.valueOffset, static_cast<uint16_t>(value), little);
        else
            write32(data.data() + entry.valueOffset, value, little);
    };

    auto arrayValue = [&](const TiffEntry& entry, uint32_t index,
                          uint32_t& value) -> bool {
        if (index >= entry.count ||
            (entry.type != TIFF_TYPE_SHORT && entry.type != TIFF_TYPE_LONG)) return false;
        const size_t step = entry.type == TIFF_TYPE_SHORT ? 2 : 4;
        const size_t at = entry.valueOffset + static_cast<size_t>(index) * step;
        if (at > data.size() || step > data.size() - at) return false;
        value = entry.type == TIFF_TYPE_SHORT
            ? read16(data.data() + at, little) : read32(data.data() + at, little);
        return true;
    };
    auto decodeChunk = [&](uint32_t offset, uint32_t byteCount,
                           uint32_t chunkWidth, uint32_t chunkHeight,
                           uint32_t chunkChannels, std::vector<uint16_t>& output,
                           uint32_t& decodedWidth, uint32_t& decodedHeight) -> bool {
        if (!chunkWidth || !chunkHeight || !chunkChannels ||
            offset > data.size() || byteCount > data.size() - offset) return false;
        decodedWidth = chunkWidth; decodedHeight = chunkHeight;
        if (compression == TIFF_COMPRESSION_NONE) {
            const uint64_t rowBits = static_cast<uint64_t>(chunkWidth) * chunkChannels * bits;
            const uint64_t rowBytes = (rowBits + 7) / 8;
            if (rowBytes > byteCount / chunkHeight ||
                chunkWidth > std::numeric_limits<size_t>::max() / chunkChannels)
                return false;
            const size_t rowSamples = static_cast<size_t>(chunkWidth) * chunkChannels;
            if (rowSamples > std::numeric_limits<size_t>::max() / chunkHeight) return false;
            output.assign(rowSamples * chunkHeight, 0);
            for (uint32_t y = 0; y < chunkHeight; ++y) {
                size_t bit = static_cast<size_t>(static_cast<uint64_t>(y) * rowBytes) * 8;
                for (size_t x = 0; x < rowSamples; ++x) {
                    uint16_t value = 0;
                    if (bits == 16) {
                        value = read16(data.data() + offset + bit / 8, little);
                        bit += 16;
                    } else {
                        for (uint32_t b = 0; b < bits; ++b, ++bit)
                            value = static_cast<uint16_t>((value << 1) |
                                ((data[offset + bit / 8] >> (7 - bit % 8)) & 1));
                    }
                    output[static_cast<size_t>(y) * chunkWidth * chunkChannels + x] = value;
                }
            }
            return true;
        }
        if (compression == TIFF_COMPRESSION_JPEG_XL) {
            return bits == 16 && decodeJPEGXL(data.data() + offset, byteCount,
                                               chunkWidth, chunkHeight,
                                               chunkChannels, output);
        }
        if (compression == TIFF_COMPRESSION_LOSSY_JPEG) {
            return bits == 8 && decodeDctJPEG(data.data() + offset, byteCount,
                                              chunkWidth, chunkHeight,
                                              chunkChannels, output);
        }
        if (compression != TIFF_COMPRESSION_JPEG) return false;
        turboAttempts.fetch_add(1, std::memory_order_relaxed);
        if (decodeLosslessJPEG(data.data() + offset, byteCount, chunkWidth,
                               chunkHeight, chunkChannels, output)) {
            turboSuccesses.fetch_add(1, std::memory_order_relaxed);
            return true;
        }
        lj92 decoder = nullptr;
        int dw = 0, dh = 0, db = 0, components = 0;
        const int openResult = lj92_open(&decoder, data.data() + offset, byteCount,
                                         &dw, &dh, &db, &components);
        if (openResult != LJ92_ERROR_NONE) {
            return (bits == 8 || bits == 12) &&
                decodeDctJPEG(data.data() + offset, byteCount, chunkWidth,
                              chunkHeight, chunkChannels, output);
        }
        const uint64_t rowSamples = static_cast<uint64_t>(std::max(0, dw)) *
                                    static_cast<uint32_t>(std::max(0, components));
        const uint64_t pixelWidth = chunkChannels && rowSamples % chunkChannels == 0
            ? rowSamples / chunkChannels : 0;
        if (dw <= 0 || dh <= 0 || components <= 0 || components > 4 ||
            db != static_cast<int>(bits) || !pixelWidth ||
            pixelWidth < chunkWidth || dh < static_cast<int>(chunkHeight) ||
            static_cast<uint64_t>(dw) * dh * components >
                std::numeric_limits<size_t>::max() / sizeof(uint16_t)) {
            lj92_close(decoder); return false;
        }
        std::vector<uint16_t> encodedPixels(static_cast<size_t>(dw) * dh * components);
        const bool ok = lj92_decode(decoder, encodedPixels.data(), dw * components,
                                    0, nullptr, 0) == LJ92_ERROR_NONE;
        lj92_close(decoder);
        if (!ok) return false;
        legacySuccesses.fetch_add(1, std::memory_order_relaxed);
        decodedWidth = static_cast<uint32_t>(pixelWidth);
        decodedHeight = static_cast<uint32_t>(dh);
        output = std::move(encodedPixels);
        return true;
    };
    auto makeLongScalar = [&](const TiffEntry& entry, uint16_t tag, uint32_t value) {
        write16(data.data() + entry.entryOffset, tag, little);
        write16(data.data() + entry.entryOffset + 2, TIFF_TYPE_LONG, little);
        write32(data.data() + entry.entryOffset + 4, 1, little);
        write32(data.data() + entry.entryOffset + 8, value, little);
    };
    auto finishCanonical = [&](std::vector<uint16_t>& pixels,
                               const TiffEntry& offsets, const TiffEntry& counts,
                               const TiffEntry& rows) -> bool {
        const size_t byteCount = pixels.size() * sizeof(uint16_t);
        if (byteCount > std::numeric_limits<uint32_t>::max() ||
            data.size() > std::numeric_limits<uint32_t>::max() - byteCount) return false;
        if (data.size() & 1u) {
            if (data.size() == std::numeric_limits<uint32_t>::max() - byteCount)
                return false;
            data.push_back(0);
        }
        const uint32_t offset = static_cast<uint32_t>(data.size());
        data.resize(data.size() + byteCount);
        for (size_t i = 0; i < pixels.size(); ++i) {
            data[offset + i * 2] = little ? pixels[i] & 0xff : pixels[i] >> 8;
            data[offset + i * 2 + 1] = little ? pixels[i] >> 8 : pixels[i] & 0xff;
        }
        makeLongScalar(offsets, TIFF_TAG_STRIP_OFFSETS, offset);
        makeLongScalar(counts, TIFF_TAG_STRIP_BYTE_COUNTS,
                       static_cast<uint32_t>(byteCount));
        makeLongScalar(rows, TIFF_TAG_ROWS_PER_STRIP, height);
        for (uint32_t i = 0; i < bitsE->count; ++i) {
            const size_t at = bitsE->valueOffset + static_cast<size_t>(i) *
                (bitsE->type == TIFF_TYPE_SHORT ? 2u : 4u);
            if (bitsE->type == TIFF_TYPE_SHORT) write16(data.data() + at, 16, little);
            else if (bitsE->type == TIFF_TYPE_LONG) write32(data.data() + at, 16, little);
            else return false;
        }
        setScalar(*compressionE, TIFF_COMPRESSION_NONE);
        if (planarE) setScalar(*planarE, 1);
        return sortTiffIfdEntries(data, photo->ifdOffset, little);
    };
    auto discardSourceChunks = [&](std::vector<std::pair<uint32_t, uint32_t>> chunks) {
        chunks.erase(std::remove_if(chunks.begin(), chunks.end(), [](const auto& chunk) {
            return chunk.second == 0;
        }), chunks.end());
        std::sort(chunks.begin(), chunks.end());
        std::vector<std::pair<uint32_t, uint32_t>> ranges;
        for (const auto& [offset, bytes] : chunks) {
            const uint64_t end = static_cast<uint64_t>(offset) + bytes;
            if (end > data.size()) return false;
            if (!ranges.empty()) {
                const uint64_t previousEnd = static_cast<uint64_t>(ranges.back().first) +
                                             ranges.back().second;
                if (offset <= previousEnd) {
                    ranges.back().second = static_cast<uint32_t>(
                        std::max<uint64_t>(previousEnd, end) - ranges.back().first);
                    continue;
                }
            }
            ranges.emplace_back(offset, bytes);
        }
        // Tile payloads written by DNG encoders are normally one contiguous
        // run. Coalescing first turns thousands of vector erases and complete
        // TIFF rescans into a single relocation operation.
        if (!removeTiffRanges(data, ranges, little)) return false;
        spdlog::info("DNG decode trace: coalesced {} source chunks into {} removal range(s)",
                     chunks.size(), ranges.size());
        return true;
    };

    // A single chunky, uncompressed strip is already the canonical topology.
    if (!tileOffsetsE && !tileCountsE && stripOffsetsE && stripCountsE &&
        stripOffsetsE->count == 1 && stripCountsE->count == 1 &&
        compression == TIFF_COMPRESSION_NONE && planar == 1) return true;

    const auto parsedAt = std::chrono::steady_clock::now();
    std::vector<uint16_t> pixels(imageSamples, 0);
    const auto allocatedAt = std::chrono::steady_clock::now();
    if (tileOffsetsE || tileCountsE) {
        const auto tileWidthE = find(TIFF_TAG_TILE_WIDTH);
        const auto tileHeightE = find(TIFF_TAG_TILE_LENGTH);
        if (!tileOffsetsE || !tileCountsE || !tileWidthE || !tileHeightE ||
            tileOffsetsE->count != tileCountsE->count || !tileOffsetsE->count) return false;
        const uint32_t tileWidth = scalar(*tileWidthE), tileHeight = scalar(*tileHeightE);
        if (!tileWidth || !tileHeight) return false;
        const uint64_t across64 = (static_cast<uint64_t>(width) + tileWidth - 1) / tileWidth;
        const uint64_t down64 = (static_cast<uint64_t>(height) + tileHeight - 1) / tileHeight;
        if (!across64 || !down64 || across64 > std::numeric_limits<uint32_t>::max() ||
            across64 > std::numeric_limits<uint64_t>::max() / down64) return false;
        const uint64_t tilesPerPlane64 = across64 * down64;
        if (tilesPerPlane64 > std::numeric_limits<uint64_t>::max() /
                (planar == 2 ? channels : 1)) return false;
        const uint64_t expectedCount64 = tilesPerPlane64 * (planar == 2 ? channels : 1);
        if (
            tilesPerPlane64 > std::numeric_limits<uint32_t>::max() ||
            expectedCount64 > std::numeric_limits<uint32_t>::max()) return false;
        const uint32_t across = static_cast<uint32_t>(across64);
        const uint32_t tilesPerPlane = static_cast<uint32_t>(tilesPerPlane64);
        const uint32_t expectedCount = static_cast<uint32_t>(expectedCount64);
        if (tileOffsetsE->count != expectedCount) return false;
        std::vector<std::pair<uint32_t, uint32_t>> sourceChunks;
        sourceChunks.reserve(expectedCount);
        for (uint32_t index = 0; index < expectedCount; ++index) {
            uint32_t offset = 0, byteCount = 0;
            if (!arrayValue(*tileOffsetsE, index, offset) ||
                !arrayValue(*tileCountsE, index, byteCount)) return false;
            sourceChunks.emplace_back(offset, byteCount);
        }
        std::atomic_uint32_t nextTile{0};
        std::atomic_bool decodeFailed{false};
        const unsigned workerCount = std::min<unsigned>(
            expectedCount, std::min(8u, std::max(1u, std::thread::hardware_concurrency())));
        auto decodeTiles = [&] {
            while (!decodeFailed.load(std::memory_order_relaxed)) {
                if (backgroundWork) yieldToForegroundDngWork();
                const uint32_t index = nextTile.fetch_add(1, std::memory_order_relaxed);
                if (index >= expectedCount) return;
                const auto [offset, byteCount] = sourceChunks[index];
                const uint32_t plane = planar == 2 ? index / tilesPerPlane : 0;
            const uint32_t spatial = planar == 2 ? index % tilesPerPlane : index;
            const uint32_t originX = (spatial % across) * tileWidth;
            const uint32_t originY = (spatial / across) * tileHeight;
            const uint32_t copyWidth = std::min(tileWidth, width - originX);
            const uint32_t copyHeight = std::min(tileHeight, height - originY);
            const uint32_t chunkChannels = planar == 2 ? 1 : channels;
            std::vector<uint16_t> tile;
            uint32_t decodedWidth = 0, decodedHeight = 0;
            const bool padded = compression == TIFF_COMPRESSION_NONE;
            bool decoded = decodeChunk(offset, byteCount,
                padded ? tileWidth : copyWidth, padded ? tileHeight : copyHeight,
                chunkChannels, tile, decodedWidth, decodedHeight);
            if (!decoded && compression != TIFF_COMPRESSION_NONE &&
                (copyWidth != tileWidth || copyHeight != tileHeight))
                decoded = decodeChunk(offset, byteCount, tileWidth, tileHeight,
                                      chunkChannels, tile, decodedWidth, decodedHeight);
                if (!decoded || decodedWidth < copyWidth || decodedHeight < copyHeight) {
                    decodeFailed.store(true, std::memory_order_relaxed);
                    return;
                }
            for (uint32_t y = 0; y < copyHeight; ++y)
                for (uint32_t x = 0; x < copyWidth; ++x)
                    for (uint32_t c = 0; c < chunkChannels; ++c) {
                        const uint32_t destinationChannel = planar == 2 ? plane : c;
                        pixels[(static_cast<size_t>(originY + y) * width + originX + x) *
                            channels + destinationChannel] =
                            tile[(static_cast<size_t>(y) * decodedWidth + x) * chunkChannels + c];
                    }
            }
        };
        std::vector<std::thread> workers;
        workers.reserve(workerCount);
        for (unsigned i = 0; i < workerCount; ++i) workers.emplace_back(decodeTiles);
        for (auto& worker : workers) worker.join();
        if (decodeFailed.load(std::memory_order_relaxed)) return false;
        const auto decodedAt = std::chrono::steady_clock::now();
        // Repurpose the four tile entries so no standard tile tag survives.
        write16(data.data() + tileHeightE->entryOffset, TIFF_TAG_UNUSED_TILE_LENGTH, little);
        if (!finishCanonical(pixels, *tileOffsetsE, *tileCountsE, *tileWidthE)) return false;
        const auto canonicalAt = std::chrono::steady_clock::now();
        if (!discardSourceChunks(std::move(sourceChunks))) return false;
        const auto completedAt = std::chrono::steady_clock::now();
        spdlog::info(
            "DNG decode trace: {}x{} bits={} channels={} tiles={} tile={}x{} "
            "workers={} background={} turbo={}/{} legacy={} parse={:.1f} ms "
            "allocate={:.1f} ms decode={:.1f} ms canonical={:.1f} ms discard={:.1f} ms total={:.1f} ms",
            width, height, bits, channels, expectedCount, tileWidth, tileHeight,
            workerCount, backgroundWork, turboSuccesses.load(), turboAttempts.load(),
            legacySuccesses.load(),
            std::chrono::duration<double, std::milli>(parsedAt - ensureStarted).count(),
            std::chrono::duration<double, std::milli>(allocatedAt - parsedAt).count(),
            std::chrono::duration<double, std::milli>(decodedAt - allocatedAt).count(),
            std::chrono::duration<double, std::milli>(canonicalAt - decodedAt).count(),
            std::chrono::duration<double, std::milli>(completedAt - canonicalAt).count(),
            std::chrono::duration<double, std::milli>(completedAt - ensureStarted).count());
        return true;
    }

    if (!stripOffsetsE || !stripCountsE || !stripOffsetsE->count ||
        stripOffsetsE->count != stripCountsE->count) return false;
    const auto rowsPerStripE = find(TIFF_TAG_ROWS_PER_STRIP);
    const uint32_t rowsPerStrip = rowsPerStripE ? scalar(*rowsPerStripE) : height;
    if (!rowsPerStrip) return false;
    const uint64_t stripsPerPlane64 =
        (static_cast<uint64_t>(height) + rowsPerStrip - 1) / rowsPerStrip;
    const uint64_t expectedStrips64 = stripsPerPlane64 * (planar == 2 ? channels : 1);
    if (!stripsPerPlane64 || stripsPerPlane64 > std::numeric_limits<uint32_t>::max() ||
        expectedStrips64 > std::numeric_limits<uint32_t>::max()) return false;
    const uint32_t stripsPerPlane = static_cast<uint32_t>(stripsPerPlane64);
    const uint32_t expectedStrips = static_cast<uint32_t>(expectedStrips64);
    if (stripOffsetsE->count != expectedStrips) return false;
    std::vector<std::pair<uint32_t, uint32_t>> sourceChunks;
    sourceChunks.reserve(expectedStrips);
    for (uint32_t index = 0; index < expectedStrips; ++index) {
        uint32_t offset = 0, byteCount = 0;
        if (!arrayValue(*stripOffsetsE, index, offset) ||
            !arrayValue(*stripCountsE, index, byteCount)) return false;
        sourceChunks.emplace_back(offset, byteCount);
        const uint32_t plane = planar == 2 ? index / stripsPerPlane : 0;
        const uint32_t strip = planar == 2 ? index % stripsPerPlane : index;
        const uint32_t originY = strip * rowsPerStrip;
        const uint32_t rows = std::min(rowsPerStrip, height - originY);
        const uint32_t chunkChannels = planar == 2 ? 1 : channels;
        std::vector<uint16_t> decoded;
        uint32_t decodedWidth = 0, decodedHeight = 0;
        bool decodedOk = decodeChunk(offset, byteCount, width, rows, chunkChannels,
                                     decoded, decodedWidth, decodedHeight);
        if (!decodedOk && compression != TIFF_COMPRESSION_NONE && rows < rowsPerStrip)
            decodedOk = decodeChunk(offset, byteCount, width, rowsPerStrip, chunkChannels,
                                    decoded, decodedWidth, decodedHeight);
        if (!decodedOk || decodedWidth < width || decodedHeight < rows) return false;
        for (uint32_t y = 0; y < rows; ++y)
            for (uint32_t x = 0; x < width; ++x)
                for (uint32_t c = 0; c < chunkChannels; ++c) {
                    const uint32_t destinationChannel = planar == 2 ? plane : c;
                    pixels[(static_cast<size_t>(originY + y) * width + x) * channels +
                        destinationChannel] =
                        decoded[(static_cast<size_t>(y) * decodedWidth + x) * chunkChannels + c];
                }
    }
    if (!rowsPerStripE) return false;
    return finishCanonical(pixels, *stripOffsetsE, *stripCountsE, *rowsPerStripE) &&
           discardSourceChunks(std::move(sourceChunks));
}

bool DNGDecoder::removeThumbnails(std::vector<uint8_t>& data) {
    bool little = true;
    auto entries = findTiffEntries(data, little);
    if (entries.empty()) return false;
    auto scalar = [&](const TiffEntry& entry, uint32_t index = 0) -> uint32_t {
        if (index >= entry.count) return 0;
        const size_t step = entry.type == TIFF_TYPE_SHORT ? 2 : 4;
        const size_t at = entry.valueOffset + static_cast<size_t>(index) * step;
        return entry.type == TIFF_TYPE_SHORT
            ? read16(data.data() + at, little)
            : entry.type == TIFF_TYPE_LONG ? read32(data.data() + at, little) : 0;
    };

    std::map<uint32_t, std::vector<TiffEntry>> byIfd;
    for (const auto& entry : entries) byIfd[entry.ifdOffset].push_back(entry);

    // Photometric interpretation does not reliably distinguish a primary image
    // from an embedded preview: IFD0 is commonly a small RGB preview while the
    // full-resolution raw image lives in a SubIFD. Preserve the image-bearing
    // IFD with the greatest pixel area, regardless of whether it is CFA,
    // LinearRaw, RGB, or another valid TIFF interpretation.
    uint32_t primaryIfd = 0;
    uint64_t primaryPixels = 0;
    bool foundPrimary = false;
    for (const auto& [ifd, ifdEntries] : byIfd) {
        const TiffEntry* width = nullptr;
        const TiffEntry* height = nullptr;
        bool hasImagePayload = false;
        for (const auto& entry : ifdEntries) {
            if (entry.tag == TIFF_TAG_IMAGE_WIDTH) width = &entry;
            else if (entry.tag == TIFF_TAG_IMAGE_HEIGHT) height = &entry;
            else if (entry.tag == TIFF_TAG_STRIP_OFFSETS ||
                     entry.tag == TIFF_TAG_TILE_OFFSETS ||
                     entry.tag == TIFF_TAG_JPEG_OFFSET)
                hasImagePayload = true;
        }
        if (!width || !height || !hasImagePayload || !width->count || !height->count)
            continue;
        const uint64_t pixels = static_cast<uint64_t>(scalar(*width)) * scalar(*height);
        if (!pixels) continue;
        if (!foundPrimary || pixels > primaryPixels) {
            primaryIfd = ifd;
            primaryPixels = pixels;
            foundPrimary = true;
        }
    }
    if (!foundPrimary) return false;

    std::vector<std::pair<uint32_t, uint32_t>> payloads;
    const std::set<uint16_t> imageTags{
        254, 255, 256, 257, 258, 259, 262, 273, 274, 277, 278, 279,
        282, 283, 284, 296, 322, 323, 324, 325, 513, 514,
        529, 530, 531, 532};

    for (const auto& [ifd, ifdEntries] : byIfd) {
        if (ifd == primaryIfd) continue;
        const TiffEntry* offsets = nullptr;
        const TiffEntry* counts = nullptr;
        bool imageIfd = false;
        for (const auto& entry : ifdEntries) {
            if (entry.tag == TIFF_TAG_STRIP_OFFSETS || entry.tag == TIFF_TAG_TILE_OFFSETS ||
                entry.tag == TIFF_TAG_JPEG_OFFSET) {
                offsets = &entry;
                imageIfd = true;
            }
            if (entry.tag == TIFF_TAG_STRIP_BYTE_COUNTS || entry.tag == TIFF_TAG_TILE_BYTE_COUNTS ||
                entry.tag == TIFF_TAG_JPEG_BYTE_COUNT)
                counts = &entry;
        }
        if (!imageIfd) continue;
        if (offsets && counts && offsets->count == counts->count) {
            for (uint32_t i = 0; i < offsets->count; ++i) {
                const uint32_t offset = scalar(*offsets, i);
                const uint32_t bytes = scalar(*counts, i);
                if (offset <= data.size() && bytes <= data.size() - offset)
                    payloads.emplace_back(offset, bytes);
            }
        }

        const uint16_t count = read16(data.data() + ifd, little);
        const size_t oldNext = static_cast<size_t>(ifd) + 2 + static_cast<size_t>(count) * 12;
        if (oldNext + 4 > data.size()) return false;
        const uint32_t nextIfd = read32(data.data() + oldNext, little);
        std::vector<std::array<uint8_t, 12>> kept;
        kept.reserve(count);
        for (uint16_t i = 0; i < count; ++i) {
            const size_t at = static_cast<size_t>(ifd) + 2 + static_cast<size_t>(i) * 12;
            if (imageTags.count(read16(data.data() + at, little))) continue;
            std::array<uint8_t, 12> record{};
            std::copy_n(data.data() + at, 12, record.data());
            kept.push_back(record);
        }
        write16(data.data() + ifd, static_cast<uint16_t>(kept.size()), little);
        size_t at = static_cast<size_t>(ifd) + 2;
        for (const auto& record : kept) {
            std::copy(record.begin(), record.end(), data.begin() + at);
            at += 12;
        }
        write32(data.data() + at, nextIfd, little);
        std::fill(data.begin() + at + 4, data.begin() + oldNext + 4, 0);
    }

    std::sort(payloads.begin(), payloads.end(), [](const auto& left, const auto& right) {
        return left.first > right.first;
    });
    const std::vector<uint8_t> empty;
    for (const auto& [offset, bytes] : payloads)
        if (bytes && !replaceTiffStrip(data, offset, bytes, empty, little)) return false;
    return true;
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
    uint32_t white = scalar(*whiteE);
    // Log-encoded DNGs declare their post-linearization white level, while the
    // stored code range is described by LinearizationTable's entry count.
    if (const auto linearization = find(TIFF_TAG_LINEARIZATION_TABLE);
        linearization && linearization->count > 1)
        white = linearization->count - 1;
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

bool DNGDecoder::applyLogTransform(std::vector<uint8_t>& data, LogTransformMode mode,
                                   uint32_t quantizationWhite) {
    if (mode == LogTransformMode::Disabled) return true;
    bool little = true;
    auto entries = findTiffEntries(data, little);
    auto scalar = [&](const TiffEntry& entry, uint32_t index = 0) -> uint32_t {
        index = std::min(index, entry.count - 1);
        const size_t pos = entry.valueOffset + static_cast<size_t>(index) *
            (entry.type == TIFF_TYPE_SHORT ? 2 : 4);
        return entry.type == TIFF_TYPE_SHORT ? read16(data.data() + pos, little)
                                             : read32(data.data() + pos, little);
    };
    const TiffEntry* photo = nullptr;
    for (const auto& entry : entries)
        if (entry.tag == TIFF_TAG_PHOTOMETRIC && entry.count &&
            (scalar(entry) == TIFF_PHOTOMETRIC_CFA || scalar(entry) == 34892)) {
            photo = &entry; break;
        }
    if (!photo) return false;
    auto find = [&](uint16_t tag) -> const TiffEntry* {
        for (const auto& entry : entries)
            if (entry.ifdOffset == photo->ifdOffset && entry.tag == tag) return &entry;
        return nullptr;
    };
    const auto inputLinearization = find(TIFF_TAG_LINEARIZATION_TABLE);
    const auto widthE=find(TIFF_TAG_IMAGE_WIDTH), heightE=find(TIFF_TAG_IMAGE_HEIGHT);
    const auto bitsE=find(TIFF_TAG_BITS_PER_SAMPLE), compressionE=find(TIFF_TAG_COMPRESSION);
    const auto offsetsE=find(TIFF_TAG_STRIP_OFFSETS), countsE=find(TIFF_TAG_STRIP_BYTE_COUNTS);
    const auto sppE=find(TIFF_TAG_SAMPLES_PER_PIXEL), whiteE=find(TIFF_TAG_WHITE_LEVEL);
    const auto blackE=find(TIFF_TAG_BLACK_LEVEL);
    if (!widthE || !heightE || !bitsE || !compressionE || !offsetsE || !countsE ||
        !sppE || !whiteE || offsetsE->count != 1 || countsE->count != 1 ||
        scalar(*compressionE) != TIFF_COMPRESSION_NONE || scalar(*bitsE) != 16)
        return false;
    if (inputLinearization &&
        (inputLinearization->type != TIFF_TYPE_SHORT || !inputLinearization->count))
        return false;
    const uint32_t width=scalar(*widthE), height=scalar(*heightE), channels=scalar(*sppE);
    const uint32_t offset=scalar(*offsetsE), bytes=scalar(*countsE);
    const uint32_t sourceWhite=scalar(*whiteE);
    const size_t samples=static_cast<size_t>(width)*height*channels;
    if (!width || !height || (channels != 1 && channels != 3) || !sourceWhite ||
        offset > data.size() || bytes < samples*2 || bytes > data.size()-offset) return false;
    // Vignette baking may temporarily expand the linear code range. When the
    // caller supplies the pre-bake white level, preserve the requested output
    // bit depth and requantize the expanded linear samples directly into it.
    const uint32_t bitDepthWhite = quantizationWhite ? quantizationWhite : sourceWhite;
    uint32_t storedBits=1;
    while (storedBits < 16 && ((uint32_t{1}<<storedBits)-1) < bitDepthWhite) ++storedBits;
    if (mode == LogTransformMode::ReduceBy2Bit) storedBits = std::max(1u, storedBits-2);
    else if (mode == LogTransformMode::ReduceBy4Bit) storedBits = std::max(1u, storedBits-4);
    else if (mode == LogTransformMode::ReduceBy6Bit) storedBits = std::max(1u, storedBits-6);
    else if (mode == LogTransformMode::ReduceBy8Bit) storedBits = std::max(1u, storedBits-8);
    const uint32_t storedWhite=(uint32_t{1}<<storedBits)-1;
    std::array<double,4> black{};
    if (blackE && blackE->count) for (uint32_t i=0;i<4;++i) {
        const uint32_t bi=std::min<uint32_t>(i,blackE->count-1);
        black[i]=blackE->type == TIFF_TYPE_RATIONAL
            ? readRational(data,*blackE,bi,little) : scalar(*blackE,bi);
    }
    for (uint32_t y=0;y<height;++y) for (uint32_t x=0;x<width;++x)
        for (uint32_t c=0;c<channels;++c) {
            const size_t i=(static_cast<size_t>(y)*width+x)*channels+c;
            const uint32_t phase=channels==1 ? ((y&1u)*2+(x&1u)) : c;
            uint32_t linearValue = read16(data.data()+offset+i*2,little);
            if (inputLinearization) {
                const uint32_t tableIndex = std::min<uint32_t>(
                    linearValue, inputLinearization->count - 1);
                linearValue = read16(data.data() + inputLinearization->valueOffset +
                                     static_cast<size_t>(tableIndex) * 2, little);
            }
            const double normalized=std::clamp(
                (linearValue-black[phase]) /
                std::max(1.0,static_cast<double>(sourceWhite)-black[phase]),0.0,1.0);
            const double encoded=std::log2(1.0+60.0*normalized)/std::log2(61.0);
            write16(data.data()+offset+i*2,
                    static_cast<uint16_t>(std::lround(encoded*storedWhite)),little);
        }
    if (inputLinearization)
        write16(data.data()+inputLinearization->entryOffset,
                TIFF_TAG_UNUSED_LINEARIZATION_TABLE,little);
    if (data.size() & 1u) data.push_back(0);
    const uint32_t tableOffset=static_cast<uint32_t>(data.size());
    data.resize(data.size()+static_cast<size_t>(storedWhite+1)*2);
    for (uint32_t i=0;i<=storedWhite;++i) {
        const double encoded=static_cast<double>(i)/storedWhite;
        const double linear=(std::pow(2.0,encoded*std::log2(61.0))-1.0)/60.0;
        write16(data.data()+tableOffset+static_cast<size_t>(i)*2,
                static_cast<uint16_t>(std::lround(std::clamp(linear,0.0,1.0)*65535.0)),little);
    }
    const uint32_t oldIfd=photo->ifdOffset;
    if (oldIfd+2>data.size()) return false;
    const uint16_t oldCount=read16(data.data()+oldIfd,little);
    const size_t oldEnd=static_cast<size_t>(oldIfd)+2+static_cast<size_t>(oldCount)*12;
    if (oldEnd+4>data.size() || oldCount==std::numeric_limits<uint16_t>::max()) return false;
    if (data.size() & 1u) data.push_back(0);
    const uint32_t newIfd=static_cast<uint32_t>(data.size());
    std::vector<std::array<uint8_t,12>> rebuilt;
    rebuilt.reserve(oldCount+1);
    for (uint16_t i=0;i<oldCount;++i) {
        std::array<uint8_t,12> entry{};
        std::memcpy(entry.data(),data.data()+oldIfd+2+static_cast<size_t>(i)*12,12);
        rebuilt.push_back(entry);
    }
    std::array<uint8_t,12> linearEntry{};
    write16(linearEntry.data(),TIFF_TAG_LINEARIZATION_TABLE,little);
    write16(linearEntry.data()+2,TIFF_TYPE_SHORT,little);
    write32(linearEntry.data()+4,storedWhite+1,little);
    write32(linearEntry.data()+8,tableOffset,little);
    rebuilt.push_back(linearEntry);
    std::sort(rebuilt.begin(),rebuilt.end(),[little](const auto&a,const auto&b){
        return read16(a.data(),little)<read16(b.data(),little); });
    data.resize(data.size()+2+rebuilt.size()*12+4,0);
    write16(data.data()+newIfd,static_cast<uint16_t>(rebuilt.size()),little);
    size_t pos=static_cast<size_t>(newIfd)+2;
    for (const auto& entry:rebuilt) { std::memcpy(data.data()+pos,entry.data(),12); pos+=12; }
    std::memcpy(data.data()+pos,data.data()+oldEnd,4);
    bool relinked = false;
    if (read32(data.data()+4,little) == oldIfd) {
        write32(data.data()+4,newIfd,little);
        relinked = true;
    } else {
        for (const auto& entry : entries) {
            if (entry.tag != TIFF_TAG_SUB_IFDS || entry.type != TIFF_TYPE_LONG) continue;
            for (uint32_t i=0;i<entry.count;++i) {
                uint8_t* value = data.data()+entry.valueOffset+static_cast<size_t>(i)*4;
                if (read32(value,little) == oldIfd) {
                    write32(value,newIfd,little);
                    relinked = true;
                }
            }
        }
        if (!relinked) {
            std::set<uint32_t> checkedIfds;
            uint32_t chainedIfd = read32(data.data() + 4, little);
            while (chainedIfd && checkedIfds.insert(chainedIfd).second &&
                   chainedIfd + 2 <= data.size()) {
                const uint16_t count = read16(data.data() + chainedIfd, little);
                const size_t nextPos = static_cast<size_t>(chainedIfd) + 2 +
                    static_cast<size_t>(count) * 12;
                if (nextPos + 4 > data.size()) break;
                const uint32_t nextIfd = read32(data.data() + nextPos, little);
                if (nextIfd == oldIfd) {
                    write32(data.data() + nextPos, newIfd, little);
                    relinked = true;
                    break;
                }
                chainedIfd = nextIfd;
            }
            for (const auto& entry : entries) {
                if (relinked) break;
                if (!checkedIfds.insert(entry.ifdOffset).second) continue;
                const uint16_t count = read16(data.data() + entry.ifdOffset, little);
                const size_t nextPos = static_cast<size_t>(entry.ifdOffset) + 2 +
                    static_cast<size_t>(count) * 12;
                if (nextPos + 4 <= data.size() &&
                    read32(data.data() + nextPos, little) == oldIfd) {
                    write32(data.data() + nextPos, newIfd, little);
                    relinked = true;
                    break;
                }
            }
        }
    }
    if (!relinked) return false;
    entries=findTiffEntries(data,little);
    for (const auto& entry:entries) if (entry.ifdOffset==newIfd) {
        if (entry.tag==TIFF_TAG_WHITE_LEVEL) {
            if(entry.type==TIFF_TYPE_SHORT) write16(data.data()+entry.valueOffset,65534,little);
            else write32(data.data()+entry.valueOffset,65534,little);
        } else if (entry.tag==TIFF_TAG_BLACK_LEVEL) {
            for(uint32_t i=0;i<entry.count;++i) {
                if(entry.type==TIFF_TYPE_RATIONAL) writeRational(data,entry,i,0.0,little);
                else if(entry.type==TIFF_TYPE_SHORT) write16(data.data()+entry.valueOffset+i*2,0,little);
                else write32(data.data()+entry.valueOffset+i*4,0,little);
            }
        }
    }
    return true;
}

bool DNGDecoder::bakeIsoOverlay(std::vector<uint8_t>& data, double iso) {
    bool little = true;
    const auto entries = findTiffEntries(data, little);
    auto scalar = [&](const TiffEntry& entry, uint32_t index = 0) -> uint32_t {
        index = std::min(index, entry.count - 1);
        const size_t position = entry.valueOffset + static_cast<size_t>(index) *
            (entry.type == TIFF_TYPE_SHORT ? 2 : 4);
        return entry.type == TIFF_TYPE_SHORT ? read16(data.data() + position, little)
                                             : read32(data.data() + position, little);
    };
    const TiffEntry* photo = nullptr;
    for (const auto& entry : entries)
        if (entry.tag == TIFF_TAG_PHOTOMETRIC && entry.count &&
            (scalar(entry) == TIFF_PHOTOMETRIC_CFA || scalar(entry) == 34892)) {
            photo = &entry; break;
        }
    if (!photo) return false;
    auto find = [&](uint16_t tag) -> const TiffEntry* {
        for (const auto& entry : entries)
            if (entry.ifdOffset == photo->ifdOffset && entry.tag == tag && entry.count) return &entry;
        return nullptr;
    };
    const auto widthE = find(TIFF_TAG_IMAGE_WIDTH), heightE = find(TIFF_TAG_IMAGE_HEIGHT);
    const auto bitsE = find(TIFF_TAG_BITS_PER_SAMPLE), compressionE = find(TIFF_TAG_COMPRESSION);
    const auto offsetsE = find(TIFF_TAG_STRIP_OFFSETS), countsE = find(TIFF_TAG_STRIP_BYTE_COUNTS);
    const auto sppE = find(TIFF_TAG_SAMPLES_PER_PIXEL), blackE = find(TIFF_TAG_BLACK_LEVEL);
    const auto whiteE = find(TIFF_TAG_WHITE_LEVEL);
    if (!widthE || !heightE || !bitsE || !compressionE || !offsetsE || !countsE ||
        !sppE || !whiteE || offsetsE->count != 1 || countsE->count != 1 ||
        scalar(*bitsE) != 16 || scalar(*compressionE) != TIFF_COMPRESSION_NONE) return false;
    const uint32_t width = scalar(*widthE), height = scalar(*heightE), channels = scalar(*sppE);
    const uint32_t offset = scalar(*offsetsE), bytes = scalar(*countsE);
    const size_t required = static_cast<size_t>(width) * height * channels * 2;
    if (!width || !height || !channels || channels > 4 || offset > data.size() ||
        bytes < required || required > data.size() - offset) return false;
    std::vector<uint16_t> samples(static_cast<size_t>(width) * height * channels);
    for (size_t i = 0; i < samples.size(); ++i)
        samples[i] = read16(data.data() + offset + i * 2, little);
    utils::bakeIsoOverlay(samples.data(), width, height, channels, iso,
                          static_cast<uint16_t>(blackE ? scalar(*blackE) : 0),
                          static_cast<uint16_t>(std::min<uint32_t>(scalar(*whiteE), 65535)));
    for (size_t i = 0; i < samples.size(); ++i)
        write16(data.data() + offset + i * 2, samples[i], little);
    return true;
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

bool DNGDecoder::replaceUncompressedRGB16(std::vector<uint8_t>& data,
                                          const std::vector<uint8_t>& rgbData,
                                          uint32_t width, uint32_t height) {
    bool little = true;
    const auto entries = findTiffEntries(data, little);
    auto scalar = [&](const TiffEntry& entry) -> uint32_t {
        return entry.type == TIFF_TYPE_SHORT ? read16(data.data() + entry.valueOffset, little)
                                             : read32(data.data() + entry.valueOffset, little);
    };
    const TiffEntry* photo = nullptr;
    for (const auto& entry : entries) {
        if (entry.tag == TIFF_TAG_PHOTOMETRIC &&
            (scalar(entry) == 2 || scalar(entry) == 34892)) { photo = &entry; break; }
    }
    if (!photo || rgbData.size() != static_cast<size_t>(width) * height * 6) return false;
    auto find = [&](uint16_t tag) -> const TiffEntry* {
        for (const auto& entry : entries)
            if (entry.ifdOffset == photo->ifdOffset && entry.tag == tag) return &entry;
        return nullptr;
    };
    const auto widthE = find(TIFF_TAG_IMAGE_WIDTH), heightE = find(TIFF_TAG_IMAGE_HEIGHT);
    const auto bitsE = find(TIFF_TAG_BITS_PER_SAMPLE), compressionE = find(TIFF_TAG_COMPRESSION);
    const auto offsetsE = find(TIFF_TAG_STRIP_OFFSETS), countsE = find(TIFF_TAG_STRIP_BYTE_COUNTS);
    const auto sppE = find(TIFF_TAG_SAMPLES_PER_PIXEL);
    if (!widthE || !heightE || !bitsE || !compressionE || !offsetsE || !countsE || !sppE ||
        scalar(*widthE) != width || scalar(*heightE) != height || scalar(*bitsE) != 16 ||
        scalar(*compressionE) != TIFF_COMPRESSION_NONE || scalar(*sppE) != 3 ||
        offsetsE->count != 1 || countsE->count != 1) return false;
    const uint32_t offset = scalar(*offsetsE), bytes = scalar(*countsE);
    auto level = [&](const TiffEntry* entry, uint32_t channel, double fallback) {
        if (!entry || !entry->count) return fallback;
        const uint32_t index = std::min(channel, entry->count - 1);
        if (entry->type == TIFF_TYPE_RATIONAL || entry->type == TIFF_TYPE_SRATIONAL)
            return readRational(data, *entry, index, little);
        const size_t position = entry->valueOffset + static_cast<size_t>(index) *
            (entry->type == TIFF_TYPE_SHORT ? 2 : 4);
        return entry->type == TIFF_TYPE_SHORT
            ? static_cast<double>(read16(data.data() + position, little))
            : static_cast<double>(read32(data.data() + position, little));
    };
    const auto black = find(TIFF_TAG_BLACK_LEVEL);
    const auto white = find(TIFF_TAG_WHITE_LEVEL);
    std::vector<uint8_t> encoded(rgbData.size());
    for (size_t i = 0; i < rgbData.size() / 2; ++i) {
        const uint16_t normalized = static_cast<uint16_t>(rgbData[i * 2] | rgbData[i * 2 + 1] << 8);
        const uint32_t channel = static_cast<uint32_t>(i % 3);
        const double blackValue = level(black, channel, 0.0);
        const double whiteValue = level(white, channel, 65535.0);
        if (!(whiteValue > blackValue)) return false;
        const uint16_t value = static_cast<uint16_t>(std::clamp(std::lround(
            blackValue + normalized / 65535.0 * (whiteValue - blackValue)), 0l, 65535l));
        encoded[i * 2] = little ? value & 0xff : value >> 8;
        encoded[i * 2 + 1] = little ? value >> 8 : value & 0xff;
    }
    return replaceTiffStrip(data, offset, bytes, encoded, little);
}

namespace {
bool setXmpBoolean(std::vector<uint8_t>& data, const char* property, bool enabled) {
    bool little = true;
    for (const auto& entry : findTiffEntries(data, little)) {
        if (entry.tag != TIFF_TAG_XMP || entry.type != TIFF_TYPE_BYTE || !entry.count) continue;
        std::string xmp(reinterpret_cast<const char*>(data.data() + entry.valueOffset), entry.count);
        const std::string prefix = std::string(property) + "=";
        const auto propertyPosition = xmp.find(prefix);
        if (propertyPosition != std::string::npos) {
            const size_t quotePosition = propertyPosition + prefix.size();
            if (quotePosition >= xmp.size() ||
                (xmp[quotePosition] != '\'' && xmp[quotePosition] != '\"')) return false;
            const char quote = xmp[quotePosition];
            const auto valueEnd = xmp.find(quote, quotePosition + 1);
            if (valueEnd == std::string::npos) return false;
            xmp.replace(quotePosition + 1, valueEnd - quotePosition - 1,
                        enabled ? "true" : "false");
        } else {
            const auto descriptionEnd = xmp.find("/>");
            if (descriptionEnd == std::string::npos) return false;
            xmp.insert(descriptionEnd, std::string(" ") + property +
                (enabled ? "='true'" : "='false'"));
        }
        while (data.size() % 4) data.push_back(0);
        const uint32_t offset = static_cast<uint32_t>(data.size());
        data.insert(data.end(), xmp.begin(), xmp.end());
        write32(data.data() + entry.entryOffset + 4, static_cast<uint32_t>(xmp.size()), little);
        write32(data.data() + entry.entryOffset + 8, offset, little);
        return true;
    }
    return false;
}
}

bool DNGDecoder::markSyntheticFrame(std::vector<uint8_t>& data) {
    return setXmpBoolean(data, "rpt:DuplicateFrame", false) &&
           setXmpBoolean(data, "rpt:SyntheticFrame", true);
}

bool DNGDecoder::markDuplicateFrame(std::vector<uint8_t>& data) {
    return setXmpBoolean(data, "rpt:DuplicateFrame", true);
}

namespace {
bool hasXmpBoolean(const std::vector<uint8_t>& data, const char* property) {
    bool little = true;
    for (const auto& entry : findTiffEntries(data, little)) {
        if (entry.tag != TIFF_TAG_XMP || entry.type != TIFF_TYPE_BYTE || !entry.count) continue;
        std::string xmp(reinterpret_cast<const char*>(data.data() + entry.valueOffset), entry.count);
        const std::string prefix = std::string(property) + "=";
        const auto position = xmp.find(prefix);
        if (position == std::string::npos) continue;
        const auto value = position + prefix.size();
        return xmp.compare(value, 6, "'true'") == 0 ||
               xmp.compare(value, 6, "\"true\"") == 0;
    }
    return false;
}
}

bool DNGDecoder::isSyntheticFrame(const std::vector<uint8_t>& data) {
    return hasXmpBoolean(data, "rpt:SyntheticFrame");
}

bool DNGDecoder::isDuplicateFrame(const std::vector<uint8_t>& data) {
    return hasXmpBoolean(data, "rpt:DuplicateFrame");
}

bool DNGDecoder::interpolateFrameMetadata(std::vector<uint8_t>& data,
                                          const std::vector<uint8_t>& leftDng,
                                          const std::vector<uint8_t>& rightDng,
                                          double ratio) {
    if (!(ratio >= 0.0 && ratio <= 1.0)) return false;
    DNGFrameMetadata left, right;
    if (!getColorMetadata(leftDng, left) || !getColorMetadata(rightDng, right)) return false;
    auto geometric = [ratio](double a, double b) {
        return a > 0.0 && b > 0.0
            ? std::exp(std::log(a) * (1.0 - ratio) + std::log(b) * ratio)
            : a * (1.0 - ratio) + b * ratio;
    };
    double baseline = 0.0;
    const double* baselinePtr = nullptr;
    if (left.hasBaselineExposure && right.hasBaselineExposure) {
        baseline = left.baselineExposure * (1.0 - ratio) + right.baselineExposure * ratio;
        baselinePtr = &baseline;
    }
    std::array<float, 3> neutral{};
    const std::array<float, 3>* neutralPtr = nullptr;
    if (left.hasAsShotNeutral && right.hasAsShotNeutral) {
        for (size_t channel = 0; channel < neutral.size(); ++channel)
            neutral[channel] = static_cast<float>(geometric(
                left.asShotNeutral[channel], right.asShotNeutral[channel]));
        neutralPtr = &neutral;
    }
    if (!updateMetadata(data, baselinePtr, neutralPtr)) return false;

    bool little = true;
    bool exposureWritten = !(left.exposureTime > 0.0 && right.exposureTime > 0.0);
    bool isoWritten = !(left.iso > 0.0 && right.iso > 0.0);
    for (const auto& entry : findTiffEntries(data, little)) {
        if (!exposureWritten && entry.tag == TIFF_TAG_EXPOSURE_TIME &&
            entry.type == TIFF_TYPE_RATIONAL && entry.count) {
            writeRational(data, entry, 0, geometric(left.exposureTime, right.exposureTime), little);
            exposureWritten = true;
        } else if (!isoWritten && entry.tag == TIFF_TAG_ISO && entry.count) {
            const uint32_t iso = static_cast<uint32_t>(std::lround(geometric(left.iso, right.iso)));
            if (entry.type == TIFF_TYPE_SHORT)
                write16(data.data() + entry.valueOffset, static_cast<uint16_t>(std::min(iso, 65535u)), little);
            else if (entry.type == TIFF_TYPE_LONG)
                write32(data.data() + entry.valueOffset, iso, little);
            else continue;
            isoWritten = true;
        }
    }
    return exposureWritten && isoWritten;
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
    size_t sampleCount = 0;
    if (!checkedImageSamples(width, height, channels, sampleCount) ||
        static_cast<size_t>(width) >
            (std::numeric_limits<size_t>::max() - 7u) / channels / bits) return false;
    const size_t rowBytes = (static_cast<size_t>(width) * channels * bits + 7u) / 8u;
    if (rowBytes > std::numeric_limits<size_t>::max() / height ||
        rowBytes * height > stripBytes) return false;
    std::vector<uint16_t> pixels(sampleCount);
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

bool DNGDecoder::compressLossyJPEG(std::vector<uint8_t>& data, int quality) {
    // Blackmagic's CinemaDNG extension retains CFA and RGB layouts, stores
    // 12-bit DCT JPEG under Compression=7, and is intentionally distinct from
    // Adobe's 8-bit LinearRaw Compression=34892 format.
    if (!ensureUncompressed(data)) return false;

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
    if (!width || !height || bits < 8 || bits > 16 ||
        (channels != 1 && channels != 3) ||
        stripOffset > data.size() || stripBytes > data.size() - stripOffset) return false;
    const size_t rowBytes = (static_cast<size_t>(width) * channels * bits + 7) / 8;
    if (rowBytes * height > stripBytes) return false;
    const uint32_t sourceMax = bits == 16 ? 65535u : (1u << bits) - 1u;
    uint32_t sourceRange = sourceMax;
    const auto linearizationTable = find(TIFF_TAG_LINEARIZATION_TABLE);
    if (linearizationTable && linearizationTable->count > 1) {
        sourceRange = std::min(sourceMax, linearizationTable->count - 1);
    } else if (const auto white = find(TIFF_TAG_WHITE_LEVEL); white && white->count &&
        (white->type == TIFF_TYPE_SHORT || white->type == TIFF_TYPE_LONG)) {
        const uint32_t taggedWhite = white->type == TIFF_TYPE_SHORT
            ? read16(data.data() + white->valueOffset, little)
            : read32(data.data() + white->valueOffset, little);
        if (taggedWhite > 0 && taggedWhite <= sourceMax) sourceRange = taggedWhite;
    }
    std::vector<uint16_t> pixels(static_cast<size_t>(width) * height * channels);
    for (uint32_t y = 0; y < height; ++y) {
        size_t bitOffset = static_cast<size_t>(y) * rowBytes * 8;
        for (uint32_t x = 0; x < width * channels; ++x) {
            uint32_t value = 0;
            if (bits == 16) {
                value = read16(data.data() + stripOffset +
                    (static_cast<size_t>(y) * width * channels + x) * 2, little);
                bitOffset += 16;
            } else {
                for (uint32_t b = 0; b < bits; ++b, ++bitOffset)
                    value = (value << 1) |
                        ((data[stripOffset + bitOffset / 8] >> (7 - bitOffset % 8)) & 1);
            }
            pixels[static_cast<size_t>(y) * width * channels + x] =
                static_cast<uint16_t>((std::min(value, sourceRange) * 4095u +
                                       sourceRange / 2u) / sourceRange);
        }
    }
    std::vector<uint8_t> encoded;
    const bool cfa = scalar(*photo) == TIFF_PHOTOMETRIC_CFA;
    // Blackmagic row deinterleaving is a reshape without moving samples: two
    // consecutive TIFF rows become one JPEG row. JPEG vertical neighbours are
    // therefore two CFA rows apart and share the same color phase.
    const uint32_t jpegWidth = cfa ? width * 2 : width;
    const uint32_t jpegHeight = cfa ? height / 2 : height;
    if ((cfa && ((height & 1u) != 0 || width > JPEG_MAX_DIMENSION / 2u)) ||
        (!cfa && (width > JPEG_MAX_DIMENSION || height > JPEG_MAX_DIMENSION)) ||
        !encodeDctJPEG12(pixels, jpegWidth, jpegHeight, channels, quality, encoded)) {
        return false;
    }
    for (uint32_t i = 0; i < bitsE->count; ++i)
        write16(data.data() + bitsE->valueOffset + static_cast<size_t>(i) * 2, 12, little);
    auto setScalar = [&](const TiffEntry& entry, uint32_t value) {
        if (entry.type == TIFF_TYPE_SHORT) write16(data.data() + entry.valueOffset, value, little);
        else if (entry.type == TIFF_TYPE_LONG) write32(data.data() + entry.valueOffset, value, little);
    };
    setScalar(*compressionE, TIFF_COMPRESSION_JPEG);
    setScalar(*countsE, static_cast<uint32_t>(encoded.size()));
    if (!linearizationTable)
      if (const auto white = find(TIFF_TAG_WHITE_LEVEL))
        for (uint32_t i = 0; i < white->count; ++i) {
            const size_t step = white->type == TIFF_TYPE_SHORT ? 2 : 4;
            if (white->type == TIFF_TYPE_SHORT) write16(data.data() + white->valueOffset + i * step, 4095, little);
            else if (white->type == TIFF_TYPE_LONG) write32(data.data() + white->valueOffset + i * step, 4095, little);
        }
    // BlackLevel is measured after LinearizationTable has been applied. When
    // retaining that table, its values therefore remain in the same domain and
    // must not be rescaled with the stored sample codes.
    if (!linearizationTable)
      if (const auto black = find(TIFF_TAG_BLACK_LEVEL))
        for (uint32_t i = 0; i < black->count; ++i) {
            const size_t step = black->type == TIFF_TYPE_SHORT ? 2 : 4;
            const size_t at = black->valueOffset + i * step;
            const uint32_t old = black->type == TIFF_TYPE_SHORT
                ? read16(data.data() + at, little) : read32(data.data() + at, little);
            const uint32_t scaled = (old * 4095u + sourceRange / 2u) / sourceRange;
            if (black->type == TIFF_TYPE_SHORT) write16(data.data() + at, scaled, little);
            else if (black->type == TIFF_TYPE_LONG) write32(data.data() + at, scaled, little);
        }
    std::vector<uint16_t> expandedLinearization;
    if (const auto table = linearizationTable;
        table && table->type == TIFF_TYPE_SHORT && table->count >= 2) {
        std::array<uint16_t, 4096> reduced{};
        for (uint32_t i = 0; i < reduced.size(); ++i) {
            const uint32_t source = static_cast<uint32_t>(
                (static_cast<uint64_t>(i) * (table->count - 1) + 2047) / 4095);
            reduced[i] = read16(data.data() + table->valueOffset + source * 2, little);
        }
        expandedLinearization.assign(reduced.begin(), reduced.end());
    }
    if (!replaceTiffStrip(data, stripOffset, stripBytes, encoded, little)) return false;
    if (!expandedLinearization.empty()) {
        const auto updatedEntries = findTiffEntries(data, little);
        const TiffEntry* updatedTable = nullptr;
        for (const auto& entry : updatedEntries)
            if (entry.tag == TIFF_TAG_LINEARIZATION_TABLE) {
                updatedTable = &entry;
                break;
            }
        if (!updatedTable) return false;
        if (data.size() & 1u) data.push_back(0);
        if (data.size() > std::numeric_limits<uint32_t>::max() -
                expandedLinearization.size() * sizeof(uint16_t)) return false;
        const uint32_t tableOffset = static_cast<uint32_t>(data.size());
        data.resize(data.size() + expandedLinearization.size() * sizeof(uint16_t));
        for (size_t i = 0; i < expandedLinearization.size(); ++i)
            write16(data.data() + tableOffset + i * 2, expandedLinearization[i], little);
        write32(data.data() + updatedTable->entryOffset + 4, 4096, little);
        write32(data.data() + updatedTable->entryOffset + 8, tableOffset, little);
    }
    if (cfa) {
        // Resolve selects the Blackmagic CFA reshape reliably for tiled raw
        // IFDs. A single full-frame tile is sufficient; genuine camera files
        // use four horizontal tiles for parallelism.
        const auto tiledEntries = findTiffEntries(data, little);
        const TiffEntry* tiledPhoto = nullptr;
        for (const auto& entry : tiledEntries)
            if (entry.tag == TIFF_TAG_PHOTOMETRIC) {
                const uint32_t value = entry.type == TIFF_TYPE_SHORT
                    ? read16(data.data() + entry.valueOffset, little)
                    : read32(data.data() + entry.valueOffset, little);
                if (value == TIFF_PHOTOMETRIC_CFA) { tiledPhoto = &entry; break; }
            }
        auto findUpdated = [&](uint16_t tag) -> const TiffEntry* {
            if (!tiledPhoto) return nullptr;
            for (const auto& entry : tiledEntries)
                if (entry.ifdOffset == tiledPhoto->ifdOffset && entry.tag == tag) return &entry;
            return nullptr;
        };
        const auto updatedOffsets = findUpdated(TIFF_TAG_STRIP_OFFSETS);
        const auto updatedCounts = findUpdated(TIFF_TAG_STRIP_BYTE_COUNTS);
        const auto updatedRows = findUpdated(TIFF_TAG_ROWS_PER_STRIP);
        if (updatedOffsets && updatedCounts && updatedRows) {
            auto currentScalar = [&](const TiffEntry& entry) {
                return entry.type == TIFF_TYPE_SHORT
                    ? static_cast<uint32_t>(read16(data.data() + entry.valueOffset, little))
                    : read32(data.data() + entry.valueOffset, little);
            };
            auto makeLong = [&](const TiffEntry& entry, uint16_t tag, uint32_t value) {
                write16(data.data() + entry.entryOffset, tag, little);
                write16(data.data() + entry.entryOffset + 2, TIFF_TYPE_LONG, little);
                write32(data.data() + entry.entryOffset + 4, 1, little);
                write32(data.data() + entry.entryOffset + 8, value, little);
            };
            makeLong(*updatedOffsets, TIFF_TAG_TILE_OFFSETS, currentScalar(*updatedOffsets));
            makeLong(*updatedCounts, TIFF_TAG_TILE_BYTE_COUNTS, currentScalar(*updatedCounts));
            makeLong(*updatedRows, TIFF_TAG_TILE_WIDTH, width);
            if (!insertTiffScalarEntry(data, tiledPhoto->ifdOffset,
                                       TIFF_TAG_TILE_LENGTH, TIFF_TYPE_LONG,
                                       height, little)) return false;
        } else {
            return false;
        }
    }
    return true;
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
                              bool colorOnly,
                              bool optimizeGainMaps,
                              bool debugGainMap,
                              int cfaRepeatSizeOverride) {
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
    const auto linearizationE = find(TIFF_TAG_LINEARIZATION_TABLE);
    if (linearizationE &&
        (linearizationE->type != TIFF_TYPE_SHORT || !linearizationE->count)) return false;
    const uint32_t width = scalar(*widthE), height = scalar(*heightE);
    const uint32_t bits = scalar(*bitsE), compression = scalar(*compressionE);
    const uint32_t stripOffset = scalar(*offsetsE), stripBytes = scalar(*countsE);
    if (!width || !height || bits < 8 || bits > 16 || stripOffset > data.size() ||
        stripBytes > data.size() - stripOffset) return false;

    std::vector<GainMap> maps;
    if (!parseOpcodeGainMaps(data.data() + opcodeE->valueOffset, opcodeE->count, maps))
        return false;
    bool motionCamProducer = false;
    for (const auto& entry : entries) {
        if (entry.tag != TIFF_TAG_SOFTWARE || !entry.count ||
            entry.valueOffset + entry.count > data.size()) continue;
        std::string software(
            reinterpret_cast<const char*>(data.data() + entry.valueOffset), entry.count);
        software = software.c_str();
        boost::algorithm::trim(software);
        boost::algorithm::to_lower(software);
        constexpr std::string_view product = "motioncam";
        motionCamProducer |= boost::algorithm::starts_with(software, product) &&
            (software.size() == product.size() ||
             std::isspace(static_cast<unsigned char>(software[product.size()])) ||
             software[product.size()] == '-' || software[product.size()] == 'v');
    }
    std::vector<uint8_t> luminanceOpcodes;
    bool gridColorSeparated = false;
    std::optional<double> luminanceBaseline;
    if (colorOnly && !maps.empty()) {
        GainMap luminance = maps.front();
        luminance.top = 0; luminance.left = 0;
        luminance.bottom = height; luminance.right = width;
        luminance.plane = 0; luminance.planes = 3;
        luminance.rowPitch = 1; luminance.colPitch = 1;
        luminance.channels = 1;
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
        float luminanceMinimum = std::numeric_limits<float>::max();
        for (float gain : luminance.data)
            if (std::isfinite(gain) && gain > 0.0f)
                luminanceMinimum = std::min(luminanceMinimum, gain);
        if (optimizeGainMaps && std::isfinite(luminanceMinimum) && luminanceMinimum > 0.0f) {
            for (float& gain : luminance.data)
                if (std::isfinite(gain) && gain > 0.0f) gain /= luminanceMinimum;
            DNGFrameMetadata metadata;
            if (!getColorMetadata(data, metadata)) return false;
            luminanceBaseline = metadata.baselineExposure + std::log2(luminanceMinimum);
        }
        luminanceOpcodes = serializeGainMap(luminance);
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
        if (bits == 16) {
            if (stripBytes < pixels.size() * 2) return false;
            for (size_t i = 0; i < pixels.size(); ++i)
                pixels[i] = read16(data.data() + stripOffset + i * 2, little);
        } else {
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
        }
    } else return false;

    if (linearizationE) {
        for (auto& pixel : pixels) {
            const uint32_t tableIndex = std::min<uint32_t>(
                pixel, linearizationE->count - 1);
            pixel = read16(data.data() + linearizationE->valueOffset +
                           static_cast<size_t>(tableIndex) * 2, little);
        }
        write16(data.data() + linearizationE->entryOffset,
                TIFF_TAG_UNUSED_LINEARIZATION_TABLE, little);
    }

    int cfaRepeatSize = cfaRepeatSizeOverride;
    std::array<uint8_t, 4> cfaPhase{};
    if (cfaRepeatSize < 2 || (cfaRepeatSize % 2) != 0) {
        if (!getCFAMetadata(data, cfaRepeatSize, cfaPhase) || cfaRepeatSize < 2 ||
            (cfaRepeatSize % 2) != 0)
            cfaRepeatSize = 2;
    }
    const uint32_t phaseGroup = static_cast<uint32_t>(cfaRepeatSize / 2);
    const size_t scalarPhaseMapCount = std::count_if(
        maps.begin(), maps.end(), [](const GainMap& map) {
            return map.channels == 1 && map.rowPitch == 2 && map.colPitch == 2;
        });
    // DNG writers use either one four-plane GainMap or complete groups of four
    // scalar GainMaps (sometimes one group per image region). Multi-plane maps
    // are handled directly below; expand every complete scalar group too.
    const bool groupedScalarPhaseMaps = phaseGroup > 1 &&
        scalarPhaseMapCount >= 4 && (scalarPhaseMapCount % 4) == 0;

    auto sharedScalarOrigin = [&](const GainMap& map, bool horizontal) {
        double origin = horizontal ? map.originH : map.originV;
        const double spacing = horizontal ? map.spacingH : map.spacingV;
        size_t compatible = 0;
        for (const auto& candidate : maps) {
            if (candidate.channels != 1 || candidate.rowPitch != 2 ||
                candidate.colPitch != 2 || candidate.right != map.right ||
                candidate.bottom != map.bottom || candidate.width != map.width ||
                candidate.height != map.height || candidate.spacingH != map.spacingH ||
                candidate.spacingV != map.spacingV)
                continue;
            origin = std::min(origin, horizontal ? candidate.originH : candidate.originV);
            ++compatible;
        }
        if (!motionCamProducer || compatible < 4 || spacing <= 0.0)
            return horizontal ? map.originH : map.originV;

        // MotionCam's four scalar CFA opcodes encode the same physical lens
        // shading grid, but older files add half a grid interval to MapOrigin
        // for the odd row/column phases. Applying those origins independently
        // displaces the optical center by about 128 sensor pixels. Keep the
        // phase selection in Top/Left and use the common grid origin for bake.
        const double encoded = horizontal ? map.originH : map.originV;
        const double halfStep = spacing * 0.5;
        if (std::abs(encoded - origin) < 1e-9 ||
            std::abs(encoded - origin - halfStep) < 1e-9)
            return origin;
        return encoded;
    };
    std::vector<std::pair<double, double>> effectiveOrigins;
    effectiveOrigins.reserve(maps.size());
    for (const auto& map : maps)
        effectiveOrigins.emplace_back(
            sharedScalarOrigin(map, true), sharedScalarOrigin(map, false));

    auto mapGain = [&](const GainMap& map, uint32_t x, uint32_t y) {
        if (x < map.left || x >= map.right || y < map.top || y >= map.bottom ||
            !map.rowPitch || !map.colPitch) return 1.0f;
        // Scalar maps select one CFA phase through their origin and pitch.
        // Multi-channel maps contain the phase values themselves and cover the
        // complete pixel grid; applying the scalar pitch test to them leaves
        // only one corrected pixel in every 2x2 block.
        if (map.channels == 1) {
            if (groupedScalarPhaseMaps && map.rowPitch == 2 && map.colPitch == 2) {
                // Canonical scalar phase maps offset top/left from the original
                // GainMap bounds. Determine phase relative to that group origin,
                // not absolute image parity: a cropped map may start on an odd
                // sensor row or column.
                uint32_t groupTop = map.top, groupLeft = map.left;
                for (const auto& candidate : maps) {
                    if (candidate.channels == 1 && candidate.rowPitch == 2 &&
                        candidate.colPitch == 2 && candidate.right == map.right &&
                        candidate.bottom == map.bottom && candidate.width == map.width &&
                        candidate.height == map.height && candidate.originH == map.originH &&
                        candidate.originV == map.originV && candidate.spacingH == map.spacingH &&
                        candidate.spacingV == map.spacingV) {
                        groupTop = std::min(groupTop, candidate.top);
                        groupLeft = std::min(groupLeft, candidate.left);
                    }
                }
                const uint32_t mapPhase = ((map.top - groupTop) & 1u) * 2u +
                                          ((map.left - groupLeft) & 1u);
                const uint32_t mapPhaseY = mapPhase / 2u;
                const uint32_t mapPhaseX = mapPhase % 2u;
                const size_t pixelPhase = gainMapPhaseChannel(
                    x, y, groupLeft, groupTop, phaseGroup);
                if (pixelPhase != mapPhaseY * 2u + mapPhaseX) return 1.0f;
            } else if ((y - map.top) % map.rowPitch ||
                       (x - map.left) % map.colPitch) {
                return 1.0f;
            }
        }
        // Match dng_gain_map_interpolator from the Adobe DNG SDK. Gain-map
        // coordinates are evaluated at pixel centers within the image bounds;
        // using the integer pixel corner offsets the map on both axes and is
        // particularly visible between four interleaved CFA phase maps.
        const double nx = (static_cast<double>(x) + 0.5) / std::max(1u, width);
        const double ny = (static_cast<double>(y) + 0.5) / std::max(1u, height);
        const size_t mapIndex = static_cast<size_t>(&map - maps.data());
        const double originH = effectiveOrigins[mapIndex].first;
        const double originV = effectiveOrigins[mapIndex].second;
        const double gx = map.spacingH > 0 ? (nx - originH) / map.spacingH : 0;
        const double gy = map.spacingV > 0 ? (ny - originV) / map.spacingV : 0;
        const size_t x0 = std::min<size_t>(map.width - 1, static_cast<size_t>(std::max(0.0, std::floor(gx))));
        const size_t y0 = std::min<size_t>(map.height - 1, static_cast<size_t>(std::max(0.0, std::floor(gy))));
        const size_t x1 = std::min<size_t>(map.width - 1, x0 + 1), y1 = std::min<size_t>(map.height - 1, y0 + 1);
        const float fx = static_cast<float>(std::clamp(gx - std::floor(gx), 0.0, 1.0));
        const float fy = static_cast<float>(std::clamp(gy - std::floor(gy), 0.0, 1.0));
        const size_t channel = map.channels >= 4
            ? std::min<size_t>(map.channels - 1,
                gainMapPhaseChannel(x, y, map.left, map.top, phaseGroup))
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
    uint32_t sourceBits = 1;
    while (sourceBits < 16 && ((uint32_t{1} << sourceBits) - 1) < white) ++sourceBits;
    const uint32_t destinationBits = std::min<uint32_t>(
        16, sourceBits + (normalizeGainMaps ? 4u : 2u));
    const double destinationWhite = static_cast<double>(
        (uint32_t{1} << destinationBits) - 1);
    for (uint32_t y = 0; y < height; ++y) for (uint32_t x = 0; x < width; ++x) {
        float gain = 1.0f;
        for (const auto& map : maps) gain *= mapGain(map, x, y);
        if (colorOnly && !gridColorSeparated) {
            float localMinimum = gain;
            const uint32_t repeat = phaseGroup * 2u;
            const uint32_t groupLeft = (x / repeat) * repeat;
            const uint32_t groupTop = (y / repeat) * repeat;
            for (uint32_t phaseY = 0; phaseY < 2; ++phaseY)
                for (uint32_t phaseX = 0; phaseX < 2; ++phaseX) {
                    float phaseGain = 1.0f;
                    const uint32_t px = std::min(
                        width - 1, groupLeft + phaseX * phaseGroup);
                    const uint32_t py = std::min(
                        height - 1, groupTop + phaseY * phaseGroup);
                    for (const auto& map : maps) phaseGain *= mapGain(map, px, py);
                    localMinimum = std::min(localMinimum, phaseGain);
                }
            if (localMinimum > 0) gain /= localMinimum;
        }
        if (normalizeGainMaps) gain /= maximumGain;
        const size_t index = static_cast<size_t>(y) * width + x;
        if (debugGainMap) {
            const double displayGain = gain > 0.0f ? 1.0 / gain : 0.0;
            pixels[index] = static_cast<uint16_t>(std::clamp(
                std::lround(displayGain * destinationWhite), 0l, 65535l));
            continue;
        }
        const double linear = std::max(0.0, (pixels[index] - black) / std::max(1.0, white - black));
        pixels[index] = static_cast<uint16_t>(std::clamp(std::lround(linear * gain * destinationWhite), 0l, 65535l));
    }

    const uint32_t newBytes = width * height * 2;
    std::vector<uint8_t> replacement(newBytes);
    for (size_t i = 0; i < pixels.size(); ++i) {
        if (little) { replacement[i*2] = pixels[i] & 0xff; replacement[i*2+1] = pixels[i] >> 8; }
        else { replacement[i*2] = pixels[i] >> 8; replacement[i*2+1] = pixels[i] & 0xff; }
    }
    auto writeScalar = [&](const TiffEntry& e, uint32_t value, uint32_t index = 0) {
        const size_t pos = e.valueOffset + index * (e.type == TIFF_TYPE_SHORT ? 2 : 4);
        if (e.type == TIFF_TYPE_SHORT) write16(data.data() + pos, static_cast<uint16_t>(value), little);
        else write32(data.data() + pos, value, little);
    };
    writeScalar(*bitsE, 16); writeScalar(*compressionE, TIFF_COMPRESSION_NONE);
    writeScalar(*offsetsE, stripOffset); writeScalar(*countsE, newBytes);
    if (whiteE) writeScalar(*whiteE, static_cast<uint32_t>(destinationWhite));
    if (blackE) {
        if (blackE->type == TIFF_TYPE_RATIONAL)
            for (uint32_t i = 0; i < blackE->count; ++i) writeRational(data, *blackE, i, 0.0, little);
        else for (uint32_t i = 0; i < blackE->count; ++i) writeScalar(*blackE, 0, i);
    }
    if (colorOnly) {
        if (data.size() & 1u) data.push_back(0);
        const uint32_t opcodeOffset = static_cast<uint32_t>(data.size());
        data.insert(data.end(), luminanceOpcodes.begin(), luminanceOpcodes.end());
        write16(data.data() + opcodeE->entryOffset, TIFF_TAG_OPCODE_LIST_3, little);
        write32(data.data() + opcodeE->entryOffset + 4,
                static_cast<uint32_t>(luminanceOpcodes.size()), little);
        write32(data.data() + opcodeE->entryOffset + 8, opcodeOffset, little);
        for (const auto& entry : entries) {
            if (entry.tag != TIFF_TAG_DNG_BACKWARD_VERSION ||
                entry.type != TIFF_TYPE_BYTE || entry.count < 4)
                continue;
            if (data[entry.valueOffset] < 1 ||
                (data[entry.valueOffset] == 1 && data[entry.valueOffset + 1] < 3)) {
                data[entry.valueOffset] = 1;
                data[entry.valueOffset + 1] = 3;
                data[entry.valueOffset + 2] = 0;
                data[entry.valueOffset + 3] = 0;
            }
        }
    } else {
        // Keep a valid OpcodeList2 tag, but make its list empty to prevent double application.
        write32(data.data() + opcodeE->entryOffset + 4, 4, little);
        write32(data.data() + opcodeE->entryOffset + 8, 0, false);
    }
    if (luminanceBaseline.has_value() &&
        !updateMetadata(data, &*luminanceBaseline, nullptr))
        return false;
    return replaceTiffStrip(data, stripOffset, stripBytes, replacement, little);
}

bool DNGDecoder::transformGainMaps(std::vector<uint8_t>& data, bool normalizeGainMaps,
                                   bool colorOnly, bool optimizeGainMaps) {
    bool little = true;
    const auto entries = findTiffEntries(data, little);
    const TiffEntry* opcode = nullptr;
    for (const auto& entry : entries)
        if (entry.tag == TIFF_TAG_OPCODE_LIST_2) { opcode = &entry; break; }
    if (!opcode || !opcode->count) return false;
    std::vector<GainMap> maps;
    if (!parseOpcodeGainMaps(data.data() + opcode->valueOffset, opcode->count, maps))
        return false;
    for (const auto& map : maps)
        if (map.channels != 1 && map.channels != 4) return false;

    // Keep the complete opcode list intact. parseOpcodeGainMaps deliberately
    // returns only GainMap opcodes, so remember the payload offsets separately
    // and overwrite only those payloads after transforming them.
    std::vector<std::pair<size_t, size_t>> payloads;
    const uint8_t* opcodeData = data.data() + opcode->valueOffset;
    const uint32_t opcodeCount = readBE32(opcodeData);
    size_t offset = 4;
    for (uint32_t i = 0; i < opcodeCount; ++i) {
        if (offset + 16 > opcode->count) return false;
        const uint32_t id = readBE32(opcodeData + offset);
        const uint32_t bytes = readBE32(opcodeData + offset + 12);
        offset += 16;
        if (bytes > opcode->count - offset) return false;
        if (id == OPCODE_GAIN_MAP) payloads.emplace_back(offset, bytes);
        offset += bytes;
    }
    if (payloads.size() != maps.size()) return false;

    std::array<uint8_t, 4> cfa{0, 1, 1, 2};
    int repeat = 2;
    getCFAMetadata(data, repeat, cfa);
    auto sampleColor = [&](const GainMap& map, size_t sample) {
        if (map.channels == 4)
            return static_cast<size_t>(std::min<uint8_t>(2, cfa[sample % 4]));
        // Four-map DNGs normally select their CFA phase through the opcode's
        // top/left origin and row/column pitch.
        const size_t phase = ((map.top & 1u) << 1u) | (map.left & 1u);
        return static_cast<size_t>(std::min<uint8_t>(2, cfa[phase]));
    };
    std::array<float, 3> minima{std::numeric_limits<float>::max(),
                                std::numeric_limits<float>::max(),
                                std::numeric_limits<float>::max()};
    if (optimizeGainMaps) {
        for (const auto& map : maps)
            for (size_t i = 0; i < map.data.size(); ++i) {
                const float gain = map.data[i];
                if (std::isfinite(gain) && gain > 0.0f) {
                    const size_t color = sampleColor(map, i);
                    minima[color] = std::min(minima[color], gain);
                }
            }
        if (maps.size() == 1 && maps.front().channels == 1)
            minima[0] = minima[2] = minima[1];
        for (float& value : minima)
            if (!std::isfinite(value) || value <= 0.0f) value = 1.0f;
        const float common = std::min({minima[0], minima[1], minima[2]});
        for (auto& map : maps)
            for (size_t i = 0; i < map.data.size(); ++i) {
                const size_t color = sampleColor(map, i);
                if (std::isfinite(map.data[i]) && map.data[i] > 0.0f)
                    map.data[i] /= minima[color];
            }
        DNGFrameMetadata metadata;
        if (!getColorMetadata(data, metadata)) return false;
        double baseline = metadata.baselineExposure + std::log2(common);
        auto neutral = metadata.asShotNeutral;
        for (size_t color = 0; color < 3; ++color)
            neutral[color] *= common / minima[color];
        if (!updateMetadata(data, &baseline, &neutral)) return false;
    }
    if (colorOnly) {
        for (auto& map : maps) {
            if (map.channels != 4) continue;
            const size_t points = static_cast<size_t>(map.width) * map.height;
            for (size_t point = 0; point < points; ++point) {
                float minimum = std::numeric_limits<float>::max();
                for (size_t channel = 0; channel < 4; ++channel)
                    minimum = std::min(minimum, map.data[point * 4 + channel]);
                if (std::isfinite(minimum) && minimum > 0.0f)
                    for (size_t channel = 0; channel < 4; ++channel)
                        map.data[point * 4 + channel] /= minimum;
            }
        }
        std::vector<GainMap*> scalarMaps;
        for (auto& map : maps)
            if (map.channels == 1) scalarMaps.push_back(&map);
        if (scalarMaps.size() == 1) {
            std::fill(scalarMaps.front()->data.begin(), scalarMaps.front()->data.end(), 1.0f);
        } else if (!scalarMaps.empty()) {
            const size_t samples = scalarMaps.front()->data.size();
            for (const auto* map : scalarMaps)
                if (map->data.size() != samples) return false;
            for (size_t sample = 0; sample < samples; ++sample) {
                float minimum = std::numeric_limits<float>::max();
                for (const auto* map : scalarMaps)
                    minimum = std::min(minimum, map->data[sample]);
                if (std::isfinite(minimum) && minimum > 0.0f)
                    for (auto* map : scalarMaps) map->data[sample] /= minimum;
            }
        }
    }
    if (normalizeGainMaps) {
        float maximum = 0.0f;
        for (const auto& map : maps)
            for (float gain : map.data)
                if (std::isfinite(gain)) maximum = std::max(maximum, gain);
        if (maximum > 0.0f)
            for (auto& map : maps)
                for (float& gain : map.data) gain /= maximum;
    }
    for (size_t i = 0; i < maps.size(); ++i) {
        const auto encoded = serializeGainMap(maps[i]);
        if (encoded.size() < 20 || encoded.size() - 20 != payloads[i].second) return false;
        std::copy(encoded.begin() + 20, encoded.end(),
                  data.begin() + opcode->valueOffset + payloads[i].first);
    }
    return true;
}

bool DNGDecoder::repairGainMapCfaPhase(
        std::vector<uint8_t>& data, std::optional<bool> sidecarOverride) {
    bool little = true;
    const auto entries = findTiffEntries(data, little);
    const TiffEntry* opcode = nullptr;
    bool affectedProducer = false;
    for (const auto& entry : entries) {
        if (entry.tag == TIFF_TAG_OPCODE_LIST_2) opcode = &entry;
        if (entry.tag == TIFF_TAG_SOFTWARE && entry.count &&
            entry.valueOffset + entry.count <= data.size()) {
            std::string software(
                reinterpret_cast<const char*>(data.data() + entry.valueOffset), entry.count);
            affectedProducer |= hasAffectedGainMapOrder(software);
        }
    }
    const bool repair = sidecarOverride.value_or(affectedProducer);
    if (!repair || !opcode || opcode->count < 4) return true;

    int repeat = 0;
    std::array<uint8_t, 4> cfa{};
    if (!getCFAMetadata(data, repeat, cfa) || repeat != 2) return true;
    const std::array<uint8_t, 4> canonical{0, 1, 1, 2}; // RGGB opcode order
    if (cfa == canonical) return true;

    std::vector<GainMap> maps;
    if (!parseOpcodeGainMaps(data.data() + opcode->valueOffset, opcode->count, maps))
        return false;
    if (maps.size() != 4) return true;
    for (size_t i = 0; i < maps.size(); ++i) {
        const auto& map = maps[i];
        if (map.channels != 1 || map.rowPitch != 2 || map.colPitch != 2 ||
            (((map.top & 1u) << 1u) | (map.left & 1u)) != i)
            return true;
    }

    std::array<uint32_t, 4> target{};
    std::array<bool, 4> used{};
    for (size_t sourcePhase = 0; sourcePhase < canonical.size(); ++sourcePhase) {
        for (size_t destinationPhase = 0; destinationPhase < cfa.size(); ++destinationPhase) {
            if (!used[destinationPhase] && cfa[destinationPhase] == canonical[sourcePhase]) {
                target[sourcePhase] = static_cast<uint32_t>(destinationPhase);
                used[destinationPhase] = true;
                break;
            }
        }
    }
    if (!std::all_of(used.begin(), used.end(), [](bool value) { return value; }))
        return true;

    std::vector<std::pair<size_t, size_t>> payloads;
    const uint8_t* list = data.data() + opcode->valueOffset;
    const uint32_t count = readBE32(list);
    size_t offset = 4;
    for (uint32_t i = 0; i < count; ++i) {
        if (offset + 16 > opcode->count) return false;
        const uint32_t id = readBE32(list + offset);
        const uint32_t bytes = readBE32(list + offset + 12);
        offset += 16;
        if (bytes > opcode->count - offset) return false;
        if (id == OPCODE_GAIN_MAP) payloads.emplace_back(offset, bytes);
        offset += bytes;
    }
    if (offset != opcode->count || payloads.size() != maps.size()) return false;
    for (size_t i = 0; i < maps.size(); ++i) {
        maps[i].top = (maps[i].top & ~1u) + target[i] / 2;
        maps[i].left = (maps[i].left & ~1u) + target[i] % 2;
        const auto encoded = serializeGainMap(maps[i]);
        if (encoded.size() < 20 || encoded.size() - 20 != payloads[i].second)
            return false;
        std::copy(encoded.begin() + 20, encoded.end(),
                  data.begin() + opcode->valueOffset + payloads[i].first);
    }
    spdlog::warn("Corrected legacy MotionCam OpcodeList2 gain-map phase from RGGB to CFA pattern {}{}{}{}",
                 cfa[0], cfa[1], cfa[2], cfa[3]);
    return true;
}

bool DNGDecoder::cropGainMapsToFullSensor(
        std::vector<uint8_t>& data, uint32_t fullWidth, uint32_t fullHeight) {
    bool little = true;
    const auto entries = findTiffEntries(data, little);
    const TiffEntry* photo = nullptr;
    auto scalar = [&](const TiffEntry& entry) -> uint32_t {
        return entry.type == TIFF_TYPE_SHORT
            ? read16(data.data() + entry.valueOffset, little)
            : read32(data.data() + entry.valueOffset, little);
    };
    for (const auto& entry : entries) {
        if (entry.tag == TIFF_TAG_PHOTOMETRIC && entry.count &&
            scalar(entry) == TIFF_PHOTOMETRIC_CFA) photo = &entry;
    }
    if (!photo) return true;
    const TiffEntry* opcode = nullptr;
    const TiffEntry* widthE = nullptr;
    const TiffEntry* heightE = nullptr;
    for (const auto& entry : entries) {
        if (entry.ifdOffset != photo->ifdOffset) continue;
        if (entry.tag == TIFF_TAG_OPCODE_LIST_2) opcode = &entry;
        if (entry.tag == TIFF_TAG_IMAGE_WIDTH) widthE = &entry;
        if (entry.tag == TIFF_TAG_IMAGE_HEIGHT) heightE = &entry;
    }
    if (!opcode || !opcode->count) return true;
    if (!widthE || !heightE) return false;
    const uint32_t width = scalar(*widthE), height = scalar(*heightE);
    if (!width || !height || fullWidth < width || fullHeight < height) return false;
    if (fullWidth == width && fullHeight == height) return true;

    std::vector<GainMap> maps;
    if (!parseOpcodeGainMaps(data.data() + opcode->valueOffset, opcode->count, maps))
        return false;
    if (maps.empty()) return true;
    const double cropLeft = (static_cast<double>(fullWidth) - width) * 0.5;
    const double cropTop = (static_cast<double>(fullHeight) - height) * 0.5;
    for (auto& map : maps) {
        // Some MotionCam gain maps use 1/pointCount instead of
        // 1/(pointCount-1), leaving the final row/column short of the sensor
        // edge. Extending that malformed grid by clamping produces visible
        // repeated bands after crop remapping. A full-sensor map is expected
        // to span the sensor, so normalize only undersized non-negative grids.
        if (map.width > 1 && map.originH >= 0.0 && map.originH < 1.0 &&
            map.originH + map.spacingH * (map.width - 1) < 1.0 - 1e-6)
            map.spacingH = (1.0 - map.originH) / (map.width - 1);
        if (map.height > 1 && map.originV >= 0.0 && map.originV < 1.0 &&
            map.originV + map.spacingV * (map.height - 1) < 1.0 - 1e-6)
            map.spacingV = (1.0 - map.originV) / (map.height - 1);

        // Materialize the cropped subset into a new grid instead of relying on
        // negative origins and expanded spacing. Some raw processors scale the
        // grid directly to Top/Left/Bottom/Right and ignore that geometry.
        const std::vector<float> source = map.data;
        auto sourceAt = [&](size_t x, size_t y, uint32_t channel) {
            return source[(y * map.width + x) * map.channels + channel];
        };
        for (uint32_t y = 0; y < map.height; ++y) {
            const double outputV = map.height > 1
                ? static_cast<double>(y) / (map.height - 1) : 0.5;
            const double sensorV = (cropTop + outputV * height) / fullHeight;
            const double sourceV = map.spacingV > 0.0
                ? (sensorV - map.originV) / map.spacingV : 0.0;
            const size_t y0 = std::min<size_t>(map.height - 1,
                static_cast<size_t>(std::max(0.0, std::floor(sourceV))));
            const size_t y1 = std::min<size_t>(map.height - 1, y0 + 1);
            const float fy = static_cast<float>(std::clamp(sourceV - y0, 0.0, 1.0));
            for (uint32_t x = 0; x < map.width; ++x) {
                const double outputH = map.width > 1
                    ? static_cast<double>(x) / (map.width - 1) : 0.5;
                const double sensorH = (cropLeft + outputH * width) / fullWidth;
                const double sourceH = map.spacingH > 0.0
                    ? (sensorH - map.originH) / map.spacingH : 0.0;
                const size_t x0 = std::min<size_t>(map.width - 1,
                    static_cast<size_t>(std::max(0.0, std::floor(sourceH))));
                const size_t x1 = std::min<size_t>(map.width - 1, x0 + 1);
                const float fx = static_cast<float>(std::clamp(sourceH - x0, 0.0, 1.0));
                for (uint32_t channel = 0; channel < map.channels; ++channel) {
                    map.data[(static_cast<size_t>(y) * map.width + x) * map.channels + channel] =
                        (sourceAt(x0, y0, channel) * (1-fx) + sourceAt(x1, y0, channel) * fx) * (1-fy) +
                        (sourceAt(x0, y1, channel) * (1-fx) + sourceAt(x1, y1, channel) * fx) * fy;
                }
            }
        }
        map.originH = 0.0;
        map.originV = 0.0;
        map.spacingH = map.width > 1 ? 1.0 / (map.width - 1) : 1.0;
        map.spacingV = map.height > 1 ? 1.0 / (map.height - 1) : 1.0;
    }

    std::vector<std::pair<size_t, size_t>> payloads;
    const uint8_t* list = data.data() + opcode->valueOffset;
    const uint32_t count = readBE32(list);
    size_t offset = 4;
    for (uint32_t i = 0; i < count; ++i) {
        if (offset + 16 > opcode->count) return false;
        const uint32_t id = readBE32(list + offset);
        const uint32_t bytes = readBE32(list + offset + 12);
        offset += 16;
        if (bytes > opcode->count - offset) return false;
        if (id == OPCODE_GAIN_MAP) payloads.emplace_back(offset, bytes);
        offset += bytes;
    }
    if (offset != opcode->count || payloads.size() != maps.size()) return false;
    for (size_t i = 0; i < maps.size(); ++i) {
        const auto encoded = serializeGainMap(maps[i]);
        if (encoded.size() < 20 || encoded.size() - 20 != payloads[i].second)
            return false;
        std::copy(encoded.begin() + 20, encoded.end(),
                  data.begin() + opcode->valueOffset + payloads[i].first);
    }
    spdlog::info(
        "Mapped {}x{} full-sensor gain maps onto centered {}x{} crop",
        fullWidth, fullHeight, width, height);
    return true;
}

bool DNGDecoder::canonicalizeGainMapOpcodes(std::vector<uint8_t>& data) {
    bool little = true;
    const auto entries = findTiffEntries(data, little);
    auto rewrite = [&](uint16_t tag, bool cfaPhases) -> bool {
        const TiffEntry* entry = nullptr;
        for (const auto& candidate : entries)
            if (candidate.tag == tag) { entry = &candidate; break; }
        if (!entry || !entry->count) return true;

        const uint8_t* source = data.data() + entry->valueOffset;
        if (entry->count < 4) return false;
        const uint32_t count = readBE32(source);
        size_t offset = 4;
        uint32_t outputCount = 0;
        bool changed = false;
        std::vector<uint8_t> output(4, 0);
        for (uint32_t i = 0; i < count; ++i) {
            if (offset + 16 > entry->count) return false;
            const uint32_t id = readBE32(source + offset);
            const uint32_t bytes = readBE32(source + offset + 12);
            if (bytes > entry->count - offset - 16) return false;
            const size_t opcodeSize = 16 + bytes;
            if (id != OPCODE_GAIN_MAP) {
                output.insert(output.end(), source + offset, source + offset + opcodeSize);
                ++outputCount;
                offset += opcodeSize;
                continue;
            }

            std::vector<uint8_t> one(4, 0);
            one[3] = 1;
            one.insert(one.end(), source + offset, source + offset + opcodeSize);
            std::vector<GainMap> maps;
            if (!parseOpcodeGainMaps(one.data(), one.size(), maps) || maps.size() != 1)
                return false;
            const GainMap& map = maps.front();
            if (map.channels == 1) {
                output.insert(output.end(), source + offset, source + offset + opcodeSize);
                ++outputCount;
            } else {
                if (cfaPhases && map.channels != 4) return false;
                changed = true;
                const size_t points = static_cast<size_t>(map.width) * map.height;
                for (uint32_t channel = 0; channel < map.channels; ++channel) {
                    GainMap single = map;
                    single.channels = 1;
                    single.data.resize(points);
                    for (size_t point = 0; point < points; ++point)
                        single.data[point] = map.data[point * map.channels + channel];
                    if (cfaPhases) {
                        single.top = map.top + channel / 2;
                        single.left = map.left + channel % 2;
                        single.rowPitch = 2;
                        single.colPitch = 2;
                    } else {
                        single.plane = map.plane + channel;
                        single.planes = 1;
                    }
                    const auto encoded = serializeGainMap(single);
                    output.insert(output.end(), encoded.begin() + 4, encoded.end());
                    ++outputCount;
                }
            }
            offset += opcodeSize;
        }
        if (offset != entry->count) return false;
        if (!changed) return true;
        output[0] = static_cast<uint8_t>(outputCount >> 24);
        output[1] = static_cast<uint8_t>(outputCount >> 16);
        output[2] = static_cast<uint8_t>(outputCount >> 8);
        output[3] = static_cast<uint8_t>(outputCount);
        if (data.size() & 1u) data.push_back(0);
        const uint32_t newOffset = static_cast<uint32_t>(data.size());
        data.insert(data.end(), output.begin(), output.end());
        write32(data.data() + entry->entryOffset + 4,
                static_cast<uint32_t>(output.size()), little);
        write32(data.data() + entry->entryOffset + 8, newOffset, little);
        return true;
    };
    return rewrite(TIFF_TAG_OPCODE_LIST_2, true) &&
           rewrite(TIFF_TAG_OPCODE_LIST_3, false);
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
