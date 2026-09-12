#include "CalibrationData.h"
#include "Types.h"
#include "DataLevels.h"

#include <cassert>
#include <cmath>

namespace {

bool nearlyEqual(float lhs, float rhs) {
    return std::abs(lhs - rhs) < 0.0001f;
}

} // namespace

int main() {
    using namespace motioncam;

    const auto levelsCalibration = CalibrationData::parse(std::string(R"({"dataLevels":"full"})"));
    assert(levelsCalibration.has_value());
    assert(levelsCalibration->hasDataLevels);
    assert(levelsCalibration->dataLevels == "Full");

    const auto exampleCalibration = nlohmann::json::parse(
        CalibrationData::createExampleJson());
    assert(exampleCalibration.contains("_dataLevels"));
    assert(exampleCalibration.value("_levels", "") == "Dynamic");
    assert(exampleCalibration.value("_centerCrop", "") == "3840,2160");
    assert(exampleCalibration.value("_leftTopCropStride", "") == "4096x2304");
    assert(exampleCalibration.contains("_cfaSize"));
    assert(exampleCalibration.contains("_needGainMapOrderFixed"));
    assert(exampleCalibration.contains("_fullSensorResolution"));

    const auto renderOverrides = CalibrationData::parse(std::string(R"({
        "levels":"4095/64,65,66",
        "centerCrop":"3840,2160",
        "leftTopCropStride":"4096x2304"
    })"));
    assert(renderOverrides && renderOverrides->hasLevels &&
           renderOverrides->levels == "4095/64,65,66");
    assert(renderOverrides->hasCenterCrop &&
           (renderOverrides->centerCrop == std::array<int, 2>{3840, 2160}));
    assert(renderOverrides->hasLeftTopCropStride &&
           (renderOverrides->leftTopCropStride == std::array<int, 2>{4096, 2304}));

    const auto gainMapCalibration = CalibrationData::parse(
        std::string(R"({"needGainMapOrderFixed":true})"));
    assert(gainMapCalibration.has_value());
    assert(gainMapCalibration->hasNeedGainMapOrderFixed);
    assert(gainMapCalibration->needGainMapOrderFixed);

    const auto sensorCalibration = CalibrationData::parse(
        std::string(R"({"fullSensorResolution":[4000,3008]})"));
    assert(sensorCalibration.has_value());
    assert(sensorCalibration->hasFullSensorResolution);
    assert((sensorCalibration->fullSensorResolution == std::array<int, 2>{4000, 3008}));

    const auto cameraCalibration = CalibrationData::parse(std::string(R"({
        "cameraCalibration1":[1,0,0,0,0.9,0,0,0,1]
    })"));
    assert(cameraCalibration && cameraCalibration->hasCameraCalibration1);
    assert(nearlyEqual(cameraCalibration->cameraCalibration1[4], 0.9f));

    const auto higherCfaCalibration = CalibrationData::parse(std::string(R"({"cfaSize":8})"));
    assert(higherCfaCalibration.has_value());
    assert(higherCfaCalibration->hasCfaSize);
    assert(higherCfaCalibration->cfaSize == 8);

    const auto stringCfaCalibration = CalibrationData::parse(std::string(R"({"cfaSize":"4"})"));
    assert(stringCfaCalibration.has_value());
    assert(stringCfaCalibration->hasCfaSize);
    assert(stringCfaCalibration->cfaSize == 4);

    const auto badPixelCalibration = CalibrationData::parse(std::string(R"({
        "badPixels":[
            {"x":12,"y":34,"treatment":"dampen","amount":"20%","threshold":{"above":"50%"},"minIso":800,"minExposure":"1/30"},
            {"x":3,"y":5,"repeat":[16,16],"treatment":"interpolate","thresholdBelow":0.1}
        ]})"));
    assert(badPixelCalibration.has_value() && badPixelCalibration->hasBadPixels);
    assert(badPixelCalibration->badPixels.size() == 2);
    assert(nearlyEqual(badPixelCalibration->badPixels[0].amount, 0.2f));
    assert(nearlyEqual(*badPixelCalibration->badPixels[0].thresholdAbove, 0.5f));
    assert(std::abs(badPixelCalibration->badPixels[0].minExposureSeconds - 1.0 / 30.0) < 0.0001);
    assert(badPixelCalibration->badPixels[1].repeatX == 16);

    const auto invalidLevelsCalibration = CalibrationData::parse(std::string(R"({"dataLevels":"Video"})"));
    assert(!invalidLevelsCalibration.has_value());

    RenderSettings defaults;
    assert(defaults.cfaPhase == "Don't override CFA");
    assert(defaults.cfrTarget.mode == CFRMode::PreferInteger);
    assert(defaults.quadBayerOption == QuadBayerMode::Demosaic);
    assert(defaults.badPixelTreatment == BadPixelTreatment::Bake);
    assert(stringToQuadBayerMode("Correct QBCFA Metadata") == QuadBayerMode::CorrectQBCFAMetadata);
    assert(stringToQuadBayerMode("Demosaic (Color)") == QuadBayerMode::DemosaicColor);
    assert(quadBayerModeToString(QuadBayerMode::DemosaicColor) == "Demosaic (Color)");
    assert(stringToQuadBayerMode("Binning") == QuadBayerMode::Binning);
    assert(stringToQuadBayerMode("Bin 8x8 to 4x4") == QuadBayerMode::Bin8x8To4x4);
    assert(quadBayerModeToString(QuadBayerMode::Binning) == "Binning");

    const std::array<float, 4> dynamicBlack{60, 61, 62, 63};
    const std::array<float, 4> staticBlack{64, 65, 66, 67};
    auto mixed = resolveDataLevels("Static/Dynamic", 1000, dynamicBlack, 1023, staticBlack);
    assert(nearlyEqual(mixed.white, 1023));
    assert(mixed.black == dynamicBlack);
    mixed = resolveDataLevels("Dynamic/Static", 1000, dynamicBlack, 1023, staticBlack);
    assert(nearlyEqual(mixed.white, 1000));
    assert(mixed.black == staticBlack);
    mixed = resolveDataLevels("1023/Dynamic", 1000, dynamicBlack, 1023, staticBlack);
    assert(nearlyEqual(mixed.white, 1023));
    assert(mixed.black == dynamicBlack);
    mixed = resolveDataLevels("1023/Static", 1000, dynamicBlack, 1023, staticBlack);
    assert(nearlyEqual(mixed.white, 1023));
    assert(mixed.black == staticBlack);
    mixed = resolveDataLevels("Dynamic/64", 1000, dynamicBlack, 1023, staticBlack);
    assert(nearlyEqual(mixed.white, 1000));
    assert(mixed.black[0] == 64 && mixed.black[3] == 64);
    mixed = resolveDataLevels("Static/64", 1000, dynamicBlack, 1023, staticBlack);
    assert(nearlyEqual(mixed.white, 1023));
    assert((mixed.black == std::array<float, 4>{64, 64, 64, 64}));
    mixed = resolveDataLevels("4095/64,65,66", 1000, dynamicBlack, 1023, staticBlack, 3);
    assert((mixed.black == std::array<float, 4>{64, 65, 66, 65}));
    mixed = resolveDataLevels("4095/64,65,66", 1000, dynamicBlack, 1023, staticBlack, 4);
    assert(mixed.black == dynamicBlack);
    mixed = resolveDataLevels("4095/64", 1000, dynamicBlack, 1023, staticBlack, 3);
    assert((mixed.black == std::array<float, 4>{64, 64, 64, 64}));

    assert(stringToVignetteCorrectionMode("Bake") == VignetteCorrectionMode::Bake);
    assert(stringToVignetteCorrectionMode("Resample") == VignetteCorrectionMode::Resample);
    assert(stringToVignetteCorrectionMode("Uncropped") == VignetteCorrectionMode::Uncropped);
    assert(stringToVignetteCorrectionMode("Exclude") == VignetteCorrectionMode::Exclude);
    assert(vignetteCorrectionModeToString(VignetteCorrectionMode::Exclude) == "Exclude");

    auto customRate = stringToCFRTarget("48");
    assert(customRate.mode == CFRMode::Custom);
    assert(nearlyEqual(customRate.customValue, 48.0f));
    customRate = stringToCFRTarget("23.976");
    assert(customRate.mode == CFRMode::Custom);
    assert(nearlyEqual(customRate.customValue, 23.976f));
    customRate = stringToCFRTarget("23,976");
    assert(customRate.mode == CFRMode::Custom);
    assert(nearlyEqual(customRate.customValue, 23.976f));
    assert(stringToCFRTarget("23.976junk").mode == CFRMode::PreferInteger);

    const auto whitespaceCalibration = CalibrationData::parse(std::string(R"({
        "colorMatrix1": [1.1 -0.2 0.1 0.0 1.0 0.0 0.2 -0.1 0.9],
        "forwardMatrix1": [0.9 0.1 0.0 0.0 1.0 0.0 0.1 0.2 0.7]
    })"));
    assert(whitespaceCalibration.has_value());
    assert(whitespaceCalibration->hasColorMatrix1);
    assert(whitespaceCalibration->hasForwardMatrix1);
    assert(nearlyEqual(whitespaceCalibration->colorMatrix1[1], -0.2f));
    assert(nearlyEqual(whitespaceCalibration->forwardMatrix1[8], 0.7f));

    // A comment may mention a field before its actual key, as generated calibration
    // files do. Multiline whitespace-separated arrays must still be normalized.
    const auto generatedWhitespaceCalibration = CalibrationData::parse(std::string(R"({
        "_comment": "Remove _ in _colorMatrix1 to enable override.",
        "colorMatrix1": [
            0.9847999811 -0.3684000075 -0.1010999978
            -0.2682000101 1.129500031 0.115199998
            0.0588000007 0.04030000046 0.534799993
        ],
        "_forwardMatrix1": [
            0.4375 0.3828125 0.140625 0.21875 0.71875
            0.0625 0.015625 0.09375 0.7109375
        ],
        "_asShotNeutral": [0.4609375 1 0.61328125],
        "fullSensorResolution": [4096 3072]
    })"));
    assert(generatedWhitespaceCalibration.has_value());
    assert(generatedWhitespaceCalibration->hasColorMatrix1);
    assert(nearlyEqual(generatedWhitespaceCalibration->colorMatrix1[1], -0.3684000075f));
    assert(generatedWhitespaceCalibration->hasFullSensorResolution);
    assert((generatedWhitespaceCalibration->fullSensorResolution ==
        std::array<int, 2>{4096, 3072}));

    return 0;
}
