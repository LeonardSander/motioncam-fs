#define TINY_DNG_WRITER_IMPLEMENTATION
#include "tinydng/tiny_dng_writer.h"
#include "liblj92/lj92.h"
#include "DNGDecoder.h"
#include <jpeglib.h>

#include <algorithm>
#include <cassert>
#include <cstdint>
#include <cstdlib>
#include <sstream>
#include <string>
#include <vector>

static std::vector<uint8_t> tiledLosslessJpegDng(
        const std::vector<uint16_t>& pixels, uint32_t width, uint32_t height,
        uint32_t tileWidth, uint32_t tileHeight, uint16_t bits,
        int jpegComponents = 1, bool padEdgeTiles = false) {
    auto put16 = [](std::vector<uint8_t>& out, size_t at, uint16_t value) {
        out[at] = value & 0xff; out[at + 1] = value >> 8;
    };
    auto put32 = [](std::vector<uint8_t>& out, size_t at, uint32_t value) {
        for (int i = 0; i < 4; ++i) out[at + i] = static_cast<uint8_t>(value >> (i * 8));
    };
    constexpr uint16_t entryCount = 11;
    std::vector<uint8_t> dng(8 + 2 + entryCount * 12 + 4, 0);
    dng[0] = 'I'; dng[1] = 'I'; put16(dng, 2, 42); put32(dng, 4, 8);
    put16(dng, 8, entryCount);
    size_t entry = 10;
    auto scalar = [&](uint16_t tag, uint16_t type, uint32_t value) {
        put16(dng, entry, tag); put16(dng, entry + 2, type); put32(dng, entry + 4, 1);
        if (type == 3) put16(dng, entry + 8, static_cast<uint16_t>(value));
        else put32(dng, entry + 8, value);
        entry += 12;
    };
    scalar(256, 4, width); scalar(257, 4, height); scalar(258, 3, bits);
    scalar(259, 3, 7); scalar(262, 3, 32803); scalar(277, 3, 1);
    scalar(322, 4, tileWidth); scalar(323, 4, tileHeight);
    const uint32_t tilesAcross = (width + tileWidth - 1) / tileWidth;
    const uint32_t tilesDown = (height + tileHeight - 1) / tileHeight;
    const uint32_t tileCount = tilesAcross * tilesDown;
    const uint32_t offsetsArray = static_cast<uint32_t>(dng.size());
    dng.resize(dng.size() + tileCount * 8);
    put16(dng, entry, 324); put16(dng, entry + 2, 4); put32(dng, entry + 4, tileCount);
    put32(dng, entry + 8, offsetsArray); entry += 12;
    put16(dng, entry, 325); put16(dng, entry + 2, 4); put32(dng, entry + 4, tileCount);
    put32(dng, entry + 8, offsetsArray + tileCount * 4); entry += 12;
    scalar(50717, 4, (1u << bits) - 1);
    for (uint32_t ty = 0, index = 0; ty < tilesDown; ++ty) {
        for (uint32_t tx = 0; tx < tilesAcross; ++tx, ++index) {
            const uint32_t imageW = std::min(tileWidth, width - tx * tileWidth);
            const uint32_t imageH = std::min(tileHeight, height - ty * tileHeight);
            const uint32_t w = padEdgeTiles ? tileWidth : imageW;
            const uint32_t h = padEdgeTiles ? tileHeight : imageH;
            std::vector<uint16_t> tile(static_cast<size_t>(w) * h);
            for (uint32_t y = 0; y < imageH; ++y)
                std::copy_n(pixels.data() + static_cast<size_t>(ty * tileHeight + y) * width +
                                tx * tileWidth,
                            imageW, tile.data() + static_cast<size_t>(y) * w);
            uint8_t* encoded = nullptr; int encodedSize = 0;
            assert(w % jpegComponents == 0);
            assert(lj92_encode(tile.data(), w / jpegComponents, h, bits,
                               jpegComponents, w, 0, nullptr, 0,
                               &encoded, &encodedSize) == LJ92_ERROR_NONE);
            put32(dng, offsetsArray + index * 4, static_cast<uint32_t>(dng.size()));
            put32(dng, offsetsArray + (tileCount + index) * 4, encodedSize);
            dng.insert(dng.end(), encoded, encoded + encodedSize);
            free(encoded);
        }
    }
    return dng;
}

static std::vector<uint8_t> multiStripUncompressedDng(
        const std::vector<uint16_t>& pixels, uint32_t width, uint32_t height,
        uint32_t rowsPerStrip) {
    auto put16 = [](std::vector<uint8_t>& out, size_t at, uint16_t value) {
        out[at] = value & 0xff; out[at + 1] = value >> 8;
    };
    auto put32 = [](std::vector<uint8_t>& out, size_t at, uint32_t value) {
        for (int i = 0; i < 4; ++i) out[at + i] = static_cast<uint8_t>(value >> (i * 8));
    };
    constexpr uint16_t entryCount = 10;
    std::vector<uint8_t> dng(8 + 2 + entryCount * 12 + 4, 0);
    dng[0] = 'I'; dng[1] = 'I'; put16(dng, 2, 42); put32(dng, 4, 8);
    put16(dng, 8, entryCount);
    size_t entry = 10;
    auto scalar = [&](uint16_t tag, uint16_t type, uint32_t value) {
        put16(dng, entry, tag); put16(dng, entry + 2, type); put32(dng, entry + 4, 1);
        if (type == 3) put16(dng, entry + 8, static_cast<uint16_t>(value));
        else put32(dng, entry + 8, value);
        entry += 12;
    };
    scalar(256, 4, width); scalar(257, 4, height); scalar(258, 3, 16);
    scalar(259, 3, 1); scalar(262, 3, 32803);
    const uint32_t stripCount = (height + rowsPerStrip - 1) / rowsPerStrip;
    const uint32_t arrays = static_cast<uint32_t>(dng.size());
    dng.resize(dng.size() + stripCount * 8);
    put16(dng, entry, 273); put16(dng, entry + 2, 4); put32(dng, entry + 4, stripCount);
    put32(dng, entry + 8, arrays); entry += 12;
    scalar(278, 4, rowsPerStrip);
    put16(dng, entry, 279); put16(dng, entry + 2, 4); put32(dng, entry + 4, stripCount);
    put32(dng, entry + 8, arrays + stripCount * 4); entry += 12;
    scalar(277, 3, 1); scalar(50717, 4, 65535);
    for (uint32_t index = 0; index < stripCount; ++index) {
        const uint32_t firstRow = index * rowsPerStrip;
        const uint32_t rows = std::min(rowsPerStrip, height - firstRow);
        const uint32_t bytes = rows * width * 2;
        put32(dng, arrays + index * 4, static_cast<uint32_t>(dng.size()));
        // Include harmless per-strip padding to ensure it is not copied into
        // the canonical image payload.
        put32(dng, arrays + (stripCount + index) * 4, bytes + 2);
        for (uint32_t i = 0; i < rows * width; ++i) {
            const uint16_t value = pixels[static_cast<size_t>(firstRow) * width + i];
            dng.push_back(value & 0xff); dng.push_back(value >> 8);
        }
        dng.push_back(0xaa); dng.push_back(0x55);
    }
    return dng;
}

