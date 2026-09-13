#define TINY_DNG_WRITER_IMPLEMENTATION
#include "tinydng/tiny_dng_writer.h"
#include "DNGDecoder.h"
#include "CalibrationData.h"
#include "LRUCache.h"
#include "VirtualFileSystemImpl.h"
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
    badPixelCalibration.badPixels.front().action =
        motioncam::CalibrationData::BadPixelAction::Interpolate;
    auto opcodeCfa = makeLogCfaDng(1023, 0);
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
