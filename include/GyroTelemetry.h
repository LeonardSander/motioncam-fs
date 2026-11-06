#pragma once

#include <nlohmann/json.hpp>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace motioncam {
// GCSV and Gyroflow protobuf/JSONL are distinct source formats. They are never
// converted into each other. Binary protobuf is decoded with its complete schema.
class GyroTelemetry {
  public:
    enum class Format { None, Gcsv, Jsonl };
    struct Frame {
        nlohmann::json message;
        int64_t timestampNs = 0; // Video timeline, relative to the first image.
    };
    // Matching JSONL/GCSV sidecars override embedded/native telemetry. When
    // both exist, JSONL takes precedence; the chosen format is retained.
    // sidecarSource routes overrides beside an original ZIP/7z import.
    static std::shared_ptr<const GyroTelemetry> load(const std::string& source,
                                                     const std::string& sidecarSource = {});
    static nlohmann::json decodeProtobuf(const uint8_t* bytes, size_t size);
    Format format() const { return mFormat; }
    std::string extension() const;
    std::string serialize(const std::vector<int64_t>& outputTimestamps = {}, double fps = 0, int width = 0,
                          int height = 0, int orientation = -1, bool excludeGps = false) const;
    const nlohmann::json& header() const { return mHeader; }
    const nlohmann::json* frame(size_t index) const;
    const std::vector<Frame>& frames() const { return mFrames; }
    // Complete, increasing image timeline: original exposure-start timestamps,
    // or explicit presentation timestamps when reimporting a finalized sequence.
    // Empty means telemetry cannot provide timing for every source image.
    std::vector<int64_t> frameTimestamps(size_t expectedFrameCount) const;
    // Writes telemetry into MOV/MP4 without an external gyro sidecar. GCSV
    // remains GCSV; protobuf/JSONL remains the complete protobuf schema.
    void embed(const std::string& videoPath, const std::vector<int64_t>& outputTimestamps, double fps,
               int width, int height, int orientation, const std::function<bool()>& keepGoing = {},
               bool excludeGps = false) const;
    bool fillMissingDngMetadata(std::vector<uint8_t>& bytes, size_t frameIndex) const;

  private:
    static void stripGps(nlohmann::json& value);
    nlohmann::json outputHeader(const std::vector<int64_t>& timestamps, double fps, int width, int height,
                                int orientation) const;
    std::vector<Frame> outputFrames(const std::vector<int64_t>& timestamps) const;
    void readJsonl(const std::string& path);
    void readContainer(const std::string& path);
    void readRaw(const std::string& path);
    Format mFormat = Format::None;
    nlohmann::json mHeader;
    std::vector<Frame> mFrames;
    std::string mGcsv;
    double mClockOriginUs = 0;
    bool mHaveClockOrigin = false;
};
} // namespace motioncam
