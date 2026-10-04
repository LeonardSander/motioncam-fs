#include "Utils.h"
#include "GainMapBake.h"
#include "CpuWorkerBudget.h"

#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <thread>

namespace motioncam {
namespace utils {

namespace {
template <typename Function>
void parallelRows(uint32_t rows, Function&& function) {
    constexpr uint32_t minimumRowsPerWorker = 128;
    const uint32_t workers = std::min<uint32_t>(16, std::max<uint32_t>(1,
        std::min<uint32_t>(availableCpuWorkers(),
            (rows + minimumRowsPerWorker - 1) / minimumRowsPerWorker)));
    std::vector<std::thread> threads;
    threads.reserve(workers - 1);
    for (uint32_t worker = 1; worker < workers; ++worker)
        threads.emplace_back(function, rows * worker / workers,
                             rows * (worker + 1) / workers);
    function(0, rows / workers);
    for (auto& thread : threads) thread.join();
}

using Offset = std::pair<int16_t, int16_t>;
struct NearestPattern {
    int repeat = 0;
    std::array<uint8_t, 4> phase{};
    std::vector<std::vector<Offset>> candidates;
    std::vector<std::array<Offset, 3>> nearest;
    bool fastInterior = false;
};

const NearestPattern& nearestPattern(int repeat, const std::array<uint8_t, 4>& phase) {
    static thread_local NearestPattern pattern;
    if (pattern.repeat == repeat && pattern.phase == phase) return pattern;
    pattern.repeat = repeat;
    pattern.phase = phase;
    pattern.candidates.assign(static_cast<size_t>(repeat) * repeat * 3, {});
    pattern.nearest.resize(static_cast<size_t>(repeat) * repeat);
    const int block = std::max(1, repeat / 2);
    auto colorAt = [&](int x, int y) {
        return phase[((y % repeat) / block) * 2 + ((x % repeat) / block)];
    };
    for (int phaseY = 0; phaseY < repeat; ++phaseY)
        for (int phaseX = 0; phaseX < repeat; ++phaseX)
            for (int channel = 0; channel < 3; ++channel) {
                auto& candidates = pattern.candidates[
                    (static_cast<size_t>(phaseY) * repeat + phaseX) * 3 + channel];
                for (int dy = -repeat; dy <= repeat; ++dy)
                    for (int dx = -repeat; dx <= repeat; ++dx) {
                        int sampleX = (phaseX + dx) % repeat;
                        int sampleY = (phaseY + dy) % repeat;
                        if (sampleX < 0) sampleX += repeat;
                        if (sampleY < 0) sampleY += repeat;
                        if (colorAt(sampleX, sampleY) == channel)
                            candidates.emplace_back(dx, dy);
                    }
                std::stable_sort(candidates.begin(), candidates.end(),
                    [](const Offset& left, const Offset& right) {
                        return left.first * left.first + left.second * left.second <
                               right.first * right.first + right.second * right.second;
                    });
            }
    pattern.fastInterior = true;
    for (size_t index = 0; index < pattern.nearest.size(); ++index)
        for (int channel = 0; channel < 3; ++channel) {
            const auto& candidates = pattern.candidates[index * 3 + channel];
            if (candidates.empty()) pattern.fastInterior = false;
            else pattern.nearest[index][channel] = candidates.front();
        }
    return pattern;
}

using LevelTables = std::array<std::vector<uint16_t>, 3>;
const LevelTables& normalizationLevels(const std::array<double, 3>& black,
                                       const std::array<double, 3>& white) {
    struct Cache {
        LevelTables channels;
        std::array<double, 3> black{}, white{};
        bool ready = false;
    };
    static thread_local Cache cache;
    if (!cache.ready || cache.black != black || cache.white != white) {
        for (size_t channel = 0; channel < 3; ++channel) {
            auto& levels = cache.channels[channel];
            levels.resize(65536);
            for (size_t value = 0; value < levels.size(); ++value)
                levels[value] = static_cast<uint16_t>(std::clamp(std::lround(
                    (value - black[channel]) /
                        (white[channel] - black[channel]) * 65535.0),
                    0l, 65535l));
        }
        cache.black = black;
        cache.white = white;
        cache.ready = true;
    }
    return cache.channels;
}
}

void demosaicCfaForOutput(
        const std::vector<uint16_t>& cfaData, std::vector<uint16_t>& rgbData,
        int width, int height, int cfaRepeatSize,
        const std::array<uint8_t, 4>& bayerPhase, QuadBayerMode mode,
        const std::array<float, 3>& channelBlack, bool nearestColour) {
    if (!nearestColour) {
        demosaicHigherCFA(cfaData, rgbData, width, height, cfaRepeatSize,
                          bayerPhase, mode, channelBlack);
        return;
    }
    if (width <= 0 || height <= 0 || cfaRepeatSize < 2 ||
        cfaData.size() != static_cast<size_t>(width) * height) {
        rgbData.clear();
        return;
    }

    const auto& pattern = nearestPattern(cfaRepeatSize, bayerPhase);

    rgbData.assign(static_cast<size_t>(width) * height * 3, 0);
    parallelRows(static_cast<uint32_t>(height), [&](uint32_t begin, uint32_t end) {
        for (int y = static_cast<int>(begin); y < static_cast<int>(end); ++y)
          for (int x = 0; x < width; ++x) {
            const size_t phase = static_cast<size_t>(y % cfaRepeatSize) *
                cfaRepeatSize + x % cfaRepeatSize;
            const size_t destination = (static_cast<size_t>(y) * width + x) * 3;
            if (pattern.fastInterior && x >= cfaRepeatSize && y >= cfaRepeatSize &&
                x + cfaRepeatSize < width && y + cfaRepeatSize < height) {
                const auto& selected = pattern.nearest[phase];
                for (int channel = 0; channel < 3; ++channel) {
                    const auto [dx, dy] = selected[channel];
                    rgbData[destination + channel] = cfaData[
                        static_cast<size_t>(y + dy) * width + x + dx];
                }
                continue;
            }
            for (int channel = 0; channel < 3; ++channel) {
                const auto& candidates = pattern.candidates[
                    phase * 3 + channel];
                for (const auto& [dx, dy] : candidates) {
                    const int sourceX = x + dx, sourceY = y + dy;
                    if (sourceX < 0 || sourceY < 0 || sourceX >= width || sourceY >= height)
                        continue;
                    rgbData[destination + channel] =
                        cfaData[static_cast<size_t>(sourceY) * width + sourceX];
                    break;
                }
            }
          }
    });
}

bool demosaicNearestCfaToRgb16Bytes(
        const uint16_t* cfaData, std::vector<uint8_t>& rgbBytes,
        int width, int height, int cfaRepeatSize,
        const std::array<uint8_t, 4>& bayerPhase,
        const std::array<double, 3>& black,
        const std::array<double, 3>& white) {
    if (!cfaData || width <= 0 || height <= 0 || cfaRepeatSize < 2) return false;
    for (size_t channel = 0; channel < 3; ++channel)
        if (!(white[channel] > black[channel])) return false;
    const auto& pattern = nearestPattern(cfaRepeatSize, bayerPhase);
    const size_t pixelCount = static_cast<size_t>(width) * height;
    rgbBytes.resize(pixelCount * 3 * sizeof(uint16_t));
    auto* output = reinterpret_cast<uint16_t*>(rgbBytes.data());
    const LevelTables* levels = pixelCount >= 256 * 1024
        ? &normalizationLevels(black, white) : nullptr;
    auto normalize = [&](uint16_t value, int channel) -> uint16_t {
        if (levels) return (*levels)[channel][value];
        return static_cast<uint16_t>(std::clamp(std::lround(
            (value - black[channel]) /
                (white[channel] - black[channel]) * 65535.0), 0l, 65535l));
    };
    parallelRows(static_cast<uint32_t>(height), [&](uint32_t begin, uint32_t end) {
        for (int y = static_cast<int>(begin); y < static_cast<int>(end); ++y)
            for (int x = 0; x < width; ++x) {
                const size_t phase = static_cast<size_t>(y % cfaRepeatSize) *
                    cfaRepeatSize + x % cfaRepeatSize;
                const size_t destination = (static_cast<size_t>(y) * width + x) * 3;
                if (pattern.fastInterior && x >= cfaRepeatSize && y >= cfaRepeatSize &&
                    x + cfaRepeatSize < width && y + cfaRepeatSize < height) {
                    const auto& selected = pattern.nearest[phase];
                    for (int channel = 0; channel < 3; ++channel) {
                        const auto [dx, dy] = selected[channel];
                        const uint16_t sample = cfaData[
                            static_cast<size_t>(y + dy) * width + x + dx];
                        output[destination + channel] = normalize(sample, channel);
                    }
                    continue;
                }
                for (int channel = 0; channel < 3; ++channel) {
                    uint16_t sample = 0;
                    for (const auto& [dx, dy] : pattern.candidates[phase * 3 + channel]) {
                        const int sourceX = x + dx, sourceY = y + dy;
                        if (sourceX < 0 || sourceY < 0 || sourceX >= width || sourceY >= height)
                            continue;
                        sample = cfaData[static_cast<size_t>(sourceY) * width + sourceX];
                        break;
                    }
                    output[destination + channel] = normalize(sample, channel);
                }
            }
    });
    return true;
}

namespace {
bool validBayerPhase(const std::array<uint8_t, 4>& phase) {
    return phase == std::array<uint8_t, 4>{0, 1, 1, 2} ||
           phase == std::array<uint8_t, 4>{2, 1, 1, 0} ||
           phase == std::array<uint8_t, 4>{1, 0, 2, 1} ||
           phase == std::array<uint8_t, 4>{1, 2, 0, 1};
}

template <typename WritePixel>
bool reconstructBilinearBayer(const uint16_t* source, int width, int height,
                              const std::array<uint8_t, 4>& phase,
                              WritePixel&& writePixel) {
    if (!source || width < 2 || height < 2 || !validBayerPhase(phase))
        return false;
    constexpr std::array<Offset, 4> cardinal{{{-1, 0}, {1, 0}, {0, -1}, {0, 1}}};
    constexpr std::array<Offset, 4> diagonal{{{-1, -1}, {1, -1}, {-1, 1}, {1, 1}}};
    constexpr std::array<Offset, 2> horizontal{{{-1, 0}, {1, 0}}};
    constexpr std::array<Offset, 2> vertical{{{0, -1}, {0, 1}}};
    auto averageAtEdge = [&](int x, int y, uint8_t color, uint8_t native) {
        if (color == native) return source[static_cast<size_t>(y) * width + x];
        const Offset* offsets = nullptr;
        size_t count = 0;
        if (native == 1) {
            const uint8_t horizontalColor = phase[((y & 1) << 1) | ((x + 1) & 1)];
            if (color == horizontalColor) {
                offsets = horizontal.data();
                count = horizontal.size();
            } else {
                offsets = vertical.data();
                count = vertical.size();
            }
        } else if (color == 1) {
            offsets = cardinal.data();
            count = cardinal.size();
        } else {
            offsets = diagonal.data();
            count = diagonal.size();
        }
        uint32_t sum = 0, valid = 0;
        for (size_t index = 0; index < count; ++index) {
            const int sx = x + offsets[index].first;
            const int sy = y + offsets[index].second;
            if (sx >= 0 && sx < width && sy >= 0 && sy < height) {
                sum += source[static_cast<size_t>(sy) * width + sx];
                ++valid;
            }
        }
        return static_cast<uint16_t>((sum + valid / 2) / valid);
    };
    parallelRows(static_cast<uint32_t>(height), [&](uint32_t begin, uint32_t end) {
        for (int y = static_cast<int>(begin); y < static_cast<int>(end); ++y) {
            const auto* row = source + static_cast<size_t>(y) * width;
            const auto* above = y > 0 ? row - width : row;
            const auto* below = y + 1 < height ? row + width : row;
            for (int x = 0; x < width; ++x) {
                const uint8_t native = phase[((y & 1) << 1) | (x & 1)];
                uint16_t red, green, blue;
                if (x > 0 && x + 1 < width && y > 0 && y + 1 < height) {
                    const uint16_t center = row[x];
                    if (native == 1) {
                        green = center;
                        const uint16_t across = static_cast<uint16_t>(
                            (static_cast<uint32_t>(row[x - 1]) + row[x + 1] + 1) / 2);
                        const uint16_t down = static_cast<uint16_t>(
                            (static_cast<uint32_t>(above[x]) + below[x] + 1) / 2);
                        const uint8_t horizontalColor =
                            phase[((y & 1) << 1) | ((x + 1) & 1)];
                        red = horizontalColor == 0 ? across : down;
                        blue = horizontalColor == 2 ? across : down;
                    } else {
                        green = static_cast<uint16_t>((
                            static_cast<uint32_t>(row[x - 1]) + row[x + 1] +
                            above[x] + below[x] + 2) / 4);
                        const uint16_t opposite = static_cast<uint16_t>((
                            static_cast<uint32_t>(above[x - 1]) + above[x + 1] +
                            below[x - 1] + below[x + 1] + 2) / 4);
                        red = native == 0 ? center : opposite;
                        blue = native == 2 ? center : opposite;
                    }
                } else {
                    red = averageAtEdge(x, y, 0, native);
                    green = averageAtEdge(x, y, 1, native);
                    blue = averageAtEdge(x, y, 2, native);
                }
                writePixel((static_cast<size_t>(y) * width + x) * 3,
                           red, green, blue);
            }
        }
    });
    return true;
}
} // namespace

bool demosaicBilinearBayerToRgb16Bytes(
        const uint16_t* cfaData, std::vector<uint8_t>& rgbBytes,
        int width, int height, const std::array<uint8_t, 4>& bayerPhase,
        const std::array<double, 3>& black,
        const std::array<double, 3>& white) {
    for (size_t channel = 0; channel < 3; ++channel)
        if (!(white[channel] > black[channel])) return false;
    if (!cfaData || width < 2 || height < 2 || !validBayerPhase(bayerPhase))
        return false;
    const size_t pixelCount = static_cast<size_t>(width) * height;
    rgbBytes.resize(pixelCount * 3 * sizeof(uint16_t));
    auto* output = reinterpret_cast<uint16_t*>(rgbBytes.data());
    const LevelTables* levels = pixelCount >= 256 * 1024
        ? &normalizationLevels(black, white) : nullptr;
    auto normalize = [&](uint16_t value, size_t channel) -> uint16_t {
        if (levels) return (*levels)[channel][value];
        return static_cast<uint16_t>(std::clamp(std::lround(
            (value - black[channel]) /
                (white[channel] - black[channel]) * 65535.0), 0l, 65535l));
    };
    return reconstructBilinearBayer(
        cfaData, width, height, bayerPhase,
        [&](size_t index, uint16_t red, uint16_t green, uint16_t blue) {
            output[index] = normalize(red, 0);
            output[index + 1] = normalize(green, 1);
            output[index + 2] = normalize(blue, 2);
        });
}

bool demosaicBilinearBayer(
        const uint16_t* cfaData, std::vector<uint16_t>& rgb,
        int width, int height, const std::array<uint8_t, 4>& bayerPhase) {
    if (!cfaData || width < 2 || height < 2 || !validBayerPhase(bayerPhase))
        return false;
    rgb.resize(static_cast<size_t>(width) * height * 3);
    return reconstructBilinearBayer(
        cfaData, width, height, bayerPhase,
        [&](size_t index, uint16_t red, uint16_t green, uint16_t blue) {
            rgb[index] = red;
            rgb[index + 1] = green;
            rgb[index + 2] = blue;
        });
}

void classifyCfaPreviewClipping(
        const uint16_t* samples, uint32_t width, uint32_t height,
        int cfaRepeatSize, const std::array<uint8_t, 4>& cfaPhase,
        const std::array<float, 4>& black,
        const std::array<float, 4>& white,
        std::vector<uint8_t>& clipping) {
    if (!samples || !width || !height) {
        clipping.clear();
        return;
    }
    const uint32_t cell = static_cast<uint32_t>(std::max(2, cfaRepeatSize));
    const uint32_t group = std::max(1u, cell / 2);
    const uint32_t cellRows = (height + cell - 1) / cell;
    clipping.resize(static_cast<size_t>(width) * height);
    parallelRows(cellRows, [&](uint32_t firstRow, uint32_t lastRow) {
        for (uint32_t row = firstRow; row < lastRow; ++row) {
            const uint32_t top = row * cell;
            const uint32_t bottom = std::min(top + cell, height);
            for (uint32_t left = 0; left < width; left += cell) {
                const uint32_t right = std::min(left + cell, width);
                uint8_t clipped = 0;
                for (uint32_t phase = 0; phase < 4; ++phase) {
                    const uint32_t x = left + (phase & 1u) * group;
                    const uint32_t y = top + (phase >> 1u) * group;
                    if (x < width && y < height &&
                        samples[static_cast<size_t>(y) * width + x] >= white[phase])
                        clipped |= static_cast<uint8_t>(
                            1u << std::min<uint8_t>(2, cfaPhase[phase]));
                }
                const uint8_t indication = clipped ? 2 + clipped : 0;
                for (uint32_t y = top; y < bottom; ++y) {
                    const uint32_t phaseRow = ((y - top) / group) * 2;
                    for (uint32_t x = left; x < right; ++x) {
                        const uint32_t phase = phaseRow + (x - left) / group;
                        const size_t index = static_cast<size_t>(y) * width + x;
                        clipping[index] = samples[index] < black[phase]
                            ? 1 : indication;
                    }
                }
            }
        }
    });
}

bool normalizeRgb16(const std::vector<uint16_t>& input,
                    std::vector<uint16_t>& output,
                    const std::array<double, 3>& black,
                    const std::array<double, 3>& white) {
    if (input.size() % 3 != 0) return false;
    for (size_t channel = 0; channel < 3; ++channel)
        if (!(white[channel] > black[channel])) return false;
    output.resize(input.size());
    const size_t pixels = input.size() / 3;
    parallelRows(static_cast<uint32_t>(pixels), [&](uint32_t begin, uint32_t end) {
        for (size_t pixel = begin; pixel < end; ++pixel)
            for (size_t channel = 0; channel < 3; ++channel) {
                const size_t index = pixel * 3 + channel;
                output[index] = static_cast<uint16_t>(std::clamp(std::lround(
                    (input[index] - black[channel]) /
                        (white[channel] - black[channel]) * 65535.0),
                    0l, 65535l));
            }
    });
    return true;
}

bool normalizeRgb16Bytes(const std::vector<uint16_t>& input,
                         std::vector<uint8_t>& output,
                         const std::array<double, 3>& black,
                         const std::array<double, 3>& white) {
    return normalizeRgb16Bytes(input.data(), input.size(), output, black, white);
}

bool normalizeRgb16Bytes(const uint16_t* input, size_t sampleCount,
                         std::vector<uint8_t>& output,
                         const std::array<double, 3>& black,
                         const std::array<double, 3>& white) {
    if (sampleCount % 3 != 0 || (!input && sampleCount)) return false;
    for (size_t channel = 0; channel < 3; ++channel)
        if (!(white[channel] > black[channel])) return false;
    if (!sampleCount) {
        output.clear();
        return true;
    }
    output.resize(sampleCount * sizeof(uint16_t));
    if (black == std::array<double, 3>{0.0, 0.0, 0.0} &&
        white == std::array<double, 3>{65535.0, 65535.0, 65535.0}) {
        std::memcpy(output.data(), input, output.size());
        return true;
    }
    auto* normalized = reinterpret_cast<uint16_t*>(output.data());
    const size_t pixels = sampleCount / 3;
    if (pixels >= 256 * 1024) {
        const auto& channels = normalizationLevels(black, white);
        parallelRows(static_cast<uint32_t>(pixels), [&](uint32_t begin, uint32_t end) {
            for (size_t pixel = begin; pixel < end; ++pixel) {
                const size_t offset = pixel * 3;
                normalized[offset] = channels[0][input[offset]];
                normalized[offset + 1] = channels[1][input[offset + 1]];
                normalized[offset + 2] = channels[2][input[offset + 2]];
            }
        });
        return true;
    }
    parallelRows(static_cast<uint32_t>(pixels), [&](uint32_t begin, uint32_t end) {
        for (size_t pixel = begin; pixel < end; ++pixel)
            for (size_t channel = 0; channel < 3; ++channel) {
                const size_t index = pixel * 3 + channel;
                normalized[index] = static_cast<uint16_t>(std::clamp(std::lround(
                    (input[index] - black[channel]) /
                        (white[channel] - black[channel]) * 65535.0),
                    0l, 65535l));
            }
    });
    return true;
}

void parseCropTarget(const std::string& target, uint32_t& width,
                     uint32_t& height, uint32_t& stride) {
    width = height = stride = 0;
    const size_t separator = target.find('x');
    if (separator == std::string::npos) return;
    try {
        size_t heightEnd = 0;
        width = std::stoul(target.substr(0, separator));
        height = std::stoul(target.substr(separator + 1), &heightEnd);
        const size_t suffix = separator + 1 + heightEnd;
        if (suffix < target.size()) {
            if (target[suffix] != '_' || suffix + 1 >= target.size())
                throw std::invalid_argument("invalid crop suffix");
            size_t strideEnd = 0;
            stride = std::stoul(target.substr(suffix + 1), &strideEnd);
            if (suffix + 1 + strideEnd != target.size())
                throw std::invalid_argument("invalid stride");
        }
    } catch (const std::exception&) {
        width = height = stride = 0;
    }
}

bool cropInterleaved(const std::vector<uint16_t>& input,
                     std::vector<uint16_t>& output,
                     uint32_t width, uint32_t height, uint32_t channels,
                     uint32_t cropWidth, uint32_t cropHeight) {
    if (!channels || !cropWidth || !cropHeight || cropWidth > width || cropHeight > height ||
        input.size() != static_cast<size_t>(width) * height * channels) return false;
    const uint32_t left = (width - cropWidth) / 2;
    const uint32_t top = (height - cropHeight) / 2;
    output.resize(static_cast<size_t>(cropWidth) * cropHeight * channels);
    for (uint32_t y = 0; y < cropHeight; ++y)
        std::copy_n(input.begin() +
                        (static_cast<size_t>(top + y) * width + left) * channels,
                    static_cast<size_t>(cropWidth) * channels,
                    output.begin() + static_cast<size_t>(y) * cropWidth * channels);
    return true;
}

void encodeLog60(std::vector<uint16_t>& samples,
                 uint32_t width, uint32_t height, uint32_t channels,
                 const std::array<double, 4>& blackLevel,
                 double whiteLevel, uint16_t encodedWhite) {
    if (!channels || (channels != 1 && channels != 3) || !encodedWhite ||
        samples.size() != static_cast<size_t>(width) * height * channels)
        throw std::invalid_argument("Invalid LOG60 image");
    for (uint32_t y = 0; y < height; ++y)
        for (uint32_t x = 0; x < width; ++x)
            for (uint32_t channel = 0; channel < channels; ++channel) {
                const size_t index = (static_cast<size_t>(y) * width + x) *
                                     channels + channel;
                const uint32_t level = channels == 1
                    ? ((y & 1u) * 2u + (x & 1u)) : channel;
                const double normalized = std::clamp(
                    (samples[index] - blackLevel[level]) /
                    std::max(1.0, whiteLevel - blackLevel[level]), 0.0, 1.0);
                const double encoded = std::log2(1.0 + 60.0 * normalized) /
                                       std::log2(61.0);
                samples[index] = static_cast<uint16_t>(
                    std::lround(encoded * encodedWhite));
            }
}

void remosaicRGBToBayer(const std::vector<uint16_t>& rgbData,
                        std::vector<uint16_t>& bayerData,
                        int width, int height, const std::string& cfaPhase) {
    const auto phase = cfaColorsFromPhase(cfaPhase);
    bayerData.resize(static_cast<size_t>(width) * height);
    for (int y = 0; y < height; ++y)
        for (int x = 0; x < width; ++x) {
            const size_t pixel = static_cast<size_t>(y) * width + x;
            bayerData[pixel] = rgbData[pixel * 3 + phase[((y & 1) << 1) | (x & 1)]];
        }
}

void demosaicHigherCFA(
    const std::vector<uint16_t>& cfaData,
    std::vector<uint16_t>& rgbData,
    int width,
    int height,
    int cfaRepeatSize,
    const std::array<uint8_t, 4>& bayerPhase,
    QuadBayerMode mode,
    const std::array<float, 3>& channelBlack)
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

