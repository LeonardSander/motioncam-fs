#define TINY_DNG_WRITER_IMPLEMENTATION
#include "tinydng/tiny_dng_writer.h"
#include "DNGDecoder.h"
#include "CalibrationData.h"
#include "LRUCache.h"
#include "VirtualFileSystemImpl.h"
#include "Utils.h"
#include "VulkanGalleryPreview.h"
#include "VulkanJpegPredictor.h"
#include "VulkanJpegEntropy.h"
#include "VulkanJpegFullEncode.h"
#include "VulkanGainBake.h"
#ifdef MOTIONCAM_HAS_DIRECTLOG_PLACEBO
#include "VulkanJpegContext.h"
#include <libplacebo/gpu.h>
#endif
#include "liblj92/lj92.h"
#include <BS_thread_pool.hpp>

#include <algorithm>
#include <cassert>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>

namespace {
int verifiedGpuEntropy(const int16_t* residuals, int sampleCount,
                       const uint16_t codes[18], const uint16_t codeBits[18],
                       uint8_t* output, int capacity, int* written,
                       void* context) {
    const int result = motioncam::jpegEntropyVulkan(
        residuals, sampleCount, codes, codeBits,
        output, capacity, written, nullptr);
    if (result == 0) *static_cast<bool*>(context) = true;
    return result;
}
struct TagValue { uint32_t value = 0, count = 0; };
TagValue tagValue(const std::vector<uint8_t>& dng, uint16_t wanted) {
    auto u16 = [&](size_t offset) { return static_cast<uint16_t>(dng[offset] | dng[offset + 1] << 8); };
    auto u32 = [&](size_t offset) { return static_cast<uint32_t>(dng[offset] | dng[offset + 1] << 8 |
        dng[offset + 2] << 16 | dng[offset + 3] << 24); };
    for (uint32_t ifd = u32(4); ifd;) {
        const uint16_t entries = u16(ifd);
        for (uint16_t i = 0; i < entries; ++i) {
            const size_t entry = static_cast<size_t>(ifd) + 2 + i * 12;
            if (u16(entry) != wanted) continue;
            const uint16_t type = u16(entry + 2);
            const uint32_t count = u32(entry + 4);
            const uint32_t typeSize = type == 1 ? 1 : type == 3 ? 2 : 4;
            const size_t position = count * typeSize > 4 ? u32(entry + 8) : entry + 8;
            return {type == 3 ? u16(position) : u32(position), count};
        }
        ifd = u32(static_cast<size_t>(ifd) + 2 + entries * 12);
    }
    return {};
}

bool nestedIfdHasTag(const std::vector<uint8_t>& dng, uint16_t pointerTag,
                     uint16_t wanted) {
    auto u16 = [&](size_t offset) { return static_cast<uint16_t>(dng[offset] | dng[offset + 1] << 8); };
    auto u32 = [&](size_t offset) { return static_cast<uint32_t>(dng[offset] | dng[offset + 1] << 8 |
        dng[offset + 2] << 16 | dng[offset + 3] << 24); };
    const uint32_t root = u32(4);
    const uint16_t rootCount = u16(root);
    uint32_t nested = 0;
    for (uint16_t i = 0; i < rootCount; ++i) {
        const size_t entry = static_cast<size_t>(root) + 2 + i * 12;
        if (u16(entry) == pointerTag) { nested = u32(entry + 8); break; }
    }
    if (!nested || nested + 2 > dng.size()) return false;
    const uint16_t count = u16(nested);
    for (uint16_t i = 0; i < count; ++i)
        if (u16(static_cast<size_t>(nested) + 2 + i * 12) == wanted) return true;
    return false;
}

bool setTagType(std::vector<uint8_t>& dng, uint16_t wanted, uint16_t type) {
    auto u16 = [&](size_t offset) { return static_cast<uint16_t>(dng[offset] | dng[offset + 1] << 8); };
    auto u32 = [&](size_t offset) { return static_cast<uint32_t>(dng[offset] | dng[offset + 1] << 8 |
        dng[offset + 2] << 16 | dng[offset + 3] << 24); };
    for (uint32_t ifd = u32(4); ifd;) {
        const uint16_t entries = u16(ifd);
        for (uint16_t i = 0; i < entries; ++i) {
            const size_t entry = static_cast<size_t>(ifd) + 2 + i * 12;
            if (u16(entry) != wanted) continue;
            dng[entry + 2] = static_cast<uint8_t>(type);
            dng[entry + 3] = static_cast<uint8_t>(type >> 8);
            return true;
        }
        ifd = u32(static_cast<size_t>(ifd) + 2 + entries * 12);
    }
    return false;
}

std::pair<size_t, uint32_t> tagPayload(const std::vector<uint8_t>& dng, uint16_t wanted) {
    auto u16 = [&](size_t offset) { return static_cast<uint16_t>(dng[offset] | dng[offset + 1] << 8); };
    auto u32 = [&](size_t offset) { return static_cast<uint32_t>(dng[offset] | dng[offset + 1] << 8 |
        dng[offset + 2] << 16 | dng[offset + 3] << 24); };
    for (uint32_t ifd = u32(4); ifd;) {
        const uint16_t entries = u16(ifd);
        for (uint16_t i = 0; i < entries; ++i) {
            const size_t entry = static_cast<size_t>(ifd) + 2 + i * 12;
            if (u16(entry) == wanted) return {u32(entry + 8), u32(entry + 4)};
        }
        ifd = u32(static_cast<size_t>(ifd) + 2 + entries * 12);
    }
    return {0, 0};
}

std::vector<uint8_t> makeDng(uint16_t value, float exposure, int iso,
                             float baseline, const std::array<float, 3>& neutral,
                             motioncam::Timestamp timestamp,
                             uint32_t width = 8, uint32_t height = 8) {
    std::vector<uint16_t> pixels(width * height * 3, value);
    tinydngwriter::DNGImage image;
    image.SetBigEndian(false);
    const unsigned short bits[3] = {16, 16, 16};
    assert(image.SetImageWidth(width) && image.SetImageLength(height));
    assert(image.SetRowsPerStrip(height) && image.SetSamplesPerPixel(3));
    assert(image.SetBitsPerSample(3, bits));
    assert(image.SetCompression(tinydngwriter::COMPRESSION_NONE));
    assert(image.SetPhotometric(tinydngwriter::PHOTOMETRIC_LINEARRAW));
    assert(image.SetPlanarConfig(tinydngwriter::PLANARCONFIG_CONTIG));
    assert(image.SetWhiteLevel(65535));
    assert(image.SetExposureTime(exposure) && image.SetIso(iso));
    assert(image.SetBaselineExposure(baseline));
    assert(image.SetAsShotNeutral(3, neutral.data()));
    assert(image.SetDNGVersion(1, 4, 0, 0));
    assert(image.SetDNGBackwardVersion(1, 4, 0, 0));
    assert(image.SetImageData(reinterpret_cast<const uint8_t*>(pixels.data()), pixels.size() * 2));
    tinydngwriter::DNGWriter writer(false);
    assert(writer.AddImage(&image));
    std::ostringstream output(std::ios::binary);
    std::string error;
    assert(writer.WriteToFile(output, &error));
    const auto bytes = output.str();
    std::vector<uint8_t> result(bytes.begin(), bytes.end());
    assert(motioncam::DNGDecoder::setTimingMetadata(result, 24.0, timestamp));
    return result;
}

std::vector<uint8_t> makeLogCfaDng(uint16_t value, motioncam::Timestamp timestamp,
                                   bool withLinearization = true,
                                   uint32_t width = 32, bool patterned = false,
                                   uint32_t repeatSize = 2) {
    constexpr uint32_t height = 32;
    std::vector<uint16_t> pixels(width * height, value);
    if (patterned)
        for (uint32_t y = 0; y < height; ++y)
            for (uint32_t x = 0; x < width; ++x)
                pixels[static_cast<size_t>(y) * width + x] =
                    static_cast<uint16_t>((y * 37 + x * 53) & 1023);
    std::vector<uint16_t> linearization(1024);
    for (size_t i = 0; i < linearization.size(); ++i)
        linearization[i] = static_cast<uint16_t>(std::min<size_t>(65535, i * 64));
    const unsigned short bits = 16, black[4] = {0, 0, 0, 0};
    const unsigned char phase[4] = {0, 1, 1, 2};
    std::vector<unsigned char> pattern(repeatSize * repeatSize);
    for (uint32_t y = 0; y < repeatSize; ++y)
        for (uint32_t x = 0; x < repeatSize; ++x)
            pattern[y * repeatSize + x] = phase[
                (y / (repeatSize / 2)) * 2 + x / (repeatSize / 2)];
    tinydngwriter::DNGImage image;
    image.SetBigEndian(false);
    assert(image.SetImageWidth(width) && image.SetImageLength(height));
    assert(image.SetRowsPerStrip(height) && image.SetSamplesPerPixel(1));
    assert(image.SetBitsPerSample(1, &bits));
    assert(image.SetCompression(tinydngwriter::COMPRESSION_NONE));
    assert(image.SetPhotometric(tinydngwriter::PHOTOMETRIC_CFA));
    assert(image.SetPlanarConfig(tinydngwriter::PLANARCONFIG_CONTIG));
    assert(image.SetCFARepeatPatternDim(repeatSize, repeatSize) &&
           image.SetCFAPattern(pattern.size(), pattern.data()));
    assert(image.SetBlackLevelRepeatDim(2, 2) && image.SetBlackLevel(4, black));
    assert(image.SetWhiteLevel(1023));
    if (withLinearization)
        assert(image.SetLinearizationTable(linearization.size(), linearization.data()));
    assert(image.SetExposureTime(0.01f) && image.SetIso(100));
    const float neutral[3] = {1.0f, 1.0f, 1.0f};
    assert(image.SetBaselineExposure(0.0f) && image.SetAsShotNeutral(3, neutral));
    assert(image.SetDNGVersion(1, 4, 0, 0) && image.SetDNGBackwardVersion(1, 4, 0, 0));
    assert(image.SetImageData(reinterpret_cast<const uint8_t*>(pixels.data()), pixels.size() * 2));
    tinydngwriter::DNGWriter writer(false);
    assert(writer.AddImage(&image));
    std::ostringstream output(std::ios::binary);
    std::string error;
    assert(writer.WriteToFile(output, &error));
    const auto bytes = output.str();
    std::vector<uint8_t> result(bytes.begin(), bytes.end());
    assert(motioncam::DNGDecoder::setTimingMetadata(result, 24.0, timestamp));
    return result;
}

class FakeFileSystem final : public motioncam::IVirtualFileSystem {
public:
    FakeFileSystem(std::vector<uint8_t> left, std::vector<uint8_t> right,
                   size_t duplicatedFrames = 1, bool markDuplicates = true,
                   bool includeAncillary = false)
        : mLeft(std::move(left)), mRight(std::move(right)) {
        if (includeAncillary) {
            motioncam::Entry audio;
            audio.type = motioncam::EntryType::FILE_ENTRY;
            audio.name = "audio.wav";
            audio.size = 4;
            mEntries.push_back(audio);
        }
        for (size_t i = 0; i < duplicatedFrames + 2; ++i) {
            motioncam::Entry entry;
            entry.type = motioncam::EntryType::FILE_ENTRY;
            char name[32];
            std::snprintf(name, sizeof(name), "frame-%06zu.dng", i);
            entry.name = name;
            entry.size = i == duplicatedFrames + 1 ? mRight.size() : mLeft.size();
            entry.userData = static_cast<int64_t>(i == duplicatedFrames + 1
                ? duplicatedFrames + 1 : 0);
            entry.duplicateFrame = markDuplicates && i > 0 && i <= duplicatedFrames;
            mEntries.push_back(entry);
        }
    }
    std::vector<motioncam::Entry> listFiles(const std::string&) const override { return mEntries; }
    std::optional<motioncam::Entry> findEntry(const std::string&) const override { return {}; }
    int readFile(const motioncam::Entry&, size_t, size_t, void*,
                 std::function<void(size_t, int)>, bool) override { return -1; }
    std::shared_ptr<std::vector<uint8_t>> materializeFile(const motioncam::Entry& entry, bool, bool = false) override {
        if (entry.name == "audio.wav") {
            ++mAncillaryMaterializations;
            return std::make_shared<std::vector<uint8_t>>(
                std::initializer_list<uint8_t>{'R', 'I', 'F', 'F'});
        }
        const auto& source = std::get<int64_t>(entry.userData) == 0 ? mLeft : mRight;
        auto timed = source;
        const auto frame = static_cast<motioncam::Timestamp>(
            std::distance(mEntries.begin(), std::find_if(mEntries.begin(), mEntries.end(),
                [&](const auto& candidate) { return candidate.name == entry.name; })));
        assert(motioncam::DNGDecoder::setTimingMetadata(timed, 24.0, frame));
        return std::make_shared<std::vector<uint8_t>>(timed.begin(), timed.end());
    }
    bool materializePreviewFrame(const motioncam::Entry&, motioncam::PreviewFrame&,
                                 bool = false) override {
        return false;
    }
    bool sourceImagePayloadsEqual(const motioncam::Entry& left,
                                  const motioncam::Entry& right) override {
        const auto leftDng = materializeFile(left, false);
        const auto rightDng = materializeFile(right, false);
        return motioncam::DNGDecoder::imagePayloadsEqual(
            std::vector<uint8_t>(leftDng->begin(), leftDng->end()),
            std::vector<uint8_t>(rightDng->begin(), rightDng->end()));
    }
    void updateOptions(const motioncam::RenderSettings&) override {}
    motioncam::FileInfo getFileInfo() const override { return {}; }
    int ancillaryMaterializations() const { return mAncillaryMaterializations; }
private:
    std::vector<uint8_t> mLeft, mRight;
    std::vector<motioncam::Entry> mEntries;
    int mAncillaryMaterializations = 0;
};
} // namespace

