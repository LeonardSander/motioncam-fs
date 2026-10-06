#define TINY_DNG_WRITER_IMPLEMENTATION
#include "tinydng/tiny_dng_writer.h"
#include "DNGDecoder.h"
#include "CalibrationData.h"
#include "LRUCache.h"
#include "VirtualFileSystemImpl.h"
#include "Utils.h"
#include <BS_thread_pool.hpp>

#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>

namespace {
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
                             motioncam::Timestamp timestamp) {
    constexpr uint32_t width = 8, height = 8;
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
                                   uint32_t width = 32, bool patterned = false) {
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
    const unsigned char pattern[4] = {0, 1, 1, 2};
    tinydngwriter::DNGImage image;
    image.SetBigEndian(false);
    assert(image.SetImageWidth(width) && image.SetImageLength(height));
    assert(image.SetRowsPerStrip(height) && image.SetSamplesPerPixel(1));
    assert(image.SetBitsPerSample(1, &bits));
    assert(image.SetCompression(tinydngwriter::COMPRESSION_NONE));
    assert(image.SetPhotometric(tinydngwriter::PHOTOMETRIC_CFA));
    assert(image.SetPlanarConfig(tinydngwriter::PLANARCONFIG_CONTIG));
    assert(image.SetCFARepeatPatternDim(2, 2) && image.SetCFAPattern(4, pattern));
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
    std::shared_ptr<std::vector<uint8_t>> materializeFile(const motioncam::Entry& entry, bool) override {
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

int main() {
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

    const std::array<float, 4> black{64.0f, 64.0f, 64.0f, 64.0f};
    assert(motioncam::vfs::getDisplayDataLevels(
        15408.0f, black, 15408.0f, black,
        "Dynamic", "", true, false, 16) == "15408/64 -> 65535/256 16b");
    const std::array<float, 4> zeroBlack{0.0f, 0.0f, 0.0f, 0.0f};
    assert(motioncam::vfs::getDisplayDataLevels(
        32000.0f, zeroBlack, 32000.0f, zeroBlack,
        "Dynamic", "", false, false) == "32000/0 16b");
    assert(motioncam::vfs::getDisplayDataLevels(
        32000.0f, zeroBlack, 32000.0f, zeroBlack,
        "Dynamic", "Reduce by 2bit", false, false) ==
        "32000/0 -> 16383/0 14b log");
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
    // The common topology pipeline must crop and reduce/remosaic before it
    // consumes gain metadata. This is the ordering shared by DNG, MCRAW and
    // DirectLog adapters.
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
    auto processedPreviewDng = std::make_shared<std::vector<uint8_t>>(
        orderedRgb.begin(), orderedRgb.end());
    motioncam::PreviewFrame processedPreview;
    assert(motioncam::vfs::decodeProcessedDngPreview(
        processedPreviewDng, processedPreview, true));
    assert(processedPreview.gainMapApplied);
    assert(motioncam::vfs::decodeProcessedDngPreview(
        processedPreviewDng, processedPreview, false));
    assert(!processedPreview.gainMapApplied);
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
    motioncam::DecodedDNGImage packedPatternImage;
    assert(motioncam::DNGDecoder::decodeImage(
        packedPattern, packedPatternImage, false, false));
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