    const bool ocl = mode == QuadBayerMode::DemosaicOCL;
    const bool color = mode == QuadBayerMode::DemosaicColor && cfaRepeatSize > 2;
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

    if (color) {
        // The ordinary path interpolates three absolute colour planes. Around
        // the quad transition that lets a phase error in the compact Bayer
        // mosaic become a sharp green/magenta edge before detail restoration
        // even begins. For Color, use the reconstructed green plane as luma
        // and interpolate only the slower R-G and B-G differences.
        for (int channel : {0, 2}) {
            std::vector<float> sparseDifference(mosaic.size(), 0.0f);
            for (int y = 0; y < lowHeight; ++y) for (int x = 0; x < lowWidth; ++x)
                if (lowColor(x, y) == channel)
                    sparseDifference[lowIndex(x, y)] =
                        mosaic[lowIndex(x, y)] - lowPlanes[1][lowIndex(x, y)];

            for (int y = 0; y < lowHeight; ++y) for (int x = 0; x < lowWidth; ++x) {
                double sum = 0.0, weights = 0.0;
                for (int radius = 0; radius <= 3 && weights == 0.0; ++radius) {
                    for (int dy = -radius; dy <= radius; ++dy)
                        for (int dx = -radius; dx <= radius; ++dx) {
                            if (std::max(std::abs(dx), std::abs(dy)) != radius) continue;
                            const int sx = x + dx, sy = y + dy;
                            if (sx < 0 || sy < 0 || sx >= lowWidth || sy >= lowHeight ||
                                lowColor(sx, sy) != channel) continue;
                            const float distance = std::sqrt(static_cast<float>(dx * dx + dy * dy));
                            const float gradient = std::abs(
                                lumaGuide[lowIndex(sx, sy)] - lumaGuide[lowIndex(x, y)]);
                            const float weight = 1.0f / std::max(1.0f, distance) /
                                                 (1.0f + gradient / 30.0f);
                            sum += sparseDifference[lowIndex(sx, sy)] * weight;
                            weights += weight;
                        }
                }
                const float difference = weights ? static_cast<float>(sum / weights) : 0.0f;
                lowPlanes[channel][lowIndex(x, y)] =
                    lowPlanes[1][lowIndex(x, y)] + difference;
            }
        }
    }

