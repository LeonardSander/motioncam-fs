#include "CalibrationData.h"
#include <algorithm>
#include <fstream>
#include <sstream>
#include <cctype>
#include <spdlog/spdlog.h>

using json = nlohmann::json;

namespace motioncam {

namespace {
    float parseFraction(const json& value, const char* name) {
        if (value.is_number()) return value.get<float>();
        if (!value.is_string())
            throw std::invalid_argument(std::string(name) + " must be a normalized number or percentage");
        std::string text = value.get<std::string>();
        const bool percent = !text.empty() && text.back() == '%';
        if (percent) text.pop_back();
        const float result = std::stof(text) / (percent ? 100.0f : 1.0f);
        return result;
    }

    double parseExposureSeconds(const json& value) {
        if (value.is_number()) return value.get<double>();
        std::string text = value.get<std::string>();
        if (text.size() > 2 && text.substr(text.size() - 2) == "ms")
            return std::stod(text.substr(0, text.size() - 2)) / 1000.0;
        if (!text.empty() && text.back() == 's')
            return std::stod(text.substr(0, text.size() - 1));
        const auto slash = text.find('/');
        if (slash != std::string::npos)
            return std::stod(text.substr(0, slash)) / std::stod(text.substr(slash + 1));
        return std::stod(text);
    }

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

