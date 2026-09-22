#pragma once

#include <nlohmann/json.hpp>
#include <vector>
#include <string>
#include <array>

namespace motioncam {

enum class ScreenOrientation : int {
    PORTRAIT = 0,
    REVERSE_PORTRAIT,
    LANDSCAPE,
    REVERSE_LANDSCAPE,
    INVALID
};

struct CameraFrameMetadata {
    std::array<float, 3> asShotNeutral;
    int compressionType;
    std::array<float, 4> dynamicBlackLevel;
    float dynamicWhiteLevel;
    int exposureCompensation;
    double exposureTime;
    std::string filename;
    float focusDistance;
    int height;
    bool isBinned;
    bool isCompressed;
    int iso;
    std::vector<std::vector<float>> lensShadingMap;
    int lensShadingMapHeight;
    int lensShadingMapWidth;
    bool needRemosaic;
    int cfaSize;
    std::string offset;
    ScreenOrientation orientation;
    int originalHeight;
    int originalWidth;
    std::string pixelFormat;
    std::string recvdTimestampMs;
    int rowStride;
    std::string timestamp;
    std::string type;
    int width;
    std::vector<double> noiseProfile;
    bool hasNoiseProfile = false;
    std::array<float, 9> colorMatrix1{};
    std::array<float, 9> colorMatrix2{};
    std::array<float, 9> forwardMatrix1{};
    std::array<float, 9> forwardMatrix2{};
    std::array<float, 9> calibrationMatrix1{};
    std::array<float, 9> calibrationMatrix2{};
    bool hasColorMatrix1 = false;
    bool hasColorMatrix2 = false;
    bool hasForwardMatrix1 = false;
    bool hasForwardMatrix2 = false;
    bool hasCalibrationMatrix1 = false;
    bool hasCalibrationMatrix2 = false;

    static CameraFrameMetadata parse(const std::string& jsonString);
    static CameraFrameMetadata parse(const nlohmann::json& j);
    static CameraFrameMetadata limitedParse(const nlohmann::json& j);
};

}
