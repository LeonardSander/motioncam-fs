#define TINY_DNG_WRITER_IMPLEMENTATION
#include "tinydng/tiny_dng_writer.h"
#include "liblj92/lj92.h"
#include "DNGDecoder.h"
#include "VulkanJpegDecode.h"
#include "VulkanGalleryPreview.h"
#ifdef MOTIONCAM_HAS_DIRECTLOG_PLACEBO
#include <libplacebo/gpu.h>
#endif
#include "VulkanJpegFullEncode.h"
#include "VulkanGainBake.h"
#include "GainMapBake.h"
#include "Utils.h"
#include <jpeglib.h>

#include <algorithm>
#include <atomic>
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <cassert>
#include <cmath>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <cstdlib>
#include <cstdio>
#include <fstream>
#include <filesystem>
#include <iterator>
#include <sstream>
#include <string>
#include <thread>
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

static int benchmarkRealJpegDng(const char* path) {
    std::ifstream file(path, std::ios::binary);
    if (!file) return 2;
    const std::vector<uint8_t> dng((std::istreambuf_iterator<char>(file)),
                                    std::istreambuf_iterator<char>());
    if (dng.size() < 8 || dng[0] != 'I' || dng[1] != 'I' ||
        dng[2] != 42 || dng[3] != 0) return 3;
    auto u16 = [&](size_t at) -> uint32_t {
        return uint32_t(dng[at]) | (uint32_t(dng[at + 1]) << 8u);
    };
    auto u32 = [&](size_t at) -> uint32_t {
        return u16(at) | (u16(at + 2) << 16u);
    };
    const size_t ifd = u32(4);
    if (ifd > dng.size() - 2) return 3;
    const uint32_t entries = u16(ifd);
    if (entries > (dng.size() - ifd - 2u) / 12u) return 3;
    auto tag = [&](uint16_t wanted) -> uint32_t {
        for (uint32_t i = 0; i < entries; ++i) {
            const size_t at = ifd + 2u + i * 12u;
            if (u16(at) == wanted && u32(at + 4u) == 1u)
                return u16(at + 2u) == 3u ? u16(at + 8u) : u32(at + 8u);
        }
        return 0;
    };
    const uint32_t width = tag(256), height = tag(257), bits = tag(258);
    const uint32_t compression = tag(259), channels = tag(277);
    const uint32_t offset = tag(273), size = tag(279);
    if (!width || !height || !size || compression != 7u ||
        (channels != 1u && channels != 3u) ||
        bits < 8u || bits > 16u || tag(278) != height ||
        size > dng.size() || offset > dng.size() - size) return 3;
    const uint8_t* jpeg = dng.data() + offset;
    lj92 decoder = nullptr;
    int jpegWidth = 0, jpegHeight = 0, jpegBits = 0, jpegChannels = 0;
    if (lj92_open(&decoder, const_cast<uint8_t*>(jpeg), int(size), &jpegWidth, &jpegHeight,
                  &jpegBits, &jpegChannels) != LJ92_ERROR_NONE) return 4;
    if (jpegWidth != int(width) || jpegHeight != int(height) ||
        jpegBits != int(bits) || jpegChannels != int(channels)) {
        lj92_close(decoder);
        return 4;
    }
    std::vector<uint16_t> cpu(size_t(width) * height * channels);
    const auto cpuStart = std::chrono::steady_clock::now();
    const bool cpuOkay = lj92_decode(decoder, cpu.data(), width * channels,
                                    0, nullptr, 0) == LJ92_ERROR_NONE;
    const auto cpuEnd = std::chrono::steady_clock::now();
    lj92_close(decoder);
    if (!cpuOkay) return 4;
    const double cpuMs = std::chrono::duration<double, std::milli>(
        cpuEnd - cpuStart).count();
    if (cpu.size() > 65536u) {
        std::fprintf(stderr,
            "Real DNG JPEG decode: size=%ux%u channels=%u bits=%u "
            "strip_bytes=%u streams=1 cpu_ms=%.3f gpu=serial_scan_unsupported\n",
            width, height, channels, bits, size, cpuMs);
        return 0;
    }
    const motioncam::VulkanJpegTile tile{jpeg, size, width, height, bits, channels};
    const motioncam::VulkanJpegContiguousSource contiguous{jpeg, size};
    for (int attempt = 0; attempt < 2; ++attempt) {
        std::vector<std::vector<uint16_t>> gpu;
        const auto started = std::chrono::steady_clock::now();
        const bool ok = motioncam::jpegDecodeTilesVulkan({tile}, gpu, &contiguous);
        const auto finished = std::chrono::steady_clock::now();
        const bool equal = ok && gpu.size() == 1u && gpu.front() == cpu;
        std::fprintf(stderr,
            "Real DNG JPEG decode: size=%ux%u channels=%u bits=%u strip_bytes=%u "
            "cpu_ms=%.3f gpu_%s_ms=%.3f equal=%d\n",
            width, height, channels, bits, size,
            cpuMs,
            attempt ? "warm" : "cold",
            std::chrono::duration<double, std::milli>(finished - started).count(),
            equal);
        if (!equal) return 5;
    }
    return 0;
}