int main(int argc, char** argv) {
    if (argc == 2 && std::strcmp(argv[1], "--jpeg-gpu-benchmark") == 0) {
        for (const int channels : {1, 3}) {
            const int width = channels == 1 ? 4096 : 2051;
            const int height = channels == 1 ? 3072 : 2048;
            const int bits = channels == 1 ? 16 : 12;
            std::vector<uint16_t> pixels(static_cast<size_t>(width) * height * channels);
            uint32_t random = 0x12345678u;
            for (auto &pixel : pixels) {
                random = random * 1664525u + 1013904223u;
                pixel = static_cast<uint16_t>((random >> (32 - bits)) &
                                              ((1u << bits) - 1u));
            }
            uint8_t *cpu = nullptr, *cold = nullptr, *warm = nullptr;
            int cpuBytes = 0, coldBytes = 0, warmBytes = 0;
            const auto start = std::chrono::steady_clock::now();
            if (lj92_encode(pixels.data(), width, height, bits, channels,
                            width * channels, 0, nullptr, 0, &cpu,
                            &cpuBytes) != LJ92_ERROR_NONE) return 2;
            const auto cpuEnd = std::chrono::steady_clock::now();
            if (!motioncam::jpegEncodeFullyVulkan(
                    pixels.data(), width, height, channels, bits,
                    &cold, &coldBytes)) return 3;
            const auto coldEnd = std::chrono::steady_clock::now();
            if (!motioncam::jpegEncodeFullyVulkan(
                    pixels.data(), width, height, channels, bits,
                    &warm, &warmBytes)) return 4;
            const auto warmEnd = std::chrono::steady_clock::now();
            const bool equal = cpuBytes == coldBytes && cpuBytes == warmBytes &&
                std::memcmp(cpu, cold, cpuBytes) == 0 &&
                std::memcmp(cpu, warm, cpuBytes) == 0;
            std::fprintf(stderr,
                "GPU JPEG benchmark: channels=%d bits=%d pixels=%zu bytes=%d "
                "cpu_ms=%.3f gpu_first_ms=%.3f gpu_warm_ms=%.3f byte_equal=%d\n",
                channels, bits, pixels.size(), cpuBytes,
                std::chrono::duration<double, std::milli>(cpuEnd - start).count(),
                std::chrono::duration<double, std::milli>(coldEnd - cpuEnd).count(),
                std::chrono::duration<double, std::milli>(warmEnd - coldEnd).count(),
                equal);
#ifdef MOTIONCAM_HAS_DIRECTLOG_PLACEBO
            if (channels == 3) {
                auto &context = motioncam::jpegContext();
                if (!context.init()) return 6;
                const pl_gpu gpu = context.vulkan->gpu;
                const pl_fmt format = pl_find_fmt(gpu, PL_FMT_UNORM, 4, 16, 16,
                                                  PL_FMT_CAP_SAMPLEABLE);
                if (!format) return 7;
                std::vector<uint16_t> rgba(static_cast<size_t>(width) * height * 4);
                for (size_t pixel = 0; pixel < pixels.size() / 3; ++pixel)
                    for (int component = 0; component < 3; ++component)
                        rgba[pixel * 4 + component] = pixels[pixel * 3 + component];
                pl_tex texture = nullptr;
                pl_tex_params params{};
                params.w = width;
                params.h = height;
                params.format = format;
                params.sampleable = true;
                params.host_writable = true;
                if (!pl_tex_recreate(gpu, &texture, &params)) return 8;
                pl_tex_transfer_params transfer{};
                transfer.tex = texture;
                transfer.ptr = rgba.data();
                transfer.row_pitch = size_t(width) * 4u * sizeof(uint16_t);
                if (!pl_tex_upload(gpu, &transfer)) return 9;
                uint8_t *textureCold = nullptr, *textureWarm = nullptr;
                int textureColdBytes = 0, textureWarmBytes = 0;
                const auto textureStart = std::chrono::steady_clock::now();
                if (!motioncam::jpegEncodeFullyVulkanTexture(
                        gpu, texture, width, height, 3, bits,
                        &textureCold, &textureColdBytes)) return 10;
                const auto textureColdEnd = std::chrono::steady_clock::now();
                if (!motioncam::jpegEncodeFullyVulkanTexture(
                        gpu, texture, width, height, 3, bits,
                        &textureWarm, &textureWarmBytes)) return 11;
                const auto textureWarmEnd = std::chrono::steady_clock::now();
                const bool textureEqual = textureColdBytes == cpuBytes &&
                    textureWarmBytes == cpuBytes &&
                    std::memcmp(textureCold, cpu, cpuBytes) == 0 &&
                    std::memcmp(textureWarm, cpu, cpuBytes) == 0;
                std::fprintf(stderr,
                    "GPU RGB texture JPEG benchmark: first_ms=%.3f warm_ms=%.3f "
                    "byte_equal=%d\n",
                    std::chrono::duration<double, std::milli>(
                        textureColdEnd - textureStart).count(),
                    std::chrono::duration<double, std::milli>(
                        textureWarmEnd - textureColdEnd).count(), textureEqual);
                std::free(textureCold);
                std::free(textureWarm);
                motioncam::releaseVulkanJpegFullEncodeDevice(gpu);
                pl_tex_destroy(gpu, &texture);
                if (!textureEqual) return 12;

                auto uncompressedDng = makeDng(
                    0, 0.01f, 100, 0.0f, {1.0f, 1.0f, 1.0f}, 0,
                    width, height);
                const size_t stripOffset = tagPayload(uncompressedDng, 273).first;
                if (stripOffset > uncompressedDng.size() ||
                    pixels.size() * sizeof(uint16_t) >
                        uncompressedDng.size() - stripOffset) return 13;
                std::memcpy(uncompressedDng.data() + stripOffset,
                            pixels.data(), pixels.size() * sizeof(uint16_t));
                auto cpuDng = uncompressedDng;
#ifdef _WIN32
                _putenv_s("MOTIONCAM_GPU_JPEG_DISABLE", "1");
#else
                setenv("MOTIONCAM_GPU_JPEG_DISABLE", "1", 1);
#endif
                const bool cpuDngOk = motioncam::DNGDecoder::compressLosslessJPEG(
                    cpuDng);
#ifdef _WIN32
                _putenv_s("MOTIONCAM_GPU_JPEG_DISABLE", "");
#else
                unsetenv("MOTIONCAM_GPU_JPEG_DISABLE");
#endif
                if (!cpuDngOk) return 14;
                auto ownedDng = uncompressedDng;
#ifdef _WIN32
                _putenv_s("MOTIONCAM_GPU_JPEG_BORROWED_DISABLE", "1");
#else
                setenv("MOTIONCAM_GPU_JPEG_BORROWED_DISABLE", "1", 1);
#endif
                const auto ownedStart = std::chrono::steady_clock::now();
                const bool ownedOk = motioncam::DNGDecoder::compressLosslessJPEG(
                    ownedDng);
                const auto ownedEnd = std::chrono::steady_clock::now();
#ifdef _WIN32
                _putenv_s("MOTIONCAM_GPU_JPEG_BORROWED_DISABLE", "");
#else
                unsetenv("MOTIONCAM_GPU_JPEG_BORROWED_DISABLE");
#endif
                if (!ownedOk) return 15;
                auto gpuDng = uncompressedDng;
                const auto dngStart = std::chrono::steady_clock::now();
                const bool gpuDngOk = motioncam::DNGDecoder::compressLosslessJPEG(
                    gpuDng);
                const auto dngEnd = std::chrono::steady_clock::now();
                motioncam::DecodedDNGImage decodedDng;
                const bool pixelEqual = gpuDngOk &&
                    motioncam::DNGDecoder::decodeImageBorrowed(
                        gpuDng, decodedDng, false, false) &&
                    decodedDng.samples == pixels;
                const bool dngEqual = gpuDngOk && gpuDng == cpuDng &&
                    gpuDng == ownedDng;
                std::fprintf(stderr,
                    "GPU RGB DNG borrowed benchmark: owned_ms=%.3f "
                    "borrowed_ms=%.3f bytes=%zu "
                    "byte_equal=%d pixel_equal=%d\n",
                    std::chrono::duration<double, std::milli>(
                        ownedEnd - ownedStart).count(),
                    std::chrono::duration<double, std::milli>(
                        dngEnd - dngStart).count(), gpuDng.size(),
                    dngEqual, pixelEqual);
                if (!dngEqual || !pixelEqual) return 16;
            }
#endif
            std::free(cpu);
            std::free(cold);
            std::free(warm);
            if (!equal) return 5;
        }
        return 0;
    }
    if ((argc == 3 || argc == 4) &&
        (std::strcmp(argv[1], "--reencode") == 0 ||
                      std::strcmp(argv[1], "--reencode-tiled") == 0)) {
        const bool tiled = std::strcmp(argv[1], "--reencode-tiled") == 0;
        std::ifstream file(argv[2], std::ios::binary);
        if (!file) return 2;
        std::vector<uint8_t> input((std::istreambuf_iterator<char>(file)),
                                    std::istreambuf_iterator<char>());
        if (!motioncam::DNGDecoder::ensureUncompressed(input)) return 3;
        auto cpu = input, gpu = input, warmGpu = input;
#ifdef _WIN32
        _putenv_s("MOTIONCAM_GPU_JPEG_DISABLE", "1");
#else
        setenv("MOTIONCAM_GPU_JPEG_DISABLE", "1", 1);
#endif
        const auto started = std::chrono::steady_clock::now();
        if (!motioncam::DNGDecoder::compressLosslessJPEG(cpu)) return 4;
        const auto cpuEnd = std::chrono::steady_clock::now();
#ifdef _WIN32
        _putenv_s("MOTIONCAM_GPU_JPEG_DISABLE", "");
        _putenv_s("MOTIONCAM_JPEG_ENCODE_PROFILE", "1");
        if (tiled) _putenv_s("MOTIONCAM_JPEG_TILED", "1");
#else
        unsetenv("MOTIONCAM_GPU_JPEG_DISABLE");
        setenv("MOTIONCAM_JPEG_ENCODE_PROFILE", "1", 1);
        if (tiled) setenv("MOTIONCAM_JPEG_TILED", "1", 1);
#endif
        if (!motioncam::DNGDecoder::compressLosslessJPEG(gpu)) return 5;
        const auto gpuEnd = std::chrono::steady_clock::now();
        if (!motioncam::DNGDecoder::compressLosslessJPEG(warmGpu)) return 6;
        const auto warmEnd = std::chrono::steady_clock::now();
#ifdef _WIN32
        _putenv_s("MOTIONCAM_JPEG_ENCODE_PROFILE", "");
        if (tiled) _putenv_s("MOTIONCAM_JPEG_TILED", "");
#else
        unsetenv("MOTIONCAM_JPEG_ENCODE_PROFILE");
        if (tiled) unsetenv("MOTIONCAM_JPEG_TILED");
#endif
        const bool byteEqual = cpu == gpu && cpu == warmGpu;
        bool equal = byteEqual;
        double cpuDecodeMs = 0, tiledDecodeMs = 0;
        if (tiled) {
            motioncam::DecodedDNGImage cpuPixels, tiledPixels;
            const auto decodeStarted = std::chrono::steady_clock::now();
            if (!motioncam::DNGDecoder::decodeImageBorrowed(cpu, cpuPixels,
                                                            false, false)) return 8;
            const auto cpuDecodeEnd = std::chrono::steady_clock::now();
            if (!motioncam::DNGDecoder::decodeImageBorrowed(gpu, tiledPixels,
                                                            false, false)) return 9;
            const auto tiledDecodeEnd = std::chrono::steady_clock::now();
            cpuDecodeMs = std::chrono::duration<double, std::milli>(
                cpuDecodeEnd - decodeStarted).count();
            tiledDecodeMs = std::chrono::duration<double, std::milli>(
                tiledDecodeEnd - cpuDecodeEnd).count();
            equal = cpuPixels.samples == tiledPixels.samples && gpu == warmGpu;
        } else if (!byteEqual) {
            motioncam::DecodedDNGImage cpuPixels, gpuPixels, warmPixels;
            if (!motioncam::DNGDecoder::decodeImageBorrowed(cpu, cpuPixels,
                                                            false, false) ||
                !motioncam::DNGDecoder::decodeImageBorrowed(gpu, gpuPixels,
                                                            false, false) ||
                !motioncam::DNGDecoder::decodeImageBorrowed(warmGpu, warmPixels,
                                                            false, false)) return 8;
            equal = cpuPixels.samples == gpuPixels.samples &&
                    cpuPixels.samples == warmPixels.samples;
        }
        std::fprintf(stderr, "DNG reencode benchmark: tiled=%d cpu_ms=%.2f gpu_first_ms=%.2f gpu_warm_ms=%.2f cpu_decode_ms=%.2f tiled_decode_ms=%.2f byte_equal=%d pixel_equal=%d\n",
                     tiled,
                     std::chrono::duration<double, std::milli>(cpuEnd - started).count(),
                     std::chrono::duration<double, std::milli>(gpuEnd - cpuEnd).count(),
                     std::chrono::duration<double, std::milli>(warmEnd - gpuEnd).count(),
                     cpuDecodeMs, tiledDecodeMs, byteEqual, equal);
        if (equal && argc == 4) {
            std::ofstream saved(argv[3], std::ios::binary);
            saved.write(reinterpret_cast<const char*>(gpu.data()), gpu.size());
            if (!saved) return 10;
        }
        return equal ? 0 : 7;
    }
    if (argc == 2) {
        std::ifstream file(argv[1], std::ios::binary);
        if (!file) return 2;
        std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(file)),
                                    std::istreambuf_iterator<char>());
        motioncam::DecodedDNGImage decoded;
        const auto started = std::chrono::steady_clock::now();
        const bool ok = motioncam::DNGDecoder::decodeImageBorrowed(
            bytes, decoded, false, false);
        const auto finished = std::chrono::steady_clock::now();
        uint64_t hash = 1469598103934665603ull;
        for (uint16_t sample : decoded.samples) {
            hash ^= sample;
            hash *= 1099511628211ull;
        }
        std::fprintf(stderr, "DNG decode benchmark: ok=%d width=%u height=%u channels=%u ms=%.2f hash=%016llx\n",
                     ok, decoded.layout.width, decoded.layout.height,
                     decoded.layout.samplesPerPixel,
                     std::chrono::duration<double, std::milli>(finished - started).count(),
                     static_cast<unsigned long long>(hash));
        return ok ? 0 : 1;
    }
    // A large strip exercises the GPU's sampled histogram path. The encoded
    // stream must match liblj92's own scan, including predictor selection.
    {
        constexpr int width = 1023, height = 1024;
        std::vector<uint16_t> pixels(static_cast<size_t>(width) * height);
        uint32_t random = 0x12345678u;
        for (auto &pixel : pixels) {
            random = random * 1664525u + 1013904223u;
            pixel = static_cast<uint16_t>((random >> 10) ^ (random >> 22));
        }
        for (int bits : {16, 12}) {
            if (bits == 12)
                for (auto &pixel : pixels)
                    pixel &= 0x0fff;
            int predictor = 0, histogram[18] = {};
            std::vector<int16_t> residuals;
            const auto gpuStart = std::chrono::steady_clock::now();
            if (motioncam::jpegPredictorHistogramVulkan(pixels.data(), width,
                                                        height, 1, bits,
                                                        predictor, histogram,
                                                        &residuals)) {
                const auto gpuScanEnd = std::chrono::steady_clock::now();
                int warmPredictor = 0, warmHistogram[18] = {};
                std::vector<int16_t> warmResiduals;
                const auto warmStart = std::chrono::steady_clock::now();
                assert(motioncam::jpegPredictorHistogramVulkan(
                    pixels.data(), width, height, 1, bits, warmPredictor,
                    warmHistogram, &warmResiduals));
                const auto warmEnd = std::chrono::steady_clock::now();
                assert(warmPredictor == predictor);
                assert(std::equal(std::begin(histogram), std::end(histogram),
                                  std::begin(warmHistogram)));
                assert(residuals == warmResiduals);
                assert(residuals.size() == pixels.size());
                uint8_t *cpu = nullptr, *gpu = nullptr;
                bool gpuEntropyUsed = false;
                int cpuBytes = 0, gpuBytes = 0;
                const auto cpuStart = std::chrono::steady_clock::now();
                assert(lj92_encode(pixels.data(), width, height, bits, 1, width,
                                   0, nullptr, 0, &cpu,
                                   &cpuBytes) == LJ92_ERROR_NONE);
                const auto cpuEnd = std::chrono::steady_clock::now();
                assert(lj92_encode_with_entropy_writer(pixels.data(), width,
                                                  height, bits, 1, width, 0,
                                                  predictor, histogram,
                                                  residuals.data(),
                                                  verifiedGpuEntropy,
                                                  &gpuEntropyUsed, &gpu,
                                                  &gpuBytes) ==
                       LJ92_ERROR_NONE);
                const auto gpuEnd = std::chrono::steady_clock::now();
                std::fprintf(
                    stderr,
                    "Vulkan JPEG predictor validation: bits=%d "
                    "cold_gpu_scan_ms=%.2f "
                    "warm_gpu_scan_ms=%.2f cpu_encode_ms=%.2f "
                    "residual_entropy_ms=%.2f\n",
                    bits,
                    std::chrono::duration<double, std::milli>(gpuScanEnd -
                                                              gpuStart)
                        .count(),
                    std::chrono::duration<double, std::milli>(warmEnd -
                                                              warmStart)
                        .count(),
                    std::chrono::duration<double, std::milli>(cpuEnd - cpuStart)
                        .count(),
                    std::chrono::duration<double, std::milli>(gpuEnd - cpuEnd)
                        .count());
                assert(cpuBytes == gpuBytes);
                assert(gpuEntropyUsed);
                assert(std::memcmp(cpu, gpu, cpuBytes) == 0);
                uint8_t* fullGpu = nullptr;
                int fullGpuBytes = 0;
                assert(motioncam::jpegEncodeFullyVulkan(
                    pixels.data(), width, height, 1, bits,
                    &fullGpu, &fullGpuBytes));
                assert(fullGpuBytes == cpuBytes &&
                       std::memcmp(cpu, fullGpu, cpuBytes) == 0);
                free(fullGpu);
                bool directUsed = false;
                motioncam::JpegEntropyImageSource source{
                    pixels.data(), width, height, 1,
                    static_cast<uint32_t>(bits),
                    static_cast<uint32_t>(predictor), &directUsed
                };
                uint8_t* direct = nullptr;
                int directBytes = 0;
                assert(lj92_encode_with_entropy_writer(
                    pixels.data(), width, height, bits, 1, width, 0,
                    predictor, histogram, nullptr, motioncam::jpegEntropyVulkan,
                    &source, &direct, &directBytes) == LJ92_ERROR_NONE);
                assert(directUsed && directBytes == cpuBytes &&
                       std::memcmp(cpu, direct, cpuBytes) == 0);
                free(direct);
                // A stale GPU image must fall back to the CPU predictor.
                source.pixels = pixels.data() + 1;
                directUsed = true;
                direct = nullptr;
                directBytes = 0;
                assert(lj92_encode_with_entropy_writer(
                    pixels.data(), width, height, bits, 1, width, 0,
                    predictor, histogram, nullptr, motioncam::jpegEntropyVulkan,
                    &source, &direct, &directBytes) == LJ92_ERROR_NONE);
                assert(!directUsed && directBytes == cpuBytes &&
                       std::memcmp(cpu, direct, cpuBytes) == 0);
                free(direct);
                free(cpu);
                free(gpu);
            } else {
                std::fprintf(stderr,
                             "Vulkan JPEG predictor validation: unavailable\n");
            }
        }
        // Interleaved RGB uses independent neighbors for each component.
        std::vector<uint16_t> rgb(static_cast<size_t>(width) * height * 3);
        for (auto &pixel : rgb) {
            random = random * 1664525u + 1013904223u;
            pixel = static_cast<uint16_t>((random >> 12) & 0x0fff);
        }
        int predictor = 0, histogram[18] = {};
        std::vector<int16_t> residuals;
        if (motioncam::jpegPredictorHistogramVulkan(
                rgb.data(), width, height, 3, 12, predictor, histogram,
                &residuals)) {
            uint8_t *cpu = nullptr, *gpu = nullptr;
            bool gpuEntropyUsed = false;
            int cpuBytes = 0, gpuBytes = 0;
            assert(lj92_encode(rgb.data(), width, height, 12, 3, width * 3, 0,
                               nullptr, 0, &cpu, &cpuBytes) == LJ92_ERROR_NONE);
            assert(residuals.size() == rgb.size());
            assert(lj92_encode_with_entropy_writer(rgb.data(), width, height, 12,
                                              3, width * 3, 0, predictor,
                                              histogram, residuals.data(),
                                              verifiedGpuEntropy,
                                              &gpuEntropyUsed,
                                              &gpu, &gpuBytes) == LJ92_ERROR_NONE);
            assert(cpuBytes == gpuBytes &&
                   std::memcmp(cpu, gpu, cpuBytes) == 0);
            assert(gpuEntropyUsed);
            uint8_t* fullGpu = nullptr;
            int fullGpuBytes = 0;
            assert(motioncam::jpegEncodeFullyVulkan(
                rgb.data(), width, height, 3, 12,
                &fullGpu, &fullGpuBytes));
            assert(fullGpuBytes == cpuBytes &&
                   std::memcmp(cpu, fullGpu, cpuBytes) == 0);
            free(fullGpu);
            bool directUsed = false;
            motioncam::JpegEntropyImageSource source{
                rgb.data(), width, height, 3, 12,
                static_cast<uint32_t>(predictor), &directUsed
            };
            uint8_t* direct = nullptr;
            int directBytes = 0;
            assert(lj92_encode_with_entropy_writer(
                rgb.data(), width, height, 12, 3, width * 3, 0,
                predictor, histogram, nullptr, motioncam::jpegEntropyVulkan,
                &source, &direct, &directBytes) == LJ92_ERROR_NONE);
            assert(directUsed && directBytes == cpuBytes &&
                   std::memcmp(cpu, direct, cpuBytes) == 0);
            free(direct);
            free(cpu);
            free(gpu);
        }
    }
    // Exercise the DNG entry point above its GPU threshold, then verify a
    // complete decode through the TIFF/JPEG path.
    {
        constexpr uint32_t width = 2048, height = 2000;
        std::vector<uint16_t> pixels(static_cast<size_t>(width) * height);
        for (uint32_t y = 0; y < height; ++y)
            for (uint32_t x = 0; x < width; ++x)
                pixels[static_cast<size_t>(y) * width + x] =
                    static_cast<uint16_t>((x * 17u + y * 29u) & 4095u);
        tinydngwriter::DNGImage image;
        image.SetBigEndian(false);
        const unsigned short bits = 16;
        assert(image.SetImageWidth(width) && image.SetImageLength(height));
        assert(image.SetRowsPerStrip(height) && image.SetSamplesPerPixel(1));
        assert(image.SetBitsPerSample(1, &bits));
        assert(image.SetCompression(tinydngwriter::COMPRESSION_NONE));
        assert(image.SetPhotometric(tinydngwriter::PHOTOMETRIC_CFA));
        assert(image.SetPlanarConfig(tinydngwriter::PLANARCONFIG_CONTIG));
        assert(image.SetWhiteLevel(4095));
        assert(image.SetImageData(reinterpret_cast<const uint8_t*>(pixels.data()),
                                  pixels.size() * sizeof(uint16_t)));
        tinydngwriter::DNGWriter writer(false);
        assert(writer.AddImage(&image));
        std::ostringstream output(std::ios::binary);
        std::string error;
        assert(writer.WriteToFile(output, &error));
        const auto bytes = output.str();
        std::vector<uint8_t> dng(bytes.begin(), bytes.end());
        auto tiled = dng;
        assert(motioncam::DNGDecoder::compressLosslessJPEG(dng));
        motioncam::DecodedDNGImage decoded;
        assert(motioncam::DNGDecoder::decodeImage(dng, decoded));
        assert(decoded.samples == pixels);
        auto defaultTiled = std::vector<uint8_t>(bytes.begin(), bytes.end());
        assert(motioncam::DNGDecoder::compressLosslessJPEGTiled(defaultTiled));
        assert(tagValue(defaultTiled, 324).count > 1);
        motioncam::DecodedDNGImage defaultTiledDecoded;
        assert(motioncam::DNGDecoder::decodeImage(defaultTiled,
                                                  defaultTiledDecoded));
        assert(defaultTiledDecoded.samples == pixels);
        motioncam::RenderSettings jpeg92Settings;
        jpeg92Settings.jxlDistance = motioncam::DNG_COMPRESSION_JPEG_LOSSLESS;
        motioncam::vfs::DngFinalizeOptions jpeg92Finalize;
        jpeg92Finalize.compression = true;
        jpeg92Finalize.writeTiming = false;
        auto finalizedTiles = std::vector<uint8_t>(bytes.begin(), bytes.end());
        motioncam::vfs::finalizeDng(finalizedTiles, jpeg92Settings, jpeg92Finalize);
        assert(tagValue(finalizedTiles, 324).count > 1);
        jpeg92Settings.jxlDistance = motioncam::DNG_COMPRESSION_JPEG92_STRIP;
        auto finalizedStrip = std::vector<uint8_t>(bytes.begin(), bytes.end());
        motioncam::vfs::finalizeDng(finalizedStrip, jpeg92Settings, jpeg92Finalize);
        assert(tagValue(finalizedStrip, 273).count == 1);
#ifdef _WIN32
        _putenv_s("MOTIONCAM_JPEG_TILED", "1");
#else
        setenv("MOTIONCAM_JPEG_TILED", "1", 1);
#endif
        assert(motioncam::DNGDecoder::compressLosslessJPEG(tiled));
#ifdef _WIN32
        _putenv_s("MOTIONCAM_JPEG_TILED", "");
#else
        unsetenv("MOTIONCAM_JPEG_TILED");
#endif
        assert(tagValue(tiled, 324).count >= 2);
        motioncam::DecodedDNGImage tiledDecoded;
        assert(motioncam::DNGDecoder::decodeImage(tiled, tiledDecoded));
        assert(tiledDecoded.samples == pixels);
    }
    assert((motioncam::vfs::inferLegacyGainMapSensorResolution(1920, 1080, 1920, 1080) ==
            std::array<int, 2>{2048, 1536}));
    assert((motioncam::vfs::inferLegacyGainMapSensorResolution(3840, 2160, 3840, 2160) ==
            std::array<int, 2>{4096, 3072}));
    assert((motioncam::vfs::inferLegacyGainMapSensorResolution(3840, 2160, 4600, 3400) ==
            std::array<int, 2>{4608, 3456}));
    assert((motioncam::vfs::inferLegacyGainMapSensorResolution(4400, 2500, 4400, 2500) ==
            std::array<int, 2>{4608, 3456}));
    assert((motioncam::vfs::inferLegacyGainMapSensorResolution(7680, 4320, 7680, 4320) ==
            std::array<int, 2>{8192, 6144}));
    assert((motioncam::vfs::inferLegacyGainMapSensorResolution(9000, 6000, 9000, 6000) ==
            std::array<int, 2>{9248, 6944}));
    motioncam::RenderSettings exposureSettings;
    exposureSettings.cameraModel.clear();
    exposureSettings.exposureCompensation = "0.8";
    assert(std::abs(motioncam::vfs::configuredExposureOffset(exposureSettings) - 0.8f) < 1e-6f);
    exposureSettings.exposureCompensation = " -1.25ev ";
    assert(std::abs(motioncam::vfs::configuredExposureOffset(exposureSettings) + 1.25f) < 1e-6f);
    exposureSettings.exposureCompensation = "2.5 EV";
    assert(std::abs(motioncam::vfs::configuredExposureOffset(exposureSettings) - 2.5f) < 1e-6f);
    exposureSettings.exposureCompensation = "0.8invalid";
    assert(motioncam::vfs::configuredExposureOffset(exposureSettings) == 0.0f);
    exposureSettings.exposureCompensation = "0.75";
    motioncam::DNGFrameMetadata galleryExposure;
    galleryExposure.baselineExposure = 1.25; // Normalized or smoothed source value.
    galleryExposure.hasBaselineExposure = true;
    motioncam::vfs::applyPreviewExposureOffset(galleryExposure, exposureSettings);
    assert(std::abs(galleryExposure.baselineExposure - 2.0) < 1e-6);

    const std::array<float, 4> black{64.0f, 64.0f, 64.0f, 64.0f};
    assert(motioncam::vfs::getDisplayDataLevels(
        15408.0f, black, 15408.0f, black,
        "Dynamic", "", true, false, 16) == "15408/64 -> 65535/256 16b");
    const std::array<float, 4> zeroBlack{0.0f, 0.0f, 0.0f, 0.0f};
    const std::array<float, 4> pro18Black{132.0f, 132.0f, 132.0f, 132.0f};
    assert(motioncam::vfs::getDisplayDataLevels(
        1024.0f, pro18Black, 1024.0f, pro18Black,
        "Dynamic", "", true, false) == "1024/132 -> 4095/256 12b");
    assert(motioncam::vfs::getDisplayDataLevels(
        32000.0f, zeroBlack, 32000.0f, zeroBlack,
        "Dynamic", "", false, false) == "32000/0 16b");
    assert(motioncam::vfs::getDisplayDataLevels(
        32000.0f, zeroBlack, 32000.0f, zeroBlack,
        "Dynamic", "Reduce by 2bit", false, false) ==
        "32000/0 -> 16383/0 14b log");
    assert(motioncam::vfs::getDisplayDataLevels(
        2047.0f, zeroBlack, 2047.0f, zeroBlack,
        "Dynamic", "", true, false) == "2047/0 -> 16383/0 14b");
    assert(motioncam::vfs::getDisplayDataLevels(
        2047.0f, zeroBlack, 2047.0f, zeroBlack,
        "Dynamic", "", false, false) == "2047/0 12b");
    assert(motioncam::vfs::getDisplayDataLevels(
        46915.0f, zeroBlack, 46915.0f, zeroBlack,
        "Dynamic", "", true, false, 16) ==
        "46915/0 -> 65535/0 16b");
    assert(motioncam::vfs::getDisplayDataLevels(
        65535.0f, zeroBlack, 65535.0f, zeroBlack,
        "46915/0", "", true, false, 16) ==
        "46915/0 -> 65535/0 16b");
    assert(motioncam::utils::evenBitsNeeded(32000) == 16);
    assert(motioncam::vfs::getDisplayDataType(false, 2) == "2x2");
    assert(motioncam::vfs::getDisplayDataType(false, 8) == "8x8");
    assert(motioncam::vfs::getDisplayDataType(true, 0) == "RGB");

    namespace fs = std::filesystem;
    const auto uncompressedA = makeDng(
        1234, 0.01f, 100, 0.0f, {1.0f, 1.0f, 1.0f}, 0);
    const auto uncompressedB = makeDng(
        1234, 0.01f, 100, 0.0f, {1.0f, 1.0f, 1.0f}, 1000000000);
    const auto uncompressedDifferent = makeDng(
        1235, 0.01f, 100, 0.0f, {1.0f, 1.0f, 1.0f}, 1000000000);
    auto gpsDng = uncompressedA;
    motioncam::DNGSidecarMetadataEntry gpsVersion;
    gpsVersion.tag = 0;
    gpsVersion.type = 1;
    gpsVersion.count = 4;
    gpsVersion.gps = true;
    gpsVersion.value = {2, 3, 0, 0};
    motioncam::DNGSidecarMetadataEntry gpsLatitudeRef;
    gpsLatitudeRef.tag = 1;
    gpsLatitudeRef.type = 2;
    gpsLatitudeRef.count = 2;
    gpsLatitudeRef.gps = true;
    gpsLatitudeRef.value = {'N', 0};
    assert(motioncam::DNGDecoder::fillMissingSidecarMetadata(
        gpsDng, {gpsVersion, gpsLatitudeRef}));
    assert(nestedIfdHasTag(gpsDng, 34853, 0));
    assert(nestedIfdHasTag(gpsDng, 34853, 1));
    assert(motioncam::DNGDecoder::compressLosslessJPEG(gpsDng));
    assert(nestedIfdHasTag(gpsDng, 34853, 0));
    assert(nestedIfdHasTag(gpsDng, 34853, 1));
    assert(motioncam::DNGDecoder::imagePayloadsEqual(uncompressedA, uncompressedB));
    assert(!motioncam::DNGDecoder::imagePayloadsEqual(
        uncompressedA, uncompressedDifferent));
    motioncam::DNGFrameMetadata packedMetadata;
    assert(motioncam::DNGDecoder::getColorMetadata(uncompressedA, packedMetadata));
    assert(packedMetadata.inputBitDepth == 16);
    auto oddWhiteDng = makeLogCfaDng(1023, 0, false);
    assert(motioncam::DNGDecoder::overrideDataLevels(oddWhiteDng, "2047/0"));
    motioncam::DNGFrameMetadata oddWhiteMetadata;
    assert(motioncam::DNGDecoder::getColorMetadata(oddWhiteDng, oddWhiteMetadata));
    assert(oddWhiteMetadata.inputBitDepth == 12);
    assert(motioncam::DNGDecoder::packUncompressedToWhiteLevel(oddWhiteDng));
    motioncam::DecodedDNGImage oddWhiteImage;
    assert(motioncam::DNGDecoder::decodeImage(oddWhiteDng, oddWhiteImage));
    assert(oddWhiteImage.layout.bitsPerSample == 12);
    // Configured exposure compensation is a metadata-only operation. Gallery
    // applies BaselineExposure while color-transforming its decoded preview;
    // the DNG sample payload itself must remain byte-for-byte equivalent.
    auto exposureTagged = uncompressedA;
    motioncam::RenderSettings taggedSettings;
    taggedSettings.cameraModel.clear();
    taggedSettings.exposureCompensation = "0.75";
    motioncam::vfs::DngFinalizeOptions taggedFinalize;
    taggedFinalize.writeTiming = false;
    motioncam::vfs::finalizeDng(exposureTagged, taggedSettings, taggedFinalize);
    motioncam::DNGFrameMetadata taggedMetadata;
    assert(motioncam::DNGDecoder::getColorMetadata(exposureTagged, taggedMetadata));
    assert(std::abs(taggedMetadata.baselineExposure - 0.75) < 1e-5);
    assert(motioncam::DNGDecoder::imagePayloadsEqual(
        exposureTagged, uncompressedA));
    auto normalizedExposureDng = uncompressedA;
    const double normalizedBaseline = 1.25;
    assert(motioncam::DNGDecoder::updateMetadata(
        normalizedExposureDng, &normalizedBaseline, nullptr));
    motioncam::vfs::finalizeDng(
        normalizedExposureDng, taggedSettings, taggedFinalize);
    motioncam::DNGFrameMetadata normalizedOutputMetadata;
    assert(motioncam::DNGDecoder::getColorMetadata(
        normalizedExposureDng, normalizedOutputMetadata));
    assert(std::abs(normalizedOutputMetadata.baselineExposure -
                    galleryExposure.baselineExposure) < 1e-5);
    assert(motioncam::DNGDecoder::imagePayloadsEqual(
        normalizedExposureDng, uncompressedA));
    motioncam::CalibrationData badPixelCalibration;
    badPixelCalibration.hasBadPixels = true;
    motioncam::CalibrationData::BadPixel defect;
    defect.x = 0;
    defect.y = 0;
    defect.action = motioncam::CalibrationData::BadPixelAction::Dampen;
    defect.amount = 0.5f;
    badPixelCalibration.badPixels.push_back(defect);
    motioncam::RenderSettings badPixelSettings;
    badPixelSettings.badPixelTreatment = motioncam::BadPixelTreatment::Bake;
    auto rgbIgnoresBadPixels = uncompressedA;
    motioncam::vfs::DngPixelPipelineOptions rgbPipeline;
    rgbPipeline.hasCfa = false;
    rgbPipeline.calibration = &badPixelCalibration;
    motioncam::vfs::processDngPixels(
        rgbIgnoresBadPixels, badPixelSettings, rgbPipeline);
    assert(motioncam::DNGDecoder::imagePayloadsEqual(
        rgbIgnoresBadPixels, uncompressedA));
    auto correctedCfa = makeLogCfaDng(1023, 0);
    motioncam::vfs::DngPixelPipelineOptions cfaPipeline;
    cfaPipeline.hasCfa = true;
    cfaPipeline.cfaRepeatSize = 2;
    cfaPipeline.calibration = &badPixelCalibration;
    cfaPipeline.iso = 100.0;
    cfaPipeline.exposureTime = 0.01;
    motioncam::vfs::processDngPixels(
        correctedCfa, badPixelSettings, cfaPipeline);
    motioncam::DecodedDNGImage correctedImage;
    assert(motioncam::DNGDecoder::decodeImage(
        correctedCfa, correctedImage, false, false));
    assert(correctedImage.samples.front() == 512);
    // Gain-map-only output is a synthetic flat field. Bad-pixel policy must
    // behave as Disabled, including suppressing FixBadPixelsList metadata.
    auto debugGainOnly = makeLogCfaDng(1023, 0);
    motioncam::RenderSettings debugBadPixelSettings;
    debugBadPixelSettings.options = static_cast<motioncam::FileRenderOptions>(
        motioncam::RENDER_OPT_APPLY_VIGNETTE_CORRECTION |
        motioncam::RENDER_OPT_DEBUG_SHADING_MAP);
    debugBadPixelSettings.badPixelTreatment =
        motioncam::BadPixelTreatment::OpcodeOnly;
    motioncam::vfs::processDngPixels(
        debugGainOnly, debugBadPixelSettings, cfaPipeline);
    const auto [debugBadPixelOffset, debugBadPixelBytes] =
        tagPayload(debugGainOnly, 51008);
    assert(!debugBadPixelOffset && !debugBadPixelBytes);
    auto debugRgb = makeLogCfaDng(1023, 0);
    std::vector<motioncam::GainMap> constantPhaseMaps(4);
    for (size_t phaseIndex = 0; phaseIndex < constantPhaseMaps.size(); ++phaseIndex) {
        auto& map = constantPhaseMaps[phaseIndex];
        map.top = static_cast<uint32_t>(phaseIndex / 2);
        map.left = static_cast<uint32_t>(phaseIndex % 2);
        map.bottom = map.right = 32;
        map.coordinateWidth = map.coordinateHeight = 32;
        map.plane = 0;
        map.planes = 1;
        map.rowPitch = map.colPitch = 2;
        map.width = map.height = map.channels = 1;
        map.spacingV = map.spacingH = 1.0;
        map.data = {1.25f + static_cast<float>(phaseIndex) * 0.25f};
    }
    auto pro18BakedDng = makeLogCfaDng(512, 0, false);
    assert(motioncam::DNGDecoder::overrideDataLevels(pro18BakedDng, "1024/132"));
    assert(motioncam::DNGDecoder::replaceGainMaps(
        pro18BakedDng, 2, constantPhaseMaps));
    assert(motioncam::DNGDecoder::bakeGainMaps(pro18BakedDng, false, false));
    motioncam::DNGFrameMetadata pro18BakedMetadata;
    assert(motioncam::DNGDecoder::getColorMetadata(
        pro18BakedDng, pro18BakedMetadata));
    assert(pro18BakedMetadata.whiteLevel[0] == 4095.0f);
    assert(pro18BakedMetadata.blackLevel[0] == 256.0f);
    assert(motioncam::DNGDecoder::packUncompressedToWhiteLevel(pro18BakedDng));
    assert(tagValue(pro18BakedDng, 258).value == 12);
    assert(motioncam::DNGDecoder::replaceGainMaps(
        debugRgb, 2, constantPhaseMaps));
    auto debugRgbSettings = debugBadPixelSettings;
    debugRgbSettings.badPixelTreatment = motioncam::BadPixelTreatment::Disabled;
    auto debugRgbPipeline = cfaPipeline;
    debugRgbPipeline.calibration = nullptr;
    debugRgbPipeline.cfaRepeatSize = 4;
    motioncam::vfs::processDngPixels(
        debugRgb, debugRgbSettings, debugRgbPipeline);
    motioncam::DecodedDNGImage debugRgbImage;
    assert(motioncam::DNGDecoder::decodeImage(
        debugRgb, debugRgbImage, false, false));
    assert(debugRgbImage.layout.pixels == motioncam::DNGPixelLayout::LinearRGB);
    const auto rgbPixel = [&](uint32_t x, uint32_t y) {
        const size_t offset = (static_cast<size_t>(y) * debugRgbImage.layout.width + x) * 3;
        return std::array<uint16_t, 3>{debugRgbImage.samples[offset],
            debugRgbImage.samples[offset + 1], debugRgbImage.samples[offset + 2]};
    };
    assert(rgbPixel(0, 0) == rgbPixel(1, 1));
    assert(rgbPixel(31, 0) == rgbPixel(30, 1));
    assert(rgbPixel(0, 31) == rgbPixel(1, 30));
    assert(rgbPixel(31, 31) == rgbPixel(30, 30));
    auto proxyCfa = makeLogCfaDng(1023, 0);
    assert(motioncam::DNGDecoder::replaceGainMaps(
        proxyCfa, 2, constantPhaseMaps));
    motioncam::RenderSettings cfaProxySettings;
    cfaProxySettings.options = static_cast<motioncam::FileRenderOptions>(
        motioncam::RENDER_OPT_DRAFT |
        motioncam::RENDER_OPT_APPLY_VIGNETTE_CORRECTION);
    cfaProxySettings.draftScale = 2;
    auto proxyPipeline = cfaPipeline;
    proxyPipeline.calibration = nullptr;
    proxyPipeline.outputScale = 2;
    motioncam::vfs::processDngPixels(
        proxyCfa, cfaProxySettings, proxyPipeline);
    motioncam::DecodedDNGImage proxyImage;
    assert(motioncam::DNGDecoder::decodeImage(
        proxyCfa, proxyImage, false, false));
    assert(proxyImage.layout.pixels == motioncam::DNGPixelLayout::CFA);
    assert(proxyImage.layout.width == 16 && proxyImage.layout.height == 16);

    auto preScaledCfa = makeLogCfaDng(1023, 0);
    assert(motioncam::DNGDecoder::replaceGainMaps(
        preScaledCfa, 2, constantPhaseMaps));
    proxyPipeline.preScaledProxy = true;
    motioncam::vfs::processDngPixels(
        preScaledCfa, cfaProxySettings, proxyPipeline);
    motioncam::DecodedDNGImage preScaledImage;
    assert(motioncam::DNGDecoder::decodeImage(
        preScaledCfa, preScaledImage, false, false));
    assert(preScaledImage.layout.pixels == motioncam::DNGPixelLayout::CFA);
    assert(preScaledImage.layout.width == 32 && preScaledImage.layout.height == 32);

    auto hqProxy = makeLogCfaDng(1023, 0);
    assert(motioncam::DNGDecoder::replaceGainMaps(
        hqProxy, 2, constantPhaseMaps));
    cfaProxySettings.options = static_cast<motioncam::FileRenderOptions>(
        cfaProxySettings.options | motioncam::RENDER_OPT_HIGHER_CFA_HQ);
    proxyPipeline.preScaledProxy = false;
    motioncam::vfs::processDngPixels(
        hqProxy, cfaProxySettings, proxyPipeline);
    motioncam::DecodedDNGImage hqProxyImage;
    assert(motioncam::DNGDecoder::decodeImage(
        hqProxy, hqProxyImage, false, false));
    assert(hqProxyImage.layout.pixels == motioncam::DNGPixelLayout::LinearRGB);
    auto keepInputGain = makeLogCfaDng(1023, 0);
    assert(motioncam::DNGDecoder::replaceGainMaps(
        keepInputGain, 2, constantPhaseMaps));
    motioncam::RenderSettings keepInputSettings;
    keepInputSettings.options = static_cast<motioncam::FileRenderOptions>(
        motioncam::RENDER_OPT_LOG_TRANSFORM |
        motioncam::RENDER_OPT_APPLY_VIGNETTE_CORRECTION);
    keepInputSettings.logTransform = motioncam::LogTransformMode::KeepInput;
    auto keepInputPipeline = cfaPipeline;
    keepInputPipeline.calibration = nullptr;
    keepInputPipeline.inputQuantizationWhite = 1023;
    motioncam::vfs::processDngPixels(
        keepInputGain, keepInputSettings, keepInputPipeline);
    assert(tagValue(keepInputGain, 50712).count == 1024);
    assert(motioncam::DNGDecoder::packUncompressedToWhiteLevel(keepInputGain));
    assert(tagValue(keepInputGain, 50712).count == 1024);
    assert(motioncam::DNGDecoder::compressLosslessJPEG(keepInputGain));
    assert(motioncam::DNGDecoder::getLinearizationTableCount(keepInputGain) == 1024);
    const std::array packedLogModes{
        motioncam::LogTransformMode::ReduceBy2Bit,
        motioncam::LogTransformMode::ReduceBy4Bit,
        motioncam::LogTransformMode::ReduceBy6Bit,
        motioncam::LogTransformMode::ReduceBy8Bit,
        motioncam::LogTransformMode::ReduceBy10Bit,
        motioncam::LogTransformMode::ReduceBy12Bit,
        motioncam::LogTransformMode::ReduceBy14Bit};
    assert(motioncam::stringToLogTransformMode("14") ==
           motioncam::LogTransformMode::ReduceBy14Bit);
    assert(motioncam::logOutputBits(10, motioncam::LogTransformMode::ReduceBy14Bit) == 2);
    for (size_t modeIndex = 0; modeIndex < packedLogModes.size(); ++modeIndex) {
        const auto mode = packedLogModes[modeIndex];
        const uint32_t ditherSeed = static_cast<uint32_t>(modeIndex + 1);
        const uint32_t expectedBits = 14 - static_cast<uint32_t>(modeIndex) * 2;
        auto fusedUnpacked = makeLogCfaDng(0, 0, false, 33, true);
        assert(motioncam::DNGDecoder::replaceGainMaps(
            fusedUnpacked, 2, constantPhaseMaps));
        auto fusedPacked = fusedUnpacked;
        auto separateBake = fusedUnpacked;
        bool logFused = false;
        assert(motioncam::DNGDecoder::bakeGainMaps(
            fusedUnpacked, false, false, false, false, 2, std::nullopt,
            mode, 65535, true, &logFused, false, ditherSeed));
        assert(logFused);
        assert(motioncam::DNGDecoder::bakeGainMaps(
            fusedPacked, false, false, false, false, 2, std::nullopt,
            mode, 65535, true, &logFused, true, ditherSeed));
        assert(logFused);
        assert(tagValue(fusedUnpacked, 258).value == 16);
        assert(tagValue(fusedPacked, 258).value == expectedBits);
        assert(motioncam::DNGDecoder::applyLogTransform(
            fusedUnpacked, mode, 65535, true, true, ditherSeed));
        assert(motioncam::DNGDecoder::applyLogTransform(
            fusedPacked, mode, 65535, true, true, ditherSeed));
        assert(tagValue(fusedPacked, 50712).count == (1u << expectedBits));
        assert(motioncam::DNGDecoder::packUncompressedToWhiteLevel(fusedUnpacked));
        assert(motioncam::DNGDecoder::packUncompressedToWhiteLevel(fusedPacked));
        assert(motioncam::DNGDecoder::imagePayloadsEqual(fusedUnpacked, fusedPacked));
        assert(motioncam::DNGDecoder::bakeGainMaps(
            separateBake, false, false, false, false, 2, std::nullopt));
        assert(motioncam::DNGDecoder::applyLogTransform(
            separateBake, mode, 65535, true, false, ditherSeed));
        assert(motioncam::DNGDecoder::packUncompressedToWhiteLevel(separateBake));
        assert(motioncam::DNGDecoder::imagePayloadsEqual(separateBake, fusedPacked));
    }
    // Compressed input must take the decoded-source path, then fuse the gain
    // and packed LOG bake without serializing an unused 16-bit intermediate.
    auto compressedGainLog = makeLogCfaDng(0, 0, false, 32, true);
    assert(motioncam::DNGDecoder::replaceGainMaps(
        compressedGainLog, 2, constantPhaseMaps));
    assert(motioncam::DNGDecoder::compressLosslessJPEG(compressedGainLog));
    auto compressedGainCpu = compressedGainLog;
    motioncam::DecodedDNGImage compressedGainImage, uncompressedGainImage;
    motioncam::RenderSettings compressedGainSettings;
    compressedGainSettings.options = static_cast<motioncam::FileRenderOptions>(
        motioncam::RENDER_OPT_APPLY_VIGNETTE_CORRECTION |
        motioncam::RENDER_OPT_LOG_TRANSFORM | motioncam::RENDER_OPT_LOG_HQ);
    compressedGainSettings.logTransform = motioncam::LogTransformMode::ReduceBy8Bit;
    auto compressedGainPipeline = cfaPipeline;
    compressedGainPipeline.calibration = nullptr;
    compressedGainPipeline.inputQuantizationWhite = 65535;
    auto compressedCpuPipeline = compressedGainPipeline;
    compressedCpuPipeline.preferVulkan = false;
    motioncam::vfs::processDngPixels(
        compressedGainLog, compressedGainSettings, compressedGainPipeline);
    motioncam::vfs::processDngPixels(
        compressedGainCpu, compressedGainSettings, compressedCpuPipeline);
    assert(tagValue(compressedGainLog, 258).value == 8);
    assert(motioncam::DNGDecoder::decodeImageBorrowed(
        compressedGainLog, compressedGainImage, false, false));
    assert(motioncam::DNGDecoder::decodeImageBorrowed(
        compressedGainCpu, uncompressedGainImage, false, false));
    assert(compressedGainImage.samples.size() == uncompressedGainImage.samples.size());
    // The shared finalizer must attach a terminal GPU JPEG after metadata work
    // without reading an intermediate uncompressed strip back to the CPU.
    auto gpuFinalDng = makeDng(32768, 0.01f, 100, 0.0f,
                               {1, 1, 1}, 0, 1024, 512);
    auto referenceFinalDng = gpuFinalDng;
    motioncam::RenderSettings gpuFinalSettings;
    gpuFinalSettings.options = motioncam::RENDER_OPT_LOG_TRANSFORM;
    gpuFinalSettings.logTransform = motioncam::LogTransformMode::ReduceBy8Bit;
    gpuFinalSettings.jxlDistance = motioncam::DNG_COMPRESSION_JPEG92_STRIP;
    std::vector<uint8_t> preparedJpeg;
    motioncam::vfs::DngPixelPipelineOptions gpuFinalPixels;
    gpuFinalPixels.gpuJpeg = &preparedJpeg;
    gpuFinalPixels.inputQuantizationWhite = 65535;
    motioncam::vfs::processDngPixels(
        gpuFinalDng, gpuFinalSettings, gpuFinalPixels);
    if (std::getenv("MOTIONCAM_REQUIRE_GPU_JPEG_TEST"))
        assert(!preparedJpeg.empty());
    motioncam::vfs::DngPixelPipelineOptions referenceFinalPixels;
    referenceFinalPixels.inputQuantizationWhite = 65535;
    motioncam::vfs::processDngPixels(
        referenceFinalDng, gpuFinalSettings, referenceFinalPixels);
    motioncam::vfs::DngFinalizeOptions gpuFinalOptions;
    gpuFinalOptions.writeTiming = false;
    gpuFinalOptions.packToWhiteLevel = true;
    gpuFinalOptions.compression = true;
    gpuFinalOptions.gpuJpeg = &preparedJpeg;
    motioncam::vfs::finalizeDng(
        gpuFinalDng, gpuFinalSettings, gpuFinalOptions);
    gpuFinalOptions.gpuJpeg = nullptr;
    motioncam::vfs::finalizeDng(
        referenceFinalDng, gpuFinalSettings, gpuFinalOptions);
    motioncam::DecodedDNGImage gpuFinalDecoded, referenceFinalDecoded;
    assert(motioncam::DNGDecoder::decodeImage(gpuFinalDng, gpuFinalDecoded));
    assert(motioncam::DNGDecoder::decodeImage(
        referenceFinalDng, referenceFinalDecoded));
    assert(gpuFinalDecoded.samples == referenceFinalDecoded.samples);
