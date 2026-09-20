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
    struct EligiblePixel {
        int x, y;
        const CalibrationData::BadPixel* defect;
    };
    std::vector<EligiblePixel> eligible;
    const size_t sampleCount = static_cast<size_t>(width) * height;
    std::vector<uint64_t> interpolationMask((sampleCount + 63) / 64, 0);
    auto isInterpolationDefect = [&](size_t index) {
        return interpolationMask[index / 64] & (uint64_t{1} << (index % 64));
    };
    for (const auto& defect : calibration.badPixels) {
        const int stepX = defect.repeatX ? defect.repeatX : static_cast<int>(width) + 1;
        const int stepY = defect.repeatY ? defect.repeatY : static_cast<int>(height) + 1;
        int startX = defect.x - sensorLeft;
        int startY = defect.y - sensorTop;
        if (defect.repeatX) while (startX < 0) startX += stepX;
        if (defect.repeatY) while (startY < 0) startY += stepY;
        const int endX = defect.endX
            ? *defect.endX - sensorLeft : static_cast<int>(width) - 1;
        const int endY = defect.endY
            ? *defect.endY - sensorTop : static_cast<int>(height) - 1;
        for (int y = startY; y < static_cast<int>(height) && y <= endY; y += stepY)
            for (int x = startX; x < static_cast<int>(width) && x <= endX; x += stepX) {
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
                eligible.push_back({x, y, &defect});
                if (defect.action == CalibrationData::BadPixelAction::Interpolate)
                    interpolationMask[index / 64] |= uint64_t{1} << (index % 64);
                if (defect.action == CalibrationData::BadPixelAction::Interpolate ||
                    treatment == BadPixelTreatment::MarkPixels)
                    active.push_back({static_cast<uint32_t>(y), static_cast<uint32_t>(x)});
            }
    }

    auto median = [](std::vector<uint16_t>& values) -> uint16_t {
        if (values.empty()) return 0;
        const size_t middle = values.size() / 2;
        std::nth_element(values.begin(), values.begin() + middle, values.end());
        if (values.size() & 1) return values[middle];
        const uint16_t upper = values[middle];
        const uint16_t lower = *std::max_element(values.begin(), values.begin() + middle);
        return static_cast<uint16_t>((static_cast<uint32_t>(lower) + upper + 1) / 2);
    };
    std::vector<uint16_t> blockValues;
    blockValues.reserve(static_cast<size_t>(group) * group);
    auto interpolate = [&](int x, int y) {
        const int absoluteX = x + sensorLeft;
        const int absoluteY = y + sensorTop;
        const int blockX = (absoluteX / group) * group;
        const int blockY = (absoluteY / group) * group;
        auto blockMedian = [&](int absoluteBlockX, int absoluteBlockY,
                               uint16_t& value) {
            blockValues.clear();
            for (int ay = absoluteBlockY; ay < absoluteBlockY + group; ++ay)
                for (int ax = absoluteBlockX; ax < absoluteBlockX + group; ++ax) {
                    const int sx = ax - sensorLeft, sy = ay - sensorTop;
                    if (sx < 0 || sy < 0 || sx >= static_cast<int>(width) ||
                        sy >= static_cast<int>(height)) continue;
                    const size_t sample = static_cast<size_t>(sy) * width + sx;
                    if (!isInterpolationDefect(sample))
                        blockValues.push_back(source[sample]);
                }
            if (blockValues.empty()) return false;
            value = median(blockValues);
            return true;
        };

        // A higher-CFA colour group contains multiple measurements of the
        // same colour at effectively the same location. Prefer those strongly.
        uint16_t local = 0;
        const bool hasLocal = blockMedian(blockX, blockY, local);

        // Stabilize the local estimate with the nearest blocks of the same
        // Bayer phase. For ordinary 2x2 Bayer, group is one and this becomes
        // the complete reconstruction source.
        std::array<uint16_t, 8> neighbouringBlocks{};
        size_t neighbouringCount = 0;
        for (int blockDy = -2; blockDy <= 2; blockDy += 2)
            for (int blockDx = -2; blockDx <= 2; blockDx += 2) {
                if (!blockDx && !blockDy) continue;
                uint16_t value = 0;
                if (blockMedian(blockX + blockDx * group,
                                blockY + blockDy * group, value))
                    neighbouringBlocks[neighbouringCount++] = value;
            }
        if (!neighbouringCount)
            return hasLocal ? local : source[static_cast<size_t>(y) * width + x];
        const size_t middle = neighbouringCount / 2;
        std::nth_element(neighbouringBlocks.begin(),
                         neighbouringBlocks.begin() + middle,
                         neighbouringBlocks.begin() + neighbouringCount);
        uint16_t nearby = neighbouringBlocks[middle];
        if (!(neighbouringCount & 1)) {
            const uint16_t lower = *std::max_element(
                neighbouringBlocks.begin(), neighbouringBlocks.begin() + middle);
            nearby = static_cast<uint16_t>(
                (static_cast<uint32_t>(lower) + nearby + 1) / 2);
        }
        if (!hasLocal) return nearby;
        // Four parts intra-group signal to one part surrounding context keeps
        // Quad/Nona Bayer detail while damping a stray local outlier.
        return static_cast<uint16_t>((static_cast<uint32_t>(local) * 4 + nearby + 2) / 5);
    };

    for (const auto& pixel : eligible) {
        const auto& defect = *pixel.defect;
        const size_t index = static_cast<size_t>(pixel.y) * width + pixel.x;
        if (treatment == BadPixelTreatment::MarkPixels && !bakeForDemosaic) {
            samples[index] = 0;
        } else if (treatment != BadPixelTreatment::Bake && !bakeForDemosaic) {
            continue;
        } else if (defect.action == CalibrationData::BadPixelAction::Interpolate) {
            samples[index] = interpolate(pixel.x, pixel.y);
        } else {
            const int phase = phaseAt(pixel.x + sensorLeft, pixel.y + sensorTop);
            const float black = blackLevel[phase];
            const float factor = defect.action == CalibrationData::BadPixelAction::Brighten
                ? 1.0f + defect.amount : 1.0f - defect.amount;
            samples[index] = static_cast<uint16_t>(std::clamp(
                std::lround(black + std::max(0.0f, source[index] - black) * factor),
                0l, 65535l));
        }
    }
    return active;
}