static std::vector<uint8_t> chunkedDng(
        uint32_t width, uint32_t height, uint16_t bits, uint16_t channels,
        uint16_t compression, uint32_t chunkWidth, uint32_t chunkHeight,
        bool tiled, bool planar, const std::vector<std::vector<uint8_t>>& chunks,
        bool omitDefaults = false) {
    auto put16 = [](std::vector<uint8_t>& out, size_t at, uint16_t value) {
        out[at] = value & 0xff; out[at + 1] = value >> 8;
    };
    auto put32 = [](std::vector<uint8_t>& out, size_t at, uint32_t value) {
        for (int i = 0; i < 4; ++i) out[at + i] = static_cast<uint8_t>(value >> (i * 8));
    };
    const uint16_t entryCount = static_cast<uint16_t>(
        7 + (tiled ? 1 : 0) + (omitDefaults ? 0 : 2) + (planar ? 1 : 0));
    std::vector<uint8_t> dng(8 + 2 + entryCount * 12 + 4, 0);
    dng[0] = 'I'; dng[1] = 'I'; put16(dng, 2, 42); put32(dng, 4, 8);
    put16(dng, 8, entryCount);
    size_t entry = 10;
    auto scalar = [&](uint16_t tag, uint16_t type, uint32_t value) {
        put16(dng, entry, tag); put16(dng, entry + 2, type); put32(dng, entry + 4, 1);
        if (type == 3) put16(dng, entry + 8, static_cast<uint16_t>(value));
        else put32(dng, entry + 8, value);
        entry += 12;
    };
    scalar(256, 4, width); scalar(257, 4, height); scalar(258, 3, bits);
    if (!omitDefaults) scalar(259, 3, compression);
    scalar(262, 3, channels == 1 ? 32803 : 34892);
    const uint32_t arrays = static_cast<uint32_t>(dng.size());
    dng.resize(dng.size() + chunks.size() * 8);
    put16(dng, entry, tiled ? 324 : 273); put16(dng, entry + 2, 4);
    put32(dng, entry + 4, chunks.size()); put32(dng, entry + 8, arrays); entry += 12;
    scalar(tiled ? 322 : 278, 4, tiled ? chunkWidth : chunkHeight);
    if (tiled) scalar(323, 4, chunkHeight);
    put16(dng, entry, tiled ? 325 : 279); put16(dng, entry + 2, 4);
    put32(dng, entry + 4, chunks.size());
    put32(dng, entry + 8, arrays + chunks.size() * 4); entry += 12;
    if (!omitDefaults) scalar(277, 3, channels);
    if (planar) scalar(284, 3, 2);
    for (size_t i = 0; i < chunks.size(); ++i) {
        put32(dng, arrays + i * 4, static_cast<uint32_t>(dng.size()));
        put32(dng, arrays + (chunks.size() + i) * 4, chunks[i].size());
        dng.insert(dng.end(), chunks[i].begin(), chunks[i].end());
    }
    return dng;
}

static std::vector<uint8_t> jpeg8(const std::vector<uint8_t>& pixels,
                                  uint32_t width, uint32_t height) {
    jpeg_compress_struct codec{};
    jpeg_error_mgr error{};
    codec.err = jpeg_std_error(&error);
    jpeg_create_compress(&codec);
    unsigned char* encoded = nullptr;
    unsigned long encodedSize = 0;
    jpeg_mem_dest(&codec, &encoded, &encodedSize);
    codec.image_width = width; codec.image_height = height;
    codec.input_components = 1; codec.in_color_space = JCS_GRAYSCALE;
    jpeg_set_defaults(&codec); jpeg_set_quality(&codec, 90, TRUE);
    jpeg_start_compress(&codec, TRUE);
    while (codec.next_scanline < codec.image_height) {
        JSAMPROW row = const_cast<JSAMPROW>(pixels.data() +
            static_cast<size_t>(codec.next_scanline) * width);
        assert(jpeg_write_scanlines(&codec, &row, 1) == 1);
    }
    jpeg_finish_compress(&codec);
    std::vector<uint8_t> result(encoded, encoded + encodedSize);
    jpeg_destroy_compress(&codec); free(encoded);
    return result;
}

static uint32_t tagScalar(const std::vector<uint8_t>& dng, uint16_t wanted) {
    auto u16 = [&](size_t at) { return static_cast<uint16_t>(dng[at] | dng[at + 1] << 8); };
    auto u32 = [&](size_t at) { return static_cast<uint32_t>(dng[at] | dng[at + 1] << 8 |
        dng[at + 2] << 16 | dng[at + 3] << 24); };
    for (uint32_t ifd = u32(4); ifd;) {
        const uint16_t count = u16(ifd);
        for (uint16_t i = 0; i < count; ++i) {
            const size_t at = static_cast<size_t>(ifd) + 2 + i * 12;
            if (u16(at) == wanted) return u16(at + 2) == 3 ? u16(at + 8) : u32(at + 8);
        }
        ifd = u32(static_cast<size_t>(ifd) + 2 + count * 12);
    }
    assert(false && "tag not found"); return 0;
}