#ifdef MOTIONCAM_HAS_DIRECTLOG_PLACEBO
    // DirectLog's terminal decode texture can supply JPEG without first
    // materializing the placeholder DNG strip.
    {
        constexpr uint32_t textureWidth = 1024, textureHeight = 512;
        std::vector<uint16_t> rgb(static_cast<size_t>(textureWidth) *
                                  textureHeight * 3u);
        std::vector<uint16_t> rgba(static_cast<size_t>(textureWidth) *
                                   textureHeight * 4u);
        for (size_t pixel = 0; pixel < rgb.size() / 3u; ++pixel)
            for (uint32_t channel = 0; channel < 3u; ++channel) {
                const uint16_t value = static_cast<uint16_t>(
                    (pixel * 31u + channel * 911u) & 65535u);
                rgb[pixel * 3u + channel] = value;
                rgba[pixel * 4u + channel] = value;
            }
        auto& context = motioncam::jpegContext();
        const bool textureGpuReady = context.init();
        if (!textureGpuReady)
            assert(!std::getenv("MOTIONCAM_REQUIRE_GPU_JPEG_TEST"));
        if (textureGpuReady) {
        const pl_gpu gpu = context.vulkan->gpu;
        const pl_fmt format = pl_find_fmt(
            gpu, PL_FMT_UNORM, 4, 16, 16, PL_FMT_CAP_SAMPLEABLE);
        assert(format);
        pl_tex texture = nullptr;
        pl_tex_params textureParams{};
        textureParams.w = textureWidth;
        textureParams.h = textureHeight;
        textureParams.format = format;
        textureParams.sampleable = true;
        textureParams.host_writable = true;
        assert(pl_tex_recreate(gpu, &texture, &textureParams));
        pl_tex_transfer_params upload{};
        upload.tex = texture;
        upload.ptr = rgba.data();
        upload.row_pitch = size_t(textureWidth) * 4u * sizeof(uint16_t);
        assert(pl_tex_upload(gpu, &upload));
        motioncam::VulkanGainBakeTextureSource source;
        source.gpu = gpu;
        source.texture = texture;
        size_t textureDownloads = 0;
        source.download = [&rgb, &textureDownloads](uint16_t* output, size_t count) {
            ++textureDownloads;
            if (count != rgb.size()) return false;
            std::copy(rgb.begin(), rgb.end(), output);
            return true;
        };
        auto textureDng = makeDng(0, 0.01f, 100, 0.0f,
                                  {1, 1, 1}, 0,
                                  textureWidth, textureHeight);
        std::vector<uint8_t> textureJpeg;
        motioncam::vfs::DngPixelPipelineOptions texturePixels;
        texturePixels.textureSource = &source;
        texturePixels.gpuJpeg = &textureJpeg;
        motioncam::RenderSettings textureSettings;
        textureSettings.jxlDistance = motioncam::DNG_COMPRESSION_JPEG92_STRIP;
        motioncam::vfs::processDngPixels(
            textureDng, textureSettings, texturePixels);
        if (!textureJpeg.empty()) assert(textureDownloads == 0);
        if (std::getenv("MOTIONCAM_REQUIRE_GPU_JPEG_TEST"))
            assert(!textureJpeg.empty());
        motioncam::vfs::DngFinalizeOptions textureFinalize;
        textureFinalize.writeTiming = false;
        textureFinalize.packToWhiteLevel = true;
        textureFinalize.compression = true;
        textureFinalize.gpuJpeg = &textureJpeg;
        motioncam::vfs::finalizeDng(
            textureDng, textureSettings, textureFinalize);
        motioncam::DecodedDNGImage textureDecoded;
        assert(motioncam::DNGDecoder::decodeImage(
            textureDng, textureDecoded));
        assert(textureDecoded.samples == rgb);

        auto textureLogDng = makeDng(0, 0.01f, 100, 0.0f,
                                     {1, 1, 1}, 0,
                                     textureWidth, textureHeight);
        auto referenceLogDng = textureLogDng;
        assert(motioncam::DNGDecoder::fillUncompressedImageStrip(
            referenceLogDng, [&rgb](uint16_t* output, size_t count) {
                if (count != rgb.size()) return false;
                std::copy(rgb.begin(), rgb.end(), output);
                return true;
            }));
        auto textureLogSettings = textureSettings;
        textureLogSettings.options = motioncam::RENDER_OPT_LOG_TRANSFORM;
        textureLogSettings.logTransform =
            motioncam::LogTransformMode::ReduceBy4Bit;
        std::vector<uint8_t> textureLogJpeg;
        auto textureLogPixels = texturePixels;
        textureLogPixels.gpuJpeg = &textureLogJpeg;
        textureLogPixels.inputQuantizationWhite = 65535;
        const size_t downloadsBeforeLog = textureDownloads;
        motioncam::vfs::processDngPixels(
            textureLogDng, textureLogSettings, textureLogPixels);
        if (std::getenv("MOTIONCAM_REQUIRE_GPU_JPEG_TEST")) {
            assert(!textureLogJpeg.empty());
            assert(textureDownloads == downloadsBeforeLog);
        }
        motioncam::vfs::DngPixelPipelineOptions referenceLogPixels;
        referenceLogPixels.inputQuantizationWhite = 65535;
        motioncam::vfs::processDngPixels(
            referenceLogDng, textureLogSettings, referenceLogPixels);
        textureFinalize.gpuJpeg = &textureLogJpeg;
        motioncam::vfs::finalizeDng(
            textureLogDng, textureLogSettings, textureFinalize);
        textureFinalize.gpuJpeg = nullptr;
        motioncam::vfs::finalizeDng(
            referenceLogDng, textureLogSettings, textureFinalize);
        motioncam::DecodedDNGImage textureLogDecoded, referenceLogDecoded;
        assert(motioncam::DNGDecoder::decodeImage(
            textureLogDng, textureLogDecoded));
        assert(motioncam::DNGDecoder::decodeImage(
            referenceLogDng, referenceLogDecoded));
        assert(textureLogDecoded.samples == referenceLogDecoded.samples);
        auto tiledTextureSettings = textureSettings;
        tiledTextureSettings.jxlDistance = motioncam::DNG_COMPRESSION_JPEG_LOSSLESS;
        auto tiledTextureDng = makeDng(0, 0.01f, 100, 0.0f,
                                       {1, 1, 1}, 0,
                                       textureWidth, textureHeight);
        motioncam::vfs::GpuJpegTiles textureTiles;
        auto tiledTexturePixels = texturePixels;
        tiledTexturePixels.gpuJpeg = nullptr;
        tiledTexturePixels.gpuJpegTiles = &textureTiles;
        const size_t downloadsBeforeTiles = textureDownloads;
        motioncam::vfs::processDngPixels(
            tiledTextureDng, tiledTextureSettings, tiledTexturePixels);
        if (std::getenv("MOTIONCAM_REQUIRE_GPU_JPEG_TEST")) {
            assert(!textureTiles.bytes.empty());
            assert(textureDownloads == downloadsBeforeTiles);
        }
        motioncam::vfs::DngFinalizeOptions tiledTextureFinalize;
        tiledTextureFinalize.writeTiming = false;
        tiledTextureFinalize.packToWhiteLevel = true;
        tiledTextureFinalize.compression = true;
        tiledTextureFinalize.gpuJpegTiles = &textureTiles;
        motioncam::vfs::finalizeDng(
            tiledTextureDng, tiledTextureSettings, tiledTextureFinalize);
        motioncam::DecodedDNGImage tiledTextureDecoded;
        assert(motioncam::DNGDecoder::decodeImage(
            tiledTextureDng, tiledTextureDecoded));
        assert(tiledTextureDecoded.samples == rgb);

        auto tiledTextureLogDng = makeDng(0, 0.01f, 100, 0.0f,
                                          {1, 1, 1}, 0,
                                          textureWidth, textureHeight);
        auto tiledTextureLogSettings = textureLogSettings;
        tiledTextureLogSettings.jxlDistance = motioncam::DNG_COMPRESSION_JPEG_LOSSLESS;
        motioncam::vfs::GpuJpegTiles textureLogTiles;
        auto tiledTextureLogPixels = textureLogPixels;
        tiledTextureLogPixels.gpuJpeg = nullptr;
        tiledTextureLogPixels.gpuJpegTiles = &textureLogTiles;
        const size_t downloadsBeforeTiledLog = textureDownloads;
        motioncam::vfs::processDngPixels(
            tiledTextureLogDng, tiledTextureLogSettings, tiledTextureLogPixels);
        if (std::getenv("MOTIONCAM_REQUIRE_GPU_JPEG_TEST")) {
            assert(!textureLogTiles.bytes.empty());
            assert(textureDownloads == downloadsBeforeTiledLog);
        }
        tiledTextureFinalize.gpuJpegTiles = &textureLogTiles;
        motioncam::vfs::finalizeDng(
            tiledTextureLogDng, tiledTextureLogSettings, tiledTextureFinalize);
        motioncam::DecodedDNGImage tiledTextureLogDecoded;
        assert(motioncam::DNGDecoder::decodeImage(
            tiledTextureLogDng, tiledTextureLogDecoded));
        assert(tiledTextureLogDecoded.samples == referenceLogDecoded.samples);
        pl_tex_destroy(gpu, &texture);
        }
    }
