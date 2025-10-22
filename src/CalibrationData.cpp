#include "CalibrationData.h"
#include <fstream>
#include <sstream>
#include <spdlog/spdlog.h>

using json = nlohmann::json;

namespace motioncam {

namespace {
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
            // Parse space-separated values
            std::string str = j.get<std::string>();
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
        
        json j;
        file >> j;
        return parse(j);
    } catch (const std::exception& e) {
        spdlog::error("Error loading calibration file {}: {}", filePath, e.what());
        return std::nullopt;
    }
}

std::optional<CalibrationData> CalibrationData::parse(const std::string& jsonString) {
    try {
        json j = json::parse(jsonString);
        return parse(j);
    } catch (const std::exception& e) {
        spdlog::error("Error parsing calibration JSON: {}", e.what());
        return std::nullopt;
    }
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
        
        // Return data only if at least one field was parsed
        if (data.hasColorMatrix1 || data.hasColorMatrix2 || 
            data.hasForwardMatrix1 || data.hasForwardMatrix2 || 
            data.hasAsShotNeutral || !data.cfaPhase.empty()) {
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
  "_cfaPhase": "bggr"
})";
}

} // namespace motioncam
