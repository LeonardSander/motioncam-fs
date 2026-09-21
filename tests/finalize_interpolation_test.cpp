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

std::vector<uint8_t> makeLogCfaDng(uint16_t value, motioncam::Timestamp timestamp) {
    constexpr uint32_t width = 32, height = 32;
    std::vector<uint16_t> pixels(width * height, value);
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
    std::shared_ptr<std::vector<char>> materializeFile(const motioncam::Entry& entry, bool) override {
        if (entry.name == "audio.wav") {
            ++mAncillaryMaterializations;
            return std::make_shared<std::vector<char>>(
                std::initializer_list<char>{'R', 'I', 'F', 'F'});
        }
        const auto& source = std::get<int64_t>(entry.userData) == 0 ? mLeft : mRight;
        auto timed = source;
        const auto frame = static_cast<motioncam::Timestamp>(
            std::distance(mEntries.begin(), std::find_if(mEntries.begin(), mEntries.end(),
                [&](const auto& candidate) { return candidate.name == entry.name; })));
        assert(motioncam::DNGDecoder::setTimingMetadata(timed, 24.0, frame));
        return std::make_shared<std::vector<char>>(timed.begin(), timed.end());
    }
    bool materializePreviewFrame(const motioncam::Entry&, motioncam::PreviewFrame&) override {
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

    namespace fs = std::filesystem;
    const auto uncompressedA = makeDng(
        1234, 0.01f, 100, 0.0f, {1.0f, 1.0f, 1.0f}, 0);
    const auto uncompressedB = makeDng(
        1234, 0.01f, 100, 0.0f, {1.0f, 1.0f, 1.0f}, 1000000000);
    const auto uncompressedDifferent = makeDng(
        1235, 0.01f, 100, 0.0f, {1.0f, 1.0f, 1.0f}, 1000000000);
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
    auto processedPreviewDng = std::make_shared<std::vector<char>>(
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
    assert(gyroflow->fisheyeRmsPixels < gyroflow->rectilinearRmsPixels);
    assert(gyroflow->fisheyeRmsPixels < 0.6 && gyroflow->fisheyeMaxPixels < 2.1);
    auto warped = uncompressedA;
    motioncam::vfs::applyGyroflowLensProfile(warped, *gyroflow);
    const auto [opcodeOffset, opcodeBytes] = tagPayload(warped, 51022);
    assert(opcodeOffset && opcodeBytes == 72);
    auto be32 = [&](size_t offset) { return static_cast<uint32_t>(warped[offset] << 24 |
        warped[offset + 1] << 16 | warped[offset + 2] << 8 | warped[offset + 3]); };
    assert(be32(opcodeOffset) == 1 && be32(opcodeOffset + 4) == 2);
    assert(be32(opcodeOffset + 8) == 0x01030000 && be32(opcodeOffset + 16) == 52);
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
        dng, motioncam::RenderSettings{}, preview, false));
    assert(preview.rgb[0] == 0 && preview.rgb[1] == 128);
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
        return std::make_shared<std::vector<char>>(
            std::initializer_list<char>{'a', 'b', 'c', 'd'});
    };
    assert(*motioncam::vfs::materializeCached(cache, mountedEntry, false, render) ==
        std::vector<char>({'a', 'b', 'c', 'd'}));
    assert(*motioncam::vfs::materializeCached(cache, mountedEntry, false, render) ==
        std::vector<char>({'a', 'b', 'c', 'd'}));
    assert(renders == 1);

    motioncam::LRUCache undersizedCache(2);
    int oversizedRenders = 0;
    const auto oversizedRender = [&] {
        ++oversizedRenders;
        return std::make_shared<std::vector<char>>(
            std::initializer_list<char>{'a', 'b', 'c', 'd'});
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