#endif
    int largestDifference = 0;
    for (size_t i = 0; i < compressedGainImage.samples.size(); ++i)
        largestDifference = std::max(largestDifference,
            std::abs(int(compressedGainImage.samples[i]) -
                     int(uncompressedGainImage.samples[i])));
    assert(largestDifference <= 1);
    // These map transforms share one preparation path before CPU or Vulkan
    // baking. Compare the combined options on compressed input, where the
    // GPU also has to consume decoded pixels and produce packed DNG output.
    for (unsigned variant = 0; variant < 16; ++variant) {
        auto gpu = makeLogCfaDng(0, 0, false, 32, true);
        assert(motioncam::DNGDecoder::replaceGainMaps(gpu, 2, constantPhaseMaps));
        assert(motioncam::DNGDecoder::compressLosslessJPEG(gpu));
        auto cpu = gpu;
        motioncam::RenderSettings matrixSettings;
        matrixSettings.options = motioncam::RENDER_OPT_APPLY_VIGNETTE_CORRECTION;
        if (variant & 1u) matrixSettings.options |= motioncam::RENDER_OPT_VIGNETTE_ONLY_COLOR;
        if (variant & 2u) matrixSettings.options |= motioncam::RENDER_OPT_NORMALIZE_SHADING_MAP;
        if (variant & 4u) matrixSettings.options |= motioncam::RENDER_OPT_OPTIMIZE_GAIN_MAPS;
        if (variant & 8u) {
            matrixSettings.options |= motioncam::RENDER_OPT_LOG_TRANSFORM |
                                      motioncam::RENDER_OPT_LOG_HQ;
            matrixSettings.logTransform = motioncam::LogTransformMode::ReduceBy8Bit;
        }
        motioncam::vfs::processDngPixels(gpu, matrixSettings, compressedGainPipeline);
        motioncam::vfs::processDngPixels(cpu, matrixSettings, compressedCpuPipeline);
        assert(tagValue(gpu, 258).value == tagValue(cpu, 258).value);
        motioncam::DecodedDNGImage gpuPixels, cpuPixels;
        assert(motioncam::DNGDecoder::decodeImageBorrowed(gpu, gpuPixels, false, false));
        assert(motioncam::DNGDecoder::decodeImageBorrowed(cpu, cpuPixels, false, false));
        assert(gpuPixels.samples.size() == cpuPixels.samples.size());
        for (size_t sample = 0; sample < gpuPixels.samples.size(); ++sample)
            assert(std::abs(int(gpuPixels.samples[sample]) -
                            int(cpuPixels.samples[sample])) <= 1);
    }
    // The GPU strip path is optional on hosts without Vulkan. When present,
    // compare the packed codes and metadata against the CPU encoder for both
    // CFA and RGB sources across the supported output widths.
    size_t gpuLogCases = 0;
    for (const auto mode : packedLogModes) {
        for (const bool rgb : {false, true}) {
            auto gpu = rgb
                ? makeDng(1000, 0.01f, 100, 0.0f, {1, 1, 1}, 0)
                : makeLogCfaDng(0, 0, false, 32, true);
            auto cpu = gpu;
            if (!motioncam::DNGDecoder::bakeLogStripVulkan(
                    gpu, mode, 65535, true, 17)) continue;
            ++gpuLogCases;
            assert(motioncam::DNGDecoder::applyLogTransform(
                cpu, mode, 65535, true, false, 17));
            assert(motioncam::DNGDecoder::packUncompressedToWhiteLevel(cpu));
            assert(tagValue(gpu, 258).value == tagValue(cpu, 258).value);
            assert(tagValue(gpu, 50712).count == tagValue(cpu, 50712).count);
            motioncam::DecodedDNGImage gpuImage, cpuImage;
            assert(motioncam::DNGDecoder::decodeImageBorrowed(
                gpu, gpuImage, false, false));
            assert(motioncam::DNGDecoder::decodeImageBorrowed(
                cpu, cpuImage, false, false));
            assert(gpuImage.samples.size() == cpuImage.samples.size());
            for (size_t i = 0; i < gpuImage.samples.size(); ++i)
                assert(std::abs(int(gpuImage.samples[i]) -
                                int(cpuImage.samples[i])) <= 1);
        }
    }
    if (std::getenv("MOTIONCAM_GALLERY_PERF_PROFILE"))
        std::fprintf(stderr, "GPU LOG comparison cases: %zu\n", gpuLogCases);
    auto overlayPacked = makeLogCfaDng(500, 0, false, 128);
    assert(motioncam::DNGDecoder::replaceGainMaps(
        overlayPacked, 2, constantPhaseMaps));
    bool overlayLogFused = false;
    assert(motioncam::DNGDecoder::bakeGainMaps(
        overlayPacked, false, false, false, false, 2, std::nullopt,
        motioncam::LogTransformMode::ReduceBy6Bit, 65535, false,
        &overlayLogFused, true));
    assert(overlayLogFused);
    assert(motioncam::DNGDecoder::applyLogTransform(
        overlayPacked, motioncam::LogTransformMode::ReduceBy6Bit,
        65535, false, true));
    std::vector<std::pair<uint32_t, uint32_t>> isoForeground;
    std::pair<uint32_t, uint32_t> isoOutline{};
    bool hasOutline = false;
    motioncam::utils::forEachIsoOverlayPixel(128, 32, 100.0,
        [&](uint32_t x, uint32_t y, bool foreground) {
            if (foreground && isoForeground.size() < 2)
                isoForeground.emplace_back(x, y);
            if (!foreground && !hasOutline) {
                isoOutline = {x, y};
                hasOutline = true;
            }
        });
    assert(isoForeground.size() == 2 && hasOutline);
    std::vector<uint16_t> isoRgb(128 * 32 * 3, 1000);
    const auto [protectedX, protectedY] = isoForeground.front();
    const size_t protectedOffset = (static_cast<size_t>(protectedY) * 128 + protectedX) * 3;
    isoRgb[protectedOffset] = isoRgb[protectedOffset + 1] =
        isoRgb[protectedOffset + 2] = 0;
    motioncam::utils::bakeIsoOverlay(
        isoRgb.data(), 128, 32, 3, 100.0, 0, 65535);
    const auto [whiteX, whiteY] = isoForeground.back();
    const size_t whiteOffset = (static_cast<size_t>(whiteY) * 128 + whiteX) * 3;
    const size_t outlineOffset =
        (static_cast<size_t>(isoOutline.second) * 128 + isoOutline.first) * 3;
    assert(isoRgb[whiteOffset] == 65535 &&
           isoRgb[whiteOffset + 1] == 65535 &&
           isoRgb[whiteOffset + 2] == 65535);
    assert(isoRgb[outlineOffset] == 0 && isoRgb[protectedOffset] == 0);
    const std::vector<motioncam::utils::ActiveBadPixel> protectedPackedMark{
        {protectedY, protectedX}};
    assert(motioncam::DNGDecoder::markBadPixels(
        overlayPacked, 128, 32, 0, 0, protectedPackedMark, false));
    auto beforeOverlay = overlayPacked;
    assert(motioncam::DNGDecoder::bakeIsoOverlay(overlayPacked, 100.0));
    assert(tagValue(overlayPacked, 258).value == 10);
    assert(!motioncam::DNGDecoder::imagePayloadsEqual(beforeOverlay, overlayPacked));
    motioncam::DecodedDNGImage isoPackedImage;
    assert(motioncam::DNGDecoder::decodeImage(
        overlayPacked, isoPackedImage, false, true));
    assert(isoPackedImage.samples[static_cast<size_t>(whiteY) * 128 + whiteX] >= 65000);
    assert(isoPackedImage.samples[
        static_cast<size_t>(isoOutline.second) * 128 + isoOutline.first] == 0);
    assert(isoPackedImage.samples[
        static_cast<size_t>(protectedY) * 128 + protectedX] == 0);
    const std::vector<motioncam::utils::ActiveBadPixel> packedMarks{{5, 5}};
    assert(motioncam::DNGDecoder::markBadPixels(
        overlayPacked, 128, 32, 0, 0, packedMarks, false));
    motioncam::DecodedDNGImage markedPackedImage;
    assert(motioncam::DNGDecoder::decodeImage(
        overlayPacked, markedPackedImage, false, false));
    assert(markedPackedImage.samples[5 * 128 + 5] == 0);
    assert(tagValue(overlayPacked, 258).value == 10);
    assert(motioncam::DNGDecoder::packUncompressedToWhiteLevel(overlayPacked));
    assert(tagValue(overlayPacked, 258).value == 10);
    auto unpackedIsoLog = makeLogCfaDng(500, 0, false, 128);
    assert(motioncam::DNGDecoder::applyLogTransform(
        unpackedIsoLog, motioncam::LogTransformMode::ReduceBy6Bit, 65535));
    assert(tagValue(unpackedIsoLog, 258).value == 16);
    assert(tagValue(unpackedIsoLog, 50712).count == 1024);
    auto compressedIsoLog = unpackedIsoLog;
    assert(motioncam::DNGDecoder::compressLosslessJPEG(compressedIsoLog));
    assert(motioncam::DNGDecoder::bakeIsoOverlay(compressedIsoLog, 100.0));
    assert(tagValue(compressedIsoLog, 50712).count == 1024);
    assert(motioncam::DNGDecoder::packUncompressedToWhiteLevel(compressedIsoLog));
    assert(tagValue(compressedIsoLog, 258).value == 10);
    motioncam::vfs::DngFinalizeOptions isoLogFinalize;
    isoLogFinalize.writeTiming = false;
    isoLogFinalize.isoOverlay = 100.0;
    isoLogFinalize.packToWhiteLevel = true;
    motioncam::RenderSettings isoLogSettings;
    motioncam::vfs::finalizeDng(unpackedIsoLog, isoLogSettings, isoLogFinalize);
    assert(tagValue(unpackedIsoLog, 258).value == 10);
    assert(tagValue(unpackedIsoLog, 50712).count == 1024);
    auto markedRgbPacked = makeDng(
        1000, 0.01f, 100, 0.0f, {1.0f, 1.0f, 1.0f}, 0);
    assert(motioncam::DNGDecoder::applyLogTransform(
        markedRgbPacked, motioncam::LogTransformMode::ReduceBy6Bit, 65535));
    assert(motioncam::DNGDecoder::packUncompressedToWhiteLevel(markedRgbPacked));
    assert(tagValue(markedRgbPacked, 258).value == 10);
    assert(motioncam::DNGDecoder::markBadPixels(
        markedRgbPacked, 8, 8, 0, 0, packedMarks, true));
    motioncam::DecodedDNGImage markedRgbPackedImage;
    assert(motioncam::DNGDecoder::decodeImage(
        markedRgbPacked, markedRgbPackedImage, false, false));
    const size_t rgbMarkOffset = (5 * 8 + 5) * 3;
    assert(markedRgbPackedImage.samples[rgbMarkOffset] == 0 &&
           markedRgbPackedImage.samples[rgbMarkOffset + 1] == 0 &&
           markedRgbPackedImage.samples[rgbMarkOffset + 2] == 0);
    auto linearCfa = makeLogCfaDng(1023, 0, false);
    assert(tagValue(linearCfa, 50712).count == 0);
    motioncam::vfs::DngPixelPipelineOptions sourceDngPipeline;
    sourceDngPipeline.hasCfa = true;
    sourceDngPipeline.cfaRepeatSize = 2;
    motioncam::RenderSettings sourceKeepInput;
    sourceKeepInput.options = motioncam::RENDER_OPT_LOG_TRANSFORM;
    sourceKeepInput.logTransform = motioncam::LogTransformMode::KeepInput;
    auto keepExistingLog = makeLogCfaDng(1023, 0);
    motioncam::vfs::processDngPixels(
        keepExistingLog, sourceKeepInput, sourceDngPipeline);
    assert(tagValue(keepExistingLog, 50712).count == 1024);
    assert(motioncam::DNGDecoder::imagePayloadsEqual(
        keepExistingLog, makeLogCfaDng(1023, 0)));
    auto keepExistingProxyLog = makeLogCfaDng(1023, 0);
    sourceKeepInput.options = static_cast<motioncam::FileRenderOptions>(
        sourceKeepInput.options | motioncam::RENDER_OPT_DRAFT);
    sourceKeepInput.draftScale = 2;
    sourceDngPipeline.outputScale = 2;
    motioncam::vfs::processDngPixels(
        keepExistingProxyLog, sourceKeepInput, sourceDngPipeline);
    assert(tagValue(keepExistingProxyLog, 50712).count == 1024);
    motioncam::DecodedDNGImage keptProxyImage;
    assert(motioncam::DNGDecoder::decodeImage(
        keepExistingProxyLog, keptProxyImage, false, false));
    assert(keptProxyImage.layout.pixels == motioncam::DNGPixelLayout::CFA);
    assert(motioncam::DNGDecoder::replaceGainMaps(
        linearCfa, 2, constantPhaseMaps));
    motioncam::vfs::processDngPixels(
        linearCfa, keepInputSettings, keepInputPipeline);
    assert(tagValue(linearCfa, 50712).count > 0);
    badPixelCalibration.badPixels.front().action =
        motioncam::CalibrationData::BadPixelAction::Interpolate;
    auto opcodeCfa = makeLogCfaDng(1023, 0);
    assert(motioncam::DNGDecoder::setWarpFisheye(
        opcodeCfa, {0.01, 0.0, 0.0, 0.0}, 0.5, 0.5));
    const auto [existingOpcode3Offset, existingOpcode3Bytes] =
        tagPayload(opcodeCfa, 51022);
    assert(existingOpcode3Offset && existingOpcode3Bytes > 4);
    badPixelSettings.badPixelTreatment = motioncam::BadPixelTreatment::OpcodeOnly;
    motioncam::vfs::processDngPixels(opcodeCfa, badPixelSettings, cfaPipeline);
    assert(motioncam::DNGDecoder::imagePayloadsEqual(
        opcodeCfa, makeLogCfaDng(1023, 0)));
    const auto [badPixelOpcodeOffset, badPixelOpcodeBytes] =
        tagPayload(opcodeCfa, 51008);
    assert(badPixelOpcodeOffset && badPixelOpcodeBytes > 4);
    auto opcodeBe32 = [&](size_t offset) {
        return static_cast<uint32_t>(opcodeCfa[offset] << 24 |
            opcodeCfa[offset + 1] << 16 | opcodeCfa[offset + 2] << 8 |
            opcodeCfa[offset + 3]);
    };
    assert(opcodeBe32(badPixelOpcodeOffset) == 1);
    assert(opcodeBe32(badPixelOpcodeOffset + 4) == 5);
    auto optionsJsonCfa = makeLogCfaDng(1023, 0);
    badPixelSettings.badPixelTreatment = motioncam::BadPixelTreatment::OptionsJsonOnly;
    motioncam::vfs::processDngPixels(optionsJsonCfa, badPixelSettings, cfaPipeline);
    assert(motioncam::DNGDecoder::imagePayloadsEqual(
        optionsJsonCfa, makeLogCfaDng(1023, 0)));
    assert(tagPayload(optionsJsonCfa, 51008).second == 0);
    badPixelCalibration.badPixels.front().action =
        motioncam::CalibrationData::BadPixelAction::Dampen;
    auto resampledBakeCfa = makeLogCfaDng(1023, 0);
    badPixelSettings.vignetteCorrection = motioncam::VignetteCorrectionMode::Resample;
    badPixelSettings.badPixelTreatment = motioncam::BadPixelTreatment::OptionsJsonOnly;
    motioncam::vfs::processDngPixels(resampledBakeCfa, badPixelSettings, cfaPipeline);
    assert(!motioncam::DNGDecoder::imagePayloadsEqual(
        resampledBakeCfa, makeLogCfaDng(1023, 0)));
    assert(tagPayload(resampledBakeCfa, 51008).second == 0);
    auto resampledDisabledCfa = makeLogCfaDng(1023, 0);
    badPixelSettings.badPixelTreatment = motioncam::BadPixelTreatment::Disabled;
    motioncam::vfs::processDngPixels(resampledDisabledCfa, badPixelSettings, cfaPipeline);
    assert(motioncam::DNGDecoder::imagePayloadsEqual(
        resampledDisabledCfa, makeLogCfaDng(1023, 0)));
    badPixelCalibration.badPixels.front().action =
        motioncam::CalibrationData::BadPixelAction::Interpolate;
    badPixelSettings.vignetteCorrection = motioncam::VignetteCorrectionMode::Bake;
    badPixelSettings.badPixelTreatment = motioncam::BadPixelTreatment::OpcodeOnly;
    const auto [preservedOpcode3Offset, preservedOpcode3Bytes] =
        tagPayload(opcodeCfa, 51022);
    assert(preservedOpcode3Offset && preservedOpcode3Bytes == existingOpcode3Bytes);
    auto appendedOpcodeCfa = opcodeCfa;
    motioncam::vfs::processDngPixels(
        appendedOpcodeCfa, badPixelSettings, cfaPipeline);
    const auto [appendedOpcodeOffset, appendedOpcodeBytes] =
        tagPayload(appendedOpcodeCfa, 51008);
    auto appendedOpcodeBe32 = [&](size_t offset) {
        return static_cast<uint32_t>(appendedOpcodeCfa[offset] << 24 |
            appendedOpcodeCfa[offset + 1] << 16 |
            appendedOpcodeCfa[offset + 2] << 8 |
            appendedOpcodeCfa[offset + 3]);
    };
    assert(appendedOpcodeBytes > badPixelOpcodeBytes);
    assert(appendedOpcodeBe32(appendedOpcodeOffset) == 2);
    assert(appendedOpcodeBe32(appendedOpcodeOffset + 4) == 5);
    const size_t secondOpcode = appendedOpcodeOffset + 20 +
        appendedOpcodeBe32(appendedOpcodeOffset + 16);
    assert(appendedOpcodeBe32(secondOpcode) == 5);
    auto finalizedOpcodeCfa = opcodeCfa;
    motioncam::vfs::DngFinalizeOptions opcodeFinalize;
    opcodeFinalize.frameRate = 24.0f;
    opcodeFinalize.packToWhiteLevel = true;
    opcodeFinalize.compression = true;
    motioncam::vfs::finalizeDng(
        finalizedOpcodeCfa, badPixelSettings, opcodeFinalize);
    const auto [finalOpcodeOffset, finalOpcodeBytes] =
        tagPayload(finalizedOpcodeCfa, 51008);
    assert(finalOpcodeBytes > 20);
    auto finalOpcodeBe32 = [&](size_t offset) {
        return static_cast<uint32_t>(finalizedOpcodeCfa[offset] << 24 |
            finalizedOpcodeCfa[offset + 1] << 16 |
            finalizedOpcodeCfa[offset + 2] << 8 |
            finalizedOpcodeCfa[offset + 3]);
    };
    assert(finalOpcodeBe32(finalOpcodeOffset) == 1);
    assert(finalOpcodeBe32(finalOpcodeOffset + 4) == 5);
    motioncam::CalibrationData boundedPattern;
    boundedPattern.hasBadPixels = true;
    motioncam::CalibrationData::BadPixel lattice;
    lattice.x = 1;
    lattice.y = 1;
    lattice.repeatX = lattice.repeatY = 2;
    lattice.endX = lattice.endY = 3;
    boundedPattern.badPixels.push_back(lattice);
    std::vector<uint16_t> marked(8 * 8, 1000);
    const auto markedPixels = motioncam::utils::applyCfaBadPixels(
        marked.data(), 8, 8, 8, 8, 2, 1023.0f, {0, 0, 0, 0},
        100.0, 0.01, boundedPattern,
        motioncam::BadPixelTreatment::MarkPixels);
    assert(markedPixels.size() == 4);
    assert(marked[1 * 8 + 1] == 0 && marked[1 * 8 + 3] == 0 &&
           marked[3 * 8 + 1] == 0 && marked[3 * 8 + 3] == 0);
    assert(marked[5 * 8 + 5] == 1000);
    motioncam::CalibrationData dampenPattern;
    dampenPattern.hasBadPixels = true;
    motioncam::CalibrationData::BadPixel dampenedPixel;
    dampenedPixel.x = dampenedPixel.y = 1;
    dampenedPixel.action =
        motioncam::CalibrationData::BadPixelAction::Dampen;
    dampenedPixel.amount = 0.25f;
    dampenPattern.badPixels.push_back(dampenedPixel);
    std::vector<uint16_t> preDemosaicMarked(8 * 8, 1000);
    const auto preDemosaicMarkedPixels = motioncam::utils::applyCfaBadPixels(
        preDemosaicMarked.data(), 8, 8, 8, 8, 2, 1023.0f,
        {0, 0, 0, 0}, 100.0, 0.01, dampenPattern,
        motioncam::BadPixelTreatment::MarkPixels, true);
    assert(preDemosaicMarkedPixels.size() == 1);
    assert(preDemosaicMarked[1 * 8 + 1] == 750);
    motioncam::CalibrationData interpolateOne;
    interpolateOne.hasBadPixels = true;
    motioncam::CalibrationData::BadPixel interpolatedPixel;
    interpolatedPixel.x = interpolatedPixel.y = 1;
    interpolateOne.badPixels.push_back(interpolatedPixel);
    std::vector<uint16_t> quadInterpolation(8 * 8, 200);
    quadInterpolation[0] = 100;
    quadInterpolation[1] = 110;
    quadInterpolation[8] = 120;
    quadInterpolation[9] = 65535;
    motioncam::utils::applyCfaBadPixels(
        quadInterpolation.data(), 8, 8, 8, 8, 4, 65535.0f,
        {0, 0, 0, 0}, 100.0, 0.01, interpolateOne,
        motioncam::BadPixelTreatment::Bake);
    // The three valid samples in this 2x2 Quad Bayer group dominate the
    // surrounding same-colour groups: (median(100,110,120) * 4 + 200) / 5.
    assert(quadInterpolation[9] == 128);
    auto directBadPixelDng = makeLogCfaDng(0, 0, false, 32, true);
    auto decodedBadPixelDng = directBadPixelDng;
    motioncam::RenderSettings badPixelBakeSettings;
    badPixelBakeSettings.badPixelTreatment = motioncam::BadPixelTreatment::Bake;
    auto badPixelPipeline = cfaPipeline;
    badPixelPipeline.calibration = &interpolateOne;
    motioncam::vfs::processDngPixels(
        directBadPixelDng, badPixelBakeSettings, badPixelPipeline);
