#define TINY_DNG_WRITER_IMPLEMENTATION
#include "tinydng/tiny_dng_writer.h"
#include "DNGDecoder.h"

#include <jxl/decode.h>

#include <algorithm>
#include <cassert>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

static std::vector<uint8_t> writeDng(tinydngwriter::DNGImage& image) {
    tinydngwriter::DNGWriter writer(false);
    assert(writer.AddImage(&image));
    std::ostringstream stream(std::ios::binary);
    std::string error;
    assert(writer.WriteToFile(stream, &error));
    const std::string bytes = stream.str();
    return {bytes.begin(), bytes.end()};
}

static std::vector<uint16_t> decode(const std::vector<unsigned char>& encoded,
                                    unsigned int width, unsigned int height,
                                    unsigned int channels = 1) {
    JxlDecoder* decoder = JxlDecoderCreate(nullptr);
    assert(decoder);
    assert(JxlDecoderSubscribeEvents(decoder, JXL_DEC_BASIC_INFO | JXL_DEC_FULL_IMAGE) == JXL_DEC_SUCCESS);
    assert(JxlDecoderSetInput(decoder, encoded.data(), encoded.size()) == JXL_DEC_SUCCESS);
    JxlDecoderCloseInput(decoder);
    std::vector<uint16_t> result(width * height * channels);
    const JxlPixelFormat format = {channels, JXL_TYPE_UINT16, JXL_NATIVE_ENDIAN, 0};
    bool gotImage = false;
    for (;;) {
        const auto status = JxlDecoderProcessInput(decoder);
        if (status == JXL_DEC_BASIC_INFO) {
            JxlBasicInfo info;
            assert(JxlDecoderGetBasicInfo(decoder, &info) == JXL_DEC_SUCCESS);
            assert(info.xsize == width && info.ysize == height && info.bits_per_sample == 16);
        } else if (status == JXL_DEC_NEED_IMAGE_OUT_BUFFER) {
            assert(JxlDecoderSetImageOutBuffer(decoder, &format, result.data(),
                                               result.size() * sizeof(uint16_t)) == JXL_DEC_SUCCESS);
        } else if (status == JXL_DEC_FULL_IMAGE) {
            gotImage = true;
        } else if (status == JXL_DEC_SUCCESS) {
            break;
        } else {
            assert(false && "JPEG XL decode failed");
        }
    }
    JxlDecoderDestroy(decoder);
    assert(gotImage);
    return result;
}

static uint32_t tiffTagValue(const std::vector<uint8_t>& dng, uint16_t wantedTag) {
    assert(dng.size() >= 8 && dng[0] == 'I' && dng[1] == 'I');
    auto u16 = [&](size_t offset) {
        assert(offset + 2 <= dng.size());
        return static_cast<uint16_t>(dng[offset] | dng[offset + 1] << 8);
    };
    auto u32 = [&](size_t offset) {
        assert(offset + 4 <= dng.size());
        return static_cast<uint32_t>(dng[offset] | dng[offset + 1] << 8 |
            dng[offset + 2] << 16 | dng[offset + 3] << 24);
    };
    for (uint32_t ifd = u32(4); ifd;) {
        const uint16_t count = u16(ifd);
        for (uint16_t i = 0; i < count; ++i) {
            const size_t entry = static_cast<size_t>(ifd) + 2 + i * 12;
            if (u16(entry) == wantedTag) {
                const uint16_t type = u16(entry + 2);
                const uint32_t values = u32(entry + 4);
                const uint32_t typeSize = type == 1 ? 1 : type == 3 ? 2 : 4;
                const size_t valueOffset = values * typeSize > 4 ? u32(entry + 8) : entry + 8;
                return type == 1 ? dng[valueOffset] : type == 3 ? u16(valueOffset) : u32(valueOffset);
            }
        }
        ifd = u32(static_cast<size_t>(ifd) + 2 + count * 12);
    }
    assert(false && "TIFF tag not found");
    return 0;
}

static uint16_t tiffTagType(const std::vector<uint8_t>& dng, uint16_t wantedTag) {
    assert(dng.size() >= 8 && dng[0] == 'I' && dng[1] == 'I');
    auto u16 = [&](size_t offset) {
        return static_cast<uint16_t>(dng[offset] | dng[offset + 1] << 8);
    };
    auto u32 = [&](size_t offset) {
        return static_cast<uint32_t>(dng[offset] | dng[offset + 1] << 8 |
            dng[offset + 2] << 16 | dng[offset + 3] << 24);
    };
    for (uint32_t ifd = u32(4); ifd;) {
        const uint16_t count = u16(ifd);
        for (uint16_t i = 0; i < count; ++i) {
            const size_t entry = static_cast<size_t>(ifd) + 2 + i * 12;
            if (u16(entry) == wantedTag) return u16(entry + 2);
        }
        ifd = u32(static_cast<size_t>(ifd) + 2 + count * 12);
    }
    assert(false && "TIFF tag not found");
    return 0;
}

static std::vector<uint8_t> tiffByteTagValues(const std::vector<uint8_t>& dng,
                                              uint16_t wantedTag) {
    auto u16 = [&](size_t offset) {
        return static_cast<uint16_t>(dng[offset] | dng[offset + 1] << 8);
    };
    auto u32 = [&](size_t offset) {
        return static_cast<uint32_t>(dng[offset] | dng[offset + 1] << 8 |
            dng[offset + 2] << 16 | dng[offset + 3] << 24);
    };
    for (uint32_t ifd = u32(4); ifd;) {
        const uint16_t count = u16(ifd);
        for (uint16_t i = 0; i < count; ++i) {
            const size_t entry = static_cast<size_t>(ifd) + 2 + i * 12;
            if (u16(entry) != wantedTag || u16(entry + 2) != 1) continue;
            const uint32_t values = u32(entry + 4);
            const size_t offset = values > 4 ? u32(entry + 8) : entry + 8;
            assert(offset <= dng.size() && values <= dng.size() - offset);
            return {dng.begin() + offset, dng.begin() + offset + values};
        }
        ifd = u32(static_cast<size_t>(ifd) + 2 + count * 12);
    }
    assert(false && "TIFF BYTE tag not found");
    return {};
}

