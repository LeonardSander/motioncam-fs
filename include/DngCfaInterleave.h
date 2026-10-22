#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <vector>

namespace motioncam {

// DNG RowInterleaveFactor/ColumnInterleaveFactor order: group each phase into
// a contiguous field, retaining row order within that field. Adobe uses the
// CFA repeat dimensions here so lossy JPEG XL sees smooth planes.
inline bool reorderDngSamples(const uint16_t* input, size_t sampleCount,
                              std::vector<uint16_t>& output,
                              uint32_t width, uint32_t height, uint32_t channels,
                              uint32_t rowFactor, uint32_t columnFactor,
                              bool encode, uint16_t clampMaximum = UINT16_MAX) {
    if (!width || !height || !channels || !rowFactor || !columnFactor ||
        rowFactor > height || columnFactor > width ||
        static_cast<size_t>(width) >
            std::numeric_limits<size_t>::max() / height / channels ||
        !input || sampleCount != static_cast<size_t>(width) * height * channels)
        return false;
    if (channels == 1 && rowFactor == 2 && columnFactor == 2 &&
        !(width & 1u) && !(height & 1u)) {
        output.resize(sampleCount);
        const uint32_t halfWidth = width / 2, halfHeight = height / 2;
        if (encode) {
            for (uint32_t y = 0; y < height; ++y) {
                const uint32_t sourceY = (y % halfHeight) * 2 + y / halfHeight;
                const uint16_t* source = input + static_cast<size_t>(sourceY) * width;
                uint16_t* destination = output.data() + static_cast<size_t>(y) * width;
                if (clampMaximum == UINT16_MAX) {
                    for (uint32_t x = 0; x < halfWidth; ++x) {
                        destination[x] = source[x * 2];
                        destination[halfWidth + x] = source[x * 2 + 1];
                    }
                } else {
                    for (uint32_t x = 0; x < halfWidth; ++x) {
                        destination[x] = std::min(source[x * 2], clampMaximum);
                        destination[halfWidth + x] =
                            std::min(source[x * 2 + 1], clampMaximum);
                    }
                }
            }
        } else {
            for (uint32_t y = 0; y < height; ++y) {
                const uint32_t sourceY = y / 2 + (y & 1u) * halfHeight;
                const uint16_t* source = input + static_cast<size_t>(sourceY) * width;
                uint16_t* destination = output.data() + static_cast<size_t>(y) * width;
                for (uint32_t x = 0; x < halfWidth; ++x) {
                    destination[x * 2] = source[x];
                    destination[x * 2 + 1] = source[halfWidth + x];
                }
            }
        }
        return true;
    }
    std::vector<uint32_t> rowMap(height), columnMap(width);
    uint32_t offset = 0;
    for (uint32_t phase = 0; phase < rowFactor; ++phase)
        for (uint32_t y = phase; y < height; y += rowFactor) {
            if (encode) rowMap[offset++] = y;
            else rowMap[y] = offset++;
        }
    offset = 0;
    for (uint32_t phase = 0; phase < columnFactor; ++phase)
        for (uint32_t x = phase; x < width; x += columnFactor) {
            if (encode) columnMap[offset++] = x;
            else columnMap[x] = offset++;
        }
    output.resize(sampleCount);
    for (uint32_t y = 0; y < height; ++y)
        for (uint32_t x = 0; x < width; ++x) {
            const size_t source = (static_cast<size_t>(rowMap[y]) * width +
                                   columnMap[x]) * channels;
            const size_t destination = (static_cast<size_t>(y) * width + x) * channels;
            if (channels == 1)
                output[destination] = encode
                    ? std::min(input[source], clampMaximum) : input[source];
            else if (encode && clampMaximum != UINT16_MAX) {
                for (uint32_t channel = 0; channel < channels; ++channel)
                    output[destination + channel] =
                        std::min(input[source + channel], clampMaximum);
            } else {
                std::copy_n(input + source, channels, output.data() + destination);
            }
        }
    return true;
}

inline bool reorderDngSamples(const std::vector<uint16_t>& input,
                              std::vector<uint16_t>& output,
                              uint32_t width, uint32_t height, uint32_t channels,
                              uint32_t rowFactor, uint32_t columnFactor,
                              bool encode, uint16_t clampMaximum = UINT16_MAX) {
    return reorderDngSamples(input.data(), input.size(), output, width, height,
                             channels, rowFactor, columnFactor, encode, clampMaximum);
}

} // namespace motioncam
