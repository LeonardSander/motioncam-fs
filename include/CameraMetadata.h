#pragma once

#include <nlohmann/json.hpp>

#include <vector>
#include <string>
#include <array>

namespace motioncam {

struct DeviceSpecificProfile {
    std::string cameraId;
    std::string deviceModel;
    bool disableShadingMap;
};

struct Metadata {
    std::string buildBrand;
    std::string buildDevice;
    std::string buildManufacturer;
    std::string buildModel;
    std::string buildName;
    std::string versionBuild;
    std::string versionMajor;
    std::string versionMinor;
};

struct PostProcessSettings {
    float blacks;
    std::string captureMode;
    float chromaEps;
    float contrast;
    bool dng;
    bool dngNoiseReduction;
    float exposure;
    bool flipped;
    float gpsAltitude;
    float gpsLatitude;
    float gpsLongitude;
    std::string gpsTime;
    bool jpeg;
    int jpegQuality;
    std::vector<float> lut;
    int lutSize;
    Metadata metadata;
    float saturation;
    float shadows;
    float sharpen0;
    float sharpen1;
    float spatialDenoiseWeight;
    int stackFrames;
    float temperature;
    float temporalDenoiseWeight;
    float tint;
    bool useUltraHdr;
    float whitePoint;
};

struct ExtraData {
    int audioChannels;
    int audioSampleRate;
    std::string packageName;
    PostProcessSettings postProcessSettings;
    int purchaseFlags;
    std::string recordingType;
    bool useAccurateTimestamp;
};

struct CameraConfiguration {
    std::vector<float> apertures;
    std::array<float, 4> blackLevel;
    std::array<float, 9> calibrationMatrix1;
    std::array<float, 9> calibrationMatrix2;
    std::string colorIlluminant1;
    std::string colorIlluminant2;
    std::array<float, 9> colorMatrix1;
    std::array<float, 9> colorMatrix2;
    DeviceSpecificProfile deviceSpecificProfile;
    ExtraData extraData;
    std::vector<float> focalLengths;
    std::array<float, 9> forwardMatrix1;
    std::array<float, 9> forwardMatrix2;
    int numSegments;
    std::string sensorArrangement;
    float whiteLevel;

    static CameraConfiguration parse(const std::string& jsonString);
    static CameraConfiguration parse(const nlohmann::json& j);
};

}
