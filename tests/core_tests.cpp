#include "CalibrationData.h"
#include "Types.h"

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

    const auto exampleCalibration = CalibrationData::parse(CalibrationData::createExampleJson());
    assert(exampleCalibration.has_value());
    assert(exampleCalibration->hasDataLevels);
    assert(exampleCalibration->dataLevels == "Auto");
    assert(exampleCalibration->hasCfaSize);
    assert(exampleCalibration->cfaSize == 2);

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
