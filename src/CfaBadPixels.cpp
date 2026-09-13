#include "Utils.h"
#include "CalibrationData.h"

#include <algorithm>
#include <cmath>
#include <spdlog/spdlog.h>

namespace motioncam::utils {

std::vector<ActiveBadPixel> applyCfaBadPixels(
        uint16_t* samples, uint32_t width, uint32_t height,
        int originalWidth, int originalHeight, int cfaRepeatSize,
        float whiteLevel, const std::array<float, 4>& blackLevel,
        double iso, double exposureSeconds, const CalibrationData& calibration,
        BadPixelTreatment treatment, bool bakeForDemosaic) {
    std::vector<ActiveBadPixel> active;
    if (!samples || !width || !height || !calibration.hasBadPixels ||
        treatment == BadPixelTreatment::Disabled) return active;
    if (treatment == BadPixelTreatment::OpcodeOnly &&
        std::any_of(calibration.badPixels.begin(), calibration.badPixels.end(),
            [](const auto& pixel) {
                return pixel.action != CalibrationData::BadPixelAction::Interpolate;
            }))
        spdlog::warn("DNG bad-pixel opcodes only represent interpolation; brighten/dampen entries require a baked CFA stage");
    const std::vector<uint16_t> source(
        samples, samples + static_cast<size_t>(width) * height);
    const int group = std::max(1, cfaRepeatSize / 2);
    const int sensorLeft = std::max(0, (originalWidth - static_cast<int>(width)) / 2);
    const int sensorTop = std::max(0, (originalHeight - static_cast<int>(height)) / 2);
    auto phaseAt = [&](int x, int y) {
        return ((y / group) & 1) * 2 + ((x / group) & 1);
    };
    auto interpolate = [&](int x, int y) {
        std::vector<uint16_t> neighbours;
        const int phase = phaseAt(x + sensorLeft, y + sensorTop);
        for (int radius = 1; radius <= std::max(4, group * 2) && neighbours.size() < 4; ++radius)
            for (int dy = -radius; dy <= radius; ++dy)
                for (int dx = -radius; dx <= radius; ++dx) {
                    if (std::max(std::abs(dx), std::abs(dy)) != radius) continue;
                    const int nx = x + dx, ny = y + dy;
                    if (nx < 0 || ny < 0 || nx >= static_cast<int>(width) ||
                        ny >= static_cast<int>(height)) continue;
                    if (phaseAt(nx + sensorLeft, ny + sensorTop) == phase)
                        neighbours.push_back(source[static_cast<size_t>(ny) * width + nx]);
                }
        if (neighbours.empty()) return source[static_cast<size_t>(y) * width + x];
        const auto middle = neighbours.begin() + neighbours.size() / 2;
        std::nth_element(neighbours.begin(), middle, neighbours.end());
        return *middle;
    };
    for (const auto& defect : calibration.badPixels) {
        const int stepX = defect.repeatX ? defect.repeatX : static_cast<int>(width) + 1;
        const int stepY = defect.repeatY ? defect.repeatY : static_cast<int>(height) + 1;
        int startX = defect.x - sensorLeft;
        int startY = defect.y - sensorTop;
        if (defect.repeatX) while (startX < 0) startX += stepX;
        if (defect.repeatY) while (startY < 0) startY += stepY;
        for (int y = startY; y < static_cast<int>(height); y += stepY)
            for (int x = startX; x < static_cast<int>(width); x += stepX) {
                if (x < 0 || y < 0 || iso < defect.minIso ||
                    exposureSeconds < defect.minExposureSeconds) continue;
                const int phase = phaseAt(x + sensorLeft, y + sensorTop);
                const float black = blackLevel[phase];
                const float range = std::max(1.0f, whiteLevel - black);
                const size_t index = static_cast<size_t>(y) * width + x;
                const float normalized = std::clamp(
                    (source[index] - black) / range, 0.0f, 1.0f);
                if ((defect.thresholdAbove && normalized < *defect.thresholdAbove) ||
                    (defect.thresholdBelow && normalized > *defect.thresholdBelow)) continue;
                if (defect.action == CalibrationData::BadPixelAction::Interpolate)
                    active.push_back({static_cast<uint32_t>(y), static_cast<uint32_t>(x)});
                if (treatment != BadPixelTreatment::Bake && !bakeForDemosaic) continue;
                if (defect.action == CalibrationData::BadPixelAction::Interpolate)
                    samples[index] = interpolate(x, y);
                else {
                    const float factor = defect.action == CalibrationData::BadPixelAction::Brighten
                        ? 1.0f + defect.amount : 1.0f - defect.amount;
                    samples[index] = static_cast<uint16_t>(std::clamp(
                        std::lround(black + std::max(0.0f, source[index] - black) * factor),
                        0l, 65535l));
                }
            }
    }
    return active;
}

} // namespace motioncam::utils