#ifdef _WIN32
    _putenv_s("MOTIONCAM_BAD_PIXEL_DIRECT_STRIP_DISABLE", "1");
#else
    setenv("MOTIONCAM_BAD_PIXEL_DIRECT_STRIP_DISABLE", "1", 1);
#endif
    motioncam::vfs::processDngPixels(
        decodedBadPixelDng, badPixelBakeSettings, badPixelPipeline);
#ifdef _WIN32
    _putenv_s("MOTIONCAM_BAD_PIXEL_DIRECT_STRIP_DISABLE", "");
#else
    unsetenv("MOTIONCAM_BAD_PIXEL_DIRECT_STRIP_DISABLE");
#endif
    assert(motioncam::DNGDecoder::imagePayloadsEqual(
        directBadPixelDng, decodedBadPixelDng));
    // Exercise the dense 2x2 median cache with an offset active area and
    // compare every output sample against the uncached interpolation path.
    motioncam::CalibrationData densePattern;
    densePattern.hasBadPixels = true;
    motioncam::CalibrationData::BadPixel denseDefect;
    denseDefect.x = denseDefect.y = 1;
    denseDefect.repeatX = denseDefect.repeatY = 2;
    densePattern.badPixels.push_back(denseDefect);
    constexpr uint32_t denseSize = 640;
    std::vector<uint16_t> denseCached(denseSize * denseSize);
    for (size_t i = 0; i < denseCached.size(); ++i)
        denseCached[i] = static_cast<uint16_t>((i * 31337u + i / denseSize * 97u) & 65535u);
    auto denseUncached = denseCached;
    motioncam::utils::applyCfaBadPixels(
        denseCached.data(), denseSize, denseSize, denseSize + 2, denseSize + 2,
        4, 65535.0f, {0, 0, 0, 0}, 100.0, 0.01, densePattern,
        motioncam::BadPixelTreatment::Bake);
#ifdef _WIN32
    _putenv_s("MOTIONCAM_BAD_PIXEL_CACHE_DISABLE", "1");
#else
    setenv("MOTIONCAM_BAD_PIXEL_CACHE_DISABLE", "1", 1);
#endif
    motioncam::utils::applyCfaBadPixels(
        denseUncached.data(), denseSize, denseSize, denseSize + 2, denseSize + 2,
        4, 65535.0f, {0, 0, 0, 0}, 100.0, 0.01, densePattern,
        motioncam::BadPixelTreatment::Bake);
#ifdef _WIN32
    _putenv_s("MOTIONCAM_BAD_PIXEL_CACHE_DISABLE", "");
#else
    unsetenv("MOTIONCAM_BAD_PIXEL_CACHE_DISABLE");