void markBadPixelsRgb(uint16_t* samples, uint32_t outputWidth,
        uint32_t outputHeight, uint32_t sourceWidth, uint32_t sourceHeight,
        uint32_t cropWidth, uint32_t cropHeight,
        const std::vector<ActiveBadPixel>& pixels) {
    if (!samples || !outputWidth || !outputHeight || !sourceWidth || !sourceHeight)
        return;
    cropWidth = cropWidth && cropWidth <= sourceWidth ? cropWidth : sourceWidth;
    cropHeight = cropHeight && cropHeight <= sourceHeight ? cropHeight : sourceHeight;
    const int left = static_cast<int>((sourceWidth - cropWidth) / 2);
    const int top = static_cast<int>((sourceHeight - cropHeight) / 2);
    for (const auto& pixel : pixels) {
        const int x = static_cast<int>(pixel.column) - left;
        const int y = static_cast<int>(pixel.row) - top;
        if (x < 0 || y < 0 || x >= static_cast<int>(cropWidth) ||
            y >= static_cast<int>(cropHeight)) continue;
        const uint32_t outputX = std::min(outputWidth - 1,
            static_cast<uint32_t>(x) * outputWidth / cropWidth);
        const uint32_t outputY = std::min(outputHeight - 1,
            static_cast<uint32_t>(y) * outputHeight / cropHeight);
        const size_t offset = (static_cast<size_t>(outputY) * outputWidth + outputX) * 3;
        samples[offset] = samples[offset + 1] = samples[offset + 2] = 0;
    }
}

void markBadPixelsCfa(uint16_t* samples, uint32_t outputWidth,
        uint32_t outputHeight, uint32_t sourceWidth, uint32_t sourceHeight,
        uint32_t cropWidth, uint32_t cropHeight,
        const std::vector<ActiveBadPixel>& pixels) {
    if (!samples || !outputWidth || !outputHeight || !sourceWidth || !sourceHeight)
        return;
    cropWidth = cropWidth && cropWidth <= sourceWidth ? cropWidth : sourceWidth;
    cropHeight = cropHeight && cropHeight <= sourceHeight ? cropHeight : sourceHeight;
    const int left = static_cast<int>((sourceWidth - cropWidth) / 2);
    const int top = static_cast<int>((sourceHeight - cropHeight) / 2);
    for (const auto& pixel : pixels) {
        const int x = static_cast<int>(pixel.column) - left;
        const int y = static_cast<int>(pixel.row) - top;
        if (x < 0 || y < 0 || x >= static_cast<int>(cropWidth) ||
            y >= static_cast<int>(cropHeight)) continue;
        const uint32_t outputX = std::min(outputWidth - 1,
            static_cast<uint32_t>(x) * outputWidth / cropWidth);
        const uint32_t outputY = std::min(outputHeight - 1,
            static_cast<uint32_t>(y) * outputHeight / cropHeight);
        samples[static_cast<size_t>(outputY) * outputWidth + outputX] = 0;
    }
}

} // namespace motioncam::utils