    std::array<std::vector<float>, 2> colorBaseline;
    if (color) {
        for (int opponent = 0; opponent < 2; ++opponent) {
            const int channel = opponent == 0 ? 0 : 2;
            std::vector<float> difference(mosaic.size()), horizontal(mosaic.size());
            colorBaseline[opponent].resize(mosaic.size());
            for (size_t i = 0; i < mosaic.size(); ++i)
                difference[i] = lowPlanes[channel][i] - lowPlanes[1][i];
            for (int y = 0; y < lowHeight; ++y) for (int x = 0; x < lowWidth; ++x)
                horizontal[lowIndex(x, y)] =
                    0.25f * difference[lowIndex(std::max(0, x - 1), y)] +
                    0.50f * difference[lowIndex(x, y)] +
                    0.25f * difference[lowIndex(std::min(lowWidth - 1, x + 1), y)];
            for (int y = 0; y < lowHeight; ++y) for (int x = 0; x < lowWidth; ++x)
                colorBaseline[opponent][lowIndex(x, y)] =
                    0.25f * horizontal[lowIndex(x, std::max(0, y - 1))] +
                    0.50f * horizontal[lowIndex(x, y)] +
                    0.25f * horizontal[lowIndex(x, std::min(lowHeight - 1, y + 1))];
        }
    }

