#include "CameraFrameMetadata.h"

using json = nlohmann::json;

namespace motioncam {

CameraFrameMetadata CameraFrameMetadata::parse(const json& j) {
    CameraFrameMetadata frame;

    // Parse asShotNeutral array
    if (j.contains("asShotNeutral") && j["asShotNeutral"].is_array()) {
        auto neutralArray = j["asShotNeutral"];
        for (size_t i = 0; i < 3 && i < neutralArray.size(); ++i) {
            frame.asShotNeutral[i] = neutralArray[i].get<float>();
        }
    }

    // Parse dynamicBlackLevel array
    if (j.contains("dynamicBlackLevel") && j["dynamicBlackLevel"].is_array()) {
        auto blackLevelArray = j["dynamicBlackLevel"];
        for (size_t i = 0; i < 4 && i < blackLevelArray.size(); ++i) {
            frame.dynamicBlackLevel[i] = blackLevelArray[i].get<float>();
        }
    }

    // Parse lens shading map (4 channels x height x width)
    if (j.contains("lensShadingMap") && j["lensShadingMap"].is_array()) {
        auto shadingMapArray = j["lensShadingMap"];
        frame.lensShadingMap.reserve(shadingMapArray.size()); // Should be 4 channels

        for (const auto& channel : shadingMapArray) {
            if (!channel.is_array())
                continue;

            // Parse 1D array for this channel
            std::vector<float> channelData1D;
            channelData1D.reserve(channel.size());

            for (const auto& value : channel)
                channelData1D.push_back(value.get<float>());

            frame.lensShadingMap.emplace_back(channelData1D);
        }
    }

    if (j.contains("noiseProfile") && j["noiseProfile"].is_array()) {
        auto noiseArray = j["noiseProfile"];
        for (size_t i = 0; i < 6 && i < noiseArray.size(); ++i) {
            frame.noiseProfile[i] = noiseArray[i].get<double>();
        }
    }


    // Parse simple fields with safe defaults
    frame.compressionType = j.value("compressionType", 0);
    frame.dynamicWhiteLevel = j.value("dynamicWhiteLevel", 0.0);
    frame.exposureCompensation = j.value("exposureCompensation", 0);
    frame.exposureTime = j.value("exposureTime", 0.0);
    frame.filename = j.value("filename", "");
    frame.height = j.value("height", 0);
    frame.isBinned = j.value("isBinned", false);
    frame.isCompressed = j.value("isCompressed", false);
    frame.iso = j.value("iso", 0);
    frame.lensShadingMapHeight = j.value("lensShadingMapHeight", 0);
    frame.lensShadingMapWidth = j.value("lensShadingMapWidth", 0);
    frame.needRemosaic = j.value("needRemosaic", false);
    frame.offset = j.value("offset", "");
    frame.orientation = static_cast<ScreenOrientation>(j.value("orientation", ScreenOrientation::INVALID));
    frame.originalHeight = j.value("originalHeight", 0);
    frame.originalWidth = j.value("originalWidth", 0);
    frame.pixelFormat = j.value("pixelFormat", "");
    frame.recvdTimestampMs = j.value("recvdTimestampMs", "");
    frame.rowStride = j.value("rowStride", 0);
    frame.timestamp = j.value("timestamp", "");
    frame.type = j.value("type", "");
    frame.width = j.value("width", 0);

    return frame;
}

CameraFrameMetadata CameraFrameMetadata::limitedParse(const json& j) {
    CameraFrameMetadata frame;

    frame.exposureTime = j.value("exposureTime", 0.0);
    frame.iso = j.value("iso", 0);

    return frame;
}

CameraFrameMetadata CameraFrameMetadata::parse(const std::string& jsonString) {
    json j = json::parse(jsonString);
    return parse(j);
}

// Helper method to convert timestamp string to numeric value
long long getTimestampAsNumber(const std::string& timestampStr) {
    try {
        return std::stoll(timestampStr);
    }
    catch (const std::exception&) {
        return 0;
    }
}

}
