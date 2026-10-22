#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace motioncam {

// DNG RowInterleaveFactor/ColumnInterleaveFactor order: group each phase into
// a contiguous field, retaining row order within that field. Adobe uses the
// CFA repeat dimensions here so lossy JPEG XL sees smooth planes.
inline bool reorderDngSamples(const uint16_t* input, size_t sampleCount,
                              std::vector<uint16_t>& output,
                              uint32_t width, uint32_t height, uint32_t channels,
                              uint32_t rowFactor, uint32_t columnFactor,
                              bool encode) {
    if (!width || !height || !channels || !rowFactor || !columnFactor ||
        rowFactor > height || columnFactor > width ||
        !input || sampleCount != static_cast<size_t>(width) * height * channels)
        return false;
    std::vector<uint32_t> rowMap(height), columnMap(width);
    uint32_t offset = 0;
    for (uint32_t phase = 0; phase < rowFactor; ++phase)
        for (uint32_t y = phase; y < height; y += rowFactor)
            rowMap[y] = offset++;
    offset = 0;
    for (uint32_t phase = 0; phase < columnFactor; ++phase)
        for (uint32_t x = phase; x < width; x += columnFactor)
            columnMap[x] = offset++;
    output.resize(sampleCount);
    for (uint32_t y = 0; y < height; ++y)
        for (uint32_t x = 0; x < width; ++x) {
            const size_t original = (static_cast<size_t>(y) * width + x) * channels;
            const size_t grouped = (static_cast<size_t>(rowMap[y]) * width +
                                    columnMap[x]) * channels;
            for (uint32_t channel = 0; channel < channels; ++channel) {
                if (encode) output[grouped + channel] = input[original + channel];
                else output[original + channel] = input[grouped + channel];
            }
        }
    return true;
}

inline bool reorderDngSamples(const std::vector<uint16_t>& input,
                              std::vector<uint16_t>& output,
                              uint32_t width, uint32_t height, uint32_t channels,
                              uint32_t rowFactor, uint32_t columnFactor,
                              bool encode) {
    return reorderDngSamples(input.data(), input.size(), output, width, height,
                             channels, rowFactor, columnFactor, encode);
}

} // namespace motioncam