        if (j.contains("orientation") && j["orientation"].is_number_integer()) {
            const int orientation = j["orientation"].get<int>();
            if (orientation == 0 || orientation == 90 || orientation == 180 || orientation == 270) {
                data.orientation = orientation;
                data.hasOrientation = true;
            } else {
                spdlog::warn("Ignoring invalid orientation override: {}", orientation);
            }
        }
        if (j.contains("ignoreForwardMat") && j["ignoreForwardMat"].is_boolean()) {
            data.ignoreForwardMat = j["ignoreForwardMat"].get<bool>();
            data.hasIgnoreForwardMat = true;
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

        if (j.contains("levels") && j["levels"].is_string()) {
            data.levels = j["levels"].get<std::string>();
            data.hasLevels = true;
        }

        auto parseDimensions = [&](const char* field, std::array<int, 2>& value,
                                   bool& present) {
            if (!j.contains(field)) return;
            if (j[field].is_array()) {
                value = parseArray<int, 2>(j[field]);
            } else if (j[field].is_string()) {
                std::string text = j[field].get<std::string>();
                std::replace(text.begin(), text.end(), 'x', ',');
                std::replace(text.begin(), text.end(), 'X', ',');
                std::stringstream stream(text);
                std::string width, height, extra;
                if (!std::getline(stream, width, ',') || !std::getline(stream, height, ',') ||
                    std::getline(stream, extra, ','))
                    throw std::invalid_argument(std::string(field) + " must contain width,height");
                value = {std::stoi(width), std::stoi(height)};
            } else {
                throw std::invalid_argument(std::string(field) + " must be an array or string");
            }
            if (value[0] <= 0 || value[1] <= 0)
                throw std::invalid_argument(std::string(field) + " dimensions must be positive");
            present = true;
        };
        parseDimensions("centerCrop", data.centerCrop, data.hasCenterCrop);
        parseDimensions("leftTopCropStride", data.leftTopCropStride,
                        data.hasLeftTopCropStride);

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

        if (j.contains("badPixels")) {
            if (!j["badPixels"].is_array())
                throw std::invalid_argument("badPixels must be an array");
            for (const auto& item : j["badPixels"]) {
                CalibrationData::BadPixel pixel;
                pixel.x = item.at("x").get<int>();
                pixel.y = item.at("y").get<int>();
                if (item.contains("repeat")) {
                    const auto repeat = parseArray<int, 2>(item["repeat"]);
                    if (repeat[0] <= 0 || repeat[1] <= 0)
                        throw std::invalid_argument("badPixels repeat values must be positive");
                    pixel.repeatX = repeat[0];
                    pixel.repeatY = repeat[1];
                    if (pixel.x < 0 || pixel.y < 0 || pixel.x >= pixel.repeatX || pixel.y >= pixel.repeatY)
                        throw std::invalid_argument("repeating badPixels x/y must lie inside the repeat tile");
                } else if (pixel.x < 0 || pixel.y < 0) {
                    throw std::invalid_argument("badPixels x/y must not be negative");
                }
                const std::string treatment = item.value("treatment", "interpolate");
                if (treatment == "brighten") pixel.action = CalibrationData::BadPixelAction::Brighten;
                else if (treatment == "dampen") pixel.action = CalibrationData::BadPixelAction::Dampen;
                else if (treatment != "interpolate")
                    throw std::invalid_argument("badPixels treatment must be brighten, dampen, or interpolate");
                if (item.contains("amount")) pixel.amount = parseFraction(item["amount"], "amount");
                if (pixel.action != CalibrationData::BadPixelAction::Interpolate && !item.contains("amount"))
                    throw std::invalid_argument("brighten and dampen badPixels require amount");
                if (item.contains("thresholdAbove")) pixel.thresholdAbove = parseFraction(item["thresholdAbove"], "thresholdAbove");
                if (item.contains("thresholdBelow")) pixel.thresholdBelow = parseFraction(item["thresholdBelow"], "thresholdBelow");
                if (item.contains("threshold")) {
                    const auto& threshold = item["threshold"];
                    if (threshold.contains("above")) pixel.thresholdAbove = parseFraction(threshold["above"], "threshold above");
                    if (threshold.contains("below")) pixel.thresholdBelow = parseFraction(threshold["below"], "threshold below");
                }
                pixel.minIso = item.value("minIso", 0);
                if (item.contains("minExposure"))
                    pixel.minExposureSeconds = parseExposureSeconds(item["minExposure"]);
                const auto valid = [](float value) { return value >= 0.0f && value <= 1.0f; };
                if (!valid(pixel.amount) || (pixel.thresholdAbove && !valid(*pixel.thresholdAbove)) ||
                    (pixel.thresholdBelow && !valid(*pixel.thresholdBelow)))
                    throw std::invalid_argument("badPixels amount and thresholds must be between 0 and 1");
                data.badPixels.push_back(pixel);
            }
            data.hasBadPixels = !data.badPixels.empty();
        }

        // Return data only if at least one field was parsed
        if (data.hasColorMatrix1 || data.hasColorMatrix2 ||
            data.hasForwardMatrix1 || data.hasForwardMatrix2 ||
            data.hasAsShotNeutral || data.hasDataLevels || data.hasLevels ||
            data.hasCenterCrop || data.hasLeftTopCropStride || data.hasCfaSize ||
            data.hasNeedGainMapOrderFixed || data.hasFullSensorResolution || data.hasBadPixels ||
            data.hasOrientation || data.hasIgnoreForwardMat ||
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
  "_comment6": "Raw white/black override; numeric RGB black levels may be written as white/r,g,b",
  "_levels": "Dynamic",
  "_centerCrop": "3840,2160",
  "_leftTopCropStride": "4096x2304",
  "_comment6b": "CFA repeat size: 2 for Bayer, 4/6/8 for quad bayer and higher CFA sensors",
  "_cfaSize": 2,
  "_comment7": "Fix gainmap cfa bayer phase mismatches",
  "_needGainMapOrderFixed": true,
  "_comment8": "Specify uncropped resolution to prevent gainmaps to be scaled to fit.",
  "_fullSensorResolution": [4096, 3072],
  "_comment9": "Bad/PDAF pixels use normalized thresholds (0=black, 1=white); repeat defines a periodic tile",
  "_badPixels": [
    {"x": 123, "y": 456, "treatment": "interpolate", "threshold": {"above": "50%"}},
    {"x": 3, "y": 5, "repeat": [16, 16], "treatment": "brighten", "amount": "12%", "threshold": {"below": "75%"}}
  ]
})";
}

} // namespace motioncam
