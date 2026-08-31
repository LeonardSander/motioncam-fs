#include "CalibrationData.h"
#include <algorithm>
#include <fstream>
#include <sstream>
#include <cctype>
#include <spdlog/spdlog.h>

using json = nlohmann::json;

namespace motioncam {

namespace {
    std::string normalizeWhitespaceSeparatedArrays(std::string jsonText) {
        static const std::array<const char*, 12> keys = {
            "colorMatrix1", "colorMatrix2", "forwardMatrix1", "forwardMatrix2",
            "asShotNeutral", "fullSensorResolution", "_colorMatrix1", "_colorMatrix2",
            "_forwardMatrix1", "_forwardMatrix2", "_asShotNeutral", "_fullSensorResolution"
        };

        for (const char* key : keys) {
            const std::string quotedKey = std::string("\"") + key + "\"";
            auto keyPos = jsonText.find(quotedKey);
            while (keyPos != std::string::npos) {
                const auto colonPos = jsonText.find_first_not_of(
                    " \t\r\n", keyPos + quotedKey.size());
                if (colonPos != std::string::npos && jsonText[colonPos] == ':') {
                    break;
                }
                keyPos = jsonText.find(quotedKey, keyPos + quotedKey.size());
            }
            if (keyPos == std::string::npos) {
                continue;
            }

            const auto openBracket = jsonText.find('[', keyPos + quotedKey.size());
            if (openBracket == std::string::npos) {
                continue;
            }
            const auto closeBracket = jsonText.find(']', openBracket + 1);
            if (closeBracket == std::string::npos) {
                continue;
            }

            std::string values =
                jsonText.substr(openBracket + 1, closeBracket - openBracket - 1);
            std::replace(values.begin(), values.end(), ',', ' ');

            std::istringstream input(values);
            std::ostringstream normalized;
            std::string value;
            bool first = true;
            while (input >> value) {
                if (!first) {
                    normalized << ", ";
                }
                normalized << value;
                first = false;
            }

            jsonText.replace(
                openBracket + 1, closeBracket - openBracket - 1, normalized.str());
        }

        return jsonText;
    }