static void rewriteTiffTag(std::vector<uint8_t>& dng, uint16_t oldTag,
                           uint16_t newTag, uint16_t newType = 0,
                           uint32_t newValue = 0) {
    auto u16 = [&](size_t at) { return static_cast<uint16_t>(dng[at] | dng[at + 1] << 8); };
    auto u32 = [&](size_t at) { return static_cast<uint32_t>(dng[at] | dng[at + 1] << 8 |
        dng[at + 2] << 16 | dng[at + 3] << 24); };
    auto put16 = [&](size_t at, uint16_t value) {
        dng[at] = value & 0xff; dng[at + 1] = value >> 8;
    };
    auto put32 = [&](size_t at, uint32_t value) {
        for (int i = 0; i < 4; ++i) dng[at + i] = static_cast<uint8_t>(value >> (i * 8));
    };
    const uint32_t ifd = u32(4);
    for (uint16_t i = 0; i < u16(ifd); ++i) {
        const size_t at = static_cast<size_t>(ifd) + 2 + i * 12;
        if (u16(at) != oldTag) continue;
        put16(at, newTag);
        if (newType) {
            put16(at + 2, newType); put32(at + 4, 1); put32(at + 8, newValue);
        }
        return;
    }
    assert(false && "TIFF tag to rewrite not found");
}

static std::array<uint32_t, 4> tiffLong4Tag(const std::vector<uint8_t>& dng,
                                            uint16_t wantedTag) {
    auto u16 = [&](size_t offset) {
        assert(offset + 2 <= dng.size());
        return static_cast<uint16_t>(dng[offset] | dng[offset + 1] << 8);
    };
    auto u32 = [&](size_t offset) {
        assert(offset + 4 <= dng.size());
        return static_cast<uint32_t>(dng[offset] | dng[offset + 1] << 8 |
            dng[offset + 2] << 16 | dng[offset + 3] << 24);
    };
    for (uint32_t ifd = u32(4); ifd;) {
        const uint16_t count = u16(ifd);
        for (uint16_t i = 0; i < count; ++i) {
            const size_t entry = static_cast<size_t>(ifd) + 2 + i * 12;
            if (u16(entry) == wantedTag) {
                assert(u16(entry + 2) == 4 && u32(entry + 4) == 4);
                const size_t offset = u32(entry + 8);
                return {u32(offset), u32(offset + 4), u32(offset + 8), u32(offset + 12)};
            }
        }
        ifd = u32(static_cast<size_t>(ifd) + 2 + count * 12);
    }
    assert(false && "TIFF LONG[4] tag not found");
    return {};
}

static std::array<uint8_t, 4> tiffByteTag(const std::vector<uint8_t>& dng,
                                         uint16_t wantedTag) {
    auto u16 = [&](size_t offset) {
        assert(offset + 2 <= dng.size());
        return static_cast<uint16_t>(dng[offset] | dng[offset + 1] << 8);
    };
    auto u32 = [&](size_t offset) {
        assert(offset + 4 <= dng.size());
        return static_cast<uint32_t>(dng[offset] | dng[offset + 1] << 8 |
            dng[offset + 2] << 16 | dng[offset + 3] << 24);
    };
    for (uint32_t ifd = u32(4); ifd;) {
        const uint16_t count = u16(ifd);
        for (uint16_t i = 0; i < count; ++i) {
            const size_t entry = static_cast<size_t>(ifd) + 2 + i * 12;
            if (u16(entry) == wantedTag) {
                assert(u16(entry + 2) == 1 && u32(entry + 4) == 4);
                return {dng[entry + 8], dng[entry + 9],
                        dng[entry + 10], dng[entry + 11]};
            }
        }
        ifd = u32(static_cast<size_t>(ifd) + 2 + count * 12);
    }
    assert(false && "TIFF byte tag not found");
    return {};
}

static void setTiffByteTag(std::vector<uint8_t>& dng, uint16_t wantedTag,
                           const std::array<uint8_t, 4>& value) {
    auto u16 = [&](size_t offset) {
        return static_cast<uint16_t>(dng[offset] | dng[offset + 1] << 8);
    };
    auto u32 = [&](size_t offset) {
        return static_cast<uint32_t>(dng[offset] | dng[offset + 1] << 8 |
            dng[offset + 2] << 16 | dng[offset + 3] << 24);
    };
    for (uint32_t ifd = u32(4); ifd;) {
        const uint16_t count = u16(ifd);
        for (uint16_t i = 0; i < count; ++i) {
            const size_t entry = static_cast<size_t>(ifd) + 2 + i * 12;
            if (u16(entry) == wantedTag) {
                assert(u16(entry + 2) == 1 && u32(entry + 4) == 4);
                std::copy(value.begin(), value.end(), dng.begin() + entry + 8);
                return;
            }
        }
        ifd = u32(static_cast<size_t>(ifd) + 2 + count * 12);
    }
    assert(false && "TIFF byte tag not found");
}

