#pragma once

#include <algorithm>
#include <array>
#include <cstdint>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace motioncam {

// Display only: locate black in the stored-code domain without altering the
// table or discarding codes below it. Ties choose the first matching code.
inline uint32_t encodedBlackPoint(const std::vector<uint16_t>& table, float linearBlack) {
    if (table.empty()) return 0;
    const auto first = std::find_if(table.begin(), table.end(),
        [linearBlack](uint16_t value) { return value >= linearBlack; });
    return static_cast<uint32_t>(first == table.end() ? table.size() - 1 : first - table.begin());
}

struct ResolvedDataLevels {
    float white;
    std::array<float, 4> black;
};

inline ResolvedDataLevels resolveDataLevels(
    const std::string& selection,
    float dynamicWhite, const std::array<float, 4>& dynamicBlack,
    float staticWhite, const std::array<float, 4>& staticBlack,
    size_t blackChannels = 4) {
    ResolvedDataLevels result{dynamicWhite, dynamicBlack};
    const std::string value = selection.empty() ? "Dynamic" : selection;

    if (value == "Dynamic")
        return result;
    if (value == "Static")
        return {staticWhite, staticBlack};

    const auto separator = value.find('/');
    if (separator == std::string::npos || value.find('/', separator + 1) != std::string::npos)
        return result;

    try {
        const std::string white = value.substr(0, separator);
        const std::string black = value.substr(separator + 1);
        if (white == "Dynamic") result.white = dynamicWhite;
        else if (white == "Static") result.white = staticWhite;
        else result.white = std::stof(white);

        if (black == "Dynamic") result.black = dynamicBlack;
        else if (black == "Static") result.black = staticBlack;
        else if (black.find(',') == std::string::npos) {
            const float level = std::stof(black);
            result.black = {level, level, level, level};
        } else {
            if (blackChannels != 3 && blackChannels != 4)
                throw std::invalid_argument("Black level channel count must be three or four");
            std::stringstream values(black);
            std::string channelValue;
            size_t channel = 0;
            while (channel < blackChannels && std::getline(values, channelValue, ','))
                result.black[channel++] = std::stof(channelValue);
            if (channel != blackChannels || std::getline(values, channelValue, ','))
                throw std::invalid_argument(blackChannels == 3
                    ? "Expected exactly three RGB black levels"
                    : "Expected exactly four CFA black levels");
            if (blackChannels == 3) {
                // RGB sources have one black level per interleaved channel.
                // Preserve the four-slot representation used by CFA callers by
                // repeating green in the unused fourth slot.
                result.black[3] = result.black[1];
            }
        }
    } catch (const std::exception&) {
        return {dynamicWhite, dynamicBlack};
    }
    return result;
}

} // namespace motioncam
