#include "CameraMetadata.h"

using namespace nlohmann;

namespace motioncam {

// Helper function to convert JSON array to std::array with templated type
template<typename T, size_t N>
std::array<T, N> jsonArrayToStdArray(const json& jsonArray) {
    static_assert(std::is_arithmetic_v<T>, "Type T must be arithmetic");

    std::array<T, N> result = {};
    for (size_t i = 0; i < N && i < jsonArray.size(); ++i) {
        result[i] = jsonArray[i].get<T>();
    }
    return result;
}

// Helper function to convert JSON array to std::vector with templated type
template<typename T>
std::vector<T> jsonArrayToVector(const json& jsonArray) {
    static_assert(std::is_arithmetic_v<T>, "Type T must be arithmetic");

    std::vector<T> result;
    result.reserve(jsonArray.size());
    for (const auto& item : jsonArray) {
        result.push_back(item.get<T>());
    }
    return result;
}

Metadata parseMetadata(const json& j) {
    Metadata metadata;
    metadata.buildBrand = j.value("build.brand", "");
    metadata.buildDevice = j.value("build.device", "");
    metadata.buildManufacturer = j.value("build.manufacturer", "");
    metadata.buildModel = j.value("build.model", "");
    metadata.buildName = j.value("build.name", "");
    metadata.versionBuild = j.value("version.build", "");
    metadata.versionMajor = j.value("version.major", "");
    metadata.versionMinor = j.value("version.minor", "");
    return metadata;
}

PostProcessSettings parsePostProcessSettings(const json& j) {
    PostProcessSettings settings;
    settings.blacks = j.value("blacks", 0.0);
    settings.captureMode = j.value("captureMode", "");
    settings.chromaEps = j.value("chromaEps", 0.0);
    settings.contrast = j.value("contrast", 0.0);
    settings.dng = j.value("dng", false);
    settings.dngNoiseReduction = j.value("dngNoiseReduction", false);
    settings.exposure = j.value("exposure", 0.0);
    settings.flipped = j.value("flipped", false);
    settings.gpsAltitude = j.value("gpsAltitude", 0.0);
    settings.gpsLatitude = j.value("gpsLatitude", 0.0);
    settings.gpsLongitude = j.value("gpsLongitude", 0.0);
    settings.gpsTime = j.value("gpsTime", "");
    settings.jpeg = j.value("jpeg", false);
    settings.jpegQuality = j.value("jpegQuality", 0);

    if (j.contains("lut") && j["lut"].is_array()) {
        settings.lut = jsonArrayToVector<float>(j["lut"]);
    }

    settings.lutSize = j.value("lutSize", 0);

    if (j.contains("metadata")) {
        settings.metadata = parseMetadata(j["metadata"]);
    }

    settings.saturation = j.value("saturation", 0.0);
    settings.shadows = j.value("shadows", 0.0);
    settings.sharpen0 = j.value("sharpen0", 0.0);
    settings.sharpen1 = j.value("sharpen1", 0.0);
    settings.spatialDenoiseWeight = j.value("spatialDenoiseWeight", 0.0);
    settings.stackFrames = j.value("stackFrames", 0);
    settings.temperature = j.value("temperature", 0.0);
    settings.temporalDenoiseWeight = j.value("temporalDenoiseWeight", 0.0);
    settings.tint = j.value("tint", 0.0);
    settings.useUltraHdr = j.value("useUltraHdr", false);
    settings.whitePoint = j.value("whitePoint", 0.0);

    return settings;
}

ExtraData parseExtraData(const json& j) {
    ExtraData extraData;
    extraData.audioChannels = j.value("audioChannels", 0);
    extraData.audioSampleRate = j.value("audioSampleRate", 0);
    extraData.packageName = j.value("packageName", "");

    if (j.contains("postProcessSettings")) {
        extraData.postProcessSettings = parsePostProcessSettings(j["postProcessSettings"]);
    }

    extraData.purchaseFlags = j.value("purchaseFlags", 0);
    extraData.recordingType = j.value("recordingType", "");
    extraData.useAccurateTimestamp = j.value("useAccurateTimestamp", false);

    return extraData;
}

DeviceSpecificProfile parseDeviceSpecificProfile(const json& j) {
    DeviceSpecificProfile profile;
    profile.cameraId = j.value("cameraId", "");
    profile.deviceModel = j.value("deviceModel", "");
    profile.disableShadingMap = j.value("disableShadingMap", false);
    return profile;
}

CameraConfiguration CameraConfiguration::parse(const std::string& jsonString) {
    json j = json::parse(jsonString);

    return parse(j);
}

CameraConfiguration CameraConfiguration::parse(const nlohmann::json& j) {
    CameraConfiguration config;

    // Parse arrays and matrices
    if (j.contains("apertures") && j["apertures"].is_array()) {
        config.apertures = jsonArrayToVector<float>(j["apertures"]);
    }

    if (j.contains("blackLevel") && j["blackLevel"].is_array()) {
        config.blackLevel = jsonArrayToStdArray<float, 4>(j["blackLevel"]);
    }

    if (j.contains("calibrationMatrix1") && j["calibrationMatrix1"].is_array()) {
        config.calibrationMatrix1 = jsonArrayToStdArray<float, 9>(j["calibrationMatrix1"]);
    }

    if (j.contains("calibrationMatrix2") && j["calibrationMatrix2"].is_array()) {
        config.calibrationMatrix2 = jsonArrayToStdArray<float, 9>(j["calibrationMatrix2"]);
    }

    if (j.contains("colorMatrix1") && j["colorMatrix1"].is_array()) {
        config.colorMatrix1 = jsonArrayToStdArray<float, 9>(j["colorMatrix1"]);
    }

    if (j.contains("colorMatrix2") && j["colorMatrix2"].is_array()) {
        config.colorMatrix2 = jsonArrayToStdArray<float, 9>(j["colorMatrix2"]);
    }

    if (j.contains("focalLengths") && j["focalLengths"].is_array()) {
        config.focalLengths = jsonArrayToVector<float>(j["focalLengths"]);
    }

    if (j.contains("forwardMatrix1") && j["forwardMatrix1"].is_array()) {
        config.forwardMatrix1 = jsonArrayToStdArray<float, 9>(j["forwardMatrix1"]);
    }

    if (j.contains("forwardMatrix2") && j["forwardMatrix2"].is_array()) {
        config.forwardMatrix2 = jsonArrayToStdArray<float, 9>(j["forwardMatrix2"]);
    }

    // Parse simple fields
    config.colorIlluminant1 = j.value("colorIlluminant1", "");
    config.colorIlluminant2 = j.value("colorIlluminant2", "");
    config.numSegments = j.value("numSegments", 0);

    auto val0 = j.value("sensorArrangment", "");
    auto val1 = j.value("sensorArrangement", "");
    config.sensorArrangement = val0.empty() ? val1 : val0;

    config.whiteLevel = j.value("whiteLevel", 0.0f);

    // Parse nested objects
    if (j.contains("deviceSpecificProfile")) {
        config.deviceSpecificProfile = parseDeviceSpecificProfile(j["deviceSpecificProfile"]);
    }

    if (j.contains("extraData")) {
        config.extraData = parseExtraData(j["extraData"]);
    }

    return config;
}

} // namespace motioncam