#endif
    assert(denseCached == denseUncached);
    std::vector<uint16_t> bayerInterpolation(8 * 8, 200);
    bayerInterpolation[9] = 65535;
    bayerInterpolation[1 * 8 + 3] = 400;
    bayerInterpolation[3 * 8 + 1] = 400;
    bayerInterpolation[3 * 8 + 3] = 400;
    motioncam::utils::applyCfaBadPixels(
        bayerInterpolation.data(), 8, 8, 8, 8, 2, 65535.0f,
        {0, 0, 0, 0}, 100.0, 0.01, interpolateOne,
        motioncam::BadPixelTreatment::Bake);
    assert(bayerInterpolation[9] == 400);
    assert(motioncam::vfs::projectedBadPixelOpcodeSize(
        boundedPattern, 8, 8) == 32 + 4 * 8);
    motioncam::CalibrationData fullSensorPattern;
    fullSensorPattern.hasBadPixels = true;
    motioncam::CalibrationData::BadPixel fullSensorPixel;
    fullSensorPixel.x = 5000;
    fullSensorPixel.y = 100;
    fullSensorPattern.badPixels.push_back(fullSensorPixel);
    assert(motioncam::vfs::projectedBadPixelOpcodeSize(
        fullSensorPattern, 4000, 3000) == 32);
    assert(motioncam::vfs::projectedBadPixelOpcodeSize(
        fullSensorPattern, 8000, 6000) == 32 + 8);
    motioncam::CalibrationData overflowingPattern;
    overflowingPattern.hasBadPixels = true;
    motioncam::CalibrationData::BadPixel overflowingLattice;
    overflowingLattice.repeatX = overflowingLattice.repeatY = 1;
    overflowingLattice.endX = overflowingLattice.endY =
        std::numeric_limits<int>::max();
    overflowingPattern.badPixels.push_back(overflowingLattice);
    assert(motioncam::vfs::projectedBadPixelOpcodeSize(
        overflowingPattern, 1, 1) == std::numeric_limits<size_t>::max());
    assert(motioncam::vfs::projectedDngSize(
        1, 1, 1, 16, std::numeric_limits<size_t>::max(), 1) ==
        std::numeric_limits<size_t>::max());
    assert(motioncam::vfs::projectedDngSize(100, 10, 1, 10, 0, 0) == 1250);
    assert(motioncam::vfs::projectedDngSize(1, 2, 3, 10, 0, 0) == 8);
    std::vector<uint16_t> markedRgb(8 * 8 * 3, 1000);
    motioncam::utils::markBadPixelsRgb(
        markedRgb.data(), 8, 8, 8, 8, 0, 0, markedPixels);
    const size_t markedRgbOffset = (1 * 8 + 1) * 3;
    assert(markedRgb[markedRgbOffset] == 0 &&
           markedRgb[markedRgbOffset + 1] == 0 &&
           markedRgb[markedRgbOffset + 2] == 0);
    auto streamingMarked = makeLogCfaDng(1023, 0);
    motioncam::RenderSettings streamingMarkSettings;
    streamingMarkSettings.badPixelTreatment =
        motioncam::BadPixelTreatment::MarkPixels;
    streamingMarkSettings.quadBayerOption = motioncam::QuadBayerMode::Demosaic;
    streamingMarkSettings.streamingPreview = true;
    auto quadPipeline = cfaPipeline;
    quadPipeline.cfaRepeatSize = 4;
    quadPipeline.calibration = &boundedPattern;
    motioncam::vfs::processDngPixels(
        streamingMarked, streamingMarkSettings, quadPipeline);
    motioncam::DecodedDNGImage streamingMarkedImage;
    assert(motioncam::DNGDecoder::decodeImage(
        streamingMarked, streamingMarkedImage, false, false));
    assert(streamingMarkedImage.layout.pixels == motioncam::DNGPixelLayout::CFA);
    assert(streamingMarkedImage.samples[1 * streamingMarkedImage.layout.width + 1] == 0);

    auto remosaicedMarked = makeLogCfaDng(1023, 0);
    motioncam::RenderSettings remosaicMarkSettings;
    remosaicMarkSettings.badPixelTreatment =
        motioncam::BadPixelTreatment::MarkPixels;
    remosaicMarkSettings.quadBayerOption = motioncam::QuadBayerMode::Demosaic;
    remosaicMarkSettings.options = static_cast<motioncam::FileRenderOptions>(
        motioncam::RENDER_OPT_REMOSAIC_TO_BAYER);
    motioncam::vfs::processDngPixels(
        remosaicedMarked, remosaicMarkSettings, quadPipeline);
    motioncam::DecodedDNGImage remosaicedMarkedImage;
    assert(motioncam::DNGDecoder::decodeImage(
        remosaicedMarked, remosaicedMarkedImage, false, false));
    assert(remosaicedMarkedImage.layout.pixels == motioncam::DNGPixelLayout::CFA);
    assert(remosaicedMarkedImage.samples[
        1 * remosaicedMarkedImage.layout.width + 1] == 0);
    // CPU higher-CFA remosaic now runs before the GPU gain stage. Its phase
    // maps are interpreted on the resulting ordinary Bayer grid.
    auto higherCfaInput = makeLogCfaDng(1000, 0, false, 32, true, 4);
    auto higherCfaMaps = constantPhaseMaps;
    for (size_t index = 0; index < higherCfaMaps.size(); ++index)
        higherCfaMaps[index].data = {1.0f + static_cast<float>(index) * 0.5f};
    assert(motioncam::DNGDecoder::replaceGainMaps(
        higherCfaInput, 2, higherCfaMaps));
    auto higherCfaReference = higherCfaInput;
    assert(motioncam::DNGDecoder::processHigherCFA(
        higherCfaReference, 4, {0, 1, 1, 2},
        motioncam::QuadBayerMode::Demosaic, true, 1, false,
        false, true));
    assert(motioncam::DNGDecoder::bakeGainMaps(
        higherCfaReference, false, false, false, false, 2));
    motioncam::RenderSettings phaseMapRemosaicSettings;
    phaseMapRemosaicSettings.options = static_cast<motioncam::FileRenderOptions>(
        motioncam::RENDER_OPT_APPLY_VIGNETTE_CORRECTION |
        motioncam::RENDER_OPT_REMOSAIC_TO_BAYER);
    phaseMapRemosaicSettings.quadBayerOption = motioncam::QuadBayerMode::Demosaic;
    auto higherCfaPipeline = cfaPipeline;
    higherCfaPipeline.cfaRepeatSize = 4;
    higherCfaPipeline.calibration = nullptr;
    higherCfaPipeline.preferVulkan = true;
    auto higherCfaActual = higherCfaInput;
    motioncam::vfs::processDngPixels(
        higherCfaActual, phaseMapRemosaicSettings, higherCfaPipeline);
    motioncam::DecodedDNGImage higherCfaExpectedImage, higherCfaActualImage;
    assert(motioncam::DNGDecoder::decodeImage(
        higherCfaReference, higherCfaExpectedImage, false, false));
    assert(motioncam::DNGDecoder::decodeImage(
        higherCfaActual, higherCfaActualImage, false, false));
    assert(higherCfaActualImage.samples == higherCfaExpectedImage.samples);
    int higherCfaOutputRepeat = 0;
    std::array<uint8_t, 4> higherCfaOutputPhase{};
    assert(motioncam::DNGDecoder::getCFAMetadata(
        higherCfaActual, higherCfaOutputRepeat, higherCfaOutputPhase));
    assert(higherCfaOutputRepeat == 2);
    auto higherCfaProxyReference = higherCfaInput;
    assert(motioncam::DNGDecoder::processHigherCFA(
        higherCfaProxyReference, 4, {0, 1, 1, 2},
        motioncam::QuadBayerMode::Demosaic, true, 2, false,
        false, true));
    assert(motioncam::DNGDecoder::bakeGainMaps(
        higherCfaProxyReference, false, false, false, false, 2));
    auto higherCfaProxyActual = higherCfaInput;
    auto higherCfaProxySettings = phaseMapRemosaicSettings;
    higherCfaProxySettings.options = static_cast<motioncam::FileRenderOptions>(
        higherCfaProxySettings.options | motioncam::RENDER_OPT_DRAFT);
    auto higherCfaProxyPipeline = higherCfaPipeline;
    higherCfaProxyPipeline.outputScale = 2;
    motioncam::vfs::processDngPixels(
        higherCfaProxyActual, higherCfaProxySettings,
        higherCfaProxyPipeline);
    motioncam::DecodedDNGImage higherCfaProxyExpectedImage,
                               higherCfaProxyActualImage;
    assert(motioncam::DNGDecoder::decodeImage(
        higherCfaProxyReference, higherCfaProxyExpectedImage, false, false));
    assert(motioncam::DNGDecoder::decodeImage(
        higherCfaProxyActual, higherCfaProxyActualImage, false, false));
    assert(higherCfaProxyActualImage.samples ==
           higherCfaProxyExpectedImage.samples);
    // RGB topology can crop and reduce/remosaic before consuming gain
    // metadata. Higher CFA phase maps are handled above on their source grid.
    auto orderedRgb = makeDng(
        1000, 0.01f, 100, 0.0f, {1.0f, 1.0f, 1.0f}, 0);
    motioncam::GainMap lumaMap{};
    lumaMap.top = lumaMap.left = 0;
    lumaMap.bottom = lumaMap.right = 8;
    lumaMap.coordinateWidth = lumaMap.coordinateHeight = 8;
    lumaMap.plane = 0;
    lumaMap.planes = 3;
    lumaMap.rowPitch = lumaMap.colPitch = 1;
    lumaMap.width = lumaMap.height = 2;
    lumaMap.channels = 1;
    lumaMap.spacingV = lumaMap.spacingH = 1.0;
    lumaMap.data.assign(4, 1.25f);
    assert(motioncam::DNGDecoder::replaceGainMaps(
        orderedRgb, 3, {lumaMap}));
    auto neutralGainDng = orderedRgb;
    auto neutralMap = lumaMap;
    neutralMap.data.assign(neutralMap.data.size(), 1.0f);
    assert(motioncam::DNGDecoder::replaceGainMaps(
        neutralGainDng, 2, {neutralMap}));
    assert(motioncam::DNGDecoder::replaceGainMaps(
        neutralGainDng, 3, {neutralMap}));
    motioncam::RenderSettings neutralPreviewSettings;
    neutralPreviewSettings.options = static_cast<motioncam::FileRenderOptions>(
        motioncam::RENDER_OPT_APPLY_VIGNETTE_CORRECTION |
        motioncam::RENDER_OPT_LOG_TRANSFORM);
    neutralPreviewSettings.logTransform = motioncam::LogTransformMode::KeepInput;
    motioncam::PreviewFrame neutralPreview;
    assert(motioncam::DNGDecoder::decodePreview(
        neutralGainDng, neutralPreviewSettings, neutralPreview, true));
    assert(!neutralPreview.gainMapApplied);
    auto noMapPreviewDng = neutralGainDng;
    assert(motioncam::DNGDecoder::replaceGainMaps(noMapPreviewDng, 2, {}));
    assert(motioncam::DNGDecoder::replaceGainMaps(noMapPreviewDng, 3, {}));
    motioncam::PreviewFrame noMapPreview;
    assert(motioncam::DNGDecoder::decodePreview(
        noMapPreviewDng, neutralPreviewSettings, noMapPreview, true));
    assert(neutralPreview.rgb == noMapPreview.rgb);
    motioncam::RenderSettings neutralSettings;
    motioncam::vfs::DngPixelPipelineOptions neutralPipeline;
    neutralPipeline.hasCfa = false;
    motioncam::vfs::processDngPixels(
        neutralGainDng, neutralSettings, neutralPipeline);
    std::vector<motioncam::GainMap> neutralMaps;
    assert(!motioncam::DNGDecoder::getGainMaps(
        neutralGainDng, 2, neutralMaps));
    assert(!motioncam::DNGDecoder::getGainMaps(
        neutralGainDng, 3, neutralMaps));
    std::vector<motioncam::GainMap> retainedMaps;
    assert(motioncam::DNGDecoder::getGainMaps(
        orderedRgb, 3, retainedMaps));
    assert(retainedMaps.size() == 1 && retainedMaps.front().data[0] == 1.25f);
    motioncam::RenderSettings orderedSettings;
    orderedSettings.options = static_cast<motioncam::FileRenderOptions>(
        motioncam::RENDER_OPT_CROPPING |
        motioncam::RENDER_OPT_DRAFT |
        motioncam::RENDER_OPT_REMOSAIC_TO_BAYER |
        motioncam::RENDER_OPT_APPLY_VIGNETTE_CORRECTION);
    orderedSettings.cropTarget = "4x4";
    orderedSettings.draftScale = 2;
    motioncam::vfs::DngPixelPipelineOptions orderedPipeline;
    orderedPipeline.hasCfa = false;
    orderedPipeline.outputScale = 2;
    motioncam::vfs::processDngPixels(
        orderedRgb, orderedSettings, orderedPipeline);
    motioncam::DNGImageLayout orderedLayout;
    assert(motioncam::DNGDecoder::getImageLayout(
        orderedRgb, orderedLayout));
    assert(orderedLayout.width == 2 && orderedLayout.height == 2);
    assert(orderedLayout.pixels == motioncam::DNGPixelLayout::CFA);
    std::vector<motioncam::GainMap> consumedOrderedMap;
    assert(!motioncam::DNGDecoder::getGainMaps(
        orderedRgb, 3, consumedOrderedMap));
    motioncam::DNGFrameMetadata logMetadata;
    const auto logDng = makeLogCfaDng(1023, 0);
    assert(motioncam::DNGDecoder::getColorMetadata(logDng, logMetadata));
    // LinearizationTable has 1024 inputs, so it takes precedence over the
    // 16-bit container declared by BitsPerSample.
    assert(logMetadata.inputBitDepth == 10);
    auto levelTaggedLog = logDng;
    motioncam::RenderSettings levelSettings;
    levelSettings.levels = "900/32";
    motioncam::vfs::DngPixelPipelineOptions levelPipeline;
    levelPipeline.hasCfa = true;
    motioncam::vfs::processDngPixels(
        levelTaggedLog, levelSettings, levelPipeline);
    assert(motioncam::DNGDecoder::imagePayloadsEqual(
        levelTaggedLog, logDng));
    assert(tagValue(levelTaggedLog, 50712).count == 1024);
    assert(tagValue(levelTaggedLog, 50717).value == 900);
    // DirectLog enters the shared pipeline as linear 16-bit RGB. KeepInput
    // therefore targets 12-bit log unless a <=10-bit white-level override
    // makes the effective input and output depth identical.
    auto directLogDefault = makeDng(
        1000, 0.01f, 100, 0.0f, {1.0f, 1.0f, 1.0f}, 0);
    const auto directLogLinear = directLogDefault;
    motioncam::RenderSettings directLogSettings;
    directLogSettings.options = motioncam::RENDER_OPT_LOG_TRANSFORM;
    directLogSettings.logTransform = motioncam::LogTransformMode::KeepInput;
    motioncam::vfs::DngPixelPipelineOptions directLogPipeline;
    directLogPipeline.hasCfa = false;
    directLogPipeline.inputQuantizationWhite = 4095;
    directLogPipeline.linearInputBitDepth = 16;
    motioncam::vfs::processDngPixels(
        directLogDefault, directLogSettings, directLogPipeline);
    assert(!motioncam::DNGDecoder::imagePayloadsEqual(
        directLogDefault, directLogLinear));
    motioncam::DNGFrameMetadata directLogMetadata;
    assert(motioncam::DNGDecoder::getColorMetadata(
        directLogDefault, directLogMetadata));
    assert(directLogMetadata.inputBitDepth == 12);

    auto matrixOverrideDng = directLogLinear;
    motioncam::DNGFrameMetadata matrixOverrides;
    matrixOverrides.colorMatrix1 = {1.1f, -0.1f, 0.0f,
                                    0.0f, 1.0f, 0.0f,
                                    0.0f, 0.0f, 0.9f};
    matrixOverrides.forwardMatrix2 = {0.9f, 0.1f, 0.0f,
                                      0.0f, 1.0f, 0.0f,
                                      0.0f, 0.1f, 0.9f};
    matrixOverrides.hasColorMatrix1 = true;
    matrixOverrides.hasForwardMatrix2 = true;
    matrixOverrides.calibrationIlluminant1 = 23;
    matrixOverrides.calibrationIlluminant2 = 17;
    assert(motioncam::DNGDecoder::updateColorMatrices(
        matrixOverrideDng, matrixOverrides));
    motioncam::DNGFrameMetadata insertedMatrices;
    assert(motioncam::DNGDecoder::getColorMetadata(
        matrixOverrideDng, insertedMatrices));
    assert(insertedMatrices.hasColorMatrix1 && insertedMatrices.hasForwardMatrix2);
    assert(std::abs(insertedMatrices.colorMatrix1[0] - 1.1f) < 0.0001f);
    assert(std::abs(insertedMatrices.forwardMatrix2[7] - 0.1f) < 0.0001f);
    assert(insertedMatrices.calibrationIlluminant1 == 23);
    assert(insertedMatrices.calibrationIlluminant2 == 17);
    assert(setTagType(matrixOverrideDng, 50721, 5));
    matrixOverrides.colorMatrix1[0] = 1.2f;
    matrixOverrides.hasForwardMatrix2 = false;
    assert(motioncam::DNGDecoder::updateColorMatrices(
        matrixOverrideDng, matrixOverrides));
    assert(motioncam::DNGDecoder::getColorMetadata(
        matrixOverrideDng, insertedMatrices));
    assert(insertedMatrices.hasColorMatrix1);
    assert(std::abs(insertedMatrices.colorMatrix1[0] - 1.2f) < 0.0001f);

    motioncam::vfs::ManualVignetteSidecars manualMetadataSidecar;
    motioncam::vfs::ManualVignetteSidecars::Candidate manualMetadataCandidate;
    manualMetadataCandidate.image.metadata.colorMatrix1 = matrixOverrides.colorMatrix1;
    manualMetadataCandidate.image.metadata.hasColorMatrix1 = true;
    manualMetadataCandidate.image.metadata.forwardMatrix2 = matrixOverrides.forwardMatrix2;
    manualMetadataCandidate.image.metadata.hasForwardMatrix2 = true;
    manualMetadataCandidate.image.metadata.asShotNeutral = {0.5f, 1.0f, 0.75f};
    manualMetadataCandidate.image.metadata.hasAsShotNeutral = true;
    manualMetadataCandidate.image.metadata.calibrationIlluminant1 = 23;
    manualMetadataCandidate.image.metadata.calibrationIlluminant2 = 17;
    manualMetadataSidecar.candidates.push_back(std::move(manualMetadataCandidate));
    assert(!motioncam::vfs::manualSidecarsNeedPixelProcessing(
        manualMetadataSidecar));
    auto spatialSidecar = manualMetadataSidecar;
    spatialSidecar.candidates.front().image.opcodeList2.push_back(
        motioncam::GainMap{});
    assert(motioncam::vfs::manualSidecarsNeedPixelProcessing(spatialSidecar));
    motioncam::DNGFrameMetadata previewMetadata;
    motioncam::vfs::mergeManualDngMetadata(
        previewMetadata, manualMetadataSidecar, nullptr);
    assert(previewMetadata.hasColorMatrix1 && previewMetadata.hasForwardMatrix2);
    assert(previewMetadata.hasAsShotNeutral);
    assert(std::abs(previewMetadata.asShotNeutral[2] - 0.75f) < 0.0001f);
    assert(previewMetadata.calibrationIlluminant1 == 23);
    assert(previewMetadata.calibrationIlluminant2 == 17);
    previewMetadata.colorMatrix1[0] = 3.0f;
    previewMetadata.hasColorMatrix1 = true;
    motioncam::vfs::mergeManualDngMetadata(
        previewMetadata, manualMetadataSidecar, nullptr);
    assert(std::abs(previewMetadata.colorMatrix1[0] - 3.0f) < 0.0001f);
    motioncam::CalibrationData previewCalibration;
    previewCalibration.hasColorMatrix1 = true;
    previewCalibration.colorMatrix1 = matrixOverrides.colorMatrix1;
    previewCalibration.colorMatrix1[0] = 4.0f;
    previewCalibration.hasAsShotNeutral = true;
    previewCalibration.asShotNeutral = {0.8f, 1.0f, 0.9f};
    motioncam::vfs::mergeManualDngMetadata(
        previewMetadata, manualMetadataSidecar, &previewCalibration);
    assert(std::abs(previewMetadata.colorMatrix1[0] - 4.0f) < 0.0001f);
    assert(previewMetadata.asShotNeutral == previewCalibration.asShotNeutral);
    motioncam::DNGFrameMetadata calibrationOnlyMetadata;
    motioncam::vfs::mergeManualDngMetadata(
        calibrationOnlyMetadata, {}, &previewCalibration);
    assert(calibrationOnlyMetadata.hasColorMatrix1 &&
           calibrationOnlyMetadata.hasAsShotNeutral);
    motioncam::DNGFrameMetadata preprocessedMcrawMetadata;
    preprocessedMcrawMetadata.asShotNeutral = {0.4f, 1.0f, 0.6f};
    preprocessedMcrawMetadata.hasAsShotNeutral = true;
    motioncam::vfs::mergeManualDngMetadata(
        preprocessedMcrawMetadata, {}, &previewCalibration, false);
    assert(preprocessedMcrawMetadata.asShotNeutral[0] == 0.4f);

    auto directLogTwelveBit = directLogLinear;
    directLogSettings.levels = "4095/Dynamic";
    directLogPipeline.linearInputBitDepth = 12;
    motioncam::vfs::processDngPixels(
        directLogTwelveBit, directLogSettings, directLogPipeline);
    assert(motioncam::DNGDecoder::imagePayloadsEqual(
        directLogTwelveBit, directLogLinear));
    auto directLogReducedOverride = directLogLinear;
    directLogSettings.levels = "1023/Dynamic";
    directLogSettings.logTransform = motioncam::LogTransformMode::ReduceBy2Bit;
    directLogPipeline.inputQuantizationWhite = 1023;
    directLogPipeline.linearInputBitDepth = 10;
    motioncam::vfs::processDngPixels(
        directLogReducedOverride, directLogSettings, directLogPipeline);
    assert(motioncam::DNGDecoder::getColorMetadata(
        directLogReducedOverride, directLogMetadata));
    assert(directLogMetadata.inputBitDepth == 8);
    auto compressedA = uncompressedA, compressedB = uncompressedB;
    assert(motioncam::DNGDecoder::compressLosslessJPEG(compressedA));
    assert(motioncam::DNGDecoder::compressLosslessJPEG(compressedB));
    assert(motioncam::DNGDecoder::imagePayloadsEqual(compressedA, compressedB));

    const auto root = fs::temp_directory_path() / "motioncam-rife-finalize-test";
    fs::remove_all(root);
    fs::create_directories(root / "rife");
    fs::create_directories(root / "calibration");
    std::ofstream(root / "calibration" / "profile.json") << "{}";
    const nlohmann::json referencedFiles{{"gyroflow", "calibration/profile.json"}};
    const boost::filesystem::path calibrationJson((root / "clip.json").string());
    const boost::filesystem::path conventional((root / "clip_gyroflow.json").string());
    assert(motioncam::vfs::referencedSidecarPath(
        conventional, referencedFiles, calibrationJson, "gyroflow") ==
        boost::filesystem::path((root / "calibration" / "profile.json").string()));
    std::ofstream(root / "clip_gyroflow.json") << "{}";
    assert(motioncam::vfs::referencedSidecarPath(
        conventional, referencedFiles, calibrationJson, "gyroflow") == conventional);
    const auto gyroflowPath = root / "clip_gyroflow.json";
    std::ofstream(gyroflowPath) << R"JSON({
      "calib_dimension":{"w":4000,"h":3008}, "asymmetrical":false,
      "fisheye_params":{"camera_matrix":[[2787.3322097799305,0,1989.3627594817033],
      [0,2789.180091093196,1489.6929103453012],[0,0,1]],
      "distortion_coeffs":[0.2527778305758484,0.9221266625169533,-1.8210242102121037,1.3375587148511772]}}
    )JSON";
    const auto gyroflow = motioncam::vfs::loadGyroflowLensProfile(
        boost::filesystem::path(gyroflowPath.string()));
    assert(gyroflow);
    assert(gyroflow->rectilinearRmsPixels < 0.8 &&
        gyroflow->rectilinearMaxPixels < 2.1);
    auto warped = uncompressedA;
    motioncam::vfs::applyGyroflowLensProfile(warped, *gyroflow);
    const auto [opcodeOffset, opcodeBytes] = tagPayload(warped, 51022);
    assert(opcodeOffset && opcodeBytes == 88);
    auto be32 = [&](size_t offset) { return static_cast<uint32_t>(warped[offset] << 24 |
        warped[offset + 1] << 16 | warped[offset + 2] << 8 | warped[offset + 3]); };
    auto beDouble = [&](size_t offset) {
        uint64_t bits = 0;
        for (size_t i = 0; i < 8; ++i) bits = bits << 8 | warped[offset + i];
        double value = 0.0;
        std::memcpy(&value, &bits, sizeof(value));
        return value;
    };
    assert(be32(opcodeOffset) == 1 && be32(opcodeOffset + 4) == 1);
    assert(be32(opcodeOffset + 8) == 0x01030000 && be32(opcodeOffset + 16) == 68);
    assert(be32(opcodeOffset + 20) == 1);
    const double croppedCx = gyroflow->cx - (gyroflow->width - 8) / 2;
    const double croppedCy = gyroflow->cy - (gyroflow->height - 8) / 2;
    auto cornerRadius = [](double x, double y, double w, double h) {
        return std::max({std::hypot(x, y), std::hypot(w - x, y),
            std::hypot(x, h - y), std::hypot(w - x, h - y)});
    };
    const double radiusRatio = cornerRadius(croppedCx, croppedCy, 7, 7) /
        cornerRadius(gyroflow->cx, gyroflow->cy,
            gyroflow->width - 1, gyroflow->height - 1);
    for (size_t i = 0; i < gyroflow->dngRectilinear.size(); ++i)
        assert(std::abs(beDouble(opcodeOffset + 24 + i * 8) -
            gyroflow->dngRectilinear[i] * std::pow(radiusRatio, 2 * i)) < 1e-15);
    assert(beDouble(opcodeOffset + 56) == 0.0);
    assert(beDouble(opcodeOffset + 64) == 0.0);
    assert(std::abs(beDouble(opcodeOffset + 72) - croppedCx / 7) < 1e-15);
    assert(std::abs(beDouble(opcodeOffset + 80) - croppedCy / 7) < 1e-15);
    auto sourceWarp = uncompressedA;
    assert(motioncam::DNGDecoder::setWarpRectilinear(
        sourceWarp, {1.0, 0.2, 0.03, 0.004}, 0.5, 0.5));
    std::array<std::vector<uint8_t>, 3> rectilinearSidecar;
    assert(motioncam::DNGDecoder::extractNonGainMapOpcodes(
        sourceWarp, rectilinearSidecar));
    auto croppedWarp = sourceWarp;
    assert(motioncam::DNGDecoder::cropImage(croppedWarp, 4, 4));
    const auto [cropWarpOffset, cropWarpBytes] = tagPayload(croppedWarp, 51022);
    assert(cropWarpOffset && cropWarpBytes == 88);
    auto cropDouble = [&](size_t at) {
        uint64_t bits = 0;
        for (size_t i = 0; i < 8; ++i) bits = bits << 8 | croppedWarp[at + i];
        double value;
        std::memcpy(&value, &bits, sizeof(value));
        return value;
    };
    assert(std::abs(cropDouble(cropWarpOffset + 72) - 0.5) < 1e-15);
    assert(std::abs(cropDouble(cropWarpOffset + 32) - 0.2 * 9.0 / 49.0) < 1e-15);
    auto packedCrop = makeLogCfaDng(400, 0);
    assert(motioncam::DNGDecoder::packUncompressedToWhiteLevel(packedCrop));
    assert(tagValue(packedCrop, 258).value == 10);
    auto packedPattern = makeLogCfaDng(0, 0, true, 18, true);
    assert(motioncam::DNGDecoder::packUncompressedToWhiteLevel(packedPattern));
    const auto packedPatternBytes = packedPattern;
    motioncam::DecodedDNGImage packedPatternImage;
    assert(motioncam::DNGDecoder::decodeImage(
        packedPattern, packedPatternImage, false, false));
    motioncam::DecodedDNGImage borrowedPackedPattern;
    assert(motioncam::DNGDecoder::decodeImageBorrowed(
        packedPattern, borrowedPackedPattern, false, false));
    assert(packedPattern == packedPatternBytes);
    assert(borrowedPackedPattern.samples == packedPatternImage.samples);
    auto omittedDefaults = makeLogCfaDng(0, 0, true, 18, true);
    assert(motioncam::DNGDecoder::removeMetadataTags(
        omittedDefaults, {259, 277, 278}));
    const auto omittedDefaultsBytes = omittedDefaults;
    motioncam::DecodedDNGImage omittedDefaultsImage;
    assert(motioncam::DNGDecoder::decodeImageBorrowed(
        omittedDefaults, omittedDefaultsImage, false, false));
    assert(omittedDefaults == omittedDefaultsBytes);
    assert(omittedDefaultsImage.samples == packedPatternImage.samples);
    for (uint32_t y = 0; y < 32; ++y)
        for (uint32_t x = 0; x < 18; ++x)
            assert(packedPatternImage.samples[static_cast<size_t>(y) * 18 + x] ==
                   ((y * 37 + x * 53) & 1023));
    assert(motioncam::DNGDecoder::cropImage(packedCrop, 16, 16));
    assert(tagValue(packedCrop, 258).value == 16);
    motioncam::DecodedDNGImage croppedSamples;
    assert(motioncam::DNGDecoder::decodeImage(
        packedCrop, croppedSamples, false, false));
    assert(croppedSamples.samples.size() == 16 * 16);
    assert(std::all_of(croppedSamples.samples.begin(), croppedSamples.samples.end(),
        [](uint16_t sample) { return sample == 400; }));
    auto mergedCropWarp = uncompressedA;
    assert(motioncam::DNGDecoder::cropImage(mergedCropWarp, 4, 4));
    assert(motioncam::DNGDecoder::mergeNonGainMapOpcodes(
        mergedCropWarp, rectilinearSidecar, 8, 8));
    const auto [mergedCropOffset, mergedCropBytes] = tagPayload(mergedCropWarp, 51022);
    assert(mergedCropOffset && mergedCropBytes == cropWarpBytes);
    for (size_t i = 24; i < 88; ++i)
        assert(mergedCropWarp[mergedCropOffset + i] == croppedWarp[cropWarpOffset + i]);
    auto fisheyeWarped = uncompressedA;
    assert(motioncam::DNGDecoder::setWarpFisheye(fisheyeWarped,
        {1.0, 0.1, -0.01, 0.001}, gyroflow->cx / (gyroflow->width - 1.0),
        gyroflow->cy / (gyroflow->height - 1.0)));
    const auto [fisheyeOffset, fisheyeBytes] = tagPayload(fisheyeWarped, 51022);
    assert(fisheyeOffset && fisheyeBytes == 72);
    auto fisheyeBe32 = [&](size_t offset) {
        return static_cast<uint32_t>(fisheyeWarped[offset] << 24 |
            fisheyeWarped[offset + 1] << 16 | fisheyeWarped[offset + 2] << 8 |
            fisheyeWarped[offset + 3]);
    };
    assert(fisheyeBe32(fisheyeOffset) == 1 && fisheyeBe32(fisheyeOffset + 4) == 2);
    assert(fisheyeBe32(fisheyeOffset + 16) == 52);
    std::array<std::vector<uint8_t>, 3> cachedSidecarOpcodes;
    assert(motioncam::DNGDecoder::extractNonGainMapOpcodes(
        fisheyeWarped, cachedSidecarOpcodes));
    motioncam::vfs::ManualVignetteSidecars mixedIlluminantSidecars;
    motioncam::vfs::ManualVignetteSidecars::Candidate d65White;
    d65White.whiteImage = true;
    d65White.illuminant = "d65";
    mixedIlluminantSidecars.candidates.push_back(std::move(d65White));
    motioncam::vfs::ManualVignetteSidecars::Candidate defaultOpcodes;
    defaultOpcodes.nonGainMapOpcodes = cachedSidecarOpcodes;
    mixedIlluminantSidecars.candidates.push_back(std::move(defaultOpcodes));
    auto mixedIlluminantOpcodes = uncompressedA;
    assert(motioncam::vfs::applyManualOpcodeSidecar(
        mixedIlluminantOpcodes, mixedIlluminantSidecars));
    const auto [mixedWarpOffset, mixedWarpBytes] =
        tagPayload(mixedIlluminantOpcodes, 51022);
    assert(mixedWarpOffset && mixedWarpBytes == fisheyeBytes);
    auto mergedOpcodes = opcodeCfa;
    assert(motioncam::DNGDecoder::mergeNonGainMapOpcodes(
        mergedOpcodes, cachedSidecarOpcodes));
    const auto [mergedList1Offset, mergedList1Bytes] =
        tagPayload(mergedOpcodes, 51008);
    assert(mergedList1Offset && mergedList1Bytes == badPixelOpcodeBytes);
    auto mergedBe32 = [&](size_t offset) {
        return static_cast<uint32_t>(mergedOpcodes[offset] << 24 |
            mergedOpcodes[offset + 1] << 16 | mergedOpcodes[offset + 2] << 8 |
            mergedOpcodes[offset + 3]);
    };
    const auto [mergedWarpOffset, mergedWarpBytes] = tagPayload(mergedOpcodes, 51022);
    assert(mergedWarpOffset && mergedWarpBytes == fisheyeBytes);
    assert(mergedBe32(mergedWarpOffset) == 1 && mergedBe32(mergedWarpOffset + 4) == 2);
    motioncam::vfs::applyGyroflowLensProfile(mergedOpcodes, *gyroflow);
    const auto [gyroOverrideOffset, gyroOverrideBytes] = tagPayload(mergedOpcodes, 51022);
    assert(gyroOverrideOffset && gyroOverrideBytes == 88);
    assert(mergedBe32(gyroOverrideOffset) == 1 &&
           mergedBe32(gyroOverrideOffset + 4) == 1);
    auto writeDng = [](const fs::path& path, const std::vector<uint8_t>& bytes) {
        std::ofstream output(path, std::ios::binary);
        output.write(reinterpret_cast<const char*>(bytes.data()),
                     static_cast<std::streamsize>(bytes.size()));
    };
    // A DCP is a TIFF profile. Spatial-only ProfileGainTableMap values can
    // become an OpcodeList2 gain map; profile and matrix fields remain usable
    // when the profile is selected from a sibling or JSON reference.
    auto profile = makeDng(0, 0.01f, 100, 0.0f, {1, 1, 1}, 0);
    motioncam::DNGFrameMetadata profileMatrices;
    profileMatrices.hasColorMatrix1 = true;
    profileMatrices.colorMatrix1 = {0.8f, 0.1f, 0.1f,
                                    0.1f, 0.8f, 0.1f,
                                    0.1f, 0.1f, 0.8f};
    assert(motioncam::DNGDecoder::updateColorMatrices(profile, profileMatrices));
    motioncam::DNGSidecarMetadataEntry profileName;
    profileName.tag = 50936; profileName.type = 2;
    profileName.value = {'T', 'e', 's', 't', ' ', 'D', 'C', 'P', 0};
    profileName.count = static_cast<uint32_t>(profileName.value.size());
    motioncam::DNGSidecarMetadataEntry gainTable;
    gainTable.tag = 52525; gainTable.type = 7;
    gainTable.value.resize(64 + 4 * 2 * 2);
    gainTable.count = static_cast<uint32_t>(gainTable.value.size());
    auto put32 = [&](size_t at, uint32_t value) {
        for (size_t i = 0; i < 4; ++i)
            gainTable.value[at + i] = static_cast<uint8_t>(value >> (8 * i));
    };
    auto putFloat = [&](size_t at, float value) {
        uint32_t bits; std::memcpy(&bits, &value, 4); put32(at, bits);
    };
    auto putDouble = [&](size_t at, double value) {
        uint64_t bits; std::memcpy(&bits, &value, 8);
        for (size_t i = 0; i < 8; ++i)
            gainTable.value[at + i] = static_cast<uint8_t>(bits >> (8 * i));
    };
    put32(0, 2); put32(4, 2);
    putDouble(8, 1.0); putDouble(16, 1.0);
    putDouble(24, 0.0); putDouble(32, 0.0);
    put32(40, 1);
    for (size_t i = 0; i < 4; ++i) putFloat(64 + i * 4, 1.0f + i * 0.25f);
    assert(motioncam::DNGDecoder::fillMissingSidecarMetadata(
        profile, {profileName, gainTable}));
    auto profileTag = [](uint16_t tag, uint16_t type,
                         std::initializer_list<uint32_t> values) {
        motioncam::DNGSidecarMetadataEntry entry;
        entry.tag = tag;
        entry.type = type;
        entry.count = static_cast<uint32_t>(values.size());
        for (const auto value : values)
            for (int byte = 0; byte < 4; ++byte)
                entry.value.push_back(static_cast<uint8_t>(value >> (byte * 8)));
        return entry;
    };
    auto profileFloatBits = [](float value) {
        uint32_t bits;
        std::memcpy(&bits, &value, sizeof(bits));
        return bits;
    };
    auto hueData = profileTag(50938, 11, {
        profileFloatBits(0), profileFloatBits(1), profileFloatBits(1),
        profileFloatBits(60), profileFloatBits(1), profileFloatBits(1),
        profileFloatBits(0), profileFloatBits(1), profileFloatBits(1),
        profileFloatBits(60), profileFloatBits(1), profileFloatBits(1)});
    auto lookData = hueData;
    lookData.tag = 50982;
    assert(motioncam::DNGDecoder::fillMissingSidecarMetadata(profile, {
        profileTag(50937, 4, {2, 2, 1}), hueData,
        profileTag(50981, 4, {2, 2, 1}), lookData,
        profileTag(50940, 11, {profileFloatBits(0), profileFloatBits(0),
                               profileFloatBits(1), profileFloatBits(1)})}));
    auto adobeDcp = profile;
    adobeDcp[2] = 'R'; adobeDcp[3] = 'C';
    writeDng(root / "profileclip.dcp", adobeDcp);
    auto selectedProfile = motioncam::vfs::loadManualVignetteSidecars(
        (root / "profileclip.dng").string());
    assert(selectedProfile.hasDcp && selectedProfile.dcpSpatialGainMap &&
           !selectedProfile.useDcpGainmap);
    assert(selectedProfile.dcpColor.profileTables &&
           selectedProfile.dcpColor.profileTables->hueSat1.values.size() == 12 &&
           selectedProfile.dcpColor.profileTables->look.values.size() == 12);
    motioncam::DNGFrameMetadata directDcpPreviewColor;
    motioncam::vfs::mergeManualDngMetadata(
        directDcpPreviewColor, selectedProfile, nullptr);
    assert(directDcpPreviewColor.profileTables &&
           directDcpPreviewColor.profileTables->hueSat1.values.size() == 12);
    assert(directDcpPreviewColor.hasColorMatrix1 &&
           std::abs(directDcpPreviewColor.colorMatrix1[0] - 0.8f) < 1e-4f);
    const nlohmann::json enableDcpGain{{"useDcpGainmap", true}};
    const boost::filesystem::path profileClipJson((root / "profileclip.json").string());
    auto activeProfile = motioncam::vfs::loadManualVignetteSidecars(
        (root / "profileclip.dng").string(), &enableDcpGain, &profileClipJson);
    assert(activeProfile.useDcpGainmap);
    auto profileOutput = makeDng(0, 0.01f, 100, 0.0f, {1, 1, 1}, 0);
    motioncam::GainMap staleMap = *activeProfile.dcpSpatialGainMap;
    staleMap.top = staleMap.left = 0; staleMap.bottom = staleMap.right = 8;
    staleMap.channels = staleMap.planes = 1;
    assert(motioncam::DNGDecoder::replaceGainMaps(profileOutput, 2, {staleMap}));
    assert(motioncam::vfs::applyManualVignetteSidecar(
        profileOutput, activeProfile));
    assert(motioncam::vfs::applyManualDngMetadata(
        profileOutput, activeProfile, nullptr));
    motioncam::DNGFrameMetadata profileOutputColor;
    assert(motioncam::DNGDecoder::getColorMetadata(
        profileOutput, profileOutputColor));
    assert(profileOutputColor.hasColorMatrix1 &&
           std::abs(profileOutputColor.colorMatrix1[0] - 0.8f) < 1e-4f);
    assert(profileOutputColor.profileTables &&
           profileOutputColor.profileTables->hueSat1.values.size() == 12 &&
           profileOutputColor.profileTables->look.values.size() == 12);
    assert(tagValue(profileOutput, 50940).count == 4);
    std::vector<motioncam::GainMap> profileOutputMaps;
    assert(!motioncam::DNGDecoder::getGainMaps(
        profileOutput, 2, profileOutputMaps));
    assert(profileOutputMaps.empty());
    assert(motioncam::DNGDecoder::getGainMaps(
        profileOutput, 3, profileOutputMaps));
    assert(profileOutputMaps.size() == 1 &&
           profileOutputMaps.front().channels == 1 &&
           profileOutputMaps.front().planes == 3);
    auto bakedProfile = profileOutput;
    assert(motioncam::DNGDecoder::bakeGainMaps(
        bakedProfile, false, false));
    profileOutputMaps.clear();
    assert(!motioncam::DNGDecoder::getGainMaps(
        bakedProfile, 3, profileOutputMaps));
    assert(tagValue(profileOutput, 50936).count == profileName.count);
    assert(tagValue(profileOutput, 52525).count == 0);
    auto optOutOutput = makeDng(0, 0.01f, 100, 0.0f, {1, 1, 1}, 0);
    assert(motioncam::vfs::applyManualVignetteSidecar(
        optOutOutput, selectedProfile));
    assert(motioncam::vfs::applyManualDngMetadata(
        optOutOutput, selectedProfile, nullptr));
    assert(tagValue(optOutOutput, 52525).count == gainTable.count);
    profileOutputMaps.clear();
    assert(!motioncam::DNGDecoder::getGainMaps(
        optOutOutput, 3, profileOutputMaps));
    assert(profileOutputMaps.empty());
    motioncam::CalibrationData explicitJson;
    explicitJson.hasColorMatrix1 = true;
    auto jsonOwnedOutput = makeDng(0, 0.01f, 100, 0.0f, {1, 1, 1}, 0);
    assert(motioncam::vfs::applyManualDngMetadata(
        jsonOwnedOutput, selectedProfile, &explicitJson));
    assert(tagValue(jsonOwnedOutput, 50721).count == 0);
    const nlohmann::json referencedProfile{{"dcp", "profileclip.dcp"}};
    const boost::filesystem::path profileJson((root / "profileclip.json").string());
    assert(motioncam::vfs::loadManualVignetteSidecars(
        (root / "otherclip.dng").string(), &referencedProfile,
        &profileJson).hasDcp);
    auto siblingProfile = profile;
    profileMatrices.colorMatrix1[0] = 0.6f;
    assert(motioncam::DNGDecoder::updateColorMatrices(
        siblingProfile, profileMatrices));
    writeDng(root / "otherclip.dcp", siblingProfile);
    const auto siblingSelected = motioncam::vfs::loadManualVignetteSidecars(
        (root / "otherclip.dng").string(), &referencedProfile, &profileJson);
    assert(siblingSelected.hasDcp &&
           std::abs(siblingSelected.dcpColor.colorMatrix1[0] - 0.6f) < 1e-4f);
    writeDng(root / "metadata_opcode.dng", profile);
    const auto metadataDngSidecar = motioncam::vfs::loadManualVignetteSidecars(
        (root / "metadata.dng").string());
    assert(metadataDngSidecar.candidates.size() == 1);
    auto metadataOutput = makeDng(0, 0.01f, 100, 0.0f, {1, 1, 1}, 0);
    assert(motioncam::vfs::applyManualDngMetadata(
        metadataOutput, metadataDngSidecar, nullptr));
    assert(tagValue(metadataOutput, 50936).count == profileName.count);
    assert(tagValue(metadataOutput, 52525).count == gainTable.count);
    writeDng(root / "legacy_gainmap.dng", fisheyeWarped);
    auto legacySidecar = motioncam::vfs::loadManualVignetteSidecars(
        (root / "legacy.dng").string());
    assert(legacySidecar.candidates.size() == 1);
    writeDng(root / "legacy_opcode.dng", warped);
    auto preferredOpcodeSidecar = motioncam::vfs::loadManualVignetteSidecars(
        (root / "legacy.dng").string());
    auto discoveredOpcodes = uncompressedA;
    assert(motioncam::vfs::applyManualOpcodeSidecar(
        discoveredOpcodes, preferredOpcodeSidecar));
    const auto [discoveredWarpOffset, discoveredWarpBytes] =
        tagPayload(discoveredOpcodes, 51022);
    assert(discoveredWarpOffset && discoveredWarpBytes == 88);
    auto discoveredBe32 = [&](size_t offset) {
        return static_cast<uint32_t>(discoveredOpcodes[offset] << 24 |
            discoveredOpcodes[offset + 1] << 16 |
            discoveredOpcodes[offset + 2] << 8 | discoveredOpcodes[offset + 3]);
    };
    assert(discoveredBe32(discoveredWarpOffset + 4) == 1);
    const nlohmann::json referencedOpcode{{"dng_opcode", "legacy_opcode.dng"}};
    const boost::filesystem::path referencedJson((root / "referenced.json").string());
    auto jsonOpcodeSidecar = motioncam::vfs::loadManualVignetteSidecars(
        (root / "referenced.dng").string(), &referencedOpcode, &referencedJson);
    assert(jsonOpcodeSidecar.candidates.size() == 1);
    std::ofstream(root / "rife" / "inference_img.py") << "# protocol double\n";
    FakeFileSystem filesystem(
        makeDng(0, 0.01f, 100, 0.0f, {1.0f, 2.0f, 4.0f}, 0),
        makeDng(65535, 0.04f, 400, 2.0f, {4.0f, 2.0f, 1.0f}, 2));
    motioncam::FinalizeOptions options;
    options.interpolateDuplicatedFrames = true;
    options.rifeDirectory = (root / "rife").string();
    options.rifePythonExecutable = RIFE_FAKE_EXECUTABLE;
    bool sawInterpolation = false, sawRebuild = false;
    std::vector<std::string> readyNames;
    std::vector<motioncam::Timestamp> readyTimestamps;
    size_t previous = 0;
    motioncam::vfs::finalize(filesystem, (root / "out").string(), false, options,
        [&](size_t completed, size_t total, const std::string& label) {
            assert(completed >= previous && completed <= total);
            previous = completed;
            sawInterpolation |= label.find("Interpolating") != std::string::npos;
            sawRebuild |= label.find("Rebuilt") != std::string::npos;
            return true;
        },
        [&](const std::vector<uint8_t>& readyDng, motioncam::Timestamp timestamp) {
            readyNames.push_back("frame-" + std::to_string(readyNames.size()));
            readyTimestamps.push_back(timestamp);
            if (readyNames.size() == 2) {
                const std::string synthetic = "rpt:SyntheticFrame='true'";
                assert(std::search(readyDng.begin(), readyDng.end(),
                    synthetic.begin(), synthetic.end()) != readyDng.end());
            }
        });
    assert(sawInterpolation && sawRebuild);
    assert((readyNames == std::vector<std::string>{"frame-0", "frame-1", "frame-2"}));
    assert((readyTimestamps == std::vector<motioncam::Timestamp>{0, 1, 2}));
    std::ifstream stream(root / "out" / "frame-000001.dng", std::ios::binary);
    std::vector<uint8_t> dng{std::istreambuf_iterator<char>(stream), {}};
    motioncam::PreviewFrame preview;
    assert(motioncam::DNGDecoder::decodePreview(
        dng, motioncam::RenderSettings{}, preview, false, true));
    assert(preview.rawSamples && preview.rawWidth > 0 && preview.rawHeight > 0);
    assert(motioncam::DNGDecoder::decodePreview(
        dng, motioncam::RenderSettings{}, preview, false, false));
    assert(!preview.rawSamples && preview.rawWidth == 0 &&
           preview.rawHeight == 0 && preview.rawChannels == 0);
    auto inactiveCrop = motioncam::RenderSettings{};
    inactiveCrop.cropTarget = "4x4_4";
    assert(motioncam::DNGDecoder::decodePreview(dng, inactiveCrop, preview, true));
    assert(preview.width == 8 && preview.height == 8);
    inactiveCrop.options |= motioncam::RENDER_OPT_CROPPING;
    assert(motioncam::DNGDecoder::decodePreview(dng, inactiveCrop, preview, true));
    assert(preview.width == 4 && preview.height == 4);
    assert(preview.rgb[0] == 0 && preview.rgb[1] == 128);
    // An inactive crop forces the existing two-pass DNG preview route. Its
    // pixels and clipping overlay must match the direct CFA output route.
    motioncam::DecodedDNGImage cfaPreviewImage;
    cfaPreviewImage.layout.width = 64;
    cfaPreviewImage.layout.height = 48;
    cfaPreviewImage.layout.bitsPerSample = 12;
    cfaPreviewImage.layout.samplesPerPixel = 1;
    cfaPreviewImage.layout.pixels = motioncam::DNGPixelLayout::CFA;
    cfaPreviewImage.layout.cfaRepeatSize = 2;
    cfaPreviewImage.layout.cfaPhase = {0, 1, 1, 2};
    cfaPreviewImage.metadata.blackLevel = {64.0f, 66.0f, 68.0f, 70.0f};
    cfaPreviewImage.metadata.blackLevelCount = 4;
    cfaPreviewImage.metadata.whiteLevel = {4095.0f, 4095.0f, 4095.0f, 4095.0f};
    cfaPreviewImage.metadata.whiteLevelCount = 4;
    cfaPreviewImage.samples.resize(64 * 48);
    for (size_t index = 0; index < cfaPreviewImage.samples.size(); ++index)
        cfaPreviewImage.samples[index] = static_cast<uint16_t>(
            index % 53 == 0 ? 4095 : index % 47 == 0 ? 32 : (index * 37) % 4096);
    auto gainPreviewImage = cfaPreviewImage;
    gainPreviewImage.opcodeList2 = constantPhaseMaps;
    motioncam::RenderSettings gainPreviewSettings;
    gainPreviewSettings.streamingPreview = true;
    gainPreviewSettings.options |= motioncam::RENDER_OPT_APPLY_VIGNETTE_CORRECTION;
    motioncam::PreviewFrame cpuGainPreview, gpuGainPreview;
