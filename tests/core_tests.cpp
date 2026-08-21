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
    assert(exampleCalibration.contains("_cfaSize"));
    assert(exampleCalibration.contains("_needGainMapOrderFixed"));
    assert(exampleCalibration.contains("_fullSensorResolution"));

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

    const auto higherCfaCalibration = CalibrationData::parse(std::string(R"({"cfaSize":8})"));
    assert(higherCfaCalibration.has_value());
    assert(higherCfaCalibration->hasCfaSize);
    assert(higherCfaCalibration->cfaSize == 8);

    const auto stringCfaCalibration = CalibrationData::parse(std::string(R"({"cfaSize":"4"})"));
    assert(stringCfaCalibration.has_value());
    assert(stringCfaCalibration->hasCfaSize);
    assert(stringCfaCalibration->cfaSize == 4);

    const auto invalidLevelsCalibration = CalibrationData::parse(std::string(R"({"dataLevels":"Video"})"));
    assert(!invalidLevelsCalibration.has_value());

    RenderSettings defaults;
    assert(defaults.cfaPhase == "Don't override CFA");
    assert(defaults.cfrTarget.mode == CFRMode::PreferDropFrame);
    assert(defaults.quadBayerOption == QuadBayerMode::Demosaic);
    assert(stringToQuadBayerMode("Correct QBCFA Metadata") == QuadBayerMode::CorrectQBCFAMetadata);

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

    auto customRate = stringToCFRTarget("48");
    assert(customRate.mode == CFRMode::Custom);
    assert(nearlyEqual(customRate.customValue, 48.0f));

    const auto whitespaceCalibration = CalibrationData::parse(std::string(R"({
        "colorMatrix1": [1.1 -0.2 0.1 0.0 1.0 0.0 0.2 -0.1 0.9],
        "forwardMatrix1": [0.9 0.1 0.0 0.0 1.0 0.0 0.1 0.2 0.7]
    })"));
    assert(whitespaceCalibration.has_value());
    assert(whitespaceCalibration->hasColorMatrix1);
    assert(whitespaceCalibration->hasForwardMatrix1);
    assert(nearlyEqual(whitespaceCalibration->colorMatrix1[1], -0.2f));
    assert(nearlyEqual(whitespaceCalibration->forwardMatrix1[8], 0.7f));

    return 0;
}
