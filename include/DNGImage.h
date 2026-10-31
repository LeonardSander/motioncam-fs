#pragma once

#include "Types.h"

#include <array>
#include <cstdint>
#include <functional>
#include <memory>
#include <vector>

namespace motioncam {
struct VulkanGalleryGain;
struct VulkanGalleryBayerGain;
struct VulkanGalleryGpuBuffer;

struct DNGProfileTable {
    uint32_t hueDivisions = 0;
    uint32_t saturationDivisions = 0;
    uint32_t valueDivisions = 0;
    uint32_t encoding = 0;
    std::vector<float> values;
};

struct DNGProfileTables {
    DNGProfileTable hueSat1;
    DNGProfileTable hueSat2;
    DNGProfileTable look;
};

struct DNGFrameMetadata {
    size_t metadataBytes = 0;
    // Reuse the image layout parsed alongside the color metadata during mount.
    bool hasImageLayout = false;
    bool imageHasCfa = false;
    int imageCfaRepeatSize = 0;
    std::array<uint8_t, 4> imageCfaPhase{};
    std::string uniqueCameraModel;
    double exposureTime = 0.0;
    double iso = 0.0;
    double baselineExposure = 0.0;
    std::array<float, 3> asShotNeutral = {1.0f, 1.0f, 1.0f};
    std::array<float, 4> blackLevel{};
    std::array<float, 4> whiteLevel{};
    uint32_t blackLevelCount = 0;
    uint32_t whiteLevelCount = 0;
    uint32_t inputBitDepth = 0;
    uint32_t linearizationInputMax = 0;
    int orientation = -1;
    // Exact TIFF Orientation value (1..8). Unlike orientation, this retains
    // reflection and transpose information for Camera Native round-trips.
    uint16_t tiffOrientation = 0;
    std::array<float, 9> colorMatrix1{};
    std::array<float, 9> colorMatrix2{};
    std::array<float, 9> forwardMatrix1{};
    std::array<float, 9> forwardMatrix2{};
    std::array<float, 9> cameraCalibration1{};
    std::array<float, 9> cameraCalibration2{};
    uint16_t calibrationIlluminant1 = 0;
    uint16_t calibrationIlluminant2 = 0;
    bool hasExposure = false;
    bool hasBaselineExposure = false;
    bool hasAsShotNeutral = false;
    bool hasColorMatrix1 = false;
    bool hasColorMatrix2 = false;
    bool hasForwardMatrix1 = false;
    bool hasForwardMatrix2 = false;
    bool hasCameraCalibration1 = false;
    bool hasCameraCalibration2 = false;
    std::shared_ptr<const DNGProfileTables> profileTables;
};

struct DNGGpuImage {
    const void* gpu = nullptr;
    const void* buffer = nullptr;
    const void* texture = nullptr;
    uint32_t width = 0, height = 0, channels = 0;
    std::shared_ptr<void> lease;
    std::shared_ptr<void> deviceLease; // GPU ownership independent of decoder resets.
    std::function<bool(uint16_t*, size_t)> download;
    bool hasStorage() const { return buffer || texture; }
    bool downloadSamples(std::vector<uint16_t>& samples) const {
        if (!download) return false;
        samples.resize(size_t(width) * height * channels);
        return download(samples.data(), samples.size());
    }
    std::function<bool(std::vector<uint16_t>&)> vectorDownloader() const {
        if (!download) return {};
        return [source = *this](std::vector<uint16_t>& samples) {
            return source.downloadSamples(samples);
        };
    }
};

struct PreviewFrame {
    size_t sourceFrameIndex = 0;
    std::vector<uint8_t> rgb;
    std::shared_ptr<const uint8_t> galleryDisplayPixels;
    std::shared_ptr<VulkanGalleryGpuBuffer> galleryDisplayGpu;
    std::shared_ptr<void> galleryStateLease;
    // Non-HQ Bayer preview can remain CFA until the gallery's final GPU pass.
    // The CPU fallback reconstructs this when Vulkan is unavailable.
    std::vector<uint16_t> galleryCfa;
    DNGGpuImage gallerySource;
    // MCRAW preprocessing already owns its CFA as little-endian bytes. Move
    // that allocation here instead of copying a full frame to galleryCfa.
    std::vector<uint8_t> galleryCfaBytes;
    std::shared_ptr<VulkanGalleryBayerGain> galleryCfaGain;
    std::function<bool(std::vector<uint16_t>&)> galleryCfaRecoverCpu;
    bool hasGalleryCfa() const {
        return (gallerySource.channels == 1 && gallerySource.hasStorage()) ||
            !galleryCfa.empty() || !galleryCfaBytes.empty();
    }
    const uint16_t* galleryCfaSamples() const {
        return galleryCfa.empty()
            ? reinterpret_cast<const uint16_t*>(galleryCfaBytes.data())
            : galleryCfa.data();
    }
    std::array<uint8_t, 4> galleryCfaPhase{0, 1, 1, 2};
    std::array<double, 3> galleryBlack{}, galleryWhite{};
    // DirectLog can keep its decoded RGB texture until the gallery's final
    // display pass. The lease protects the decoder texture through callback.
    std::array<double, 3> galleryGpuBlack{}, galleryGpuWhite{};
    std::shared_ptr<VulkanGalleryGain> galleryGpuGain;
    uint32_t displayBitDepth = 16;
    bool clippingRequested = false;
    // 0 = normal, 1 = below black, 3..9 = clipped RGB channel bitmask + 2.
    std::vector<uint8_t> clipping;
    // Unmodified unpacked source samples captured before preview scaling,
    // gain-map application, demosaic, normalization, and colour transforms.
    std::shared_ptr<const std::vector<uint16_t>> rawSamples;
    uint32_t rawWidth = 0;
    uint32_t rawHeight = 0;
    uint32_t rawChannels = 0;
    float rawBlack = 0.0f;
    std::array<float, 4> rawBlackLevels{};
    float rawWhite = 65535.0f;
    int rawCfaSize = 2;
    std::array<uint8_t, 4> rawCfaPhase{0, 1, 1, 2};
    uint32_t width = 0;
    uint32_t height = 0;
    DNGFrameMetadata metadata;
    Timestamp timestamp = 0;
    bool gainMapApplied = false;
};

enum class DNGPixelLayout { CFA, LinearRGB, RGB };
enum class DNGStorageLayout { Strips, Tiles };

struct DNGImageLayout {
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t bitsPerSample = 0;
    uint32_t samplesPerPixel = 0;
    uint32_t compression = 0;
    uint32_t planarConfiguration = 1;
    DNGPixelLayout pixels = DNGPixelLayout::CFA;
    DNGStorageLayout storage = DNGStorageLayout::Strips;
    int cfaRepeatSize = 0;
    std::array<uint8_t, 4> cfaPhase{};
};

struct GainMap {
    uint32_t top, left, bottom, right;
    uint32_t coordinateWidth = 0, coordinateHeight = 0;
    uint32_t plane, planes;
    uint32_t rowPitch, colPitch;
    uint32_t width, height, channels;
    double spacingV, spacingH, originV, originH;
    std::vector<float> data;
};

// Compact TIFF value extracted from a DNG sidecar. Values use little-endian
// element encoding so the source file itself need not remain resident.
struct DNGSidecarMetadataEntry {
    uint16_t tag = 0;
    uint16_t type = 0;
    uint32_t count = 0;
    bool exif = false;
    bool gps = false;
    std::vector<uint8_t> value;
};

// Storage-independent, unpacked image samples shared by DNG decoding,
// source processing, preview rendering, and DNG serialization.
struct DecodedDNGImage {
    std::shared_ptr<DNGGpuImage> gpuImage;
    // Applied by the gallery shader when samples remain resident.
    std::vector<uint16_t> residentLinearization;
    std::vector<uint16_t> samples;
    DNGImageLayout layout;
    uint32_t storedBitsPerSample = 0;
    DNGFrameMetadata metadata;
    std::vector<GainMap> opcodeList2;
    std::vector<GainMap> opcodeList3;
    Timestamp timestamp = 0;
    uint32_t ditherSeed = 0;
    bool linearizationApplied = false;
};

} // namespace motioncam