    auto sampleGrid = [&](const std::vector<float>& plane, float x, float y) {
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
        const float top = plane[lowIndex(x0, y0)] * (1.0f - fx) +
                          plane[lowIndex(x1, y0)] * fx;
        const float bottom = plane[lowIndex(x0, y1)] * (1.0f - fx) +
                             plane[lowIndex(x1, y1)] * fx;
        return top * (1.0f - fy) + bottom * fy;
    };
    auto samplePlane = [&](int channel, float x, float y) {
        return sampleGrid(lowPlanes[channel], x, y);
    };

    std::vector<float> rawLogDetail;
    std::vector<float> groupLogDetail;
    std::vector<float> smoothGroupLogDetail;
    std::vector<std::vector<float>> smoothPhaseLogDetail;
    if (color) {
        rawLogDetail.resize(static_cast<size_t>(width) * height);
        groupLogDetail.assign(mosaic.size(), 0.0f);
        std::vector<int> groupCounts(mosaic.size(), 0);
        for (int y = 0; y < height; ++y) for (int x = 0; x < width; ++x) {
            const float lx = (x + 0.5f) / group - 0.5f;
            const float ly = (y + 0.5f) / group - 0.5f;
            const int blockX = x / group, blockY = y / group;
            const int native = lowColor(blockX, blockY);
            const float black = channelBlack[native];
            const float base = std::max(1.0f, samplePlane(native, lx, ly) - black);
            const float measured = std::max(
                1.0f, workingSamples[sourceIndex(x, y)] - black);
            const float logDetail = std::log(measured / base);
            rawLogDetail[sourceIndex(x, y)] = logDetail;
            groupLogDetail[lowIndex(blockX, blockY)] += logDetail;
            ++groupCounts[lowIndex(blockX, blockY)];
        }
        for (size_t i = 0; i < groupLogDetail.size(); ++i)
            if (groupCounts[i]) groupLogDetail[i] /= groupCounts[i];

        // Smooth only the per-quad mean gain error. The log-detail variation
        // within each quad remains separate and retains the sensor's fine detail.
        std::vector<float> horizontal(mosaic.size());
        smoothGroupLogDetail.resize(mosaic.size());
        for (int y = 0; y < lowHeight; ++y) for (int x = 0; x < lowWidth; ++x)
            horizontal[lowIndex(x, y)] =
                0.25f * groupLogDetail[lowIndex(std::max(0, x - 1), y)] +
                0.50f * groupLogDetail[lowIndex(x, y)] +
                0.25f * groupLogDetail[lowIndex(std::min(lowWidth - 1, x + 1), y)];
        for (int y = 0; y < lowHeight; ++y) for (int x = 0; x < lowWidth; ++x)
            smoothGroupLogDetail[lowIndex(x, y)] =
                0.25f * horizontal[lowIndex(x, std::max(0, y - 1))] +
                0.50f * horizontal[lowIndex(x, y)] +
                0.25f * horizontal[lowIndex(x, std::min(lowHeight - 1, y + 1))];

        // A quad artifact can also have zero mean but a stable value at each
        // of its four pixel positions. Estimate only the component that stays
        // coherent across neighbouring quads; scene detail that changes from
        // quad to quad remains in rawLogDetail.
        smoothPhaseLogDetail.resize(static_cast<size_t>(group) * group);
        for (int py = 0; py < group; ++py) for (int px = 0; px < group; ++px) {
            const size_t phaseIndex = static_cast<size_t>(py) * group + px;
            std::vector<float> phaseDetail(mosaic.size(), 0.0f);
            std::vector<uint8_t> phaseValid(mosaic.size(), 0);
            smoothPhaseLogDetail[phaseIndex].resize(mosaic.size());
            for (int by = 0; by < lowHeight; ++by) for (int bx = 0; bx < lowWidth; ++bx) {
                const int x = bx * group + px, y = by * group + py;
                if (x < width && y < height) {
                    phaseDetail[lowIndex(bx, by)] =
                        rawLogDetail[sourceIndex(x, y)] - groupLogDetail[lowIndex(bx, by)];
                    phaseValid[lowIndex(bx, by)] = 1;
                }
            }
            constexpr std::array<float, 3> kernel = {0.25f, 0.50f, 0.25f};
            for (int by = 0; by < lowHeight; ++by) for (int bx = 0; bx < lowWidth; ++bx) {
                float sum = 0.0f, weights = 0.0f;
                for (int ky = -1; ky <= 1; ++ky) for (int kx = -1; kx <= 1; ++kx) {
                    const int sx = std::clamp(bx + kx, 0, lowWidth - 1);
                    const int sy = std::clamp(by + ky, 0, lowHeight - 1);
                    const size_t sample = lowIndex(sx, sy);
                    if (!phaseValid[sample]) continue;
                    const float weight = kernel[kx + 1] * kernel[ky + 1];
                    sum += phaseDetail[sample] * weight;
                    weights += weight;
                }
                smoothPhaseLogDetail[phaseIndex][lowIndex(bx, by)] =
                    weights > 0.0f ? sum / weights : 0.0f;
            }
        }
    }

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
        if (color) {
            // A large native-channel disagreement marks a locally unreliable
            // colour reconstruction. Fade only its local opponent-colour
            // variation toward a smooth raw-colour baseline; fading toward
            // zero would shift white balance in unbalanced sensor space.
            // Remove only the discontinuous per-quad mean gain from detail;
            // the intra-quad multiplicative detail is restored at full strength.
            const float groupDetail = groupLogDetail[lowIndex(blockX, blockY)];
            const float smoothGroupDetail = sampleGrid(smoothGroupLogDetail, lx, ly);
            const float blockError = groupDetail - smoothGroupDetail;
            const size_t phaseIndex = static_cast<size_t>(y % group) * group + (x % group);
            const float phaseError = smoothPhaseLogDetail[phaseIndex][lowIndex(blockX, blockY)];
            const float correctedLogDetail = rawLogDetail[sourceIndex(x, y)] -
                                             groupDetail + smoothGroupDetail - phaseError;
            const float artifactError = std::abs(blockError) + std::abs(phaseError);
            const float colorConfidence = 0.02f / (0.02f + artifactError);
            const float luma = 0.25f * rgb[0] + 0.50f * rgb[1] + 0.25f * rgb[2];
            const float baselineRedGreen = sampleGrid(colorBaseline[0], lx, ly);
            const float baselineBlueGreen = sampleGrid(colorBaseline[1], lx, ly);
            const float currentRedGreen = rgb[0] - rgb[1];
            const float currentBlueGreen = rgb[2] - rgb[1];
            // The remaining transition-band defect is predominantly a green
            // excursion: both opponent components fall below their local raw
            // colour baseline. Attenuate only that direction more strongly so
            // genuine detail and non-green colour transitions keep the normal
            // confidence response.
            const bool greenExcursion = currentRedGreen + currentBlueGreen <
                baselineRedGreen + baselineBlueGreen;
            // Apply the asymmetric green reduction only in proportion to the
            // detected artifact. With full confidence, genuine green detail
            // receives no additional attenuation.
            const float greenAttenuation = greenExcursion
                ? 0.25f + 0.75f * colorConfidence
                : 1.0f;
            const float opponentConfidence = colorConfidence * greenAttenuation;
            const float redGreen = baselineRedGreen +
                (currentRedGreen - baselineRedGreen) * opponentConfidence;
            const float blueGreen = baselineBlueGreen +
                (currentBlueGreen - baselineBlueGreen) * opponentConfidence;
            const float green = luma - 0.25f * (redGreen + blueGreen);
            rgb = {green + redGreen, green, green + blueGreen};
            const float detail = std::exp(correctedLogDetail);
            for (int channel = 0; channel < 3; ++channel)
                rgb[channel] = channelBlack[channel] +
                    (rgb[channel] - channelBlack[channel]) * detail;
        } else {
            const float base = std::max(1.0f, group == 1
                ? interpolate(blockX, blockY, native, &lumaGuide)
                : rgb[native]);
            const float detail = std::pow(
                std::max(0.0f, detailSample / base), group == 1 ? 1.0f : 1.10f);
            for (float& value : rgb) value *= detail;
            rgb[native] = detailSample;
        }

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
    parallelRows(outputHeight, [&](uint32_t begin, uint32_t end) {
      for (uint32_t y = begin; y < end; ++y) {
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
    });
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

void binHigherCFA(const std::vector<uint16_t>& input, std::vector<uint16_t>& output,
                  uint32_t width, uint32_t height, uint32_t factor,
                  uint32_t& outputWidth, uint32_t& outputHeight,
                  uint16_t logWhiteLevel) {
    outputWidth = factor ? width / factor : 0;
    outputHeight = factor ? height / factor : 0;
    if (!factor || !outputWidth || !outputHeight ||
        input.size() < static_cast<size_t>(width) * height) {
        output.clear();
        return;
    }
    output.resize(static_cast<size_t>(outputWidth) * outputHeight);
    const uint64_t area = static_cast<uint64_t>(factor) * factor;
    for (uint32_t y = 0; y < outputHeight; ++y) {
        for (uint32_t x = 0; x < outputWidth; ++x) {
            double linearSum = 0.0;
            uint64_t sum = 0;
            for (uint32_t by = 0; by < factor; ++by) {
                for (uint32_t bx = 0; bx < factor; ++bx) {
                    const uint16_t sample = input[
                        (static_cast<size_t>(y) * factor + by) * width + x * factor + bx];
                    if (logWhiteLevel) {
                        const double encoded = static_cast<double>(sample) / logWhiteLevel;
                        linearSum += (std::pow(61.0, encoded) - 1.0) / 60.0;
                    } else {
                        sum += sample;
                    }
                }
            }
            output[static_cast<size_t>(y) * outputWidth + x] = logWhiteLevel
                ? static_cast<uint16_t>(std::llround(
                    std::log2(1.0 + 60.0 * linearSum / area) /
                    std::log2(61.0) * logWhiteLevel))
                : static_cast<uint16_t>((sum + area / 2) / area);
        }
    }
}


} // namespace utils
} // namespace motioncam