int main() {
    // Adapting a common GPU image to vector readback must keep its lease.
    {
        auto owner = std::make_shared<int>(7);
        std::weak_ptr<int> lifetime = owner;
        motioncam::DNGGpuImage source;
        source.buffer = owner.get();
        source.width = source.height = 2; source.channels = 1;
        source.lease = owner;
        source.download = [lifetime](uint16_t* samples, size_t count) {
            assert(!lifetime.expired() && count == 4);
            std::fill_n(samples, count, 17);
            return true;
        };
        motioncam::PreviewFrame frame;
        frame.gallerySource = source;
        assert(frame.hasGalleryCfa());
        auto readback = frame.gallerySource.vectorDownloader();
        source = {}; frame.gallerySource = {}; owner.reset();
        assert(!lifetime.expired() && !frame.hasGalleryCfa());
        std::vector<uint16_t> samples;
        assert(readback(samples) && samples == std::vector<uint16_t>(4, 17));
        readback = {};
        assert(lifetime.expired());
    }
    // A displayed texture must be destroyed before the decoder device it uses.
    {
        bool resourceReleased = false;
        auto source = std::make_shared<int>(1);
        std::weak_ptr<int> device = source;
        auto frame = std::make_shared<motioncam::VulkanGalleryGpuBuffer>();
        frame->sourceLease = std::move(source);
        frame->resourceLease = std::shared_ptr<void>(new int(2), [&](void* value) {
            assert(!device.expired());
            resourceReleased = true;
            delete static_cast<int*>(value);
        });
        frame.reset();
        assert(resourceReleased && device.expired());
    }
    if (const char* path = std::getenv("MOTIONCAM_GPU_JPEG_REAL_DNG_BENCHMARK"))
        return benchmarkRealJpegDng(path);
    // Only the two CFA representations may be split into color and luminance.
    {
        const auto directory = std::filesystem::temp_directory_path() /
            ("mcfuse-incomplete-sequence-" + std::to_string(
                std::chrono::steady_clock::now().time_since_epoch().count()));
        std::filesystem::create_directory(directory);
        struct Cleanup {
            std::filesystem::path path;
            ~Cleanup() { std::error_code ignored; std::filesystem::remove_all(path, ignored); }
        } cleanup{directory};
        const auto complete = tiledLosslessJpegDng(std::vector<uint16_t>(256, 512),
                                                   16, 16, 8, 8, 10);
        auto write = [&](const char* name, size_t size) {
            std::ofstream output(directory / name, std::ios::binary);
            output.write(reinterpret_cast<const char*>(complete.data()), size);
        };
        write("frame-000001.dng", complete.size());
        write("frame-000002.dng", 0);
        write("frame-000003.dng", 8);
        motioncam::DNGDecoder sequence(directory.string());
        assert(sequence.getFrames().size() == 1);
        bool rejected = false;
        try { motioncam::DNGDecoder invalid((directory / "frame-000002.dng").string()); }
        catch (const std::runtime_error&) { rejected = true; }
        assert(rejected);
    }
    motioncam::GainMap region{};
    region.top = 0; region.left = 0; region.bottom = 8; region.right = 8;
    region.plane = 0; region.planes = 1;
    region.rowPitch = region.colPitch = 2;
    region.width = region.height = 2;
    region.spacingV = region.spacingH = 1.0;
    region.channels = 1;
    region.data.assign(4, 2.0f);
    std::vector<motioncam::GainMap> unrelatedRegions(4, region);
    unrelatedRegions[1].left = 2;
    unrelatedRegions[2].top = 2;
    unrelatedRegions[3].top = unrelatedRegions[3].left = 2;
    const auto originalRegions = unrelatedRegions;
    assert(!motioncam::reduceGainMapStackToColor(unrelatedRegions));
    for (size_t i = 0; i < unrelatedRegions.size(); ++i)
        assert(unrelatedRegions[i].data == originalRegions[i].data);

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
        motioncam::DecodedDNGImage decoded;
        assert(motioncam::DNGDecoder::decodeImage(planar, decoded, false, false));
        assert(decoded.layout.width == w && decoded.layout.height == h &&
               decoded.samples.size() == expected.size());
        for (size_t i = 0; i < expected.size(); ++i)
            assert(decoded.samples[i] == expected[i]);
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
        motioncam::DecodedDNGImage decoded;
        assert(motioncam::DNGDecoder::decodeImage(planar, decoded, false, false));
        assert(decoded.layout.width == w && decoded.layout.height == h &&
               decoded.samples.size() == expected.size());
        for (size_t i = 0; i < expected.size(); ++i)
            assert(decoded.samples[i] == expected[i]);
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

    // Independent lossless JPEG strips decode into disjoint output rows. The
    // last scan is padded to RowsPerStrip, exercising the fallback copy path.
    {
        constexpr uint32_t w = 16, h = 10, rowsPerStrip = 4;
        std::vector<uint16_t> expected(w * h);
        for (size_t i = 0; i < expected.size(); ++i)
            expected[i] = static_cast<uint16_t>((i * 173u + i / w * 997u) & 65535u);
        std::vector<std::vector<uint8_t>> strips;
        for (uint32_t firstRow = 0; firstRow < h; firstRow += rowsPerStrip) {
            std::vector<uint16_t> samples(w * rowsPerStrip, 0);
            const uint32_t rows = std::min(rowsPerStrip, h - firstRow);
            std::copy_n(expected.data() + static_cast<size_t>(firstRow) * w,
                        static_cast<size_t>(rows) * w, samples.data());
            uint8_t* encoded = nullptr;
            int bytes = 0;
            assert(lj92_encode(samples.data(), w, rowsPerStrip, 16, 1,
                               w, 0, nullptr, 0, &encoded, &bytes) == LJ92_ERROR_NONE);
            strips.emplace_back(encoded, encoded + bytes);
            free(encoded);
        }
        auto dng = chunkedDng(w, h, 16, 1, 7, w, rowsPerStrip,
                              false, false, strips);
        motioncam::DecodedDNGImage decoded;
        assert(motioncam::DNGDecoder::decodeImage(dng, decoded, false, false));
        assert(decoded.samples == expected);
        assert(motioncam::DNGDecoder::ensureUncompressed(dng));
        assert(ifdsAreTagSorted(dng));
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
    assert(image.SetImageDescription("metadata added after compression"));
    assert((image.GetStripOffset() & 1u) == 0);
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

    // The writer also accepts byte buffers without uint16_t alignment.
    std::vector<uint8_t> unaligned(pixels.size() * sizeof(uint16_t) + 1);
    std::memcpy(unaligned.data() + 1, pixels.data(), pixels.size() * sizeof(uint16_t));
    unsigned char *alignedJpeg = nullptr, *unalignedJpeg = nullptr;
    int alignedJpegSize = 0, unalignedJpegSize = 0;
    std::string jpegError;
    assert(tinydngwriter::CompressJPEGLossless(
        reinterpret_cast<const uint8_t*>(pixels.data()), pixels.size() * sizeof(uint16_t),
        width, height, 1, bits, &alignedJpeg, &alignedJpegSize, &jpegError));
    assert(tinydngwriter::CompressJPEGLossless(
        unaligned.data() + 1, pixels.size() * sizeof(uint16_t),
        width, height, 1, bits, &unalignedJpeg, &unalignedJpegSize, &jpegError));
    assert(alignedJpegSize == unalignedJpegSize);
    assert(std::memcmp(alignedJpeg, unalignedJpeg, alignedJpegSize) == 0);
    std::free(alignedJpeg);
    std::free(unalignedJpeg);
    pixels[0] = 1u << bits;
    unsigned char* invalidJpeg = nullptr;
    int invalidJpegSize = 0;
    assert(!tinydngwriter::CompressJPEGLossless(
        reinterpret_cast<const uint8_t*>(pixels.data()), pixels.size() * sizeof(uint16_t),
        width, height, 1, bits, &invalidJpeg, &invalidJpegSize, &jpegError));
    assert(invalidJpeg == nullptr);
    pixels[0] = 0;

    // Camera DNGs commonly store lossless JPEG as independently coded tiles.
    // Include partial right/bottom tiles and verify canonical row placement.
    {
        constexpr uint32_t tiledWidth = 8, tiledHeight = 5;
        std::vector<uint16_t> tiledPixels(tiledWidth * tiledHeight);
        for (uint32_t y = 0; y < tiledHeight; ++y)
            for (uint32_t x = 0; x < tiledWidth; ++x)
                tiledPixels[y * tiledWidth + x] = static_cast<uint16_t>(y * 100 + x);
        auto directTiles = tiledLosslessJpegDng(
            tiledPixels, tiledWidth, tiledHeight, 4, 3, bits);
        motioncam::DecodedDNGImage directDecoded;
        assert(motioncam::DNGDecoder::decodeImageBorrowed(directTiles, directDecoded));
        assert(directDecoded.samples == tiledPixels);
        auto tiled = tiledLosslessJpegDng(
            tiledPixels, tiledWidth, tiledHeight, 4, 3, bits, 2);
        const size_t compressedTiledSize = tiled.size();
        assert(motioncam::DNGDecoder::ensureUncompressed(tiled));
        // Canonicalization must replace compressed tile payloads, not retain
        // them as unreachable data before the new uncompressed strip.
        assert(tiled.size() < compressedTiledSize +
               tiledPixels.size() * sizeof(uint16_t));
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

    // Exercise the JPEG lossless category-16 difference and byte stuffing.
    std::vector<uint16_t> extreme(32 * 8);
    for (size_t i = 0; i < extreme.size(); ++i)
        extreme[i] = (i & 1) ? 65535 : 0;
    uint8_t* extremeEncoded = nullptr;
    int extremeSize = 0;
    assert(lj92_encode(extreme.data(), 32, 8, 16, 1, 32, 0,
                       nullptr, 0, &extremeEncoded, &extremeSize) == LJ92_ERROR_NONE);
    decoder = nullptr;
    assert(lj92_open(&decoder, extremeEncoded, extremeSize,
                     &decodedWidth, &decodedHeight, &decodedBits,
                     &decodedComponents) == LJ92_ERROR_NONE);
    std::vector<uint16_t> decodedExtreme(extreme.size());
    assert(lj92_decode(decoder, decodedExtreme.data(), 32, 0,
                       nullptr, 0) == LJ92_ERROR_NONE);
    lj92_close(decoder);
    free(extremeEncoded);
    assert(decodedExtreme == extreme);

    // A category seen only outside the predictor sample must still have a
    // Huffman code in the large-image path.
    std::vector<uint16_t> rareCategory(1024 * 1024, 1000);
    rareCategory[2] = 65535;
    uint8_t* rareEncoded = nullptr;
    int rareSize = 0;
    assert(lj92_encode(rareCategory.data(), 1024, 1024, 16, 1, 1024, 0,
                       nullptr, 0, &rareEncoded, &rareSize) == LJ92_ERROR_NONE);
    decoder = nullptr;
    assert(lj92_open(&decoder, rareEncoded, rareSize,
                     &decodedWidth, &decodedHeight, &decodedBits,
                     &decodedComponents) == LJ92_ERROR_NONE);
    std::vector<uint16_t> decodedRare(rareCategory.size());
    assert(lj92_decode(decoder, decodedRare.data(), 1024, 0,
                       nullptr, 0) == LJ92_ERROR_NONE);
    lj92_close(decoder);
    free(rareEncoded);
    assert(decodedRare == rareCategory);

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
    motioncam::RenderSettings gainOnlySettings;
    gainOnlySettings.options = static_cast<motioncam::FileRenderOptions>(
        motioncam::RENDER_OPT_APPLY_VIGNETTE_CORRECTION |
        motioncam::RENDER_OPT_DEBUG_SHADING_MAP);
    motioncam::PreviewFrame gainOnlyPreview;
    assert(motioncam::DNGDecoder::decodePreview(
        replacedGainMapDng, gainOnlySettings, gainOnlyPreview, false));
    assert(gainOnlyPreview.gainMapApplied);
    assert(std::any_of(gainOnlyPreview.rgb.begin(), gainOnlyPreview.rgb.end(),
                       [](uint8_t value) { return value != 0xff; }));
    // A scalar spatial map cannot be separated into CFA color and luminance.
    // Full preview baking may consume it, but color-only preview decoding must
    // reject it so the VFS can use the mounted-DNG transform fallback instead
    // of silently applying the complete vignette correction.
    auto scalarSpatialMaps = originalMaps;
    scalarSpatialMaps.front().channels = 1;
    scalarSpatialMaps.front().data.resize(
        static_cast<size_t>(scalarSpatialMaps.front().width) *
        scalarSpatialMaps.front().height);
    std::vector<uint8_t> scalarSpatialDng(gainMapDng.begin(), gainMapDng.end());
    assert(motioncam::DNGDecoder::replaceGainMaps(
        scalarSpatialDng, 2, scalarSpatialMaps));
    auto scalarCachedBake = scalarSpatialDng;
    auto scalarCachedBakeAgain = scalarSpatialDng;
    auto scalarReferenceBake = scalarSpatialDng;
    assert(motioncam::DNGDecoder::bakeGainMaps(scalarCachedBake, false, false));
    assert(motioncam::DNGDecoder::bakeGainMaps(scalarCachedBakeAgain, false, false));
#ifdef _WIN32
    _putenv_s("MOTIONCAM_GAIN_BAKE_CPU_OPT", "0");
#else
    setenv("MOTIONCAM_GAIN_BAKE_CPU_OPT", "0", 1);
#endif
    assert(motioncam::DNGDecoder::bakeGainMaps(scalarReferenceBake, false, false));
#ifdef _WIN32
    _putenv_s("MOTIONCAM_GAIN_BAKE_CPU_OPT", "");
#else
    unsetenv("MOTIONCAM_GAIN_BAKE_CPU_OPT");
#endif
    assert(motioncam::DNGDecoder::imagePayloadsEqual(
        scalarCachedBake, scalarReferenceBake));
    assert(motioncam::DNGDecoder::imagePayloadsEqual(
        scalarCachedBakeAgain, scalarReferenceBake));
    // RGB DirectLog frames can carry one scalar gain map shared by all three
    // channels. The optimized path must match the generic channel loop.
    tinydngwriter::DNGImage rgbScalarImage;
    rgbScalarImage.SetBigEndian(false);
    const unsigned short rgbBits[] = {16, 16, 16};
    assert(rgbScalarImage.SetImageWidth(rgbWidth));
    assert(rgbScalarImage.SetImageLength(rgbHeight));
    assert(rgbScalarImage.SetRowsPerStrip(rgbHeight));
    assert(rgbScalarImage.SetSamplesPerPixel(3));
    assert(rgbScalarImage.SetBitsPerSample(3, rgbBits));
    assert(rgbScalarImage.SetCompression(tinydngwriter::COMPRESSION_NONE));
    assert(rgbScalarImage.SetPhotometric(tinydngwriter::PHOTOMETRIC_LINEARRAW));
    assert(rgbScalarImage.SetPlanarConfig(tinydngwriter::PLANARCONFIG_CONTIG));
    assert(rgbScalarImage.SetWhiteLevel(0x0fff));
    assert(rgbScalarImage.SetImageData(
        reinterpret_cast<const unsigned char*>(rgb.data()),
        rgb.size() * sizeof(uint16_t)));
    tinydngwriter::GainMapParams rgbScalarMap{};
    rgbScalarMap.top = 0; rgbScalarMap.left = 0;
    rgbScalarMap.bottom = rgbHeight; rgbScalarMap.right = rgbWidth;
    rgbScalarMap.plane = 0; rgbScalarMap.planes = 3;
    rgbScalarMap.row_pitch = 1; rgbScalarMap.col_pitch = 1;
    rgbScalarMap.map_points_v = 2; rgbScalarMap.map_points_h = 2;
    rgbScalarMap.map_spacing_v = 0.5; rgbScalarMap.map_spacing_h = 0.5;
    rgbScalarMap.map_planes = 1;
    rgbScalarMap.gain_data = {1.0f, 1.1f, 1.2f, 1.3f};
    tinydngwriter::OpcodeList rgbScalarOpcodes;
    rgbScalarOpcodes.AddGainMap(rgbScalarMap);
    assert(rgbScalarImage.SetOpcodeList2(rgbScalarOpcodes));
    tinydngwriter::DNGWriter rgbScalarWriter(false);
    assert(rgbScalarWriter.AddImage(&rgbScalarImage));
    std::ostringstream rgbScalarOutput(std::ios::binary);
    assert(rgbScalarWriter.WriteToFile(rgbScalarOutput, &error));
    const std::string rgbScalarDng = rgbScalarOutput.str();
    std::vector<uint8_t> rgbLosslessDng(rgbScalarDng.begin(), rgbScalarDng.end());
    auto rgbTiledDng = rgbLosslessDng;
    assert(motioncam::DNGDecoder::compressLosslessJPEGTiled(rgbTiledDng));
    motioncam::DecodedDNGImage rgbTiledDecoded;
    assert(motioncam::DNGDecoder::decodeImage(rgbTiledDng, rgbTiledDecoded));
    assert(rgbTiledDecoded.samples == rgb);
    assert(motioncam::DNGDecoder::compressLosslessJPEG(rgbLosslessDng));
    motioncam::DecodedDNGImage rgbLosslessDecoded;
    assert(motioncam::DNGDecoder::decodeImage(rgbLosslessDng, rgbLosslessDecoded));
    assert(rgbLosslessDecoded.samples == rgb);
    auto rgbProxy = rgbLosslessDng;
    assert(motioncam::DNGDecoder::processHigherCFA(rgbProxy, 2, {0,1,1,2},
        motioncam::QuadBayerMode::Binning, false, 2, false));
    motioncam::DecodedDNGImage decodedProxy;
    assert(motioncam::DNGDecoder::decodeImage(rgbProxy, decodedProxy));
    std::vector<uint16_t> expectedProxy;
    uint32_t proxyWidth = 0, proxyHeight = 0;
    motioncam::utils::reduceRGB(rgb, expectedProxy, rgbWidth, rgbHeight,
                              2, false, proxyWidth, proxyHeight);
    assert(decodedProxy.layout.width == proxyWidth && decodedProxy.layout.height == proxyHeight);
    assert(decodedProxy.storedBitsPerSample == 16 && decodedProxy.samples == expectedProxy);
    const uint8_t soi[] = {0xff, 0xd8};
    const uint8_t eoi[] = {0xff, 0xd9};
    const auto jpegBegin = std::search(rgbLosslessDng.begin(), rgbLosslessDng.end(),
                                       std::begin(soi), std::end(soi));
    assert(jpegBegin != rgbLosslessDng.end());
    const auto jpegEnd = std::search(jpegBegin + 2, rgbLosslessDng.end(),
                                     std::begin(eoi), std::end(eoi));
    assert(jpegEnd != rgbLosslessDng.end());
    lj92 rgbIndependentDecoder = nullptr;
    int jpegWidth = 0, jpegHeight = 0, jpegBits = 0, jpegChannels = 0;
    assert(lj92_open(&rgbIndependentDecoder, const_cast<uint8_t*>(&*jpegBegin),
                     static_cast<int>(jpegEnd + 2 - jpegBegin), &jpegWidth,
                     &jpegHeight, &jpegBits, &jpegChannels) == LJ92_ERROR_NONE);
    assert(jpegWidth == rgbWidth && jpegHeight == rgbHeight &&
           jpegBits == 16 && jpegChannels == 3);
    std::vector<uint16_t> rgbIndependent(rgb.size());
    assert(lj92_decode(rgbIndependentDecoder, rgbIndependent.data(),
                       rgbWidth * 3, 0, nullptr, 0) == LJ92_ERROR_NONE);
    lj92_close(rgbIndependentDecoder);
    assert(rgbIndependent == rgb);
    std::vector<uint8_t> rgbScalarBaked(rgbScalarDng.begin(), rgbScalarDng.end());
    std::vector<motioncam::GainMap> rgbScalarMaps;
    assert(motioncam::DNGDecoder::getGainMaps(rgbScalarBaked, 2, rgbScalarMaps));
    assert(rgbScalarMaps.size() == 1 && rgbScalarMaps.front().channels == 1);
    auto rgbScalarReference = rgbScalarBaked;
    assert(motioncam::DNGDecoder::bakeGainMaps(rgbScalarBaked, false, false));
#ifdef _WIN32
    _putenv_s("MOTIONCAM_GAIN_BAKE_CPU_OPT", "0");
#else
    setenv("MOTIONCAM_GAIN_BAKE_CPU_OPT", "0", 1);
#endif
    assert(motioncam::DNGDecoder::bakeGainMaps(rgbScalarReference, false, false));
#ifdef _WIN32
    _putenv_s("MOTIONCAM_GAIN_BAKE_CPU_OPT", "");
#else
    unsetenv("MOTIONCAM_GAIN_BAKE_CPU_OPT");
#endif
    assert(motioncam::DNGDecoder::imagePayloadsEqual(
        rgbScalarBaked, rgbScalarReference));
    // RGB maps may cover only part of the image. Pixels outside the map
    // must keep unit gain even with fused, dithered LOG60 packing.
    auto rgbUnitMap = rgbScalarMaps.front();
    rgbUnitMap.channels = 3;
    rgbUnitMap.data.assign(static_cast<size_t>(rgbUnitMap.width) *
                           rgbUnitMap.height * 3, 1.0f);
    auto rgbCropMap = rgbUnitMap;
    rgbCropMap.left = 4; rgbCropMap.top = 3;
    rgbCropMap.right = rgbWidth - 4; rgbCropMap.bottom = rgbHeight - 3;
    std::fill(rgbCropMap.data.begin(), rgbCropMap.data.end(), 1.5f);
    std::vector<uint8_t> rgbUnitDng(rgbScalarDng.begin(), rgbScalarDng.end());
    auto rgbCropDng = rgbUnitDng;
    assert(motioncam::DNGDecoder::replaceGainMaps(rgbUnitDng, 2, {rgbUnitMap}));
    assert(motioncam::DNGDecoder::replaceGainMaps(rgbCropDng, 2, {rgbCropMap}));
    bool rgbUnitFused = false, rgbCropFused = false;
    assert(motioncam::DNGDecoder::bakeGainMaps(
        rgbUnitDng, false, false, false, false, 0, std::nullopt,
        motioncam::LogTransformMode::ReduceBy4Bit, 4095, false,
        &rgbUnitFused, true, 17));
    assert(motioncam::DNGDecoder::bakeGainMaps(
        rgbCropDng, false, false, false, false, 0, std::nullopt,
        motioncam::LogTransformMode::ReduceBy4Bit, 4095, false,
        &rgbCropFused, true, 17));
    assert(rgbUnitFused && rgbCropFused);
    motioncam::DecodedDNGImage rgbUnitDecoded, rgbCropDecoded;
    assert(motioncam::DNGDecoder::decodeImage(rgbUnitDng, rgbUnitDecoded));
    assert(motioncam::DNGDecoder::decodeImage(rgbCropDng, rgbCropDecoded));
    bool cropChanged = false;
    for (uint32_t y = 0; y < rgbHeight; ++y)
        for (uint32_t x = 0; x < rgbWidth; ++x)
            for (uint32_t channel = 0; channel < 3; ++channel) {
                const size_t index = (static_cast<size_t>(y) * rgbWidth + x) * 3 + channel;
                if (x >= rgbCropMap.left && x < rgbCropMap.right &&
                    y >= rgbCropMap.top && y < rgbCropMap.bottom)
                    cropChanged |= rgbUnitDecoded.samples[index] !=
                        rgbCropDecoded.samples[index];
                else
                    assert(rgbUnitDecoded.samples[index] ==
                           rgbCropDecoded.samples[index]);
            }
    assert(cropChanged);
    for (int variation = 0; variation < 3; ++variation) {
        auto changedMaps = scalarSpatialMaps;
        changedMaps.front().data.front() += 0.1f * (variation + 1);
        auto changed = scalarSpatialDng;
        assert(motioncam::DNGDecoder::replaceGainMaps(changed, 2, changedMaps));
        auto changedReference = changed;
        assert(motioncam::DNGDecoder::bakeGainMaps(changed, false, false));
#ifdef _WIN32
        _putenv_s("MOTIONCAM_GAIN_BAKE_CPU_OPT", "0");
#else
        setenv("MOTIONCAM_GAIN_BAKE_CPU_OPT", "0", 1);
#endif
        assert(motioncam::DNGDecoder::bakeGainMaps(changedReference, false, false));
#ifdef _WIN32
        _putenv_s("MOTIONCAM_GAIN_BAKE_CPU_OPT", "");
#else
        unsetenv("MOTIONCAM_GAIN_BAKE_CPU_OPT");
#endif
        assert(motioncam::DNGDecoder::imagePayloadsEqual(changed, changedReference));
    }
    auto packedScalarBake = scalarSpatialDng;
    assert(motioncam::DNGDecoder::ensureUncompressed(packedScalarBake));
    auto packedScalarReference = packedScalarBake;
    bool packedLogFused = false;
    assert(motioncam::DNGDecoder::bakeGainMaps(
        packedScalarBake, false, false, false, false, 0, std::nullopt,
        motioncam::LogTransformMode::ReduceBy4Bit, 4095, false,
        &packedLogFused, true));
    assert(packedLogFused);
#ifdef _WIN32
    _putenv_s("MOTIONCAM_GAIN_BAKE_CPU_OPT", "0");
#else
    setenv("MOTIONCAM_GAIN_BAKE_CPU_OPT", "0", 1);
#endif
    assert(motioncam::DNGDecoder::bakeGainMaps(
        packedScalarReference, false, false, false, false, 0, std::nullopt,
        motioncam::LogTransformMode::ReduceBy4Bit, 4095, false,
        &packedLogFused, true));
#ifdef _WIN32
    _putenv_s("MOTIONCAM_GAIN_BAKE_CPU_OPT", "");
#else
    unsetenv("MOTIONCAM_GAIN_BAKE_CPU_OPT");
#endif
    assert(packedLogFused);
    assert(motioncam::DNGDecoder::imagePayloadsEqual(
        packedScalarBake, packedScalarReference));
    motioncam::RenderSettings fullPreviewSettings;
    fullPreviewSettings.options = motioncam::RENDER_OPT_APPLY_VIGNETTE_CORRECTION;
    motioncam::PreviewFrame fullPreview;
    assert(motioncam::DNGDecoder::decodePreview(
        scalarSpatialDng, fullPreviewSettings, fullPreview, false));
    // KeepInput with a vignette bake still runs the log quantization and
    // linearization path. The fast Bayer preview must not bypass it.
    auto keepInputPreviewSettings = fullPreviewSettings;
    keepInputPreviewSettings.options |= motioncam::RENDER_OPT_LOG_TRANSFORM;
    keepInputPreviewSettings.logTransform = motioncam::LogTransformMode::KeepInput;
    motioncam::PreviewFrame keepInputPreview;
    assert(motioncam::DNGDecoder::decodePreview(
        scalarSpatialDng, keepInputPreviewSettings, keepInputPreview, true));
    assert(keepInputPreview.rgb.size() == fullPreview.rgb.size());
    assert(keepInputPreview.rgb != fullPreview.rgb);
    auto twoBitPreviewSettings = keepInputPreviewSettings;
    twoBitPreviewSettings.logTransform = motioncam::LogTransformMode::ReduceBy14Bit;
    motioncam::PreviewFrame twoBitPreview;
    assert(motioncam::DNGDecoder::decodePreview(
        scalarSpatialDng, twoBitPreviewSettings, twoBitPreview, true));
    assert(twoBitPreview.displayBitDepth == 2);
    assert(twoBitPreview.rgb.size() == fullPreview.rgb.size());
    auto colorPreviewSettings = fullPreviewSettings;
    colorPreviewSettings.options |= motioncam::RENDER_OPT_VIGNETTE_ONLY_COLOR;
    motioncam::PreviewFrame colorPreview;
    assert(motioncam::DNGDecoder::decodePreview(
        scalarSpatialDng, colorPreviewSettings, colorPreview, false));
    assert(!colorPreview.gainMapApplied);
    motioncam::PreviewFrame scalarUnprocessedPreview;
    assert(motioncam::DNGDecoder::decodePreview(
        scalarSpatialDng, motioncam::RenderSettings{}, scalarUnprocessedPreview, false));
    assert(colorPreview.rgb == scalarUnprocessedPreview.rgb);
    std::vector<uint8_t> clearedGainMapDng(gainMapDng.begin(), gainMapDng.end());
    assert(motioncam::DNGDecoder::replaceGainMaps(clearedGainMapDng, 2, {}));
    std::vector<motioncam::GainMap> clearedMaps;
    assert(!motioncam::DNGDecoder::getGainMaps(clearedGainMapDng, 2, clearedMaps));
    assert(clearedMaps.empty());
    const auto clearedBeforeDebugBake = clearedGainMapDng;
    assert(motioncam::DNGDecoder::bakeGainMaps(
        clearedGainMapDng, false, false, false, true));
    assert(motioncam::DNGDecoder::imagePayloadsEqual(
        clearedGainMapDng, clearedBeforeDebugBake));
    motioncam::PreviewFrame noGainOnlyPreview;
    assert(motioncam::DNGDecoder::decodePreview(
        clearedGainMapDng, gainOnlySettings, noGainOnlyPreview, false));
    assert(!noGainOnlyPreview.gainMapApplied);
    // Excluding all gain maps removes OpcodeList2 instead of leaving a
    // four-byte zero-count list behind.
    assert(motioncam::DNGDecoder::repairGainMapCfaPhase(clearedGainMapDng, true));
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
        foundClearedOpcodeList = true;
    }
    assert(!foundClearedOpcodeList);
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
    std::vector<uint8_t> malformedGainMapDng(gainMapDng.begin(), gainMapDng.end());
    bool corruptedOpcodeList = false;
    for (uint16_t i = 0; i < read16le(rootIfd); ++i) {
        const size_t entry = static_cast<size_t>(rootIfd) + 2 + i * 12;
        if (read16le(entry) != 51009) continue;
        const uint32_t opcodeOffset = read32le(entry + 8);
        assert(opcodeOffset + 20 <= malformedGainMapDng.size());
        // The opcode payload byte count is big-endian. Make it exceed the TIFF
        // tag payload so baking must reject the malformed list, not ignore it.
        std::fill_n(malformedGainMapDng.begin() + opcodeOffset + 16, 4, 0xff);
        corruptedOpcodeList = true;
        break;
    }
    assert(corruptedOpcodeList);
    assert(!motioncam::DNGDecoder::bakeGainMaps(
        malformedGainMapDng, false, false));

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

    std::vector<uint8_t> croppedDeferredGainMapDng(
        gainMapDng.begin(), gainMapDng.end());
    assert(motioncam::DNGDecoder::replaceGainMaps(
        croppedDeferredGainMapDng, 3, originalMaps));
    assert(motioncam::DNGDecoder::cropGainMapsToFullSensor(
        croppedDeferredGainMapDng, width * 2, height * 2));
    std::vector<motioncam::GainMap> croppedDeferredMaps;
    assert(motioncam::DNGDecoder::getGainMaps(
        croppedDeferredGainMapDng, 3, croppedDeferredMaps));
    assert(croppedDeferredMaps.size() == originalMaps.size());
    assert(croppedDeferredMaps.front().data != originalMaps.front().data);

    // A single GainMap containing all four CFA planes must bake across the
    // complete higher-CFA grid, even when its encoded pitch is 2x2.
    auto fourPlaneMap = originalMaps;
    assert(fourPlaneMap.size() == 1 && fourPlaneMap.front().channels == 4);
    fourPlaneMap.front().rowPitch = 2;
    fourPlaneMap.front().colPitch = 2;
    std::vector<uint8_t> fourPlaneDng(gainMapDng.begin(), gainMapDng.end());
    assert(motioncam::DNGDecoder::replaceGainMaps(fourPlaneDng, 2, fourPlaneMap));
    assert(motioncam::DNGDecoder::bakeGainMaps(
        fourPlaneDng, false, false, false, true, 4));

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

    // Baking expands the intermediate linear range, but a following log
    // transform must retain the bit-depth target of the original raw samples.
    std::vector<uint8_t> bakedLog(gainMapDng.begin(), gainMapDng.end());
    motioncam::DNGFrameMetadata preBakeMetadata;
    assert(motioncam::DNGDecoder::getColorMetadata(bakedLog, preBakeMetadata));
    const uint32_t preBakeWhite = static_cast<uint32_t>(preBakeMetadata.whiteLevel[0]);
    assert(motioncam::DNGDecoder::bakeGainMaps(bakedLog, false, false));
    auto expandedLog = bakedLog;
    assert(motioncam::DNGDecoder::applyLogTransform(
        expandedLog, motioncam::LogTransformMode::ReduceBy4Bit));
    assert(motioncam::DNGDecoder::packUncompressedToWhiteLevel(expandedLog));
    assert(motioncam::DNGDecoder::applyLogTransform(
        bakedLog, motioncam::LogTransformMode::ReduceBy4Bit, preBakeWhite));
    assert(motioncam::DNGDecoder::packUncompressedToWhiteLevel(bakedLog));
    assert(bakedLog.size() < expandedLog.size());

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
    // Color/luminance separation must be representation-independent. A single
    // four-plane map and its canonical four scalar maps produce identical
    // baked pixels and the same deferred luminance layer.
    std::vector<uint8_t> scalarColorBaked(gainMapDng.begin(), gainMapDng.end());
    assert(motioncam::DNGDecoder::canonicalizeGainMapOpcodes(scalarColorBaked));
    assert(motioncam::DNGDecoder::bakeGainMaps(scalarColorBaked, false, true));
    assert(motioncam::DNGDecoder::imagePayloadsEqual(colorBaked, scalarColorBaked));
    // The 2x2 color-only CFA reuse path must match the unoptimized bake.
    std::vector<uint8_t> scalarColorReference(gainMapDng.begin(), gainMapDng.end());
    assert(motioncam::DNGDecoder::canonicalizeGainMapOpcodes(scalarColorReference));
#ifdef _WIN32
    _putenv_s("MOTIONCAM_GAIN_BAKE_CPU_OPT", "0");
#else
    setenv("MOTIONCAM_GAIN_BAKE_CPU_OPT", "0", 1);
#endif
    assert(motioncam::DNGDecoder::bakeGainMaps(scalarColorReference, false, true));
#ifdef _WIN32
    _putenv_s("MOTIONCAM_GAIN_BAKE_CPU_OPT", "");
#else
    unsetenv("MOTIONCAM_GAIN_BAKE_CPU_OPT");
#endif
    assert(motioncam::DNGDecoder::imagePayloadsEqual(
        scalarColorBaked, scalarColorReference));
    std::vector<motioncam::GainMap> scalarLuminanceMaps;
    assert(motioncam::DNGDecoder::getGainMaps(
        scalarColorBaked, 3, scalarLuminanceMaps));
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
    assert(scalarLuminanceMaps.size() == 1);
    assert(scalarLuminanceMaps.front().data == luminanceMaps.front().data);

    // With color reduction, normalize the baked color correction and the
    // deferred luminance correction independently.
    std::vector<uint8_t> normalizedColorBaked(gainMapDng.begin(), gainMapDng.end());
    assert(motioncam::DNGDecoder::bakeGainMaps(
        normalizedColorBaked, true, true));
    std::vector<motioncam::GainMap> normalizedBakedLuminance;
    assert(motioncam::DNGDecoder::getGainMaps(
        normalizedColorBaked, 3, normalizedBakedLuminance));
    float normalizedBakedLuminanceMaximum = 0.0f;
    for (const float gain : normalizedBakedLuminance.front().data)
        normalizedBakedLuminanceMaximum = std::max(
            normalizedBakedLuminanceMaximum, gain);
    assert(std::abs(normalizedBakedLuminanceMaximum - 1.0f) < 1e-6f);

    // A previously reduced DNG can contain only deferred luminance. It must
    // still honor normalization when no OpcodeList2 color maps remain.
    std::vector<uint8_t> normalizedLuminanceOnly = colorBaked;
    assert(motioncam::DNGDecoder::transformGainMaps(
        normalizedLuminanceOnly, true, true, false));
    std::vector<motioncam::GainMap> normalizedLuminanceOnlyMaps;
    assert(motioncam::DNGDecoder::getGainMaps(
        normalizedLuminanceOnly, 3, normalizedLuminanceOnlyMaps));
    float normalizedLuminanceOnlyMaximum = 0.0f;
    for (const float gain : normalizedLuminanceOnlyMaps.front().data)
        normalizedLuminanceOnlyMaximum = std::max(
            normalizedLuminanceOnlyMaximum, gain);
    assert(std::abs(normalizedLuminanceOnlyMaximum - 1.0f) < 1e-6f);

    // Color-only baking must leave an already deferred luminance map alone.
    const auto colorOnlyLuminance = colorBaked;
    std::vector<uint8_t> colorOnlyAgain = colorOnlyLuminance;
    assert(motioncam::DNGDecoder::bakeGainMaps(colorOnlyAgain, false, true));
    assert(colorOnlyAgain == colorOnlyLuminance);

    // If both opcode stages carry gain, color-only baking combines the newly
    // separated luminance with the existing OpcodeList3 remainder instead of
    // creating a second OpcodeList3 tag.
    auto existingLuminance = luminanceMaps.front();
    std::fill(existingLuminance.data.begin(), existingLuminance.data.end(), 2.0f);
    std::vector<uint8_t> bothOpcodeStages(gainMapDng.begin(), gainMapDng.end());
    assert(motioncam::DNGDecoder::replaceGainMaps(
        bothOpcodeStages, 3, {existingLuminance}));
    motioncam::DNGFrameMetadata beforeCombinedOptimization;
    assert(motioncam::DNGDecoder::getColorMetadata(
        bothOpcodeStages, beforeCombinedOptimization));
    assert(motioncam::DNGDecoder::bakeGainMaps(
        bothOpcodeStages, false, true, true));
    motioncam::DNGFrameMetadata afterCombinedOptimization;
    assert(motioncam::DNGDecoder::getColorMetadata(
        bothOpcodeStages, afterCombinedOptimization));
    assert(std::abs(afterCombinedOptimization.baselineExposure -
                    beforeCombinedOptimization.baselineExposure - 1.0) < 1e-5);
    std::vector<motioncam::GainMap> combinedLuminance;
    assert(motioncam::DNGDecoder::getGainMaps(
        bothOpcodeStages, 3, combinedLuminance));
    assert(combinedLuminance.size() == 1);
    std::vector<motioncam::GainMap> consumedColorMaps;
    assert(!motioncam::DNGDecoder::getGainMaps(
        bothOpcodeStages, 2, consumedColorMaps));

    // A later full bake must accept the lone OpcodeList3 luminance remainder;
    // the channel-relative correction is already present in the pixels.
    std::vector<uint8_t> fullyBakedAfterColor = colorBaked;
    assert(motioncam::DNGDecoder::bakeGainMaps(
        fullyBakedAfterColor, false, false));
    std::vector<motioncam::GainMap> consumedLuminanceMaps;
    assert(!motioncam::DNGDecoder::getGainMaps(
        fullyBakedAfterColor, 3, consumedLuminanceMaps));
    assert(consumedLuminanceMaps.empty());

    // Legacy MotionCam DNGs store the four CFA phases as four separate
    // one-channel GainMap opcodes. All of them must be transformed in place.
    tinydngwriter::OpcodeList fourGainOpcodes;
    for (unsigned int phase = 0; phase < 4; ++phase) {
        tinydngwriter::GainMapParams phaseMap = gainMap;
        phaseMap.top = phase / 2;
        phaseMap.left = phase % 2;
        phaseMap.row_pitch = 2;
        phaseMap.col_pitch = 2;
        // Exercise real-world phase-dependent inferred coordinate extents:
        // the one-pixel Top/Left offsets round to adjacent dimensions even
        // though all four opcodes describe the same gain-map grid.
        phaseMap.bottom = 2448;
        phaseMap.right = 3264;
        phaseMap.map_points_v = 13;
        phaseMap.map_points_h = 17;
        phaseMap.map_spacing_v = 1.0 / 13.0;
        phaseMap.map_spacing_h = 1.0 / 17.0;
        phaseMap.map_planes = 1;
        phaseMap.gain_data.resize(13 * 17);
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
    std::vector<uint8_t> normalizedOutputGainMaps = fourGainBytes;
    assert(motioncam::DNGDecoder::transformGainMaps(
        normalizedOutputGainMaps, true, false, false));
    std::vector<motioncam::GainMap> normalizedMaps;
    assert(motioncam::DNGDecoder::getGainMaps(
        normalizedOutputGainMaps, 2, normalizedMaps));
    float normalizedMaximum = 0.0f;
    for (const auto& map : normalizedMaps)
        for (const float gain : map.data)
            normalizedMaximum = std::max(normalizedMaximum, gain);
    assert(std::abs(normalizedMaximum - 1.0f) < 1e-6f);

    std::vector<uint8_t> normalizedReducedOutputGainMaps = fourGainBytes;
    assert(motioncam::DNGDecoder::transformGainMaps(
        normalizedReducedOutputGainMaps, true, true, false));
    std::vector<motioncam::GainMap> normalizedOutputColor;
    std::vector<motioncam::GainMap> normalizedOutputLuminance;
    assert(motioncam::DNGDecoder::getGainMaps(
        normalizedReducedOutputGainMaps, 2, normalizedOutputColor));
    assert(motioncam::DNGDecoder::getGainMaps(
        normalizedReducedOutputGainMaps, 3, normalizedOutputLuminance));
    auto maximumGain = [](const std::vector<motioncam::GainMap>& maps) {
        float maximum = 0.0f;
        for (const auto& map : maps)
            for (const float gain : map.data)
                maximum = std::max(maximum, gain);
        return maximum;
    };
    assert(std::abs(maximumGain(normalizedOutputColor) - 1.0f) < 1e-6f);
    assert(std::abs(maximumGain(normalizedOutputLuminance) - 1.0f) < 1e-6f);

    // Existing deferred luminance and newly separated luminance represent one
    // multiplicative correction. Merge them before normalizing the result.
    std::vector<uint8_t> normalizedCombinedOutput = fourGainBytes;
    assert(motioncam::DNGDecoder::replaceGainMaps(
        normalizedCombinedOutput, 3, {existingLuminance}));
    assert(motioncam::DNGDecoder::transformGainMaps(
        normalizedCombinedOutput, true, true, false));
    std::vector<motioncam::GainMap> normalizedCombinedLuminance;
    assert(motioncam::DNGDecoder::getGainMaps(
        normalizedCombinedOutput, 3, normalizedCombinedLuminance));
    assert(normalizedCombinedLuminance.size() == 1);
    assert(std::abs(maximumGain(normalizedCombinedLuminance) - 1.0f) < 1e-6f);

    assert(motioncam::DNGDecoder::transformGainMaps(fourGainBytes, false, true, false));
    std::vector<motioncam::GainMap> retainedColorMaps, retainedLuminanceMaps;
    assert(motioncam::DNGDecoder::getGainMaps(
        fourGainBytes, 2, retainedColorMaps));
    assert(motioncam::DNGDecoder::getGainMaps(
        fourGainBytes, 3, retainedLuminanceMaps));
    assert(retainedColorMaps.size() == 4);
    assert(retainedLuminanceMaps.size() == 1);
    assert(retainedLuminanceMaps.front().channels == 1);
    assert(retainedLuminanceMaps.front().plane == 0);
    assert(retainedLuminanceMaps.front().planes == 3);

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

    // A terminal Vulkan LOG pass can feed JPEG from its packed GPU buffer.
    // Compare its JPEG DNG with the same GPU pass using the ordinary strip
    // readback. Run both CFA and RGB; CPU-only builds simply skip this path.
    auto checkGpuPackedJpeg = [&](uint32_t testWidth, uint32_t testHeight,
                                  uint32_t channels, bool fusedGain = false,
                                  uint32_t sourceWhite = 4095,
                                  motioncam::LogTransformMode mode =
                                      motioncam::LogTransformMode::ReduceBy4Bit,
                                  bool packOutput = true,
                                  bool tileOutput = false) {
        std::vector<uint16_t> samples(static_cast<size_t>(testWidth) *
                                      testHeight * channels);
        for (size_t i = 0; i < samples.size(); ++i)
            samples[i] = static_cast<uint16_t>(
                (i * 37u + i / 91u) % (sourceWhite + 1u));
        tinydngwriter::DNGImage testImage;
        testImage.SetBigEndian(false);
        const unsigned short testBits[] = {16, 16, 16};
        assert(testImage.SetImageWidth(testWidth));
        assert(testImage.SetImageLength(testHeight));
        assert(testImage.SetRowsPerStrip(testHeight));
        assert(testImage.SetSamplesPerPixel(channels));
        assert(testImage.SetBitsPerSample(channels, testBits));
        assert(testImage.SetCompression(tinydngwriter::COMPRESSION_NONE));
        assert(testImage.SetPhotometric(channels == 1
            ? tinydngwriter::PHOTOMETRIC_CFA
            : tinydngwriter::PHOTOMETRIC_LINEARRAW));
        assert(testImage.SetPlanarConfig(tinydngwriter::PLANARCONFIG_CONTIG));
        assert(testImage.SetWhiteLevel(sourceWhite));
        assert(testImage.SetImageData(
            reinterpret_cast<const unsigned char*>(samples.data()),
            samples.size() * sizeof(uint16_t)));
        if (fusedGain) {
            tinydngwriter::GainMapParams map{};
            map.top = 0; map.left = 0;
            map.bottom = testHeight; map.right = testWidth;
            map.plane = 0; map.planes = 3;
            map.row_pitch = 1; map.col_pitch = 1;
            map.map_points_v = 2; map.map_points_h = 2;
            map.map_spacing_v = 0.5; map.map_spacing_h = 0.5;
            map.map_planes = 1;
            map.gain_data = {1.0f, 1.1f, 1.2f, 1.3f};
            tinydngwriter::OpcodeList opcodes;
            opcodes.AddGainMap(map);
            assert(testImage.SetOpcodeList2(opcodes));
        }
        tinydngwriter::DNGWriter testWriter(false);
        assert(testWriter.AddImage(&testImage));
        std::ostringstream testOutput(std::ios::binary);
        std::string testError;
        assert(testWriter.WriteToFile(testOutput, &testError));
        const std::string serialized = testOutput.str();
        std::vector<uint8_t> reference(serialized.begin(), serialized.end());
        auto fast = reference;
        std::vector<uint8_t> gpuJpeg;
        std::vector<std::vector<uint8_t>> gpuTiles;
        bool fused = false;
        const bool baked = fusedGain
            ? motioncam::DNGDecoder::bakeGainMaps(
                  fast, false, false, false, false, 0, std::nullopt,
                  mode, sourceWhite, false, &fused, packOutput, 7, true, nullptr,
                  tileOutput ? nullptr : &gpuJpeg,
                  tileOutput ? &gpuTiles : nullptr)
            : motioncam::DNGDecoder::bakeLogStripVulkan(
                  fast, mode, sourceWhite, false, 7,
                  tileOutput ? nullptr : &gpuJpeg,
                  tileOutput ? &gpuTiles : nullptr);
        if (!baked) {
            assert(!std::getenv("MOTIONCAM_REQUIRE_GPU_JPEG_TEST"));
            return;
        }
        if (tileOutput ? gpuTiles.empty() : gpuJpeg.empty()) {
            std::fprintf(stderr, "GPU JPEG output missing: %ux%u channels=%u sourceWhite=%u mode=%d packed=%d tiled=%d\n",
                         testWidth, testHeight, channels, sourceWhite,
                         int(mode), packOutput, tileOutput);
            assert(!std::getenv("MOTIONCAM_REQUIRE_GPU_JPEG_TEST"));
            return; // Vulkan unavailable: CPU fallback ran.
        }
        if (fusedGain) {
            bool referenceFused = false;
            assert(motioncam::DNGDecoder::bakeGainMaps(
                reference, false, false, false, false, 0, std::nullopt,
                mode, sourceWhite, false, &referenceFused, packOutput, 7, true));
            assert(fused == referenceFused);
            assert(fused == (mode != motioncam::LogTransformMode::Disabled));
            if (mode == motioncam::LogTransformMode::Disabled && packOutput)
                assert(motioncam::DNGDecoder::packUncompressedToWhiteLevel(
                    reference));
        } else {
            assert(motioncam::DNGDecoder::bakeLogStripVulkan(
                reference, mode, sourceWhite, false, 7));
        }
        if (tileOutput)
            assert(motioncam::DNGDecoder::attachLosslessJPEGTiles(
                fast, gpuTiles, channels == 1 ? 16u : 8u,
                channels == 1 ? 64u : 128u));
        else
            assert(motioncam::DNGDecoder::attachLosslessJPEG(fast, gpuJpeg));
        motioncam::DecodedDNGImage fastImage, referenceImage;
        assert(motioncam::DNGDecoder::decodeImage(fast, fastImage));
        assert(motioncam::DNGDecoder::decodeImage(reference, referenceImage));
        assert(fastImage.samples == referenceImage.samples);
    };
    checkGpuPackedJpeg(1024, 1024, 1);
    checkGpuPackedJpeg(1024, 512, 3);
    checkGpuPackedJpeg(1024, 512, 3, true);
    checkGpuPackedJpeg(1024, 512, 3, true, 4095,
                       motioncam::LogTransformMode::Disabled);
    checkGpuPackedJpeg(1024, 512, 3, true, 4095,
                       motioncam::LogTransformMode::Disabled, false);
    checkGpuPackedJpeg(1025, 1024, 1, false, 65535,
                       motioncam::LogTransformMode::ReduceBy4Bit);
    checkGpuPackedJpeg(1023, 512, 3, false, 4095,
                       motioncam::LogTransformMode::ReduceBy2Bit);
    checkGpuPackedJpeg(1024, 1024, 1, true, 65535,
                       motioncam::LogTransformMode::Disabled, false, true);
    checkGpuPackedJpeg(1024, 512, 3, true, 65535,
                       motioncam::LogTransformMode::Disabled, false, true);
    checkGpuPackedJpeg(1024, 1024, 1, false, 65535,
                       motioncam::LogTransformMode::ReduceBy4Bit, true, true);
    checkGpuPackedJpeg(1024, 512, 3, false, 65535,
                       motioncam::LogTransformMode::ReduceBy4Bit, true, true);
    checkGpuPackedJpeg(1024, 512, 3, true, 4095,
                       motioncam::LogTransformMode::ReduceBy2Bit, true, true);
    checkGpuPackedJpeg(1024, 1024, 1, false, 65535,
                       motioncam::LogTransformMode::ReduceBy8Bit, true, true);
    checkGpuPackedJpeg(1024, 512, 3, false, 65535,
                       motioncam::LogTransformMode::ReduceBy2Bit, true, true);
    checkGpuPackedJpeg(1000, 512, 3, true, 4095,
                       motioncam::LogTransformMode::ReduceBy2Bit, true, true);
    checkGpuPackedJpeg(1024, 1024, 1, true, 4095,
                       motioncam::LogTransformMode::Disabled, true, true);
    checkGpuPackedJpeg(1024, 512, 3, true, 4095,
                       motioncam::LogTransformMode::Disabled, true, true);

    // Independent lossless JPEG tiles exercise the Vulkan entropy and
    // predictor decoder, including its tile placement and 16-bit wrapping.
    {
        const bool smallTiles = std::getenv("MOTIONCAM_JPEG_DECODE_SMALL_TILES");
        const uint32_t tileWidth = smallTiles ? 32u : 128u;
        const uint32_t tileHeight = tileWidth;
        const uint32_t tileCount = smallTiles ? 12288u :
            std::getenv("MOTIONCAM_JPEG_DECODE_LARGE_BENCHMARK") ? 768u : 64u;
        const bool residentTest = std::getenv("MOTIONCAM_JPEG_DECODE_RESIDENT_TEST");
        const bool residentLog = std::getenv("MOTIONCAM_JPEG_DECODE_LOG_TEST");
        std::vector<std::vector<uint16_t>> expected(tileCount);
        std::vector<std::vector<uint8_t>> encoded(tileCount);
        std::vector<motioncam::VulkanJpegTile> inputs(tileCount);
        for (uint32_t tile = 0; tile < tileCount; ++tile) {
            const uint32_t bits = residentTest ? 12u : (tile & 1u) ? 16u : 12u;
            auto& samples = expected[tile];
            samples.resize(tileWidth * tileHeight);
            for (size_t i = 0; i < samples.size(); ++i)
                samples[i] = uint16_t((i * 37u + tile * 109u + i / 31u) &
                                      ((1u << bits) - 1u));
            uint8_t* bytes = nullptr;
            int count = 0;
            assert(lj92_encode(samples.data(), tileWidth, tileHeight, bits, 1,
                               tileWidth, 0, nullptr, 0, &bytes, &count) == LJ92_ERROR_NONE);
            encoded[tile].assign(bytes, bytes + count);
            free(bytes);
            inputs[tile] = {encoded[tile].data(), encoded[tile].size(),
                            tileWidth, tileHeight, bits};
        }
        std::vector<std::vector<uint16_t>> actual;
        if (std::getenv("MOTIONCAM_JPEG_DECODE_PROFILE")) {
            std::vector<std::vector<uint16_t>> warm;
            assert(motioncam::jpegDecodeTilesVulkan(inputs, warm));
            assert(warm == expected);
        }
        const auto gpuStart = std::chrono::steady_clock::now();
        const bool gpuDecoded = motioncam::jpegDecodeTilesVulkan(inputs, actual);
        const auto gpuEnd = std::chrono::steady_clock::now();
        if (gpuDecoded) {
            if (actual != expected)
                for (size_t t = 0; t < actual.size(); ++t) {
                    auto mismatch = std::mismatch(actual[t].begin(), actual[t].end(),
                                                  expected[t].begin());
                    if (mismatch.first != actual[t].end()) {
                        std::fprintf(stderr, "GPU JPEG mismatch tile=%zu sample=%zu got=%u expected=%u\n",
                            t, size_t(mismatch.first - actual[t].begin()),
                            unsigned(*mismatch.first), unsigned(*mismatch.second));
                        for (size_t i = 0; i < 12; ++i)
                            std::fprintf(stderr, "%zu:%u/%u ", i,
                                unsigned(actual[t][i]), unsigned(expected[t][i]));
                        std::fprintf(stderr, "\n");
                        break;
                    }
                }
            assert(actual == expected);
            if (tileCount == 64 && !residentTest) {
                // Exercise the DNG import dispatch as well as the standalone
                // decoder. All tiles use one precision for the TIFF tag.
                std::vector<uint16_t> imageSamples(1024u * 1024u);
                for (uint32_t tile = 0; tile < 64; ++tile)
                    for (uint32_t y = 0; y < 128; ++y)
                        std::copy_n(expected[tile].data() + y * 128u, 128u,
                            imageSamples.data() +
                            (static_cast<size_t>(tile / 8u) * 128u + y) * 1024u +
                            (tile % 8u) * 128u);
                auto dng = tiledLosslessJpegDng(
                    imageSamples, 1024, 1024, 128, 128, 16);
                motioncam::DecodedDNGImage imported;
                assert(motioncam::DNGDecoder::decodeImageBorrowed(dng, imported));
                assert(imported.samples == imageSamples);
                motioncam::DecodedDNGImage resident;
                assert(motioncam::DNGDecoder::decodeImage(dng, resident, false, true, true));
                assert(resident.gpuImage && resident.samples.empty());
                // Reusing decoder scratch for another frame cannot overwrite a
                // retained frame that is waiting for gallery processing.
                motioncam::DecodedDNGImage next;
                assert(motioncam::DNGDecoder::decodeImage(dng, next, false, true, true));
                assert(next.gpuImage && next.gpuImage->buffer != resident.gpuImage->buffer);
                std::vector<uint16_t> recovered(imageSamples.size());
                assert(resident.gpuImage->download(recovered.data(), recovered.size()));
                assert(recovered == imageSamples);
                resident.layout.cfaRepeatSize = 2;
                resident.layout.cfaPhase = {0, 1, 1, 2};
                motioncam::RenderSettings settings;
                settings.options = static_cast<motioncam::FileRenderOptions>(0);
                settings.streamingPreview = true;
                motioncam::PreviewFrame frame;
                assert(motioncam::DNGDecoder::decodePreview(resident, settings, frame));
                assert(frame.gallerySource.buffer && frame.galleryCfa.empty() && frame.rgb.empty());
                motioncam::VulkanGalleryBayerSource source;
                source.gpu = frame.gallerySource.gpu;
                source.buffer = frame.gallerySource.buffer;
                source.width = frame.width; source.height = frame.height;
                source.phase = frame.galleryCfaPhase;
                source.black = frame.galleryBlack; source.white = frame.galleryWhite;
                const std::array<float, 3> gains{1, 1, 1};
                const std::array<float, 9> identity{1,0,0, 0,1,0, 0,0,1};
                std::vector<uint8_t> gpuColor, hostColor;
                assert(motioncam::colorGalleryRgbVulkan(gpuColor, gains, identity, identity,
                    1, nullptr, nullptr, false, false, &source));
                source.gpu = source.buffer = nullptr;
                source.samples = imageSamples.data();
                assert(motioncam::colorGalleryRgbVulkan(hostColor, gains, identity, identity,
                    1, nullptr, nullptr, false, false, &source));
                assert(gpuColor == hostColor);
                auto withGain = resident;
                motioncam::GainMap map{};
                map.right = map.bottom = 1024;
                map.planes = map.channels = map.rowPitch = map.colPitch = 1;
                map.width = map.height = 1;
                map.spacingH = map.spacingV = 1;
                map.data = {1.1f};
                withGain.opcodeList2 = {map};
                auto gainSettings = settings;
                gainSettings.options |= motioncam::RENDER_OPT_APPLY_VIGNETTE_CORRECTION;
                motioncam::PreviewFrame gainFrame;
                assert(motioncam::DNGDecoder::decodePreview(withGain, gainSettings, gainFrame));
                assert(gainFrame.gallerySource.buffer && gainFrame.galleryCfaGain && gainFrame.gainMapApplied);
                source.samples = nullptr;
                source.gpu = gainFrame.gallerySource.gpu;
                source.buffer = gainFrame.gallerySource.buffer;
                source.black = gainFrame.galleryBlack; source.white = gainFrame.galleryWhite;
                source.gain = gainFrame.galleryCfaGain.get();
                assert(motioncam::colorGalleryRgbVulkan(gpuColor, gains, identity, identity,
                    1, nullptr, nullptr, false, false, &source));
                source.gpu = source.buffer = nullptr;
                source.samples = imageSamples.data();
                assert(motioncam::colorGalleryRgbVulkan(hostColor, gains, identity, identity,
                    1, nullptr, nullptr, false, false, &source));
                assert(gpuColor == hostColor);
                // CPU recovery must match the ordinary CPU preview for both
                // gain baking and LOG-only processing of resident JPEG frames.
                for (bool bakeGain : {false, true}) for (bool log : {false, true}) {
                    auto input = bakeGain ? withGain : resident;
                    auto recoverySettings = bakeGain ? gainSettings : settings;
                    if (log) recoverySettings.options |= motioncam::RENDER_OPT_LOG_TRANSFORM;
                    recoverySettings.logTransform = motioncam::LogTransformMode::ReduceBy6Bit;
                    motioncam::PreviewFrame deferred;
                    assert(motioncam::DNGDecoder::decodePreview(input, recoverySettings, deferred, true));
                    std::vector<uint16_t> samples;
                    assert(deferred.gallerySource.downloadSamples(samples));
                    if (deferred.galleryCfaGain) {
                        assert(deferred.galleryCfaRecoverCpu);
                        assert(deferred.galleryCfaRecoverCpu(samples));
                    }
                    std::vector<uint8_t> recoveredRgb;
                    assert(motioncam::utils::demosaicBilinearBayerToRgb16Bytes(
                        samples.data(), recoveredRgb, deferred.width, deferred.height,
                        deferred.galleryCfaPhase, deferred.galleryBlack, deferred.galleryWhite));
                    input.gpuImage.reset(); input.samples = imageSamples;
                    recoverySettings.streamingPreview = false;
                    motioncam::PreviewFrame ordinary;
                    assert(motioncam::DNGDecoder::decodePreview(input, recoverySettings, ordinary, true));
                    assert(recoveredRgb == ordinary.rgb);
                }
#ifdef MOTIONCAM_HAS_DIRECTLOG_PLACEBO
                const auto gpu = static_cast<pl_gpu>(resident.gpuImage->gpu);
                pl_tex_params textureParams{};
                textureParams.w = textureParams.h = 1024;
                textureParams.format = pl_find_fmt(gpu, PL_FMT_UNORM, 1, 16, 16,
                                                   PL_FMT_CAP_SAMPLEABLE);
                textureParams.sampleable = textureParams.host_writable = true;
                textureParams.initial_data = imageSamples.data();
                pl_tex cfaTexture = pl_tex_create(gpu, &textureParams);
                assert(cfaTexture);
                auto textureLease = std::shared_ptr<void>(new pl_tex(cfaTexture),
                    [gpu](void* pointer) {
                        auto* texture = static_cast<pl_tex*>(pointer);
                        pl_tex_destroy(gpu, texture); delete texture;
                    });
                for (bool log : {false, true}) {
                    auto textureImage = resident;
                    textureImage.gpuImage = std::make_shared<motioncam::DNGGpuImage>(*resident.gpuImage);
                    textureImage.gpuImage->buffer = nullptr;
                    textureImage.gpuImage->texture = cfaTexture;
                    textureImage.gpuImage->lease = textureLease;
                    auto textureSettings = settings;
                    if (log) textureSettings.options |= motioncam::RENDER_OPT_LOG_TRANSFORM;
                    textureSettings.logTransform = motioncam::LogTransformMode::ReduceBy6Bit;
                    motioncam::PreviewFrame textureFrame;
                    assert(motioncam::DNGDecoder::decodePreview(textureImage, textureSettings,
                                                               textureFrame, true));
                    assert(textureFrame.gallerySource.texture && textureFrame.rgb.empty());
                    source.samples = nullptr;
                    source.gpu = textureFrame.gallerySource.gpu;
                    source.buffer = nullptr; source.texture = textureFrame.gallerySource.texture;
                    source.black = textureFrame.galleryBlack; source.white = textureFrame.galleryWhite;
                    source.gain = textureFrame.galleryCfaGain.get();
                    assert(motioncam::colorGalleryRgbVulkan(gpuColor, gains, identity, identity,
                        1, nullptr, nullptr, false, false, &source));
                    auto cpuImage = resident;
                    cpuImage.gpuImage.reset(); cpuImage.samples = imageSamples;
                    motioncam::PreviewFrame cpuFrame;
                    assert(motioncam::DNGDecoder::decodePreview(cpuImage, textureSettings, cpuFrame, true));
                    source.gpu = source.texture = nullptr;
                    source.samples = cpuFrame.galleryCfaSamples();
                    source.black = cpuFrame.galleryBlack; source.white = cpuFrame.galleryWhite;
                    source.gain = cpuFrame.galleryCfaGain.get();
                    assert(motioncam::colorGalleryRgbVulkan(hostColor, gains, identity, identity,
                        1, nullptr, nullptr, false, false, &source));
                    if (gpuColor != hostColor) {
                        unsigned maximum = 0; size_t different = 0;
                        for (size_t i = 0; i < gpuColor.size(); i += 2) {
                            uint16_t a, b;
                            std::memcpy(&a, gpuColor.data()+i, 2);
                            std::memcpy(&b, hostColor.data()+i, 2);
                            maximum = std::max(maximum, unsigned(std::abs(int(a)-int(b))));
                            different += a != b;
                        }
                        std::fprintf(stderr, "CFA texture log=%d max_error=%u different=%zu\n", log, maximum, different);
                    }
                    assert(gpuColor == hostColor);
                }
#endif
                // Finalized LOG JPEG DNGs must linearize resident codes before
                // demosaic, matching the ordinary CPU import exactly.
                auto logDng = dng;
                assert(motioncam::DNGDecoder::ensureUncompressed(logDng));
                assert(motioncam::DNGDecoder::applyLogTransform(logDng,
                    motioncam::LogTransformMode::ReduceBy6Bit));
                assert(motioncam::DNGDecoder::compressLosslessJPEGTiled(logDng));
                motioncam::DecodedDNGImage logResident, logCpu;
                assert(motioncam::DNGDecoder::decodeImage(logDng, logResident, false, true, true));
                assert(logResident.gpuImage && !logResident.residentLinearization.empty());
                assert(motioncam::DNGDecoder::decodeImage(logDng, logCpu));
                logResident.layout.cfaRepeatSize = 2;
                logResident.layout.cfaPhase = {0,1,1,2};
                motioncam::PreviewFrame logFrame;
                assert(motioncam::DNGDecoder::decodePreview(logResident, settings, logFrame));
                assert(logFrame.gallerySource.buffer && logFrame.galleryCfaGain);
                source.samples = nullptr;
                source.gpu = logFrame.gallerySource.gpu; source.buffer = logFrame.gallerySource.buffer;
                source.black = logFrame.galleryBlack; source.white = logFrame.galleryWhite;
                source.gain = logFrame.galleryCfaGain.get();
                assert(motioncam::colorGalleryRgbVulkan(gpuColor, gains, identity, identity,
                    1, nullptr, nullptr, false, false, &source));
                source.gpu = source.buffer = nullptr;
                source.samples = logCpu.samples.data();
                source.gain = nullptr;
                assert(motioncam::colorGalleryRgbVulkan(hostColor, gains, identity, identity,
                    1, nullptr, nullptr, false, false, &source));
                assert(gpuColor == hostColor);
                std::vector<uint16_t> fallback;
                assert(logFrame.gallerySource.downloadSamples(fallback));
                assert(fallback == logCpu.samples);
                // Histogram/clipping requests recover through shared CPU processing.
                motioncam::PreviewFrame histogram;
                assert(motioncam::DNGDecoder::decodePreview(resident, settings,
                    histogram, false, true));
                assert(histogram.rawSamples && *histogram.rawSamples == imageSamples);
            }
            if (std::getenv("MOTIONCAM_JPEG_DECODE_PROFILE")) {
                std::fprintf(stderr, "Vulkan JPEG tile decode %.3f ms\n",
                    std::chrono::duration<double, std::milli>(gpuEnd - gpuStart).count());
                std::atomic_uint nextTile{0};
                std::atomic_bool cpuOk{true};
                std::vector<std::vector<uint16_t>> cpu(tileCount);
                const auto cpuStart = std::chrono::steady_clock::now();
                std::vector<std::thread> workers;
                for (int worker = 0; worker < 8; ++worker)
                    workers.emplace_back([&] {
                        for (uint32_t t; (t = nextTile.fetch_add(1)) < tileCount;) {
                            lj92 decoder = nullptr;
                            int w = 0, h = 0, bits = 0, channels = 0;
                            if (lj92_open(&decoder, encoded[t].data(),
                                          int(encoded[t].size()), &w, &h,
                                          &bits, &channels) != LJ92_ERROR_NONE) {
                                cpuOk = false;
                                continue;
                            }
                            cpu[t].resize(tileWidth * tileHeight);
                            if (lj92_decode(decoder, cpu[t].data(), tileWidth, 0,
                                            nullptr, 0) != LJ92_ERROR_NONE)
                                cpuOk = false;
                            lj92_close(decoder);
                        }
                    });
                for (auto& worker : workers) worker.join();
                const auto cpuEnd = std::chrono::steady_clock::now();
                assert(cpuOk && cpu == expected);
                std::fprintf(stderr, "CPU JPEG tile decode %.3f ms\n",
                    std::chrono::duration<double, std::milli>(cpuEnd - cpuStart).count());
            }
        } else {
            assert(!std::getenv("MOTIONCAM_REQUIRE_GPU_JPEG_DECODE_TEST"));
        }
        if (residentTest && gpuDecoded) {
            const uint32_t canvasWidth = tileCount == 64 ? 1024u : 4096u;
            const uint32_t canvasHeight = tileCount == 64 ? 1024u : 3072u;
            std::vector<uint8_t> recompressed;
            const void* residentGpu = nullptr;
            double residentMs[2]{};
            for (int attempt = 0; attempt < 2; ++attempt) {
                const auto start = std::chrono::steady_clock::now();
                assert(motioncam::jpegDecodeTiledImageVulkan(
                inputs, canvasWidth, canvasHeight, tileWidth, tileHeight,
                [&](const motioncam::VulkanDecodedJpegImage& image) {
                    residentGpu = image.gpu;
                    const std::vector<float> gains{1.1f};
                    motioncam::VulkanGainBakeBufferSource source{image.gpu, image.buffer};
                    motioncam::VulkanGainBakeParams params;
                    params.width = image.width;
                    params.height = image.height;
                    params.channels = 1;
                    params.outputBits = residentLog ? 12u : 16u;
                    params.sourceNominalWhite = 4095;
                    params.destinationWhite = 4095;
                    params.encodedWhite = residentLog ? 4095u : 0u;
                    params.log60 = residentLog;
                    params.scale.fill(1.0f);
                    params.bufferSource = &source;
                    params.map = motioncam::VulkanRgbGainMap{
                        1, 1, 1, 0, 0, image.width, image.height,
                        1, 1, 0, 0, 1, 1, &gains};
                    params.cfaMapCount = 1;
                    params.cfaMaps[0] = params.map;
                    params.consumePackedOutput = [&](const motioncam::VulkanPackedImage& packed) {
                        return motioncam::jpegEncodeFullyVulkanPackedBufferBorrowed(
                            packed.gpu, packed.buffer, packed.width, packed.height,
                            packed.channels, packed.bits, packed.wordsPerRow,
                            [&](const uint8_t* bytes, size_t count) {
                                recompressed.assign(bytes, bytes + count);
                                return true;
                            }, image.validationBuffer);
                    };
                    std::vector<uint8_t> unused(size_t(image.width) * image.height *
                                                (residentLog ? 3u : 4u) / 2u);
                    return motioncam::bakeCfaGainVulkan(nullptr, params,
                                                         unused.data(), unused.size());
                }));
                const auto end = std::chrono::steady_clock::now();
                residentMs[attempt] =
                    std::chrono::duration<double, std::milli>(end - start).count();
            }
            motioncam::releaseVulkanGainBakeBufferDevice(residentGpu);
            if (tileCount == 64) {
                auto brokenInputs = inputs;
                auto brokenTile = encoded.front();
                assert(brokenTile.size() > 102u);
                brokenTile.erase(brokenTile.end() - 102, brokenTile.end() - 2);
                brokenInputs.front().bytes = brokenTile.data();
                brokenInputs.front().size = brokenTile.size();
                assert(!motioncam::jpegDecodeTiledImageVulkan(
                    brokenInputs, canvasWidth, canvasHeight, tileWidth, tileHeight,
                    [&](const motioncam::VulkanDecodedJpegImage& image) {
                        return motioncam::jpegEncodeFullyVulkanPackedBufferBorrowed(
                            image.gpu, image.buffer, image.width, image.height,
                            1, 16, image.wordsPerRow,
                            [](const uint8_t*, size_t) { return true; },
                            image.validationBuffer);
                    }));
            }
            lj92 decoder = nullptr;
            int w = 0, h = 0, bits = 0, channels = 0;
            assert(lj92_open(&decoder, recompressed.data(), int(recompressed.size()),
                             &w, &h, &bits, &channels) == LJ92_ERROR_NONE);
            assert(w == int(canvasWidth) && h == int(canvasHeight) &&
                   bits == int(residentLog ? 12u : 16u) && channels == 1);
            std::vector<uint16_t> canvas(size_t(canvasWidth) * canvasHeight);
            assert(lj92_decode(decoder, canvas.data(), canvasWidth, 0,
                               nullptr, 0) == LJ92_ERROR_NONE);
            lj92_close(decoder);
            if (!residentLog) for (uint32_t t = 0; t < tileCount; ++t) {
                const uint32_t tilesAcross = canvasWidth / tileWidth;
                const uint32_t ox = (t % tilesAcross) * tileWidth;
                const uint32_t oy = (t / tilesAcross) * tileHeight;
                for (uint32_t y = 0; y < tileHeight; ++y)
                    for (uint32_t x = 0; x < tileWidth; ++x) {
                        const auto source = expected[t][size_t(y) * tileWidth + x];
                        const auto result = uint16_t(std::lround(source * 1.1f));
                        assert(canvas[size_t(oy + y) * canvasWidth + ox + x] == result);
                    }
            }
            std::fprintf(stderr, "Vulkan JPEG decode->gain->encode resident cold=%.3f warm=%.3f ms\n",
                         residentMs[0], residentMs[1]);
            if (std::getenv("MOTIONCAM_JPEG_DECODE_PROFILE") || residentLog) {
                std::vector<uint16_t> sourceCanvas(canvas.size());
                const uint32_t tilesAcross = canvasWidth / tileWidth;
                for (uint32_t t = 0; t < tileCount; ++t) {
                    const uint32_t ox = (t % tilesAcross) * tileWidth;
                    const uint32_t oy = (t / tilesAcross) * tileHeight;
                    for (uint32_t y = 0; y < tileHeight; ++y)
                        std::copy_n(expected[t].data() + size_t(y) * tileWidth,
                                    tileWidth,
                                    sourceCanvas.data() + size_t(oy + y) * canvasWidth + ox);
                }
                const std::vector<float> gains{1.1f};
                motioncam::VulkanGainBakeParams params;
                params.width = canvasWidth;
                params.height = canvasHeight;
                params.channels = 1;
                params.outputBits = residentLog ? 12u : 16u;
                params.sourceNominalWhite = 4095;
                params.destinationWhite = 4095;
                params.encodedWhite = residentLog ? 4095u : 0u;
                params.log60 = residentLog;
                params.scale.fill(1.0f);
                params.map = motioncam::VulkanRgbGainMap{
                    1, 1, 1, 0, 0, canvasWidth, canvasHeight,
                    1, 1, 0, 0, 1, 1, &gains};
                params.cfaMapCount = 1;
                params.cfaMaps[0] = params.map;
                params.consumePackedOutput = [&](const motioncam::VulkanPackedImage& packed) {
                    return motioncam::jpegEncodeFullyVulkanPackedBufferBorrowed(
                        packed.gpu, packed.buffer, packed.width, packed.height,
                        packed.channels, packed.bits, packed.wordsPerRow,
                        [&](const uint8_t*, size_t) { return true; });
                };
                std::vector<uint8_t> unused(sourceCanvas.size() *
                                            (residentLog ? 3u : 4u) / 2u);
                double cpuSourceMs[2]{};
                for (int attempt = 0; attempt < 2; ++attempt) {
                    const auto start = std::chrono::steady_clock::now();
                    assert(motioncam::bakeCfaGainVulkan(
                        sourceCanvas.data(), params, unused.data(), unused.size()));
                    cpuSourceMs[attempt] = std::chrono::duration<double, std::milli>(
                        std::chrono::steady_clock::now() - start).count();
                }
                if (std::getenv("MOTIONCAM_JPEG_DECODE_PROFILE"))
                    std::fprintf(stderr, "CPU pixels->Vulkan gain->JPEG cold=%.3f warm=%.3f ms\n",
                                 cpuSourceMs[0], cpuSourceMs[1]);
                if (residentLog) {
                    params.consumePackedOutput = {};
                    assert(motioncam::bakeCfaGainVulkan(
                        sourceCanvas.data(), params, unused.data(), unused.size()));
                    const size_t rowBytes = size_t(canvasWidth) * 12u / 8u;
                    for (uint32_t y = 0; y < canvasHeight; ++y)
                        for (uint32_t x = 0; x < canvasWidth; ++x) {
                            const size_t bit = size_t(x) * 12u;
                            const size_t byte = size_t(y) * rowBytes + bit / 8u;
                            const uint32_t word = (uint32_t(unused[byte]) << 16u) |
                                (uint32_t(unused[byte + 1u]) << 8u) |
                                (byte + 2u < unused.size() ? unused[byte + 2u] : 0u);
                            const uint16_t code = uint16_t((word >>
                                (12u - (bit & 7u))) & 4095u);
                            assert(canvas[size_t(y) * canvasWidth + x] == code);
                        }
                }
            }
        }
    }
#ifdef MOTIONCAM_HAS_DIRECTLOG_PLACEBO
    if (std::getenv("MOTIONCAM_GPU_JPEG_RGB_DECODE_TEST")) {
        constexpr uint32_t tileSide = 128, across = 8, tileCount = across * across;
        constexpr uint32_t width = tileSide * across, height = width;
        std::vector<std::vector<uint16_t>> reference(tileCount);
        std::vector<std::vector<uint8_t>> encoded(tileCount);
        std::vector<motioncam::VulkanJpegTile> tiles(tileCount);
        for (uint32_t t = 0; t < tileCount; ++t) {
            reference[t].resize(size_t(tileSide) * tileSide * 3u);
            for (size_t i = 0; i < reference[t].size(); ++i)
                reference[t][i] = uint16_t((i * 101u + t * 31u + i / 7u) & 65535u);
            uint8_t* bytes = nullptr;
            int count = 0;
            assert(lj92_encode(reference[t].data(), tileSide, tileSide,
                               16, 3, tileSide * 3, 0, nullptr, 0,
                               &bytes, &count) == LJ92_ERROR_NONE);
            encoded[t].assign(bytes, bytes + count);
            free(bytes);
            tiles[t] = {encoded[t].data(), encoded[t].size(),
                        tileSide, tileSide, 16, 3};
        }
        std::vector<std::vector<uint16_t>> decodedTiles;
        assert(motioncam::jpegDecodeTilesVulkan(tiles, decodedTiles));
        assert(decodedTiles == reference);
        motioncam::VulkanDecodedJpegImage retained;
        assert(motioncam::jpegDecodeTiledImageVulkanRetained(
            tiles, width, height, tileSide, tileSide, retained));
        motioncam::DecodedDNGImage rgbImage;
        rgbImage.gpuImage = std::make_shared<motioncam::DNGGpuImage>();
        rgbImage.gpuImage->gpu = retained.gpu; rgbImage.gpuImage->buffer = retained.buffer;
        rgbImage.gpuImage->width = retained.width; rgbImage.gpuImage->height = retained.height;
        rgbImage.gpuImage->channels = retained.channels;
        rgbImage.gpuImage->lease = retained.lease;
        rgbImage.gpuImage->download = [retained](uint16_t* pixels, size_t samples) {
            return motioncam::downloadVulkanDecodedJpegImage(retained, pixels, samples);
        };
        rgbImage.layout.width = width; rgbImage.layout.height = height;
        rgbImage.layout.samplesPerPixel = 3;
        rgbImage.layout.bitsPerSample = rgbImage.storedBitsPerSample = 16;
        rgbImage.layout.pixels = motioncam::DNGPixelLayout::LinearRGB;
        rgbImage.metadata.blackLevelCount = rgbImage.metadata.whiteLevelCount = 3;
        rgbImage.metadata.blackLevel.fill(123);
        rgbImage.metadata.whiteLevel.fill(60000);
        motioncam::RenderSettings rgbSettings;
        rgbSettings.options = static_cast<motioncam::FileRenderOptions>(0);
        rgbSettings.streamingPreview = true;
        motioncam::PreviewFrame rgbFrame;
        assert(motioncam::DNGDecoder::decodePreview(rgbImage, rgbSettings, rgbFrame));
        assert(rgbFrame.gallerySource.buffer && rgbFrame.rgb.empty());
        motioncam::VulkanGalleryTextureSource rgbSource;
        rgbSource.gpu = rgbFrame.gallerySource.gpu;
        rgbSource.buffer = rgbFrame.gallerySource.buffer;
        rgbSource.width = width; rgbSource.height = height;
        rgbSource.black = rgbFrame.galleryGpuBlack; rgbSource.white = rgbFrame.galleryGpuWhite;
        const std::array<float, 3> gains{1, 1, 1};
        const std::array<float, 9> identity{1,0,0, 0,1,0, 0,0,1};
        std::vector<uint8_t> residentColor;
        assert(motioncam::colorGalleryRgbVulkan(residentColor, gains, identity, identity,
            1, nullptr, nullptr, false, false, nullptr, nullptr, &rgbSource));
        rgbImage.samples.resize(size_t(width) * height * 3u);
        assert(motioncam::downloadVulkanDecodedJpegImage(retained,
            rgbImage.samples.data(), rgbImage.samples.size()));
        // Plain sampled RGB needs the neutral gain binding too. Baking used
        // to hide its missing descriptor by supplying a nonempty gain buffer.
        auto gpu = static_cast<pl_gpu>(retained.gpu);
        const auto rgbaFormat = pl_find_fmt(gpu, PL_FMT_UNORM, 4, 16, 16,
                                            PL_FMT_CAP_SAMPLEABLE);
        assert(rgbaFormat);
        std::vector<uint16_t> rgba(size_t(width) * height * 4u, 65535u);
        for (size_t pixel = 0; pixel < size_t(width) * height; ++pixel)
            std::copy_n(rgbImage.samples.data() + pixel * 3u, 3u,
                        rgba.data() + pixel * 4u);
        pl_tex_params textureParams{};
        textureParams.w = width; textureParams.h = height;
        textureParams.format = rgbaFormat;
        textureParams.sampleable = textureParams.host_writable = true;
        pl_tex rgbTexture = pl_tex_create(gpu, &textureParams);
        assert(rgbTexture);
        pl_tex_transfer_params transfer{};
        transfer.tex = rgbTexture; transfer.ptr = rgba.data();
        assert(pl_tex_upload(gpu, &transfer));
        rgbSource.buffer = nullptr; rgbSource.texture = rgbTexture;
        std::vector<uint8_t> textureColor;
        assert(motioncam::colorGalleryRgbVulkan(textureColor, gains, identity, identity,
            1, nullptr, nullptr, false, false, nullptr, nullptr, &rgbSource));
        assert(textureColor == residentColor);
        assert(std::any_of(textureColor.begin(), textureColor.end(),
                           [](uint8_t value) { return value != 0; }));
        pl_tex_destroy(gpu, &rgbTexture);
        rgbImage.gpuImage.reset();
        motioncam::PreviewFrame cpuFrame;
        assert(motioncam::DNGDecoder::decodePreview(rgbImage, rgbSettings, cpuFrame));
        assert(motioncam::colorGalleryRgbVulkan(cpuFrame.rgb, gains, identity, identity,
            1, nullptr, nullptr, false, false));
        assert(residentColor.size() == cpuFrame.rgb.size());
        unsigned maximumError = 0;
        uint64_t errorSum = 0;
        for (size_t sample = 0; sample < residentColor.size() / 2u; ++sample) {
            uint16_t residentCode, cpuCode;
            std::memcpy(&residentCode, residentColor.data() + sample * 2u, 2u);
            std::memcpy(&cpuCode, cpuFrame.rgb.data() + sample * 2u, 2u);
            const unsigned error = std::abs(int(residentCode) - int(cpuCode));
            maximumError = std::max(maximumError, error);
            errorSum += error;
        }
        std::fprintf(stderr, "Resident RGB normalization: max_error=%u mean_error=%.6f\n",
            maximumError, double(errorSum) / (residentColor.size() / 2u));
        // Float normalization can differ by one linear code from the CPU's
        // double-precision LUT; sRGB expands that by at most thirteen codes.
        assert(maximumError <= 13u);
        assert(errorSum < residentColor.size() / 2u);
        std::vector<uint8_t> jpeg;
        assert(motioncam::jpegDecodeTiledImageVulkan(
            tiles, width, height, tileSide, tileSide,
            [&](const motioncam::VulkanDecodedJpegImage& image) {
                assert(image.channels == 3);
                return motioncam::jpegEncodeFullyVulkanPackedBufferBorrowed(
                    image.gpu, image.buffer, image.width, image.height,
                    3, 16, image.wordsPerRow,
                    [&](const uint8_t* bytes, size_t size) {
                        jpeg.assign(bytes, bytes + size);
                        return true;
                    }, image.validationBuffer);
            }));
        lj92 decoder = nullptr;
        int jpegWidth = 0, jpegHeight = 0, bits = 0, components = 0;
        assert(lj92_open(&decoder, jpeg.data(), int(jpeg.size()),
                         &jpegWidth, &jpegHeight, &bits, &components) == LJ92_ERROR_NONE);
        assert(jpegWidth == int(width) && jpegHeight == int(height) &&
               bits == 16 && components == 3);
        std::vector<uint16_t> pixels(size_t(width) * height * 3u);
        assert(lj92_decode(decoder, pixels.data(), width * 3u, 0,
                           nullptr, 0) == LJ92_ERROR_NONE);
        lj92_close(decoder);
        for (uint32_t t = 0; t < tileCount; ++t) {
            const uint32_t ox = (t % across) * tileSide;
            const uint32_t oy = (t / across) * tileSide;
            for (uint32_t y = 0; y < tileSide; ++y)
                assert(std::equal(reference[t].begin() + size_t(y) * tileSide * 3u,
                                  reference[t].begin() + size_t(y + 1u) * tileSide * 3u,
                                  pixels.begin() +
                                    (size_t(oy + y) * width + ox) * 3u));
        }
    }
#endif

    return 0;
}
