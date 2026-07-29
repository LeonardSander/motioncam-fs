#include "ExposureKeyframes.h"
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

    RenderSettings defaults;
    assert(defaults.cfaPhase == "Don't override CFA");
    assert(defaults.cfrTarget.mode == CFRMode::PreferDropFrame);

    assert(!ExposureKeyframes::parse("0ev").has_value());
    assert(!ExposureKeyframes::parse("garbage").has_value());

    auto keyframes = ExposureKeyframes::parse("start:-2, 0.5:0, end:2");
    assert(keyframes.has_value());
    assert(nearlyEqual(keyframes->getExposureAtFrame(0, 11), -2.0f));
    assert(nearlyEqual(keyframes->getExposureAtFrame(5, 11), 0.0f));
    assert(nearlyEqual(keyframes->getExposureAtFrame(10, 11), 2.0f));

    auto duplicates = ExposureKeyframes::parse("0:0, 0.5:1, 0.5:2, 1:4");
    assert(duplicates.has_value());
    assert(duplicates->getKeyframes().size() == 3);
    assert(nearlyEqual(duplicates->getExposureAt(0.5f), 2.0f));

    assert(!ExposureKeyframes::parse("0.5:1junk").has_value());
    assert(!ExposureKeyframes::parse("2:1").has_value());

    auto customRate = stringToCFRTarget("48");
    assert(customRate.mode == CFRMode::Custom);
    assert(nearlyEqual(customRate.customValue, 48.0f));

    return 0;
}
