#pragma once

#include <nlohmann/json.hpp>
#include <array>
#include <string>
#include <optional>

namespace motioncam {

struct CalibrationData {
    std::array<float, 9> colorMatrix1;
    std::array<float, 9> colorMatrix2;
    std::array<float, 9> forwardMatrix1;
    std::array<float, 9> forwardMatrix2;
    std::array<float, 3> asShotNeutral;
    std::string cfaPhase;  // Bayer pattern: "bggr", "rggb", "grbg", "gbrg"
    std::string dataLevels = "Auto";  // DirectLog: "Auto", "Full", or "Limited"
    int cfaSize = 0; // Per-clip CFA repeat size. 0 means detect from input metadata.
    bool needGainMapOrderFixed = false;
    std::array<int, 2> fullSensorResolution = {0, 0}; // width, height
    
    bool hasColorMatrix1 = false;
    bool hasColorMatrix2 = false;
    bool hasForwardMatrix1 = false;
    bool hasForwardMatrix2 = false;
    bool hasAsShotNeutral = false;
    bool hasDataLevels = false;
    bool hasCfaSize = false;
    bool hasNeedGainMapOrderFixed = false;
    bool hasFullSensorResolution = false;
    
    // Parse from JSON file
    static std::optional<CalibrationData> loadFromFile(const std::string& filePath);
    
    // Parse from JSON string
    static std::optional<CalibrationData> parse(const std::string& jsonString);
    
    // Parse from nlohmann::json object
    static std::optional<CalibrationData> parse(const nlohmann::json& j);
    
    // Create example JSON content
    static std::string createExampleJson();
};

} // namespace motioncam
