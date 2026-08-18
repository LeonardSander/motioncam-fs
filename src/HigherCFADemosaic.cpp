#include "Utils.h"

#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace motioncam {
namespace utils {

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
        // A one-pixel Bayer group lands exactly on its measured sample, unlike
        // a higher-CFA group. Estimate that channel from neighbouring samples
        // so the residual is applied to every channel as brightness detail,
        // rather than surviving as red/green/blue noise structure.
        const float base = std::max(1.0f, group == 1
            ? interpolate(blockX, blockY, native, &lumaGuide)
            : rgb[native]);
        const float detail = std::pow(
            std::max(0.0f, detailSample / base), group == 1 ? 1.0f : 1.10f);
        for (float& value : rgb) value *= detail;
        rgb[native] = detailSample;

        const size_t output = sourceIndex(x, y) * 3;
        for (int channel = 0; channel < 3; ++channel)
            rgbData[output + channel] = static_cast<uint16_t>(
                std::clamp(std::lround(rgb[channel]), 0l, 65535l));
    }
}

void reduceRGB(const std::vector<uint16_t>& input, std::vector<uint16_t>& output,
               uint32_t width, uint32_t height, uint32_t scale, bool highQuality,
               uint32_t& outputWidth, uint32_t& outputHeight,
               uint16_t logWhiteLevel) {
    scale = std::max(1u, scale);
    outputWidth = width / scale;
    outputHeight = height / scale;
    if (!outputWidth || !outputHeight ||
        input.size() < static_cast<size_t>(width) * height * 3) {
        output.clear();
        return;
    }
    output.resize(static_cast<size_t>(outputWidth) * outputHeight * 3);
    const uint32_t sample = (scale - 1) / 2;
    for (uint32_t y = 0; y < outputHeight; ++y) {
        for (uint32_t x = 0; x < outputWidth; ++x) {
            for (uint32_t channel = 0; channel < 3; ++channel) {
                uint64_t value = 0;
                if (highQuality) {
                    if (logWhiteLevel) {
                        double linearSum = 0.0;
                        for (uint32_t sy = 0; sy < scale; ++sy)
                            for (uint32_t sx = 0; sx < scale; ++sx) {
                                const uint16_t encoded = input[
                                    ((static_cast<size_t>(y) * scale + sy) * width +
                                     x * scale + sx) * 3 + channel];
                                const double logValue = static_cast<double>(encoded) / logWhiteLevel;
                                linearSum += (std::pow(61.0, logValue) - 1.0) / 60.0;
                            }
                        const double linearAverage = linearSum /
                            (static_cast<double>(scale) * scale);
                        value = static_cast<uint64_t>(std::llround(
                            std::log2(1.0 + 60.0 * linearAverage) /
                            std::log2(61.0) * logWhiteLevel));
                    } else {
                        for (uint32_t sy = 0; sy < scale; ++sy)
                            for (uint32_t sx = 0; sx < scale; ++sx)
                                value += input[((static_cast<size_t>(y) * scale + sy) * width +
                                                x * scale + sx) * 3 + channel];
                        value = (value + static_cast<uint64_t>(scale) * scale / 2) /
                                (static_cast<uint64_t>(scale) * scale);
                    }
                } else {
                    value = input[((static_cast<size_t>(y) * scale + sample) * width +
                                   x * scale + sample) * 3 + channel];
                }
                output[(static_cast<size_t>(y) * outputWidth + x) * 3 + channel] =
                    static_cast<uint16_t>(value);
            }
        }
    }
}

void binQuadBayer(const std::vector<uint16_t>& input, std::vector<uint16_t>& output,
                  uint32_t width, uint32_t height,
                  uint32_t& outputWidth, uint32_t& outputHeight,
                  uint16_t logWhiteLevel) {
    outputWidth = width / 2;
    outputHeight = height / 2;
    if (!outputWidth || !outputHeight ||
        input.size() < static_cast<size_t>(width) * height) {
        output.clear();
        return;
    }
    output.resize(static_cast<size_t>(outputWidth) * outputHeight);
    for (uint32_t y = 0; y < outputHeight; ++y) {
        for (uint32_t x = 0; x < outputWidth; ++x) {
            const uint32_t sourceX = x * 2;
            const uint32_t sourceY = y * 2;
            const std::array<uint16_t, 4> values = {
                input[static_cast<size_t>(sourceY) * width + sourceX],
                input[static_cast<size_t>(sourceY) * width + sourceX + 1],
                input[static_cast<size_t>(sourceY + 1) * width + sourceX],
                input[static_cast<size_t>(sourceY + 1) * width + sourceX + 1]};
            uint16_t average = 0;
            if (logWhiteLevel) {
                double linearSum = 0.0;
                for (uint16_t encoded : values) {
                    const double logValue = static_cast<double>(encoded) / logWhiteLevel;
                    linearSum += (std::pow(61.0, logValue) - 1.0) / 60.0;
                }
                average = static_cast<uint16_t>(std::llround(
                    std::log2(1.0 + 60.0 * linearSum / 4.0) /
                    std::log2(61.0) * logWhiteLevel));
            } else {
                const uint64_t sum = values[0] + values[1] + values[2] + values[3];
                average = static_cast<uint16_t>((sum + 2) / 4);
            }
            output[static_cast<size_t>(y) * outputWidth + x] = average;
        }
    }
}


} // namespace utils
} // namespace motioncam
