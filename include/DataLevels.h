#pragma once

#include <array>
#include <sstream>
#include <stdexcept>
#include <string>

namespace motioncam {

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