static bool ifdsAreTagSorted(const std::vector<uint8_t>& dng) {
    auto u16 = [&](size_t at) { return static_cast<uint16_t>(dng[at] | dng[at + 1] << 8); };
    auto u32 = [&](size_t at) { return static_cast<uint32_t>(dng[at] | dng[at + 1] << 8 |
        dng[at + 2] << 16 | dng[at + 3] << 24); };
    for (uint32_t ifd = u32(4); ifd;) {
        if (ifd > dng.size() || dng.size() - ifd < 2) return false;
        const uint16_t count = u16(ifd);
        if (count > (dng.size() - ifd - 2) / 12) return false;
        uint16_t previous = 0;
        for (uint16_t i = 0; i < count; ++i) {
            const uint16_t tag = u16(static_cast<size_t>(ifd) + 2 + i * 12);
            if (i && tag < previous) return false;
            previous = tag;
        }
        const size_t next = static_cast<size_t>(ifd) + 2 + count * 12;
        if (next + 4 > dng.size()) return false;
        ifd = u32(next);
    }
    return true;
}

int main() {
    // Uncompressed tiles, compressed strips, planar LinearRaw, and omitted
    // TIFF defaults all normalize to one chunky, uncompressed strip.
    {
        constexpr uint32_t w = 5, h = 3, tw = 3, th = 2;
        std::vector<std::vector<uint8_t>> chunks;
        for (uint32_t ty = 0; ty < 2; ++ty) for (uint32_t tx = 0; tx < 2; ++tx) {
            std::vector<uint8_t> tile(tw * th * 2, 0);
            for (uint32_t y = 0; y < th && ty * th + y < h; ++y)
                for (uint32_t x = 0; x < tw && tx * tw + x < w; ++x) {
                    const uint16_t value = static_cast<uint16_t>((ty * th + y) * 100 + tx * tw + x);
                    const size_t at = (static_cast<size_t>(y) * tw + x) * 2;
                    tile[at] = value & 0xff; tile[at + 1] = value >> 8;
                }
            chunks.push_back(std::move(tile));
        }
        auto tiled = chunkedDng(w, h, 16, 1, 1, tw, th, true, false, chunks);
        assert(motioncam::DNGDecoder::ensureUncompressed(tiled));
        assert(tagScalar(tiled, 259) == 1 && tagScalar(tiled, 273) > 0);
        assert(tagScalar(tiled, 278) == h && tagScalar(tiled, 279) == w * h * 2);
        assert(ifdsAreTagSorted(tiled));

        // Absurd declared tile dimensions must be rejected without overflowing
        // byte or allocation-size arithmetic.
        auto hostile = chunkedDng(w, h, 16, 1, 1, UINT32_MAX, UINT32_MAX,
                                  true, false, {{0, 0}});
        assert(!motioncam::DNGDecoder::ensureUncompressed(hostile));
    }
    {
        constexpr uint32_t w = 6, h = 5, rows = 2;
        std::vector<uint16_t> source(w * h);
        for (size_t i = 0; i < source.size(); ++i) source[i] = static_cast<uint16_t>(i * 17);
        std::vector<std::vector<uint8_t>> chunks;
        for (uint32_t y = 0; y < h; y += rows) {
            const uint32_t count = std::min(rows, h - y);
            uint8_t* encoded = nullptr; int bytes = 0;
            assert(lj92_encode(source.data() + static_cast<size_t>(y) * w,
                w, count, 12, 1, w, 0, nullptr, 0, &encoded, &bytes) == LJ92_ERROR_NONE);
            chunks.emplace_back(encoded, encoded + bytes); free(encoded);
        }
        auto strips = chunkedDng(w, h, 12, 1, 7, w, rows, false, false, chunks);
        assert(motioncam::DNGDecoder::ensureUncompressed(strips));
        assert(tagScalar(strips, 259) == 1 && tagScalar(strips, 258) == 16);
    }
    {
        constexpr uint32_t w = 8, h = 6, rows = 2;
        std::vector<std::vector<uint8_t>> chunks;
        for (uint32_t y = 0; y < h; y += rows) {
            std::vector<uint8_t> pixels(w * rows);
            for (size_t i = 0; i < pixels.size(); ++i)
                pixels[i] = static_cast<uint8_t>(20 + y * w + i);
            chunks.push_back(jpeg8(pixels, w, rows));
        }
        auto strips = chunkedDng(w, h, 8, 1, 34892, w, rows, false, false, chunks);
        assert(motioncam::DNGDecoder::ensureUncompressed(strips));
        assert(tagScalar(strips, 259) == 1 && tagScalar(strips, 258) == 16);
    }
    {
        constexpr uint32_t w = 4, h = 3;
        std::vector<uint16_t> expected(w * h * 3);
        std::vector<std::vector<uint8_t>> planes(3);
        for (uint32_t c = 0; c < 3; ++c) {
            planes[c].resize(w * h * 2);
            for (uint32_t i = 0; i < w * h; ++i) {
                const uint16_t value = static_cast<uint16_t>(c * 1000 + i);
                expected[i * 3 + c] = value;
                planes[c][i * 2] = value & 0xff; planes[c][i * 2 + 1] = value >> 8;
            }
        }
        auto planar = chunkedDng(w, h, 16, 3, 1, w, h, false, true, planes);
        assert(motioncam::DNGDecoder::ensureUncompressed(planar));
        assert(tagScalar(planar, 284) == 1);
        std::vector<uint8_t> rgb; uint32_t outW = 0, outH = 0;
        assert(motioncam::DNGDecoder::extractUncompressedRGB16(planar, rgb, outW, outH));
        assert(outW == w && outH == h && rgb.size() == expected.size() * 2);
        for (size_t i = 0; i < expected.size(); ++i)
            assert(static_cast<uint16_t>(rgb[i * 2] | rgb[i * 2 + 1] << 8) == expected[i]);
    }
    {
        constexpr uint32_t w = 5, h = 4;
        std::vector<uint16_t> expected(w * h * 3);
        std::vector<std::vector<uint8_t>> encodedPlanes;
        for (uint32_t c = 0; c < 3; ++c) {
            std::vector<uint16_t> plane(w * h);
            for (uint32_t i = 0; i < w * h; ++i) {
                plane[i] = static_cast<uint16_t>(c * 900 + i * 3);
                expected[i * 3 + c] = plane[i];
            }
            uint8_t* encoded = nullptr; int bytes = 0;
            assert(lj92_encode(plane.data(), w, h, 12, 1, w, 0, nullptr, 0,
                               &encoded, &bytes) == LJ92_ERROR_NONE);
            encodedPlanes.emplace_back(encoded, encoded + bytes);
            free(encoded);
        }
        auto planar = chunkedDng(w, h, 12, 3, 7, w, h, false, true, encodedPlanes);
        assert(motioncam::DNGDecoder::ensureUncompressed(planar));
        assert(tagScalar(planar, 284) == 1 && ifdsAreTagSorted(planar));
        std::vector<uint8_t> rgb; uint32_t outW = 0, outH = 0;
        assert(motioncam::DNGDecoder::extractUncompressedRGB16(planar, rgb, outW, outH));
        assert(outW == w && outH == h && rgb.size() == expected.size() * 2);
        for (size_t i = 0; i < expected.size(); ++i)
            assert(static_cast<uint16_t>(rgb[i * 2] | rgb[i * 2 + 1] << 8) == expected[i]);
    }
    {
        constexpr uint32_t w = 4, h = 2;
        std::vector<uint8_t> pixels(w * h * 2);
        auto defaults = chunkedDng(w, h, 16, 1, 1, w, h, false, false, {pixels}, true);
        assert(motioncam::DNGDecoder::ensureUncompressed(defaults));
        assert(tagScalar(defaults, 259) == 1 && tagScalar(defaults, 277) == 1);
        assert(ifdsAreTagSorted(defaults));
    }

    // Valid uncompressed TIFF/DNG data may be split across many strips.  The
    // decoder must canonicalize it for processing rather than rejecting it.
    {
        constexpr uint32_t stripWidth = 5, stripHeight = 7, rowsPerStrip = 2;
        std::vector<uint16_t> stripPixels(stripWidth * stripHeight);
        for (size_t i = 0; i < stripPixels.size(); ++i)
            stripPixels[i] = static_cast<uint16_t>(1000 + i);
        auto multiStrip = multiStripUncompressedDng(
            stripPixels, stripWidth, stripHeight, rowsPerStrip);
        assert(motioncam::DNGDecoder::ensureUncompressed(multiStrip));
        auto read16 = [&](size_t at) {
            return static_cast<uint16_t>(multiStrip[at] | multiStrip[at + 1] << 8);
        };
        auto read32 = [&](size_t at) {
            return static_cast<uint32_t>(multiStrip[at] | multiStrip[at + 1] << 8 |
                multiStrip[at + 2] << 16 | multiStrip[at + 3] << 24);
        };
        uint32_t stripOffset = 0, stripBytes = 0, outputRowsPerStrip = 0;
        for (uint16_t i = 0; i < read16(8); ++i) {
            const size_t at = 10 + i * 12;
            if (read16(at) == 273) {
                assert(read32(at + 4) == 1); stripOffset = read32(at + 8);
            }
            if (read16(at) == 279) {
                assert(read32(at + 4) == 1); stripBytes = read32(at + 8);
            }
            if (read16(at) == 278) outputRowsPerStrip = read32(at + 8);
        }
        assert(stripOffset && stripBytes == stripPixels.size() * sizeof(uint16_t));
        assert(outputRowsPerStrip == stripHeight);
        for (size_t i = 0; i < stripPixels.size(); ++i)
            assert(read16(stripOffset + i * 2) == stripPixels[i]);
    }

    // Two-component Android DNG tiles may assign a different DC Huffman table
    // to each component. Verify that the selectors in SOS are honored.
    {
        const std::vector<uint8_t> jpeg = {
            0xff,0xd8,
            0xff,0xc3, 0x00,0x0e, 0x08, 0x00,0x01, 0x00,0x01, 0x02,
                0x00,0x11,0x00, 0x01,0x11,0x00,
            // Both DC tables deliberately share one DHT marker.
            0xff,0xc4, 0x00,0x27, 0x00,
                0x01,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
                0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00, 0x07,
            0x01,
                0x02,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
                0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00, 0x00,0x07,
            0xff,0xda, 0x00,0x0a, 0x02, 0x00,0x00, 0x01,0x10, 0x01,0x00,0x00,
            0x09,0x93,0x00,0x00, 0xff,0xd9
        };
        lj92 twoTableDecoder = nullptr;
        int w=0,h=0,b=0,c=0;
        assert(lj92_open(&twoTableDecoder, const_cast<uint8_t*>(jpeg.data()), jpeg.size(),
                         &w,&h,&b,&c) == LJ92_ERROR_NONE);
        std::array<uint16_t,2> result{};
        assert(lj92_decode(twoTableDecoder, result.data(), 2, 0, nullptr, 0) == LJ92_ERROR_NONE);
        lj92_close(twoTableDecoder);
        assert(w == 1 && h == 1 && b == 8 && c == 2);
        assert(result[0] == 10 && result[1] == 20);

        // Component selectors, rather than their position in SOS, define the
        // destination channel. Exercise a valid scan whose order differs from SOF.
        auto reordered = jpeg;
        const std::array<uint8_t, 2> sosMarker = {0xff, 0xda};
        const auto reorderedSos = std::search(
            reordered.begin(), reordered.end(), sosMarker.begin(), sosMarker.end());
        assert(reorderedSos != reordered.end());
        reorderedSos[5] = 1;
        reorderedSos[7] = 0;
        twoTableDecoder = nullptr;
        assert(lj92_open(&twoTableDecoder, reordered.data(), reordered.size(),
                         &w,&h,&b,&c) == LJ92_ERROR_NONE);
        result = {};
        assert(lj92_decode(twoTableDecoder, result.data(), 2, 0, nullptr, 0) == LJ92_ERROR_NONE);
        lj92_close(twoTableDecoder);
        assert(result[0] == 20 && result[1] == 10);

        // Reject a frame component count that exceeds liblj92's fixed storage.
        auto excessiveComponents = jpeg;
        excessiveComponents[11] = 5;
        twoTableDecoder = nullptr;
        assert(lj92_open(&twoTableDecoder, excessiveComponents.data(), excessiveComponents.size(),
                         &w,&h,&b,&c) == LJ92_ERROR_CORRUPT);
        assert(twoTableDecoder == nullptr);
    }

    constexpr unsigned int width = 32;
    constexpr unsigned int height = 24;
    constexpr unsigned short bits = 12;

    std::vector<uint16_t> pixels(width * height);
    for (unsigned int y = 0; y < height; ++y) {
        for (unsigned int x = 0; x < width; ++x) {
            pixels[y * width + x] =
                static_cast<uint16_t>((x * 31 + y * 17) & 0x0fff);
        }
    }

    tinydngwriter::DNGImage image;
    image.SetBigEndian(false);
    assert(image.SetImageWidth(width));
    assert(image.SetImageLength(height));
    assert(image.SetRowsPerStrip(height));
    assert(image.SetSamplesPerPixel(1));
    assert(image.SetBitsPerSample(1, &bits));
    assert(image.SetCompression(tinydngwriter::COMPRESSION_JPEG));
    assert(image.SetPhotometric(tinydngwriter::PHOTOMETRIC_CFA));
    assert(image.SetPlanarConfig(tinydngwriter::PLANARCONFIG_CONTIG));
    assert(image.SetWhiteLevel(0x0fff));
    assert(image.SetImageData(
        reinterpret_cast<const unsigned char*>(pixels.data()),
        pixels.size() * sizeof(uint16_t)));
    assert(image.GetStripBytes() > 0);
    assert(image.GetStripBytes() < pixels.size() * sizeof(uint16_t));

    tinydngwriter::DNGWriter writer(false);
    assert(writer.AddImage(&image));
    std::ostringstream output(std::ios::binary);
    std::string error;
    assert(writer.WriteToFile(output, &error));
    assert(error.empty());
    assert(output.str().size() > image.GetStripBytes());

    std::string dng = output.str();
    const char jpegMarker[] = {
        static_cast<char>(0xff), static_cast<char>(0xd8)};
    auto jpegStart = std::search(
        dng.begin(), dng.end(), std::begin(jpegMarker), std::end(jpegMarker));
    assert(jpegStart != dng.end());

    lj92 decoder = nullptr;
    int decodedWidth = 0;
    int decodedHeight = 0;
    int decodedBits = 0;
    int decodedComponents = 0;
    auto* encodedData = reinterpret_cast<uint8_t*>(&*jpegStart);
    int encodedSize = static_cast<int>(dng.end() - jpegStart);
    assert(lj92_open(
        &decoder,
        encodedData,
        encodedSize,
        &decodedWidth,
        &decodedHeight,
        &decodedBits,
        &decodedComponents) == LJ92_ERROR_NONE);
    assert(decodedWidth == static_cast<int>(width));
    assert(decodedHeight == static_cast<int>(height));
    assert(decodedBits == bits);
    assert(decodedComponents == 1);

    std::vector<uint16_t> decoded(pixels.size());
    assert(lj92_decode(
        decoder, decoded.data(), decodedWidth, 0, nullptr, 0) ==
        LJ92_ERROR_NONE);
    lj92_close(decoder);
    assert(decoded == pixels);

    // Camera DNGs commonly store lossless JPEG as independently coded tiles.
    // Include partial right/bottom tiles and verify canonical row placement.
    {
        constexpr uint32_t tiledWidth = 8, tiledHeight = 5;
        std::vector<uint16_t> tiledPixels(tiledWidth * tiledHeight);
        for (uint32_t y = 0; y < tiledHeight; ++y)
            for (uint32_t x = 0; x < tiledWidth; ++x)
                tiledPixels[y * tiledWidth + x] = static_cast<uint16_t>(y * 100 + x);
        auto tiled = tiledLosslessJpegDng(
            tiledPixels, tiledWidth, tiledHeight, 4, 3, bits, 2);
        assert(motioncam::DNGDecoder::ensureUncompressed(tiled));
        auto read16 = [&](size_t at) { return static_cast<uint16_t>(tiled[at] | tiled[at + 1] << 8); };
        auto read32 = [&](size_t at) { return static_cast<uint32_t>(tiled[at] | tiled[at + 1] << 8 |
            tiled[at + 2] << 16 | tiled[at + 3] << 24); };
        uint32_t stripOffset = 0, stripBytes = 0;
        const uint16_t count = read16(8);
        for (uint16_t i = 0; i < count; ++i) {
            const size_t at = 10 + i * 12;
            if (read16(at) == 259) assert(read16(at + 8) == 1);
            if (read16(at) == 273) stripOffset = read32(at + 8);
            if (read16(at) == 279) stripBytes = read32(at + 8);
        }
        assert(stripOffset && stripBytes == tiledPixels.size() * sizeof(uint16_t));
        for (size_t i = 0; i < tiledPixels.size(); ++i)
            assert(read16(stripOffset + i * 2) == tiledPixels[i]);

        // TIFF tiles keep their declared dimensions at image boundaries and
        // pad samples outside the image. Those padding samples must be ignored.
        auto padded = tiledLosslessJpegDng(
            tiledPixels, tiledWidth, tiledHeight, 4, 3, bits, 2, true);
        assert(motioncam::DNGDecoder::ensureUncompressed(padded));
        auto padded16 = [&](size_t at) {
            return static_cast<uint16_t>(padded[at] | padded[at + 1] << 8);
        };
        auto padded32 = [&](size_t at) {
            return static_cast<uint32_t>(padded[at] | padded[at + 1] << 8 |
                padded[at + 2] << 16 | padded[at + 3] << 24);
        };
        uint32_t paddedStripOffset = 0;
        for (uint16_t i = 0; i < padded16(8); ++i) {
            const size_t at = 10 + i * 12;
            if (padded16(at) == 273) paddedStripOffset = padded32(at + 8);
        }
        assert(paddedStripOffset);
        for (size_t i = 0; i < tiledPixels.size(); ++i)
            assert(padded16(paddedStripOffset + i * 2) == tiledPixels[i]);

        // A short interior tile must not be accepted and padded with black data.
        auto shortTile = tiledLosslessJpegDng(
            tiledPixels, tiledWidth, tiledHeight, 4, 3, bits, 2);
        const std::array<uint8_t, 2> sofMarker = {0xff, 0xc3};
        const auto sof = std::search(shortTile.begin(), shortTile.end(),
                                     sofMarker.begin(), sofMarker.end());
        assert(sof != shortTile.end());
        // JPEG width is two pixels with two components; make it one pixel.
        sof[7] = 0;
        sof[8] = 1;
        assert(!motioncam::DNGDecoder::ensureUncompressed(shortTile));
    }

    constexpr unsigned int rgbWidth = 19;
    constexpr unsigned int rgbHeight = 13;
    constexpr int rgbComponents = 3;
    std::vector<uint16_t> rgb(rgbWidth * rgbHeight * rgbComponents);
    for (unsigned int y = 0; y < rgbHeight; ++y) {
        for (unsigned int x = 0; x < rgbWidth; ++x) {
            const auto offset = (y * rgbWidth + x) * rgbComponents;
            rgb[offset + 0] = static_cast<uint16_t>((x * 37 + y * 11) & 0x0fff);
            rgb[offset + 1] = static_cast<uint16_t>((x * 13 + y * 43 + 700) & 0x0fff);
            rgb[offset + 2] = static_cast<uint16_t>((x * 29 + y * 7 + 1400) & 0x0fff);
        }
    }

    uint8_t* rgbEncoded = nullptr;
    int rgbEncodedSize = 0;
    assert(lj92_encode(
        rgb.data(), rgbWidth, rgbHeight, bits, rgbComponents,
        rgbWidth * rgbComponents, 0, nullptr, 0,
        &rgbEncoded, &rgbEncodedSize) == LJ92_ERROR_NONE);
    assert(rgbEncoded != nullptr);
    assert(rgbEncodedSize > 0);

    decoder = nullptr;
    assert(lj92_open(
        &decoder, rgbEncoded, rgbEncodedSize,
        &decodedWidth, &decodedHeight, &decodedBits,
        &decodedComponents) == LJ92_ERROR_NONE);
    assert(decodedWidth == static_cast<int>(rgbWidth));
    assert(decodedHeight == static_cast<int>(rgbHeight));
    assert(decodedBits == bits);
    assert(decodedComponents == rgbComponents);

    std::vector<uint16_t> decodedRgb(rgb.size());
    assert(lj92_decode(
        decoder, decodedRgb.data(), decodedWidth * rgbComponents,
        0, nullptr, 0) == LJ92_ERROR_NONE);
    lj92_close(decoder);
    free(rgbEncoded);
    assert(decodedRgb == rgb);

    // Alternating mosaiced data can be strongly correlated diagonally. The
    // former hard-coded JPEG predictor 6 compares unlike neighboring samples
    // and compresses this kind of data poorly; select diagonal predictor 3.
    constexpr int cfaWidth = 256;
    constexpr int cfaHeight = 192;
    std::vector<uint16_t> cfa(cfaWidth * cfaHeight);
    for (int y = 0; y < cfaHeight; ++y)
        for (int x = 0; x < cfaWidth; ++x)
            cfa[y * cfaWidth + x] = ((x + y) & 1) ? 3000 : 600;
    uint8_t* cfaEncoded = nullptr;
    int cfaEncodedSize = 0;
    assert(lj92_encode(cfa.data(), cfaWidth, cfaHeight, bits, 1,
                      cfaWidth, 0, nullptr, 0,
                      &cfaEncoded, &cfaEncodedSize) == LJ92_ERROR_NONE);
    assert(cfaEncodedSize < static_cast<int>(cfa.size() * sizeof(uint16_t) / 8));
    const std::array<uint8_t, 2> startOfScan = {0xff, 0xda};
    const auto sos = std::search(cfaEncoded, cfaEncoded + cfaEncodedSize,
                                 startOfScan.begin(), startOfScan.end());
    assert(sos != cfaEncoded + cfaEncodedSize);
    const int componentsInScan = sos[4];
    assert(sos[5 + componentsInScan * 2] == 3);
    free(cfaEncoded);

    tinydngwriter::GainMapParams gainMap{};
    gainMap.top = 4; gainMap.left = 6; gainMap.bottom = height; gainMap.right = width;
    gainMap.plane = 0; gainMap.planes = 1;
    gainMap.row_pitch = 1; gainMap.col_pitch = 1;
    gainMap.map_points_v = 2; gainMap.map_points_h = 2;
    // Deliberately use the legacy 1/pointCount spacing. Full-sensor remapping
    // must expand the final control point to the sensor edge.
    gainMap.map_spacing_v = 0.5; gainMap.map_spacing_h = 0.5;
    gainMap.map_origin_v = 0.0; gainMap.map_origin_h = 0.0;
    gainMap.map_planes = 4;
    gainMap.gain_data.resize(16);
    for (size_t plane = 0; plane < 4; ++plane)
        for (size_t point = 0; point < 4; ++point)
            gainMap.gain_data[plane * 4 + point] = 1.0f + 0.1f * plane + 0.05f * point;
    tinydngwriter::OpcodeList opcodes;
    opcodes.AddGainMap(gainMap);
    assert(image.SetOpcodeList2(opcodes));
    std::ostringstream gainMapOutput(std::ios::binary);
    assert(writer.WriteToFile(gainMapOutput, &error));
    const std::string gainMapDng = gainMapOutput.str();
    std::vector<uint8_t> croppedGainMapDng(gainMapDng.begin(), gainMapDng.end());
    std::vector<motioncam::GainMap> originalMaps;
    assert(motioncam::DNGDecoder::getGainMaps(croppedGainMapDng, 2, originalMaps));
    assert(!originalMaps.empty());
    auto replacementMaps = originalMaps;
    std::fill(replacementMaps.front().data.begin(), replacementMaps.front().data.end(), 2.0f);
    std::vector<uint8_t> replacedGainMapDng(gainMapDng.begin(), gainMapDng.end());
    assert(motioncam::DNGDecoder::replaceGainMaps(replacedGainMapDng, 2, replacementMaps));
    std::vector<motioncam::GainMap> replacedMaps;
    assert(motioncam::DNGDecoder::getGainMaps(replacedGainMapDng, 2, replacedMaps));
    assert(replacedMaps.size() == replacementMaps.size());
    assert(replacedMaps.front().data == replacementMaps.front().data);
    std::vector<uint8_t> clearedGainMapDng(gainMapDng.begin(), gainMapDng.end());
    assert(motioncam::DNGDecoder::replaceGainMaps(clearedGainMapDng, 2, {}));
    std::vector<motioncam::GainMap> clearedMaps;
    assert(!motioncam::DNGDecoder::getGainMaps(clearedGainMapDng, 2, clearedMaps));
    assert(clearedMaps.empty());
    auto cleared16 = [&](size_t offset) {
        return static_cast<uint16_t>(clearedGainMapDng[offset] |
                                     clearedGainMapDng[offset + 1] << 8);
    };
    auto cleared32 = [&](size_t offset) {
        return static_cast<uint32_t>(clearedGainMapDng[offset] |
            clearedGainMapDng[offset + 1] << 8 | clearedGainMapDng[offset + 2] << 16 |
            clearedGainMapDng[offset + 3] << 24);
    };
    const uint32_t clearedIfd = cleared32(4);
    bool foundClearedOpcodeList = false;
    for (uint16_t i = 0; i < cleared16(clearedIfd); ++i) {
        const size_t entry = static_cast<size_t>(clearedIfd) + 2 + i * 12;
        if (cleared16(entry) != 51009) continue;
        assert(cleared32(entry + 4) == 4);
        assert(cleared32(entry + 8) == 0);
        foundClearedOpcodeList = true;
    }
    assert(foundClearedOpcodeList);
    std::vector<uint8_t> addedGainMapDng(gainMapDng.begin(), gainMapDng.end());
    auto read16le = [&](size_t offset) {
        return static_cast<uint16_t>(addedGainMapDng[offset] |
                                     addedGainMapDng[offset + 1] << 8);
    };
    auto read32le = [&](size_t offset) {
        return static_cast<uint32_t>(addedGainMapDng[offset] |
            addedGainMapDng[offset + 1] << 8 | addedGainMapDng[offset + 2] << 16 |
            addedGainMapDng[offset + 3] << 24);
    };
    const uint32_t rootIfd = read32le(4);
    bool removedOpcodeTag = false;
    for (uint16_t i = 0; i < read16le(rootIfd); ++i) {
        const size_t entry = static_cast<size_t>(rootIfd) + 2 + i * 12;
        if (read16le(entry) != 51009) continue;
        addedGainMapDng[entry] = 0;
        addedGainMapDng[entry + 1] = 0;
        removedOpcodeTag = true;
        break;
    }
    assert(removedOpcodeTag);
    assert(motioncam::DNGDecoder::replaceGainMaps(addedGainMapDng, 2, replacementMaps));
    std::vector<motioncam::GainMap> addedMaps;
    assert(motioncam::DNGDecoder::getGainMaps(addedGainMapDng, 2, addedMaps));
    assert(addedMaps.front().data == replacementMaps.front().data);
    assert(motioncam::DNGDecoder::cropGainMapsToFullSensor(
        croppedGainMapDng, width * 2, height * 2));
    std::vector<motioncam::GainMap> croppedMaps;
    assert(motioncam::DNGDecoder::getGainMaps(croppedGainMapDng, 2, croppedMaps));
    assert(croppedMaps.size() == originalMaps.size());
    assert(std::abs(croppedMaps.front().spacingH - 1.0) < 1e-9);
    assert(std::abs(croppedMaps.front().spacingV - 1.0) < 1e-9);
    assert(std::abs(croppedMaps.front().originH) < 1e-9);
    assert(std::abs(croppedMaps.front().originV) < 1e-9);
    assert(croppedMaps.front().data != originalMaps.front().data);
    std::vector<uint8_t> canonical(gainMapDng.begin(), gainMapDng.end());
    assert(motioncam::DNGDecoder::canonicalizeGainMapOpcodes(canonical));
    std::vector<motioncam::GainMap> canonicalMaps;
    assert(motioncam::DNGDecoder::getGainMaps(canonical, 2, canonicalMaps));
    assert(canonicalMaps.size() == 4);
    for (size_t phase = 0; phase < canonicalMaps.size(); ++phase) {
        assert(canonicalMaps[phase].channels == 1);
        assert(canonicalMaps[phase].rowPitch == 2);
        assert(canonicalMaps[phase].colPitch == 2);
        assert(canonicalMaps[phase].top == gainMap.top + phase / 2);
        assert(canonicalMaps[phase].left == gainMap.left + phase % 2);
    }
    std::vector<uint8_t> baked(gainMapDng.begin(), gainMapDng.end());
    const size_t originalSize = baked.size();
    assert(motioncam::DNGDecoder::bakeGainMaps(baked, false, false));
    assert(baked.size() > originalSize);
    assert(motioncam::DNGDecoder::canonicalizeGainMapOpcodes(baked));

    std::vector<uint8_t> debugBaked(gainMapDng.begin(), gainMapDng.end());
    assert(motioncam::DNGDecoder::bakeGainMaps(
        debugBaked, false, false, false, true));
    assert(debugBaked != baked);

    std::vector<uint8_t> croppedDebug(gainMapDng.begin(), gainMapDng.end());
    assert(motioncam::DNGDecoder::cropGainMapsToFullSensor(
        croppedDebug, width * 2, height * 2));
    assert(motioncam::DNGDecoder::bakeGainMaps(
        croppedDebug, false, false, false, true));
    assert(croppedDebug != debugBaked);

    // Existing linearization must be consumed before gain-map baking rather
    // than making the vignette path reject the DNG.
    std::vector<uint8_t> linearizedThenBaked(gainMapDng.begin(), gainMapDng.end());
    assert(motioncam::DNGDecoder::ensureUncompressed(linearizedThenBaked));
    assert(motioncam::DNGDecoder::applyLogTransform(
        linearizedThenBaked, motioncam::LogTransformMode::KeepInput));
    assert(motioncam::DNGDecoder::bakeGainMaps(linearizedThenBaked, false, false));

    // A raw IFD may be reached through the ordinary TIFF next-IFD chain rather
    // than IFD0 or SubIFDs. Rebuilding it for the log table must preserve that chain.
    std::vector<uint8_t> nextIfdLog(gainMapDng.begin(), gainMapDng.end());
    assert(motioncam::DNGDecoder::ensureUncompressed(nextIfdLog));
    auto put16le = [](std::vector<uint8_t>& bytes, size_t at, uint16_t value) {
        bytes[at] = static_cast<uint8_t>(value);
        bytes[at + 1] = static_cast<uint8_t>(value >> 8);
    };
    auto put32le = [](std::vector<uint8_t>& bytes, size_t at, uint32_t value) {
        for (int i = 0; i < 4; ++i)
            bytes[at + i] = static_cast<uint8_t>(value >> (8 * i));
    };
    const uint32_t rawIfd = static_cast<uint32_t>(nextIfdLog[4] |
        nextIfdLog[5] << 8 | nextIfdLog[6] << 16 | nextIfdLog[7] << 24);
    const uint32_t emptyRoot = static_cast<uint32_t>(nextIfdLog.size());
    nextIfdLog.resize(nextIfdLog.size() + 6);
    put16le(nextIfdLog, emptyRoot, 0);
    put32le(nextIfdLog, emptyRoot + 2, rawIfd);
    put32le(nextIfdLog, 4, emptyRoot);
    assert(motioncam::DNGDecoder::applyLogTransform(
        nextIfdLog, motioncam::LogTransformMode::KeepInput));
    const uint32_t rebuiltRawIfd = static_cast<uint32_t>(nextIfdLog[emptyRoot + 2] |
        nextIfdLog[emptyRoot + 3] << 8 | nextIfdLog[emptyRoot + 4] << 16 |
        nextIfdLog[emptyRoot + 5] << 24);
    assert(rebuiltRawIfd != 0 && rebuiltRawIfd != rawIfd);

    std::vector<uint8_t> ordinaryBayer(gainMapDng.begin(), gainMapDng.end());
    assert(motioncam::DNGDecoder::ensureUncompressed(ordinaryBayer));
    const std::vector<uint8_t> beforeNoopRemosaic = ordinaryBayer;
    const std::array<uint8_t, 4> ordinaryPhase = {0, 1, 1, 2};
    assert(motioncam::DNGDecoder::processHigherCFA(
        ordinaryBayer, 2, ordinaryPhase, motioncam::QuadBayerMode::Demosaic,
        true, 1, true));
    assert(ordinaryBayer == beforeNoopRemosaic);

    std::vector<uint8_t> colorBaked(gainMapDng.begin(), gainMapDng.end());
    assert(motioncam::DNGDecoder::bakeGainMaps(colorBaked, false, true));
    // The unbaked luminance remainder is a single-map-plane OpcodeList3
    // operation targeting all three post-demosaic image planes.
    std::vector<motioncam::GainMap> luminanceMaps;
    assert(motioncam::DNGDecoder::getGainMaps(colorBaked, 3, luminanceMaps));
    assert(luminanceMaps.size() == 1);
    assert(luminanceMaps.front().plane == 0);
    assert(luminanceMaps.front().planes == 3);
    assert(luminanceMaps.front().top == 0 && luminanceMaps.front().left == 0);
    assert(luminanceMaps.front().bottom == height && luminanceMaps.front().right == width);
    assert(luminanceMaps.front().rowPitch == 1);
    assert(luminanceMaps.front().colPitch == 1);
    assert(luminanceMaps.front().channels == 1);

    // Legacy MotionCam DNGs store the four CFA phases as four separate
    // one-channel GainMap opcodes. All of them must be transformed in place.
    tinydngwriter::OpcodeList fourGainOpcodes;
    for (unsigned int phase = 0; phase < 4; ++phase) {
        tinydngwriter::GainMapParams phaseMap = gainMap;
        phaseMap.top = phase / 2;
        phaseMap.left = phase % 2;
        phaseMap.row_pitch = 2;
        phaseMap.col_pitch = 2;
        phaseMap.map_planes = 1;
        phaseMap.gain_data.resize(4);
        for (size_t point = 0; point < phaseMap.gain_data.size(); ++point)
            phaseMap.gain_data[point] = 1.0f + 0.1f * phase + 0.05f * point;
        fourGainOpcodes.AddGainMap(phaseMap);
    }
    assert(image.SetOpcodeList2(fourGainOpcodes));
    const std::array<unsigned char, 4> bggr = {2, 1, 1, 0};
    assert(image.SetCFARepeatPatternDim(2, 2));
    assert(image.SetCFAPattern(bggr.size(), bggr.data()));
    std::ostringstream fourGainOutput(std::ios::binary);
    assert(writer.WriteToFile(fourGainOutput, &error));
    const std::string fourGainDng = fourGainOutput.str();
    std::vector<uint8_t> fourGainBytes(fourGainDng.begin(), fourGainDng.end());
    assert(motioncam::DNGDecoder::repairGainMapCfaPhase(fourGainBytes, true));
    assert(motioncam::DNGDecoder::transformGainMaps(fourGainBytes, false, true, false));

    const std::string softwarePlaceholder(32, 'X');
    assert(image.SetSoftware(softwarePlaceholder));
    std::ostringstream producerOutput(std::ios::binary);
    assert(writer.WriteToFile(producerOutput, &error));
    const std::string producerTemplate = producerOutput.str();
    auto producerIsRepaired = [&](const std::string& software) {
        std::vector<uint8_t> bytes(producerTemplate.begin(), producerTemplate.end());
        const auto placeholder = std::search(bytes.begin(), bytes.end(),
            softwarePlaceholder.begin(), softwarePlaceholder.end());
        assert(placeholder != bytes.end() && software.size() < softwarePlaceholder.size());
        std::fill_n(placeholder, softwarePlaceholder.size(), 0);
        std::copy(software.begin(), software.end(), placeholder);
        const std::vector<uint8_t> original = bytes;
        assert(motioncam::DNGDecoder::repairGainMapCfaPhase(bytes));
        return bytes != original;
    };
    assert(producerIsRepaired("MotionCam 4.0.4"));
    assert(!producerIsRepaired("MotionCam 4.0.5"));
    assert(producerIsRepaired("MotionCam Tools 1.0.0.0"));
    assert(!producerIsRepaired("MotionCam Tools 1.0.0.1"));
    assert(producerIsRepaired("MotionCam Tools"));
    assert(!producerIsRepaired("MotionCamera 3.0"));

    return 0;
}