    // Helper to parse array from JSON - supports both comma-separated and space-separated
    template<typename T, size_t N>
    std::array<T, N> parseArray(const json& j) {
        std::array<T, N> result = {};
        
        if (j.is_array()) {
            size_t count = std::min(j.size(), N);
            for (size_t i = 0; i < count; ++i) {
                result[i] = j[i].get<T>();
            }
        } else if (j.is_string()) {
            // Parse comma- or space-separated values
            std::string str = j.get<std::string>();
            std::replace(str.begin(), str.end(), ',', ' ');
            std::istringstream iss(str);
            for (size_t i = 0; i < N && iss >> result[i]; ++i) {
                // Continue reading
            }
        }
        
        return result;
    }
}

std::optional<CalibrationData> CalibrationData::loadFromFile(const std::string& filePath) {
    try {
        std::ifstream file(filePath);
        if (!file.is_open()) {
            spdlog::warn("Could not open calibration file: {}", filePath);
            return std::nullopt;
        }
        
        std::stringstream buffer;
        buffer << file.rdbuf();
        json j = parseSidecarJson(buffer.str());
        return parse(j);
    } catch (const std::exception& e) {
        spdlog::error("Error loading calibration file {}: {}", filePath, e.what());
        return std::nullopt;
    }
}

std::optional<CalibrationData> CalibrationData::parse(const std::string& jsonString) {
    try {
        json j = parseSidecarJson(jsonString);
        return parse(j);
    } catch (const std::exception& e) {
        spdlog::error("Error parsing calibration JSON: {}", e.what());
        return std::nullopt;
    }
}

nlohmann::json CalibrationData::parseSidecarJson(const std::string& jsonString) {
    return json::parse(normalizeWhitespaceSeparatedArrays(jsonString));
}

std::optional<CalibrationData> CalibrationData::parse(const nlohmann::json& j) {
    try {
        CalibrationData data;
        
        if (j.contains("colorMatrix1")) {
            data.colorMatrix1 = parseArray<float, 9>(j["colorMatrix1"]);
            data.hasColorMatrix1 = true;
        }
        
        if (j.contains("colorMatrix2")) {
            data.colorMatrix2 = parseArray<float, 9>(j["colorMatrix2"]);
            data.hasColorMatrix2 = true;
        }
        
        if (j.contains("forwardMatrix1")) {
            data.forwardMatrix1 = parseArray<float, 9>(j["forwardMatrix1"]);
            data.hasForwardMatrix1 = true;
        }
        
        if (j.contains("forwardMatrix2")) {
            data.forwardMatrix2 = parseArray<float, 9>(j["forwardMatrix2"]);
            data.hasForwardMatrix2 = true;
        }
        
        if (j.contains("asShotNeutral")) {
            data.asShotNeutral = parseArray<float, 3>(j["asShotNeutral"]);
            data.hasAsShotNeutral = true;
        }
        
        if (j.contains("cfaPhase")) {
            data.cfaPhase = j["cfaPhase"].get<std::string>();
        }

        if (j.contains("dataLevels")) {
            const std::string levels = j["dataLevels"].get<std::string>();
            std::string normalized = levels;
            std::transform(normalized.begin(), normalized.end(), normalized.begin(),
                [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            if (normalized == "auto" || normalized == "full" || normalized == "limited") {
                normalized[0] = static_cast<char>(std::toupper(normalized[0]));
                data.dataLevels = normalized;
                data.hasDataLevels = true;
            } else {
                spdlog::warn(
                    "Ignoring invalid dataLevels '{}'; expected Auto, Full, or Limited",
                    levels);
            }
        }

        if (j.contains("cfaSize")) {
            int size = 0;
            if (j["cfaSize"].is_number_integer()) {
                size = j["cfaSize"].get<int>();
            } else if (j["cfaSize"].is_string()) {
                const std::string value = j["cfaSize"].get<std::string>();
                size_t consumed = 0;
                size = std::stoi(value, &consumed);
                if (consumed != value.size())
                    throw std::invalid_argument("cfaSize contains non-numeric characters");
            } else {
                throw std::invalid_argument("cfaSize must be an integer or numeric string");
            }
            if (size >= 2 && (size % 2) == 0) {
                data.cfaSize = size;
                data.hasCfaSize = true;
            } else {
                spdlog::warn("Ignoring invalid cfaSize {}; expected an even integer >= 2", size);
            }
        }

        if (j.contains("needGainMapOrderFixed")) {
            if (!j["needGainMapOrderFixed"].is_boolean())
                throw std::invalid_argument("needGainMapOrderFixed must be a boolean");
            data.needGainMapOrderFixed = j["needGainMapOrderFixed"].get<bool>();
            data.hasNeedGainMapOrderFixed = true;
        }

        if (j.contains("fullSensorResolution")) {
            data.fullSensorResolution = parseArray<int, 2>(j["fullSensorResolution"]);
            if (data.fullSensorResolution[0] > 0 && data.fullSensorResolution[1] > 0) {
                data.hasFullSensorResolution = true;
            } else {
                throw std::invalid_argument(
                    "fullSensorResolution must contain positive width and height");
            }
        }

        // Return data only if at least one field was parsed
        if (data.hasColorMatrix1 || data.hasColorMatrix2 ||
            data.hasForwardMatrix1 || data.hasForwardMatrix2 ||
            data.hasAsShotNeutral || data.hasDataLevels || data.hasCfaSize ||
            data.hasNeedGainMapOrderFixed || data.hasFullSensorResolution ||
            !data.cfaPhase.empty()) {
            return data;
        }
        
        spdlog::warn("No valid calibration data found in JSON");
        return std::nullopt;
        
    } catch (const std::exception& e) {
        spdlog::error("Error parsing calibration data: {}", e.what());
        return std::nullopt;
    }
}

std::string CalibrationData::createExampleJson() {
    return R"({
  "_comment": "Calibration data for DNG color processing",
  "_comment2": "Matrix values can be separated by comma or space",
  "_comment3": "So far only these fields can be overriden. Remove _ in _colorMatrix1 to enable override.",
  "_colorMatrix1": [0.7643, -0.2137, -0.0822, -0.5013, 1.3478, 0.1644, -0.1315, 0.1972, 0.5588],
  "_colorMatrix2": [0.9329, -0.3914, -0.0326, -0.5806, 1.4092, 0.1827, -0.0913, 0.1761, 0.5872],
  "_forwardMatrix1": [0.6484, 0.2734, 0.0469, 0.2344, 0.8984, -0.1328, 0.0469, -0.1797, 0.9609],
  "_forwardMatrix2": [0.6875, 0.1563, 0.125, 0.2734, 0.7578, -0.0313, 0.0859, -0.4688, 1.2109],
  "_asShotNeutral": [0.5, 1.0, 0.5],
  "_comment4": "For DirectLog RGB remosaic Bayer phases rggb grbg gbrg bggr default bggr if not specified",
  "_cfaPhase": "bggr",
  "_comment5": "DirectLog input levels: Auto uses video metadata; Full or Limited overrides it per clip",
  "_dataLevels": "Full",
  "_comment6": "CFA repeat size: 2 for Bayer, 4/6/8 for quad bayer and higher CFA sensors",
  "_cfaSize": 2,
  "_comment7": "Fix gainmap cfa bayer phase mismatches",
  "_needGainMapOrderFixed": true,
  "_comment8": "Specify uncropped resolution to prevent gainmaps to be scaled to fit.",
  "_fullSensorResolution": [4096, 3072]
})";
}

} // namespace motioncam