int main() {
    // Codec input remains a single raw plane; CFA repeat geometry stays in DNG
    // metadata and therefore works identically for Bayer and higher CFAs.
    for (const unsigned int repeat : {2u, 4u, 6u, 8u}) {
        const unsigned int width = repeat * 8, height = repeat * 6;
        std::vector<uint16_t> cfa(width * height);
        for (unsigned int y = 0; y < height; ++y)
            for (unsigned int x = 0; x < width; ++x)
                cfa[y * width + x] = static_cast<uint16_t>(
                    ((y % repeat) * 733 + (x % repeat) * 991 + x * 13 + y * 29) & 0x3fff);

        std::string error;
        std::vector<unsigned char> lossless;
        assert(tinydngwriter::CompressJPEGXL(
            reinterpret_cast<const unsigned char*>(cfa.data()), cfa.size() * sizeof(uint16_t),
            width, height, 1, 0.0f, lossless, &error));
        assert(decode(lossless, width, height) == cfa);

        std::vector<unsigned char> lossy;
        assert(tinydngwriter::CompressJPEGXL(
            reinterpret_cast<const unsigned char*>(cfa.data()), cfa.size() * sizeof(uint16_t),
            width, height, 1, 0.5f, lossy, &error));
        assert(decode(lossy, width, height).size() == cfa.size());

        tinydngwriter::DNGImage image;
        image.SetBigEndian(false);
        const unsigned short bits = 16;
        std::vector<unsigned char> pattern(repeat * repeat, 1);
        assert(image.SetImageWidth(width) && image.SetImageLength(height));
        assert(image.SetRowsPerStrip(height) && image.SetSamplesPerPixel(1));
        assert(image.SetYResolution(300));
        assert(image.SetBitsPerSample(1, &bits));
        assert(image.SetCompression(tinydngwriter::COMPRESSION_JPEG_XL));
        assert(image.SetJXLDistance(0.0f));
        assert(image.SetPhotometric(tinydngwriter::PHOTOMETRIC_CFA));
        assert(image.SetPlanarConfig(tinydngwriter::PLANARCONFIG_CONTIG));
        assert(image.SetCFARepeatPatternDim(repeat, repeat));
        assert(image.SetCFAPattern(static_cast<unsigned int>(pattern.size()), pattern.data()));
        assert(image.SetDNGVersion(1, 7, 0, 0));
        assert(image.SetDNGBackwardVersion(1, 7, 0, 0));
        assert(image.SetImageData(reinterpret_cast<const unsigned char*>(cfa.data()),
                                  cfa.size() * sizeof(uint16_t)));
        assert(image.GetStripBytes() > 0);
        auto losslessDng = writeDng(image);
        assert(tiffTagValue(losslessDng, 259) == 52546);
        auto tiledJxl = losslessDng;
        rewriteTiffTag(tiledJxl, 273, 324);
        rewriteTiffTag(tiledJxl, 278, 322, 4, width);
        rewriteTiffTag(tiledJxl, 279, 325);
        rewriteTiffTag(tiledJxl, 283, 323, 4, height);
        assert(motioncam::DNGDecoder::ensureUncompressed(tiledJxl));
        assert(tiffTagValue(tiledJxl, 259) == 1);
        assert(tiffTagValue(tiledJxl, 279) == cfa.size() * sizeof(uint16_t));
        assert(motioncam::DNGDecoder::ensureUncompressed(losslessDng));
        assert(tiffTagValue(losslessDng, 259) == 1);
        assert(tiffTagValue(losslessDng, 279) == cfa.size() * sizeof(uint16_t));

        auto jpeg92Dng = losslessDng;
        assert(motioncam::DNGDecoder::compressLosslessJPEG(jpeg92Dng));
        assert(tiffTagValue(jpeg92Dng, 259) == 7);
        assert(motioncam::DNGDecoder::ensureUncompressed(jpeg92Dng));
        assert(tiffTagValue(jpeg92Dng, 277) == 1);

        // CinemaDNG keeps every CFA geometry as one raw plane in a 12-bit
        // lossy DCT JPEG stream under Compression=7.
        auto dctDng = losslessDng;
        assert(motioncam::DNGDecoder::compressLossyJPEG(dctDng, 90));
        assert(tiffTagValue(dctDng, 259) == 7);
        assert(tiffTagValue(dctDng, 262) == 32803);
        assert(tiffTagValue(dctDng, 258) == 12);
        assert(tiffTagValue(dctDng, 277) == 1);
        assert(tiffTagValue(dctDng, 322) == width);
        assert(tiffTagValue(dctDng, 323) == height);
        assert(tiffTagValue(dctDng, 324) > 0);
        // Adding TileLength must not consume the existing resolution metadata.
        assert(tiffTagValue(dctDng, 283) > 0);
        assert(motioncam::DNGDecoder::ensureUncompressed(dctDng));
        assert(tiffTagValue(dctDng, 259) == 1);
        assert(tiffTagValue(dctDng, 277) == 1);

        // Exercise the complete lossy CFA DNG, not only the bare codestream.
        // Low-range uint16 samples model DirectLog RGB16 after a reduced-bit
        // log transform and remosaic.
        tinydngwriter::DNGImage lossyImage;
        lossyImage.SetBigEndian(false);
        assert(lossyImage.SetImageWidth(width) && lossyImage.SetImageLength(height));
        assert(lossyImage.SetRowsPerStrip(height) && lossyImage.SetSamplesPerPixel(1));
        assert(lossyImage.SetBitsPerSample(1, &bits));
        assert(lossyImage.SetCompression(tinydngwriter::COMPRESSION_JPEG_XL));
        assert(lossyImage.SetJXLDistance(0.5f));
        assert(lossyImage.SetPhotometric(tinydngwriter::PHOTOMETRIC_CFA));
        assert(lossyImage.SetPlanarConfig(tinydngwriter::PLANARCONFIG_CONTIG));
        assert(lossyImage.SetCFARepeatPatternDim(repeat, repeat));
        assert(lossyImage.SetCFAPattern(static_cast<unsigned int>(pattern.size()), pattern.data()));
        assert(lossyImage.SetWhiteLevel(16383));
        assert(lossyImage.SetDNGVersion(1, 7, 0, 0));
        assert(lossyImage.SetDNGBackwardVersion(1, 7, 0, 0));
        assert(lossyImage.SetImageData(reinterpret_cast<const unsigned char*>(cfa.data()),
                                       cfa.size() * sizeof(uint16_t)));
        auto lossyDng = writeDng(lossyImage);
        assert(tiffTagValue(lossyDng, 259) == 52546);
        assert(motioncam::DNGDecoder::ensureUncompressed(lossyDng));
        assert(tiffTagValue(lossyDng, 259) == 1);
        assert(tiffTagValue(lossyDng, 277) == 1);
    }

    constexpr unsigned int higherWidth = 64, higherHeight = 48, higherRepeat = 4;
    std::vector<uint16_t> higher(higherWidth * higherHeight);
    for (unsigned int y = 0; y < higherHeight; ++y)
        for (unsigned int x = 0; x < higherWidth; ++x)
            higher[y * higherWidth + x] = static_cast<uint16_t>(
                ((y % higherRepeat) * 1901 + (x % higherRepeat) * 1301 + x * 17 + y * 23) & 0x3fff);
    const unsigned short rawBits = 16;
    const unsigned short black[4] = {64, 64, 64, 64};
    const unsigned char higherPattern[16] = {
        0,0,1,1, 0,0,1,1, 1,1,2,2, 1,1,2,2};
    auto configureHigher = [&](tinydngwriter::DNGImage& image) {
        image.SetBigEndian(false);
        assert(image.SetImageWidth(higherWidth) && image.SetImageLength(higherHeight));
        assert(image.SetRowsPerStrip(higherHeight) && image.SetSamplesPerPixel(1));
        assert(image.SetBitsPerSample(1, &rawBits));
        assert(image.SetCompression(tinydngwriter::COMPRESSION_JPEG_XL));
        assert(image.SetJXLDistance(0.5f));
        assert(image.SetPhotometric(tinydngwriter::PHOTOMETRIC_CFA));
        assert(image.SetPlanarConfig(tinydngwriter::PLANARCONFIG_CONTIG));
        assert(image.SetCFARepeatPatternDim(higherRepeat, higherRepeat));
        assert(image.SetCFAPattern(16, higherPattern));
        assert(image.SetBlackLevelRepeatDim(2, 2));
        assert(image.SetBlackLevel(4, black));
        assert(image.SetWhiteLevel(0x3fff));
        const unsigned int activeArea[4] = {0, 0, higherHeight, higherWidth};
        assert(image.SetActiveArea(activeArea));
        assert(image.SetExposureTime(0.003009814f));
        assert(image.SetIso(100));
        assert(image.SetDNGVersion(1, 7, 0, 0));
        assert(image.SetDNGBackwardVersion(1, 7, 0, 0));
    };

    tinydngwriter::DNGImage processImage;
    configureHigher(processImage);
    assert(processImage.SetImageData(reinterpret_cast<const unsigned char*>(higher.data()),
                                     higher.size() * sizeof(uint16_t)));
    auto processBytes = writeDng(processImage);
    auto mountedHigher = processBytes;
    assert(motioncam::DNGDecoder::ensureUncompressed(mountedHigher));
    assert(tiffTagValue(mountedHigher, 259) == 1);
    assert(tiffTagValue(mountedHigher, 279) == higher.size() * sizeof(uint16_t));
    // Reproduce the legacy FloatToRational overflow found in already-finalized
    // DNGs: ExposureTime numerator 12927053 with a wrapped 2^32 denominator.
    auto legacyBytes = processBytes;
    const uint32_t ifd = static_cast<uint32_t>(legacyBytes[4] | legacyBytes[5] << 8 |
                                               legacyBytes[6] << 16 | legacyBytes[7] << 24);
    const uint16_t entryCount = static_cast<uint16_t>(legacyBytes[ifd] | legacyBytes[ifd + 1] << 8);
    bool patchedExposure = false;
    for (uint16_t i = 0; i < entryCount; ++i) {
        const size_t entry = static_cast<size_t>(ifd) + 2 + i * 12;
        const uint16_t tag = static_cast<uint16_t>(legacyBytes[entry] | legacyBytes[entry + 1] << 8);
        if (tag != 33434) continue;
        const uint32_t offset = static_cast<uint32_t>(legacyBytes[entry + 8] |
            legacyBytes[entry + 9] << 8 | legacyBytes[entry + 10] << 16 | legacyBytes[entry + 11] << 24);
        const uint32_t numerator = 12927053;
        for (int byte = 0; byte < 4; ++byte)
            legacyBytes[offset + byte] = static_cast<uint8_t>(numerator >> (byte * 8));
        for (int byte = 0; byte < 4; ++byte) legacyBytes[offset + 4 + byte] = 0;
        patchedExposure = true;
    }
    assert(patchedExposure);
    assert(motioncam::DNGDecoder::repairExposureTime(legacyBytes, 0.003009814));
    for (uint16_t i = 0; i < entryCount; ++i) {
        const size_t entry = static_cast<size_t>(ifd) + 2 + i * 12;
        const uint16_t tag = static_cast<uint16_t>(legacyBytes[entry] | legacyBytes[entry + 1] << 8);
        if (tag != 33434) continue;
        const uint32_t offset = static_cast<uint32_t>(legacyBytes[entry + 8] |
            legacyBytes[entry + 9] << 8 | legacyBytes[entry + 10] << 16 | legacyBytes[entry + 11] << 24);
        const uint32_t denominator = static_cast<uint32_t>(legacyBytes[offset + 4] |
            legacyBytes[offset + 5] << 8 | legacyBytes[offset + 6] << 16 | legacyBytes[offset + 7] << 24);
        assert(denominator != 0);
    }
    const auto sequencePath = std::filesystem::temp_directory_path() /
        ("motioncam-jxl-sequence-" + std::to_string(
            std::chrono::steady_clock::now().time_since_epoch().count()));
    std::filesystem::create_directories(sequencePath);
    for (int frame = 0; frame < 3; ++frame) {
        auto timedBytes = legacyBytes;
        const motioncam::Timestamp timestamp = frame * 40000000LL;
        assert(motioncam::DNGDecoder::setTimingMetadata(timedBytes, 25.0, timestamp));
        assert(tiffTagType(timedBytes, 51044) == 10); // SRATIONAL
        const size_t timedSize = timedBytes.size();
        assert(motioncam::DNGDecoder::setTimingMetadata(timedBytes, 25.0, timestamp));
        assert(timedBytes.size() == timedSize);
        const auto filename = sequencePath /
            ("260524_165247_IMAGE_qb__-00000" + std::to_string(frame) + ".dng");
        std::ofstream output(filename, std::ios::binary);
        output.write(reinterpret_cast<const char*>(timedBytes.data()),
                     static_cast<std::streamsize>(timedBytes.size()));
        assert(output.good());
    }
    {
        motioncam::DNGDecoder sequence(sequencePath.string());
        const auto& info = sequence.getSequenceInfo();
        assert(info.width == higherWidth && info.height == higherHeight);
        assert(info.totalFrames == 3 && std::abs(info.fps - 25.0) < 0.001);
        assert(info.hasFrameNumberSequence);
        const auto& frames = sequence.getFrames();
        assert(frames[0].hasExactPresentationTimestamp && frames[0].timestamp == 0);
        assert(frames[1].hasExactPresentationTimestamp && frames[1].timestamp == 40000000);
        assert(frames[2].hasExactPresentationTimestamp && frames[2].timestamp == 80000000);
        motioncam::DNGFrameMetadata metadata;
        assert(sequence.getFrameMetadata(0, metadata));
        assert(metadata.hasExposure && metadata.iso == 100);
        assert(metadata.exposureTime > 0.0 && std::isfinite(metadata.exposureTime));
    }
    std::filesystem::remove_all(sequencePath);
    const auto shortSequencePath = std::filesystem::temp_directory_path() /
        ("motioncam-jxl-short-sequence-" + std::to_string(
            std::chrono::steady_clock::now().time_since_epoch().count()));
    std::filesystem::create_directories(shortSequencePath);
    for (int frame = 0; frame < 2; ++frame) {
        const auto filename = shortSequencePath /
            ("still-00000" + std::to_string(frame) + ".dng");
        std::ofstream output(filename, std::ios::binary);
        output.write(reinterpret_cast<const char*>(legacyBytes.data()),
                     static_cast<std::streamsize>(legacyBytes.size()));
        assert(output.good());
    }
    {
        motioncam::DNGDecoder sequence(shortSequencePath.string());
        assert(sequence.getSequenceInfo().totalFrames == 2);
        assert(std::abs(sequence.getSequenceInfo().fps - 24.0) < 0.001);
        assert(!sequence.getSequenceInfo().hasFrameNumberSequence);
    }
    std::filesystem::remove_all(shortSequencePath);
    const auto timedStillPath = std::filesystem::temp_directory_path() /
        ("motioncam-jxl-timed-stills-" + std::to_string(
            std::chrono::steady_clock::now().time_since_epoch().count()));
    std::filesystem::create_directories(timedStillPath);
    for (int frame = 0; frame < 2; ++frame) {
        auto timedBytes = legacyBytes;
        assert(motioncam::DNGDecoder::setTimingMetadata(
            timedBytes, 25.0, 100000000LL + frame * 40000000LL));
        std::ofstream output(timedStillPath / (std::string("still-") +
            static_cast<char>('a' + frame) + ".dng"), std::ios::binary);
        output.write(reinterpret_cast<const char*>(timedBytes.data()),
                     static_cast<std::streamsize>(timedBytes.size()));
        assert(output.good());
    }
    {
        motioncam::DNGDecoder sequence(timedStillPath.string());
        assert(!sequence.getSequenceInfo().hasFrameNumberSequence);
        const auto& frames = sequence.getFrames();
        assert(frames.size() == 2);
        assert(frames[0].timestamp == 100000000LL);
        assert(frames[1].timestamp == 140000000LL);
    }
    std::filesystem::remove_all(timedStillPath);
    const auto datedSequencePath = std::filesystem::temp_directory_path() /
        ("motioncam-jxl-dated-sequence-" + std::to_string(
            std::chrono::steady_clock::now().time_since_epoch().count()));
    std::filesystem::create_directories(datedSequencePath);
    const std::array<const char*, 4> datedNames = {
        "IMG_20201217_145306.dng", "IMG_20201217_145756.dng",
        "IMG_20201217_145827.dng", "IMG_20201217_145902.dng"};
    for (const char* name : datedNames) {
        std::ofstream output(datedSequencePath / name, std::ios::binary);
        output.write(reinterpret_cast<const char*>(legacyBytes.data()),
                     static_cast<std::streamsize>(legacyBytes.size()));
        assert(output.good());
    }
    {
        motioncam::DNGDecoder sequence(datedSequencePath.string());
        assert(std::abs(sequence.getSequenceInfo().fps - 24.0) < 0.001);
        assert(!sequence.getSequenceInfo().hasFrameNumberSequence);
        const auto& frames = sequence.getFrames();
        assert(frames.size() == 4 && frames[0].timestamp == 0);
        assert(frames[1].timestamp > frames[0].timestamp);
        assert(frames[2].timestamp > frames[1].timestamp);
        assert(frames[3].timestamp > frames[2].timestamp);
    }
    std::filesystem::remove_all(datedSequencePath);
    const std::array<uint8_t, 4> phase = {0, 1, 1, 2};
    auto metadataOverride = processBytes;
    assert(motioncam::DNGDecoder::processHigherCFA(
        metadataOverride, 6, phase, motioncam::QuadBayerMode::CorrectQBCFAMetadata,
        false, 1, true));
    assert(tiffTagValue(metadataOverride, 33421) == 6);
    const std::vector<uint8_t> expected6x6 = {
        0,0,0,1,1,1, 0,0,0,1,1,1, 0,0,0,1,1,1,
        1,1,1,2,2,2, 1,1,1,2,2,2, 1,1,1,2,2,2};
    assert(tiffByteTagValues(metadataOverride, 33422) == expected6x6);
    assert(motioncam::DNGDecoder::setTimingMetadata(metadataOverride, 25.0, 0));
    auto hqProxy = processBytes;
    assert(motioncam::DNGDecoder::ensureUncompressed(hqProxy));
    const size_t fullResolutionSize = hqProxy.size();
    assert(motioncam::DNGDecoder::processHigherCFA(
        hqProxy, higherRepeat, phase, motioncam::QuadBayerMode::Demosaic,
        false, 2, true));
    assert(hqProxy.size() < fullResolutionSize);
    assert(tiffTagValue(hqProxy, 256) == higherWidth / 2);
    assert(tiffTagValue(hqProxy, 257) == higherHeight / 2);
    assert(tiffTagValue(hqProxy, 262) == 32803);
    assert(tiffTagValue(hqProxy, 277) == 1);
    assert((tiffLong4Tag(hqProxy, 50829) ==
            std::array<uint32_t, 4>{0, 0, higherHeight / 2, higherWidth / 2}));
    auto hqRemosaic = processBytes;
    assert(motioncam::DNGDecoder::processHigherCFA(
        hqRemosaic, higherRepeat, phase, motioncam::QuadBayerMode::Demosaic,
        true, 2, true));
    assert(tiffTagValue(hqRemosaic, 256) == higherWidth / 2);
    assert(tiffTagValue(hqRemosaic, 257) == higherHeight / 2);
    assert(tiffTagValue(hqRemosaic, 262) == 32803);
    assert(tiffTagValue(hqRemosaic, 277) == 1);
    auto hqFourProxy = processBytes;
    assert(motioncam::DNGDecoder::processHigherCFA(
        hqFourProxy, higherRepeat, phase, motioncam::QuadBayerMode::Demosaic,
        false, 4, true));
    assert(tiffTagValue(hqFourProxy, 256) == higherWidth / 4);
    assert(tiffTagValue(hqFourProxy, 257) == higherHeight / 4);
    assert(tiffTagValue(hqFourProxy, 262) == 34892);
    assert(tiffTagValue(hqFourProxy, 277) == 3);
    assert(motioncam::DNGDecoder::processHigherCFA(
        processBytes, higherRepeat, phase, motioncam::QuadBayerMode::Demosaic,
        false, 1, true));
    assert(tiffTagValue(processBytes, 262) == 34892);
    assert(tiffTagValue(processBytes, 277) == 3);
    assert(motioncam::DNGDecoder::processHigherCFA(
        processBytes, higherRepeat, phase, motioncam::QuadBayerMode::Demosaic,
        true, 1, true));
    assert(tiffTagValue(processBytes, 259) == 1);
    assert(tiffTagValue(processBytes, 262) == 32803);
    assert(tiffTagValue(processBytes, 277) == 1);
    assert(tiffTagValue(processBytes, 279) == higher.size() * sizeof(uint16_t));

    tinydngwriter::GainMapParams gain{};
    gain.top = 0; gain.left = 0; gain.bottom = higherHeight; gain.right = higherWidth;
    gain.plane = 0; gain.planes = 1; gain.row_pitch = 1; gain.col_pitch = 1;
    gain.map_points_v = 2; gain.map_points_h = 2;
    gain.map_spacing_v = 1.0; gain.map_spacing_h = 1.0;
    gain.map_planes = 1; gain.gain_data = {1.1f, 1.2f, 1.3f, 1.4f};
    tinydngwriter::OpcodeList gainOpcodes;
    gainOpcodes.AddGainMap(gain);
    tinydngwriter::DNGImage gainImage;
    configureHigher(gainImage);
    assert(gainImage.SetOpcodeList2(gainOpcodes));
    assert(gainImage.SetImageData(reinterpret_cast<const unsigned char*>(higher.data()),
                                  higher.size() * sizeof(uint16_t)));
    auto gainBytes = writeDng(gainImage);
    assert(motioncam::DNGDecoder::bakeGainMaps(gainBytes, false, false));

    constexpr unsigned int rgbWidth = 48, rgbHeight = 32, channels = 3;
    std::vector<uint16_t> rgb(rgbWidth * rgbHeight * channels);
    for (unsigned int y = 0; y < rgbHeight; ++y) {
        for (unsigned int x = 0; x < rgbWidth; ++x) {
            const size_t offset = (static_cast<size_t>(y) * rgbWidth + x) * channels;
            rgb[offset] = static_cast<uint16_t>((x * 701 + y * 31) & 0xffff);
            rgb[offset + 1] = static_cast<uint16_t>((x * 43 + y * 977 + 12000) & 0xffff);
            rgb[offset + 2] = static_cast<uint16_t>((x * 313 + y * 271 + 24000) & 0xffff);
        }
    }
    std::string error;
    std::vector<unsigned char> rgbLossless;
    assert(tinydngwriter::CompressJPEGXL(
        reinterpret_cast<const unsigned char*>(rgb.data()), rgb.size() * sizeof(uint16_t),
        rgbWidth, rgbHeight, channels, 0.0f, rgbLossless, &error));
    assert(decode(rgbLossless, rgbWidth, rgbHeight, channels) == rgb);

    for (const float distance : {0.1f, 0.3f, 0.5f, 1.0f}) {
        std::vector<unsigned char> rgbLossy;
        assert(tinydngwriter::CompressJPEGXL(
            reinterpret_cast<const unsigned char*>(rgb.data()), rgb.size() * sizeof(uint16_t),
            rgbWidth, rgbHeight, channels, distance, rgbLossy, &error));
        assert(decode(rgbLossy, rgbWidth, rgbHeight, channels).size() == rgb.size());
    }

    tinydngwriter::DNGImage rgbDng;
    rgbDng.SetBigEndian(false);
    const unsigned short rgbBits[3] = {16, 16, 16};
    assert(rgbDng.SetImageWidth(rgbWidth) && rgbDng.SetImageLength(rgbHeight));
    assert(rgbDng.SetRowsPerStrip(rgbHeight) && rgbDng.SetSamplesPerPixel(channels));
    assert(rgbDng.SetBitsPerSample(channels, rgbBits));
    assert(rgbDng.SetCompression(tinydngwriter::COMPRESSION_JPEG_XL));
    assert(rgbDng.SetJXLDistance(0.5f));
    assert(rgbDng.SetPhotometric(tinydngwriter::PHOTOMETRIC_LINEARRAW));
    assert(rgbDng.SetPlanarConfig(tinydngwriter::PLANARCONFIG_CONTIG));
    assert(rgbDng.SetWhiteLevel(1023));
    const unsigned int rgbActiveArea[4] = {4, 6, 28, 42};
    assert(rgbDng.SetActiveArea(rgbActiveArea));
    assert(rgbDng.SetDNGVersion(1, 7, 0, 0));
    assert(rgbDng.SetDNGBackwardVersion(1, 7, 0, 0));
    assert(rgbDng.SetImageData(reinterpret_cast<const unsigned char*>(rgb.data()),
                               rgb.size() * sizeof(uint16_t)));
    assert(rgbDng.GetStripBytes() > 0);
    auto mountedRgb = writeDng(rgbDng);
    // RGB is also a valid primary DNG photometric interpretation. Thumbnail
    // removal must not reject the file merely because it has no CFA/LinearRaw IFD.
    assert(rgbDng.SetPhotometric(tinydngwriter::PHOTOMETRIC_RGB));
    auto photometricRgb = writeDng(rgbDng);
    assert(motioncam::DNGDecoder::removeThumbnails(photometricRgb));
    assert(motioncam::DNGDecoder::ensureUncompressed(mountedRgb));
    assert(tiffTagValue(mountedRgb, 259) == 1);
    assert(tiffTagValue(mountedRgb, 279) == rgb.size() * sizeof(uint16_t));
    auto jpeg92Rgb = mountedRgb;
    assert(motioncam::DNGDecoder::compressLosslessJPEG(jpeg92Rgb));
    assert(motioncam::DNGDecoder::ensureUncompressed(jpeg92Rgb));
    assert(tiffTagValue(jpeg92Rgb, 277) == 3);
    auto dctRgb = mountedRgb;
    assert(motioncam::DNGDecoder::compressLossyJPEG(dctRgb));
    assert(tiffTagValue(dctRgb, 259) == 7);
    assert(tiffTagValue(dctRgb, 258) == 12);
    assert(motioncam::DNGDecoder::ensureUncompressed(dctRgb));
    assert(tiffTagValue(dctRgb, 277) == 3);
    auto tenBitDctRgb = mountedRgb;
    assert(motioncam::DNGDecoder::packUncompressedToWhiteLevel(tenBitDctRgb));
    assert(tiffTagValue(tenBitDctRgb, 258) == 10);
    assert(motioncam::DNGDecoder::compressLossyJPEG(tenBitDctRgb));
    assert(tiffTagValue(tenBitDctRgb, 258) == 12);
    assert(motioncam::DNGDecoder::ensureUncompressed(tenBitDctRgb));

    tinydngwriter::DNGImage logRgb;
    logRgb.SetBigEndian(false);
    assert(logRgb.SetImageWidth(rgbWidth) && logRgb.SetImageLength(rgbHeight));
    assert(logRgb.SetRowsPerStrip(rgbHeight) && logRgb.SetSamplesPerPixel(3));
    assert(logRgb.SetBitsPerSample(3, rgbBits));
    assert(logRgb.SetCompression(tinydngwriter::COMPRESSION_NONE));
    assert(logRgb.SetPhotometric(tinydngwriter::PHOTOMETRIC_LINEARRAW));
    assert(logRgb.SetPlanarConfig(tinydngwriter::PLANARCONFIG_CONTIG));
    assert(logRgb.SetWhiteLevel(65534));
    const unsigned short logBlack[3] = {1024, 2048, 3072};
    assert(logRgb.SetBlackLevel(3, logBlack));
    std::vector<uint16_t> logTable(1024);
    for (size_t i = 0; i < logTable.size(); ++i)
        logTable[i] = static_cast<uint16_t>(i * 65535u / (logTable.size() - 1));
    assert(logRgb.SetLinearizationTable(logTable.size(), logTable.data()));
    assert(logRgb.SetDNGVersion(1, 4, 0, 0));
    assert(logRgb.SetDNGBackwardVersion(1, 1, 0, 0));
    std::vector<uint16_t> logSamples(rgb.size());
    for (size_t i = 0; i < logSamples.size(); ++i) logSamples[i] = rgb[i] & 1023u;
    assert(logRgb.SetImageData(reinterpret_cast<const unsigned char*>(logSamples.data()),
                               logSamples.size() * sizeof(uint16_t)));
    auto logDctRgb = writeDng(logRgb);
    auto reducedLogRgb = logDctRgb;
    assert(motioncam::DNGDecoder::applyLogTransform(
        reducedLogRgb, motioncam::LogTransformMode::ReduceBy4Bit, 1023));
    assert(motioncam::DNGDecoder::packUncompressedToWhiteLevel(reducedLogRgb));
    assert(tiffTagValue(reducedLogRgb, 258) == 6);
    assert(motioncam::DNGDecoder::compressLossyJPEG(logDctRgb));
    assert(tiffTagValue(logDctRgb, 50717) == 65534);
    assert(tiffTagValue(logDctRgb, 50714) == logBlack[0]);
    assert(motioncam::DNGDecoder::ensureUncompressed(logDctRgb));
    assert(motioncam::DNGDecoder::packUncompressedToWhiteLevel(logDctRgb));
    assert(tiffTagValue(logDctRgb, 258) == 12);
    auto proxyRgb = mountedRgb;
    assert(motioncam::DNGDecoder::processHigherCFA(
        proxyRgb, 2, phase, motioncam::QuadBayerMode::Demosaic,
        false, 2, true));
    assert(tiffTagValue(proxyRgb, 256) == rgbWidth / 2);
    assert(tiffTagValue(proxyRgb, 257) == rgbHeight / 2);
    assert(tiffTagValue(proxyRgb, 279) ==
           (rgbWidth / 2) * (rgbHeight / 2) * channels * sizeof(uint16_t));
    assert((tiffLong4Tag(proxyRgb, 50829) ==
            std::array<uint32_t, 4>{2, 3, 14, 21}));
    auto syntheticRgb = mountedRgb;
    assert(motioncam::DNGDecoder::setTimingMetadata(syntheticRgb, 24.0, 0));
    std::vector<uint8_t> replacement(static_cast<size_t>(rgbWidth) * rgbHeight * channels * 2);
    for (size_t i = 0; i < replacement.size(); i += 2) {
        const uint16_t value = static_cast<uint16_t>((i / 2) * 997u);
        replacement[i] = static_cast<uint8_t>(value & 0xff);
        replacement[i + 1] = static_cast<uint8_t>(value >> 8);
    }
    assert(motioncam::DNGDecoder::replaceUncompressedRGB16(
        syntheticRgb, replacement, rgbWidth, rgbHeight));
    std::vector<uint8_t> replaced;
    uint32_t replacedWidth = 0, replacedHeight = 0;
    assert(motioncam::DNGDecoder::extractUncompressedRGB16(
        syntheticRgb, replaced, replacedWidth, replacedHeight));
    assert(replaced.size() == replacement.size());
    for (size_t i = 0; i < replaced.size(); i += 2) {
        const int actual = replaced[i] | replaced[i + 1] << 8;
        const int wanted = replacement[i] | replacement[i + 1] << 8;
        assert(std::abs(actual - wanted) <= 33);
    }
    assert(motioncam::DNGDecoder::markDuplicateFrame(syntheticRgb));
    assert(motioncam::DNGDecoder::isDuplicateFrame(syntheticRgb));
    assert(!motioncam::DNGDecoder::isSyntheticFrame(syntheticRgb));
    assert(motioncam::DNGDecoder::markSyntheticFrame(syntheticRgb));
    assert(!motioncam::DNGDecoder::isDuplicateFrame(syntheticRgb));
    assert(motioncam::DNGDecoder::isSyntheticFrame(syntheticRgb));
    assert(motioncam::DNGDecoder::markDuplicateFrame(syntheticRgb));
    assert(motioncam::DNGDecoder::isDuplicateFrame(syntheticRgb));
    assert(motioncam::DNGDecoder::markSyntheticFrame(syntheticRgb));
    assert(!motioncam::DNGDecoder::isDuplicateFrame(syntheticRgb));
    const std::string syntheticMarker = "rpt:SyntheticFrame='true'";
    assert(std::search(syntheticRgb.begin(), syntheticRgb.end(),
        syntheticMarker.begin(), syntheticMarker.end()) != syntheticRgb.end());
    auto overriddenRgb = mountedRgb;
    assert(motioncam::DNGDecoder::overrideDataLevels(overriddenRgb, "Static"));
    assert(tiffTagValue(overriddenRgb, 50717) == 1023);
    assert(motioncam::DNGDecoder::overrideDataLevels(overriddenRgb, "4095/Dynamic"));
    assert(tiffTagValue(overriddenRgb, 50717) == 4095);
    assert(motioncam::DNGDecoder::packUncompressedToWhiteLevel(overriddenRgb));
    assert(tiffTagValue(overriddenRgb, 258) == 12);
    assert(motioncam::DNGDecoder::packUncompressedToWhiteLevel(mountedRgb));
    assert(tiffTagValue(mountedRgb, 258) == 10);
    assert(tiffTagValue(mountedRgb, 279) ==
           ((rgbWidth * channels * 10 + 7) / 8) * rgbHeight);

    auto remosaicedRgb = writeDng(rgbDng);
    assert(motioncam::DNGDecoder::ensureUncompressed(remosaicedRgb));
    assert(motioncam::DNGDecoder::processHigherCFA(
        remosaicedRgb, 2, phase, motioncam::QuadBayerMode::Demosaic,
        true, 1, true));
    assert(tiffTagValue(remosaicedRgb, 262) == 32803);
    assert(tiffTagValue(remosaicedRgb, 277) == 1);
    assert(motioncam::DNGDecoder::packUncompressedToWhiteLevel(remosaicedRgb));
    assert(tiffTagValue(remosaicedRgb, 258) == 10);
    assert(tiffTagValue(remosaicedRgb, 279) ==
           ((rgbWidth * 10 + 7) / 8) * rgbHeight);
    // Conversion of an older DNG must advertise the DNG 1.7 requirement of
    // compression 52546, rather than retaining stale version metadata.
    setTiffByteTag(remosaicedRgb, 50706, {1, 4, 0, 0});
    setTiffByteTag(remosaicedRgb, 50707, {1, 4, 0, 0});
    assert(tiffByteTag(remosaicedRgb, 50706) ==
           (std::array<uint8_t, 4>{1, 4, 0, 0}));
    auto lossyFinalized = remosaicedRgb;
    assert(motioncam::DNGDecoder::compressJPEGXL(lossyFinalized, 0.5f));
    assert(tiffTagValue(lossyFinalized, 259) == 52546);
    assert(tiffTagValue(lossyFinalized, 258) == 16);
    assert(motioncam::DNGDecoder::ensureUncompressed(lossyFinalized));
    assert(tiffTagValue(lossyFinalized, 259) == 1);
    assert(tiffTagValue(lossyFinalized, 277) == 1);

    assert(motioncam::DNGDecoder::compressJPEGXL(remosaicedRgb, 0.0f));
    assert(tiffTagValue(remosaicedRgb, 259) == 52546);
    assert(tiffByteTag(remosaicedRgb, 50706) ==
           (std::array<uint8_t, 4>{1, 7, 0, 0}));
    assert(tiffByteTag(remosaicedRgb, 50707) ==
           (std::array<uint8_t, 4>{1, 7, 0, 0}));
    assert(tiffTagValue(remosaicedRgb, 258) == 16);
    assert(tiffTagValue(remosaicedRgb, 277) == 1);
}
