#include "Utils.h"

#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace motioncam {
namespace utils {

namespace {

// Directional colour-difference demosaic for an ordinary Bayer mosaic.  The
// green pass uses an RCD-inspired directional correction; red and blue are
// then reconstructed from C-G differences.  As
// in VNG4, directions whose gradient is more than 30 raw codes above the best
// direction are rejected.  Restricting chroma to one or two accepted axes
// avoids the broad colour averaging used by the higher-CFA path.
void demosaicBayer(
    const std::vector<uint16_t>& cfaData,
    std::vector<uint16_t>& rgbData,
    int width,
    int height,
    const std::array<uint8_t, 4>& phase)
{
    constexpr float gradientThreshold = 30.0f;
    const size_t pixelCount = static_cast<size_t>(width) * height;
    auto index = [width](int x, int y) { return static_cast<size_t>(y) * width + x; };
    auto color = [&](int x, int y) { return static_cast<int>(phase[(y & 1) * 2 + (x & 1)]); };
    auto inside = [=](int x, int y) { return x >= 0 && y >= 0 && x < width && y < height; };
    auto raw = [&](int x, int y) { return static_cast<float>(cfaData[index(x, y)]); };

    std::vector<float> green(pixelCount);
    for (int y = 0; y < height; ++y) for (int x = 0; x < width; ++x) {
        const size_t i = index(x, y);
        if (color(x, y) == 1) {
            green[i] = raw(x, y);
            continue;
        }

        struct Candidate { float value; float gradient; };
        std::array<Candidate, 2> candidates{};
        int count = 0;
        for (const auto [dx, dy] : {std::pair<int, int>{1, 0}, {0, 1}}) {
            float adjacent = 0.0f, same = 0.0f;
            int adjacentCount = 0, sameCount = 0;
            float adjacentDifference = 0.0f, sameDifference = 0.0f;
            if (inside(x - dx, y - dy)) {
                adjacent += raw(x - dx, y - dy); ++adjacentCount;
            }
            if (inside(x + dx, y + dy)) {
                const float value = raw(x + dx, y + dy);
                if (adjacentCount) adjacentDifference = std::abs(value - adjacent);
                adjacent += value; ++adjacentCount;
            }
            if (!adjacentCount) continue;
            if (inside(x - 2 * dx, y - 2 * dy)) {
                same += raw(x - 2 * dx, y - 2 * dy); ++sameCount;
            }
            if (inside(x + 2 * dx, y + 2 * dy)) {
                const float value = raw(x + 2 * dx, y + 2 * dy);
                if (sameCount) sameDifference = std::abs(value - same);
                same += value; ++sameCount;
            }
            const float correction = sameCount
                ? 0.5f * (raw(x, y) - same / sameCount) : 0.0f;
            candidates[count++] = {
                adjacent / adjacentCount + correction,
                adjacentDifference + sameDifference};
        }
        if (!count) green[i] = raw(x, y);
        else if (count == 1) green[i] = candidates[0].value;
        else {
            const float best = std::min(candidates[0].gradient, candidates[1].gradient);
            float sum = 0.0f, weights = 0.0f;
            for (const Candidate& candidate : candidates) {
                if (candidate.gradient > best + gradientThreshold) continue;
                const float weight = 1.0f / (1.0f + candidate.gradient);
                sum += candidate.value * weight; weights += weight;
            }
            green[i] = sum / weights;
        }
        green[i] = std::clamp(green[i], 0.0f, 65535.0f);
    }

    std::array<std::vector<float>, 3> planes;
    for (auto& plane : planes) plane.assign(pixelCount, 0.0f);
    planes[1] = green;
    for (int y = 0; y < height; ++y) for (int x = 0; x < width; ++x) {
        const int native = color(x, y);
        if (native != 1) planes[native][index(x, y)] = raw(x, y);
    }

    auto interpolateDifference = [&](int x, int y, int channel) {
        struct Direction { int dx; int dy; };
        std::array<Direction, 2> directions{};
        int directionCount = 0;
        if (color(x, y) == 1) {
            // At green, one chroma colour lies horizontally and the other vertically.
            if ((inside(x - 1, y) && color(x - 1, y) == channel) ||
                (inside(x + 1, y) && color(x + 1, y) == channel))
                directions[directionCount++] = {1, 0};
            else
                directions[directionCount++] = {0, 1};
        } else {
            directions[directionCount++] = {1, 1};
            directions[directionCount++] = {1, -1};
        }

        struct Candidate { float difference; float gradient; };
        std::array<Candidate, 2> candidates{};
        int count = 0;
        for (int d = 0; d < directionCount; ++d) {
            const int dx = directions[d].dx, dy = directions[d].dy;
            float difference = 0.0f, firstGreen = 0.0f;
            int samples = 0;
            float gradient = 0.0f;
            for (int sign : {-1, 1}) {
                const int sx = x + sign * dx, sy = y + sign * dy;
                if (!inside(sx, sy) || color(sx, sy) != channel) continue;
                const float sampleGreen = green[index(sx, sy)];
                difference += raw(sx, sy) - sampleGreen;
                if (samples) gradient += std::abs(sampleGreen - firstGreen);
                else firstGreen = sampleGreen;
                ++samples;
            }
            if (samples) candidates[count++] = {difference / samples, gradient};
        }
        if (!count) return 0.0f;
        const float best = std::min_element(candidates.begin(), candidates.begin() + count,
            [](const Candidate& a, const Candidate& b) { return a.gradient < b.gradient; })->gradient;
        float sum = 0.0f, weights = 0.0f;
        for (int i = 0; i < count; ++i) {
            if (candidates[i].gradient > best + gradientThreshold) continue;
            const float weight = 1.0f / (1.0f + candidates[i].gradient);
            sum += candidates[i].difference * weight; weights += weight;
        }
        return sum / weights;
    };

    rgbData.resize(pixelCount * 3);
    for (int y = 0; y < height; ++y) for (int x = 0; x < width; ++x) {
        const size_t i = index(x, y);
        const int native = color(x, y);
        for (int channel : {0, 2}) if (native != channel)
            planes[channel][i] = green[i] + interpolateDifference(x, y, channel);

        // Chroma comes from colour differences; restore the measured sample at
        // the end so interpolation can never soften the pixel's native luma detail.
        std::array<float, 3> rgb = {planes[0][i], planes[1][i], planes[2][i]};
        rgb[native] = raw(x, y);
        for (int channel = 0; channel < 3; ++channel)
            rgbData[i * 3 + channel] = static_cast<uint16_t>(
                std::clamp(std::lround(rgb[channel]), 0l, 65535l));
    }
}

} // namespace

void demosaicHigherCFA(
    const std::vector<uint16_t>& cfaData,
    std::vector<uint16_t>& rgbData,
    int width,
    int height,
    int cfaRepeatSize,
    const std::array<uint8_t, 4>& bayerPhase,
    bool ocl)
{
    if (width <= 0 || height <= 0 || cfaRepeatSize < 2 || (cfaRepeatSize & 1) ||
        cfaData.size() < static_cast<size_t>(width) * height)
        throw std::invalid_argument("Invalid higher-CFA demosaic input");

    if (cfaRepeatSize == 2) {
        demosaicBayer(cfaData, rgbData, width, height, bayerPhase);
        return;
    }

    const int group = cfaRepeatSize / 2;
    const int lowWidth = (width + group - 1) / group;
    const int lowHeight = (height + group - 1) / group;
    auto sourceIndex = [width](int x, int y) { return static_cast<size_t>(y) * width + x; };
    auto lowIndex = [lowWidth](int x, int y) { return static_cast<size_t>(y) * lowWidth + x; };
    auto lowColor = [&](int x, int y) {
        return static_cast<int>(bayerPhase[(y & 1) * 2 + (x & 1)]);
    };

    std::vector<float> workingSamples(cfaData.begin(), cfaData.end());
    if (ocl) {
        const float centreX = (width - 1) * 0.5f;
        const float centreY = (height - 1) * 0.5f;
        const float halfWidth = std::max(1.0f, centreX);
        const float halfHeight = std::max(1.0f, centreY);
        for (int y = 0; y < height; ++y) for (int x = 0; x < width; ++x) {
            const int blockX = x / group;
            const int blockY = y / group;
            const float blockCentreX = blockX * group + (group - 1) * 0.5f;
            const float blockCentreY = blockY * group + (group - 1) * 0.5f;
            const float offsetX = centreX - blockCentreX;
            const float offsetY = centreY - blockCentreY;
            const bool inwardX = offsetX >= 0.0f
                ? x == std::min(width - 1, (blockX + 1) * group - 1)
                : x == blockX * group;
            const bool inwardY = offsetY >= 0.0f
                ? y == std::min(height - 1, (blockY + 1) * group - 1)
                : y == blockY * group;
            const float absX = std::abs(offsetX);
            const float absY = std::abs(offsetY);
            const float directionSum = std::max(1.0f, absX + absY);
            const float affected = (inwardX ? absX / directionSum : 0.0f) +
                                   (inwardY ? absY / directionSum : 0.0f);
            const float nx = (x - centreX) / halfWidth;
            const float ny = (y - centreY) / halfHeight;
            const float radial = std::min(1.0f, std::sqrt(nx * nx + ny * ny));
            // Zero correction in the optical centre, rising uniformly to
            // 0.123 stop at the image boundary.
            workingSamples[sourceIndex(x, y)] *=
                std::exp2(radial * affected * 0.123f);
        }
    }

    // Treat each same-colour group as one spatially centred Bayer sample. This
    // prevents the four colour planes from being reconstructed on offset grids,
    // which was the source of the residual coloured edge/aberration pattern.
    std::vector<float> mosaic(static_cast<size_t>(lowWidth) * lowHeight);
    for (int by = 0; by < lowHeight; ++by) for (int bx = 0; bx < lowWidth; ++bx) {
        double sum = 0.0;
        int count = 0;
        for (int y = by * group; y < std::min(height, (by + 1) * group); ++y)
            for (int x = bx * group; x < std::min(width, (bx + 1) * group); ++x) {
                sum += workingSamples[sourceIndex(x, y)];
                ++count;
            }
        mosaic[lowIndex(bx, by)] = count ? static_cast<float>(sum / count) : 0.0f;
    }

    // First form provisional complete planes on the compact Bayer grid. A
    // second pass uses an RGB luma estimate as its edge guide (VNG threshold
    // 30), while isotropic weighting avoids favouring horizontal/vertical edges.
    std::array<std::vector<float>, 3> lowPlanes;
    for (auto& plane : lowPlanes) plane.assign(mosaic.size(), 0.0f);
    auto interpolate = [&](int x, int y, int channel,
                           const std::vector<float>* lumaGuide) {
        double sum = 0.0, weights = 0.0;
        for (int radius = 1; radius <= 3 && weights == 0.0; ++radius) {
            for (int dy = -radius; dy <= radius; ++dy) for (int dx = -radius; dx <= radius; ++dx) {
                if (std::max(std::abs(dx), std::abs(dy)) != radius) continue;
                const int sx = x + dx, sy = y + dy;
                if (sx < 0 || sy < 0 || sx >= lowWidth || sy >= lowHeight ||
                    lowColor(sx, sy) != channel) continue;
                const float distance = std::sqrt(static_cast<float>(dx * dx + dy * dy));
                float weight = 1.0f / std::max(1.0f, distance);
                if (lumaGuide) {
                    const float gradient = std::abs(
                        (*lumaGuide)[lowIndex(sx, sy)] - (*lumaGuide)[lowIndex(x, y)]);
                    weight /= 1.0f + gradient / 30.0f;
                }
                sum += mosaic[lowIndex(sx, sy)] * weight;
                weights += weight;
            }
        }
        return weights ? static_cast<float>(sum / weights) : mosaic[lowIndex(x, y)];
    };
    for (int channel = 0; channel < 3; ++channel)
        for (int y = 0; y < lowHeight; ++y) for (int x = 0; x < lowWidth; ++x)
            lowPlanes[channel][lowIndex(x, y)] = lowColor(x, y) == channel
                ? mosaic[lowIndex(x, y)] : interpolate(x, y, channel, nullptr);

    std::vector<float> lumaGuide(mosaic.size());
    for (size_t i = 0; i < mosaic.size(); ++i)
        lumaGuide[i] = 0.25f * lowPlanes[0][i] +
                       0.50f * lowPlanes[1][i] +
                       0.25f * lowPlanes[2][i];

    auto refinedPlanes = lowPlanes;
    for (int channel = 0; channel < 3; ++channel)
        for (int y = 0; y < lowHeight; ++y) for (int x = 0; x < lowWidth; ++x)
            if (lowColor(x, y) != channel)
                refinedPlanes[channel][lowIndex(x, y)] =
                    interpolate(x, y, channel, &lumaGuide);
    lowPlanes = std::move(refinedPlanes);

    auto samplePlane = [&](int channel, float x, float y) {
        x = std::clamp(x, 0.0f, static_cast<float>(lowWidth - 1));
        y = std::clamp(y, 0.0f, static_cast<float>(lowHeight - 1));
        const int x0 = static_cast<int>(std::floor(x)), y0 = static_cast<int>(std::floor(y));
        const int x1 = std::min(x0 + 1, lowWidth - 1), y1 = std::min(y0 + 1, lowHeight - 1);
        const float linearX = x - x0, linearY = y - y0;
        const float smoothX = linearX * linearX * (3.0f - 2.0f * linearX);
        const float smoothY = linearY * linearY * (3.0f - 2.0f * linearY);
        // Retain mostly linear interpolation, but bias slightly toward the
        // registered group centres to avoid over-smoothing chroma transitions.
        const float fx = 0.75f * linearX + 0.25f * smoothX;
        const float fy = 0.75f * linearY + 0.25f * smoothY;
        const float top = lowPlanes[channel][lowIndex(x0, y0)] * (1.0f - fx) +
                          lowPlanes[channel][lowIndex(x1, y0)] * fx;
        const float bottom = lowPlanes[channel][lowIndex(x0, y1)] * (1.0f - fx) +
                             lowPlanes[channel][lowIndex(x1, y1)] * fx;
        return top * (1.0f - fy) + bottom * fy;
    };

    rgbData.resize(static_cast<size_t>(width) * height * 3);
    for (int y = 0; y < height; ++y) for (int x = 0; x < width; ++x) {
        const float lx = (x + 0.5f) / group - 0.5f;
        const float ly = (y + 0.5f) / group - 0.5f;
        std::array<float, 3> rgb = {
            samplePlane(0, lx, ly), samplePlane(1, lx, ly), samplePlane(2, lx, ly)};

        // Restore unsmoothed intra-group information as luminance only.
        const int blockX = x / group, blockY = y / group;
        const int native = lowColor(blockX, blockY);
        const float detailSample = workingSamples[sourceIndex(x, y)];
        const float base = std::max(1.0f, rgb[native]);
        const float detail = std::pow(
            std::max(0.0f, detailSample / base), 1.10f);
        for (float& value : rgb) value *= detail;
        rgb[native] = detailSample;

        const size_t output = sourceIndex(x, y) * 3;
        for (int channel = 0; channel < 3; ++channel)
            rgbData[output + channel] = static_cast<uint16_t>(
                std::clamp(std::lround(rgb[channel]), 0l, 65535l));
    }
}


} // namespace utils
} // namespace motioncam