#ifdef _WIN32
    _putenv_s("MOTIONCAM_GALLERY_FORCE_CPU", "1");
#else
    setenv("MOTIONCAM_GALLERY_FORCE_CPU", "1", 1);
#endif
    assert(motioncam::DNGDecoder::decodePreview(
        gainPreviewImage, gainPreviewSettings, cpuGainPreview, true));
#ifdef _WIN32
    _putenv_s("MOTIONCAM_GALLERY_FORCE_CPU", "");
#else
    unsetenv("MOTIONCAM_GALLERY_FORCE_CPU");
#endif
    assert(motioncam::DNGDecoder::decodePreview(
        gainPreviewImage, gainPreviewSettings, gpuGainPreview, true));
    assert(gpuGainPreview.galleryCfa.size() == cpuGainPreview.galleryCfa.size());
    auto gpuGainSamples = gpuGainPreview.galleryCfa;
    if (gpuGainPreview.galleryCfaGain)
        assert(gpuGainPreview.galleryCfaRecoverCpu(gpuGainSamples));
    for (size_t i = 0; i < gpuGainSamples.size(); ++i)
        assert(std::abs(int(gpuGainSamples[i]) -
                        int(cpuGainPreview.galleryCfa[i])) <= 1);
    if (motioncam::waitVulkanGalleryPreviewReady()) {
        motioncam::PreviewFrame deferredGainPreview;
        assert(motioncam::DNGDecoder::decodePreview(
            gainPreviewImage, gainPreviewSettings, deferredGainPreview, true));
        assert(deferredGainPreview.galleryCfaGain);
        assert(deferredGainPreview.galleryCfaRecoverCpu);
        motioncam::VulkanGalleryBayerSource deferredSource;
        deferredSource.samples = deferredGainPreview.galleryCfaSamples();
        deferredSource.width = deferredGainPreview.width;
        deferredSource.height = deferredGainPreview.height;
        deferredSource.phase = deferredGainPreview.galleryCfaPhase;
        deferredSource.black = deferredGainPreview.galleryBlack;
        deferredSource.white = deferredGainPreview.galleryWhite;
        deferredSource.gain = deferredGainPreview.galleryCfaGain.get();
        auto cpuSource = deferredSource;
        cpuSource.samples = cpuGainPreview.galleryCfaSamples();
        cpuSource.gain = nullptr;
        cpuSource.black = cpuGainPreview.galleryBlack;
        cpuSource.white = cpuGainPreview.galleryWhite;
        const std::array<float, 3> unity{1.0f, 1.0f, 1.0f};
        const std::array<float, 9> identity{1,0,0,0,1,0,0,0,1};
        std::vector<uint8_t> deferredColor, referenceColor;
        assert(motioncam::colorGalleryRgbVulkan(deferredColor, unity,
            identity, identity, 1.0f, nullptr, nullptr, false, false,
            &deferredSource));
        assert(motioncam::colorGalleryRgbVulkan(referenceColor, unity,
            identity, identity, 1.0f, nullptr, nullptr, false, false,
            &cpuSource));
        assert(deferredColor.size() == referenceColor.size());
        for (size_t i = 0; i < deferredColor.size() / 2; ++i) {
            uint16_t actual, expected;
            std::memcpy(&actual, deferredColor.data() + i * 2, 2);
            std::memcpy(&expected, referenceColor.data() + i * 2, 2);
            assert(std::abs(int(actual) - int(expected)) <= 8);
        }
        auto overrideImage = cfaPreviewImage;
        motioncam::GainMap rgbGrid{};
        rgbGrid.top = rgbGrid.left = 0;
        rgbGrid.bottom = overrideImage.layout.height;
        rgbGrid.right = overrideImage.layout.width;
        rgbGrid.rowPitch = rgbGrid.colPitch = 1;
        rgbGrid.width = rgbGrid.height = 2;
        rgbGrid.channels = 3;
        rgbGrid.originH = rgbGrid.originV = 0.1;
        rgbGrid.spacingH = rgbGrid.spacingV = 0.75;
        rgbGrid.data = {0.8f, 1.0f, 1.4f,
                        0.9f, 1.1f, 1.3f,
                        1.0f, 0.9f, 1.2f,
                        1.1f, 0.8f, 1.1f};
        overrideImage.opcodeList2 = {rgbGrid};
        auto overrideSettings = gainPreviewSettings;
        overrideSettings.cfaPhase = "BGGR";
        motioncam::PreviewFrame overrideGpu, overrideCpu;
        assert(motioncam::DNGDecoder::decodePreview(
            overrideImage, overrideSettings, overrideGpu, true));
        assert(overrideGpu.galleryCfaGain);
#ifdef _WIN32
        _putenv_s("MOTIONCAM_GALLERY_FORCE_CPU", "1");
#else
        setenv("MOTIONCAM_GALLERY_FORCE_CPU", "1", 1);
#endif
        assert(motioncam::DNGDecoder::decodePreview(
            overrideImage, overrideSettings, overrideCpu, true));
#ifdef _WIN32
        _putenv_s("MOTIONCAM_GALLERY_FORCE_CPU", "");
#else
        unsetenv("MOTIONCAM_GALLERY_FORCE_CPU");
#endif
        deferredSource.samples = overrideGpu.galleryCfaSamples();
        deferredSource.gain = overrideGpu.galleryCfaGain.get();
        cpuSource.samples = overrideCpu.galleryCfaSamples();
        deferredColor.clear();
        referenceColor.clear();
        assert(motioncam::colorGalleryRgbVulkan(deferredColor, unity,
            identity, identity, 1.0f, nullptr, nullptr, false, false,
            &deferredSource));
        assert(motioncam::colorGalleryRgbVulkan(referenceColor, unity,
            identity, identity, 1.0f, nullptr, nullptr, false, false,
            &cpuSource));
        assert(deferredColor.size() == referenceColor.size());
        for (size_t i = 0; i < deferredColor.size() / 2; ++i) {
            uint16_t actual, expected;
            std::memcpy(&actual, deferredColor.data() + i * 2, 2);
            std::memcpy(&expected, referenceColor.data() + i * 2, 2);
            assert(std::abs(int(actual) - int(expected)) <= 8);
        }
        auto logPreviewSettings = gainPreviewSettings;
        logPreviewSettings.options |= motioncam::RENDER_OPT_LOG_TRANSFORM |
                                      motioncam::RENDER_OPT_LOG_HQ;
        logPreviewSettings.logTransform = motioncam::LogTransformMode::ReduceBy8Bit;
        motioncam::PreviewFrame deferredLog, cpuLog;
        assert(motioncam::DNGDecoder::decodePreview(
            gainPreviewImage, logPreviewSettings, deferredLog, true));
        assert(deferredLog.galleryCfaGain &&
               deferredLog.galleryCfaGain->encodedWhite != 0);
#ifdef _WIN32
        _putenv_s("MOTIONCAM_GALLERY_FORCE_CPU", "1");
#else
        setenv("MOTIONCAM_GALLERY_FORCE_CPU", "1", 1);
#endif
        assert(motioncam::DNGDecoder::decodePreview(
            gainPreviewImage, logPreviewSettings, cpuLog, true));
#ifdef _WIN32
        _putenv_s("MOTIONCAM_GALLERY_FORCE_CPU", "");
#else
        unsetenv("MOTIONCAM_GALLERY_FORCE_CPU");
#endif
        auto recoveredLog = deferredLog.galleryCfa;
        assert(deferredLog.galleryCfaRecoverCpu(recoveredLog));
        assert(recoveredLog == cpuLog.galleryCfa);
        deferredSource.samples = deferredLog.galleryCfaSamples();
        deferredSource.gain = deferredLog.galleryCfaGain.get();
        cpuSource.samples = cpuLog.galleryCfaSamples();
        deferredColor.clear();
        referenceColor.clear();
        assert(motioncam::colorGalleryRgbVulkan(deferredColor, unity,
            identity, identity, 1.0f, nullptr, nullptr, false, false,
            &deferredSource));
        assert(motioncam::colorGalleryRgbVulkan(referenceColor, unity,
            identity, identity, 1.0f, nullptr, nullptr, false, false,
            &cpuSource));
        assert(deferredColor.size() == referenceColor.size());
        for (size_t i = 0; i < deferredColor.size() / 2; ++i) {
            uint16_t actual, expected;
            std::memcpy(&actual, deferredColor.data() + i * 2, 2);
            std::memcpy(&expected, referenceColor.data() + i * 2, 2);
            assert(std::abs(int(actual) - int(expected)) <= 8);
        }
    }
    motioncam::PreviewFrame directCfaPreview, twoPassCfaPreview;
    directCfaPreview.clippingRequested = true;
    twoPassCfaPreview.clippingRequested = true;
    motioncam::RenderSettings directSettings, twoPassSettings;
    twoPassSettings.options |= motioncam::RENDER_OPT_CROPPING;
    if (!motioncam::DNGDecoder::decodePreview(
            cfaPreviewImage, directSettings, directCfaPreview, true) ||
        !motioncam::DNGDecoder::decodePreview(
            std::move(cfaPreviewImage), twoPassSettings, twoPassCfaPreview, true) ||
        directCfaPreview.width != twoPassCfaPreview.width ||
        directCfaPreview.height != twoPassCfaPreview.height ||
        directCfaPreview.rgb != twoPassCfaPreview.rgb ||
        directCfaPreview.clipping != twoPassCfaPreview.clipping) return 2;
    // An explicit 8x8-to-4x4 reduction must still reach Bayer before the
    // non-HQ gallery demosaic; no higher-CFA nearest path remains.
    motioncam::DecodedDNGImage higherCfaPreview;
    higherCfaPreview.layout.width = 64;
    higherCfaPreview.layout.height = 64;
    higherCfaPreview.layout.bitsPerSample = 12;
    higherCfaPreview.layout.samplesPerPixel = 1;
    higherCfaPreview.layout.pixels = motioncam::DNGPixelLayout::CFA;
    higherCfaPreview.layout.cfaRepeatSize = 8;
    higherCfaPreview.layout.cfaPhase = {0, 1, 1, 2};
    higherCfaPreview.metadata.blackLevel = {64.0f, 64.0f, 64.0f, 64.0f};
    higherCfaPreview.metadata.blackLevelCount = 4;
    higherCfaPreview.metadata.whiteLevel = {4095.0f, 4095.0f, 4095.0f, 4095.0f};
    higherCfaPreview.metadata.whiteLevelCount = 4;
    higherCfaPreview.samples.resize(64 * 64);
    for (uint32_t y = 0; y < 64; ++y)
        for (uint32_t x = 0; x < 64; ++x)
            higherCfaPreview.samples[y * 64 + x] =
                std::array<uint16_t, 3>{1000, 2000, 3000}[
                    higherCfaPreview.layout.cfaPhase[(y / 4 % 2) * 2 + x / 4 % 2]];
    motioncam::RenderSettings higherCfaSettings;
    higherCfaSettings.quadBayerOption = motioncam::QuadBayerMode::Bin8x8To4x4;
    motioncam::PreviewFrame higherCfaResult;
    higherCfaResult.clippingRequested = true;
    if (!motioncam::DNGDecoder::decodePreview(
            std::move(higherCfaPreview), higherCfaSettings,
            higherCfaResult, true) ||
        higherCfaResult.width != 32 || higherCfaResult.height != 32 ||
        higherCfaResult.rgb.size() != 32 * 32 * 6 ||
        higherCfaResult.clipping.size() != 32 * 32) return 3;
    motioncam::DNGFrameMetadata metadata;
    assert(motioncam::DNGDecoder::getColorMetadata(dng, metadata));
    assert(std::abs(metadata.exposureTime - 0.02) < 1e-5);
    assert(std::abs(metadata.iso - 200.0) < 1.0);
    assert(std::abs(metadata.baselineExposure - 1.0) < 1e-5);
    for (const auto neutral : metadata.asShotNeutral) assert(std::abs(neutral - 2.0f) < 1e-4f);
    const std::string marker = "rpt:SyntheticFrame='true'";
    assert(std::search(dng.begin(), dng.end(), marker.begin(), marker.end()) != dng.end());
    assert(motioncam::DNGDecoder::isSyntheticFrame(dng));
    assert(!motioncam::DNGDecoder::isDuplicateFrame(dng));

    // Pixel duplicate detection must persist its result even when interpolation
    // is disabled. This also covers Camera Native, which consumes the same
    // finalized DNG stream and copies this marker into duplicateFrame JSON.
    FakeFileSystem detectedFilesystem(
        makeDng(42, 0.01f, 100, 0.0f, {1.0f, 1.0f, 1.0f}, 0),
        makeDng(84, 0.01f, 100, 0.0f, {1.0f, 1.0f, 1.0f}, 2), 1, false);
    motioncam::FinalizeOptions detectionOptions;
    detectionOptions.detectDuplicateDngs = true;
    size_t detectedFrames = 0;
    motioncam::vfs::finalize(detectedFilesystem, (root / "detected-out").string(),
        false, detectionOptions, {},
        [&](const std::vector<uint8_t>& detectedDng, motioncam::Timestamp timestamp) {
            assert(motioncam::DNGDecoder::isDuplicateFrame(detectedDng) == (timestamp == 1));
            ++detectedFrames;
        }, false);
    assert(detectedFrames == 3);
    assert(fs::is_empty(root / "detected-out"));

    FakeFileSystem cfaFilesystem(makeLogCfaDng(0, 0), makeLogCfaDng(1023, 2));
    motioncam::vfs::finalize(cfaFilesystem, (root / "cfa-out").string(), false, options, {});
    std::ifstream cfaStream(root / "cfa-out" / "frame-000001.dng", std::ios::binary);
    std::vector<uint8_t> cfa{std::istreambuf_iterator<char>(cfaStream), {}};
    int repeat = 0;
    std::array<uint8_t, 4> phase{};
    assert(motioncam::DNGDecoder::getCFAMetadata(cfa, repeat, phase) && repeat == 2);
    assert(std::search(cfa.begin(), cfa.end(), marker.begin(), marker.end()) != cfa.end());
    assert(tagValue(cfa, 50712).count == 1024); // LinearizationTable survived.
    assert(tagValue(cfa, 50717).value == 1023); // Original log code range survived.
    assert(tagValue(cfa, 258).value == 10); // Synthetic CFA is packed like ordinary output.
    assert(tagValue(cfa, 279).value == 32 * 32 * 10 / 8);

    FakeFileSystem streamingFilesystem(
        makeDng(0, 0.01f, 100, 0.0f, {1.0f, 2.0f, 4.0f}, 0),
        makeDng(65535, 0.04f, 400, 2.0f, {4.0f, 2.0f, 1.0f}, 2),
        1, true, true);
    size_t streamedFrames = 0;
    motioncam::vfs::finalize(streamingFilesystem, (root / "stream-out").string(),
        false, options, {},
        [&](const std::vector<uint8_t>& streamedDng, motioncam::Timestamp) {
            assert(!streamedDng.empty());
            ++streamedFrames;
        }, false);
    assert(streamedFrames == 3);
    assert(streamingFilesystem.ancillaryMaterializations() == 0);
    assert(fs::is_empty(root / "stream-out"));

    FakeFileSystem compressedStreamingFilesystem(
        makeDng(0, 0.01f, 100, 0.0f, {1.0f, 2.0f, 4.0f}, 0),
        makeDng(65535, 0.04f, 400, 2.0f, {4.0f, 2.0f, 1.0f}, 2));
    auto compressedOptions = options;
    compressedOptions.jxlDistance = 0.0f;
    size_t compressedFrames = 0;
    motioncam::vfs::finalize(compressedStreamingFilesystem,
        (root / "compressed-stream-out").string(), true, compressedOptions, {},
        [&](const std::vector<uint8_t>& streamedDng, motioncam::Timestamp) {
            assert(tagValue(streamedDng, 259).value == 52546);
            ++compressedFrames;
        }, false);
    assert(compressedFrames == 3);
    assert(fs::is_empty(root / "compressed-stream-out"));

    FakeFileSystem longGapFilesystem(
        makeDng(0, 0.01f, 100, 0.0f, {1.0f, 2.0f, 4.0f}, 0),
        makeDng(65535, 0.04f, 400, 2.0f, {4.0f, 2.0f, 1.0f}, 33), 32);
    bool interpolatedLongGap = false;
    size_t longGapFrames = 0;
    motioncam::vfs::finalize(longGapFilesystem, (root / "long-gap-out").string(),
        false, options,
        [&](size_t, size_t, const std::string& label) {
            interpolatedLongGap |= label.find("Interpolating") != std::string::npos;
            return true;
        },
        [&](const std::vector<uint8_t>& heldDng, motioncam::Timestamp) {
            assert(std::search(heldDng.begin(), heldDng.end(), marker.begin(), marker.end()) ==
                heldDng.end());
            ++longGapFrames;
        }, false);
    assert(!interpolatedLongGap);
    assert(longGapFrames == 34);

    bool rejectedMissingCallback = false;
    try {
        motioncam::vfs::finalize(streamingFilesystem,
            (root / "invalid-stream-out").string(), false, options, {}, {}, false);
    } catch (const std::invalid_argument&) {
        rejectedMissingCallback = true;
    }
    assert(rejectedMissingCallback);

    // Exposure analysis operates on effective exposure, so source baseline
    // metadata participates in normalization and smoothing instead of being
    // blindly copied to the output.
    const std::vector<motioncam::vfs::ExposureSample> exposureSamples{
        {0, 100.0, 0.01, 2.0, {1.0f, 1.0f, 1.0f}},
        {1, 200.0, 0.01, 0.0, {2.0f, 1.0f, 0.5f}}};
    const auto exposureAnalysis = motioncam::vfs::analyzeExposureMetadata(
        exposureSamples, 24.0f);
    // The second frame defines the minimum effective exposure. The first
    // therefore needs a replacement baseline of one stop.
    assert(std::abs(exposureAnalysis.normalizedBaseline.at(0) - 1.0f) < 1e-5f);
    assert(std::abs(exposureAnalysis.normalizedBaseline.at(1) - 0.0f) < 1e-5f);

    std::vector<motioncam::Entry> cadenceSources(2);
    cadenceSources[0].type = cadenceSources[1].type = motioncam::EntryType::FILE_ENTRY;
    cadenceSources[0].userData = 0;
    cadenceSources[1].userData = 2000000000LL;
    int cadenceDrops = 0, cadenceDuplicates = 0;
    const auto cadence = motioncam::vfs::mapFramesToCfr(cadenceSources,
        {0, 2000000000LL}, "frame-", 1.0f, true,
        cadenceDrops, cadenceDuplicates);
    assert(cadence.size() == 3 && cadenceDuplicates == 1 && cadenceDrops == 0);
    assert(cadence[1].duplicateFrame && std::get<int64_t>(cadence[1].userData) == 0);
    assert(motioncam::vfs::outputFrameNumber(cadence[2]) == 2);
    assert(motioncam::vfs::outputTimestamp(
        cadence[2], 2000000000LL, 0, 1.0f, true) == 2000000000LL);

    motioncam::RenderSettings proxySettings;
    proxySettings.options = static_cast<motioncam::FileRenderOptions>(
        motioncam::RENDER_OPT_DRAFT);
    proxySettings.draftScale = 4;
    const auto mountedPlan = motioncam::vfs::planDngRender(
        cadence[0], 0, 0, proxySettings, 24.0f, false, true);
    assert(mountedPlan.nativeMetadataFrame && mountedPlan.scale == 1);
    const auto finalizedPlan = motioncam::vfs::planDngRender(
        cadence[0], 0, 0, proxySettings, 24.0f, true, true);
    assert(!finalizedPlan.nativeMetadataFrame && finalizedPlan.scale == 4);
    const auto stillPlan = motioncam::vfs::planDngRender(
        cadence[0], 0, 0, proxySettings, 24.0f, false, false);
    assert(!stillPlan.nativeMetadataFrame && stillPlan.scale == 4);

    // Source identity must not depend on timestamps being unique. Cameras can
    // emit repeated or repaired timestamps without making either frame a drop.
    std::vector<motioncam::Entry> repeatedTimestampSources(2);
    repeatedTimestampSources[0].type=repeatedTimestampSources[1].type=
        motioncam::EntryType::FILE_ENTRY;
    repeatedTimestampSources[0].userData=repeatedTimestampSources[1].userData=42LL;
    int repeatedDrops=0,repeatedDuplicates=0;
    const auto repeatedMapped=motioncam::vfs::mapFramesToCfr(
        repeatedTimestampSources,{42LL,42LL},"repeat-",24.0f,false,
        repeatedDrops,repeatedDuplicates);
    std::shared_ptr<const std::vector<int>> repeatedOutputs;
    std::shared_ptr<const std::vector<bool>> repeatedFlags;
    motioncam::vfs::buildGalleryFrameMap({42LL,42LL},repeatedMapped,
        repeatedOutputs,repeatedFlags);
    assert(repeatedOutputs&&repeatedOutputs->size()==2);
    assert((*repeatedOutputs)[0]==0&&(*repeatedOutputs)[1]==1);
    assert(!(*repeatedFlags)[0]&&!(*repeatedFlags)[1]);

    motioncam::Entry mountedEntry;
    mountedEntry.type = motioncam::EntryType::FILE_ENTRY;
    mountedEntry.name = "frame-000001.dng";
    mountedEntry.userData = 1LL;
    motioncam::LRUCache cache(1024);
    int renders = 0;
    const auto render = [&] {
        ++renders;
        return std::make_shared<std::vector<uint8_t>>(
            std::initializer_list<uint8_t>{'a', 'b', 'c', 'd'});
    };
    assert(*motioncam::vfs::materializeCached(cache, mountedEntry, false, render) ==
        std::vector<uint8_t>({'a', 'b', 'c', 'd'}));
    assert(*motioncam::vfs::materializeCached(cache, mountedEntry, false, render) ==
        std::vector<uint8_t>({'a', 'b', 'c', 'd'}));
    assert(renders == 1);

    motioncam::LRUCache undersizedCache(2);
    int oversizedRenders = 0;
    const auto oversizedRender = [&] {
        ++oversizedRenders;
        return std::make_shared<std::vector<uint8_t>>(
            std::initializer_list<uint8_t>{'a', 'b', 'c', 'd'});
    };
    assert(motioncam::vfs::materializeCached(
        undersizedCache, mountedEntry, false, oversizedRender)->size() == 4);
    assert(motioncam::vfs::materializeCached(
        undersizedCache, mountedEntry, false, oversizedRender)->size() == 4);
    assert(oversizedRenders == 1);
    assert(undersizedCache.size() == 4);

    BS::thread_pool pool(1);
    char range[2]{};
    size_t callbackBytes = 0;
    int callbackError = -1;
    assert(motioncam::vfs::readMountedEntry(
        mountedEntry, 1, sizeof(range), range,
        [&](size_t bytes, int error) { callbackBytes = bytes; callbackError = error; },
        false, pool, render) == 2);
    assert(range[0] == 'b' && range[1] == 'c');
    assert(callbackBytes == 2 && callbackError == 0);

    std::vector<motioncam::Entry> desktopEntries;
    motioncam::vfs::appendDesktopIni(desktopEntries);
#ifdef _WIN32
    char desktopRange[8]{};
    assert(desktopEntries.size() == 1);
    assert(motioncam::vfs::readDesktopIni(
        desktopEntries.front(), 0, sizeof(desktopRange), desktopRange,
        [&](size_t bytes, int error) { callbackBytes = bytes; callbackError = error; }) == 0);
    assert(callbackBytes == 8 && callbackError == 0);
#else
    assert(desktopEntries.empty());
#endif

    fs::remove_all(root);
}
