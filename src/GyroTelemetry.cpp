#include "GyroTelemetry.h"
#include "SourceInput.h"
#include "SourceMedia.h"
#include "gyroflow.pb.h"

#include <google/protobuf/util/json_util.h>
#include <motioncam/Decoder.hpp>

#include <algorithm>
#include <cmath>
#include <cctype>
#include <string_view>
#include <cstring>
#include <filesystem>
#include <limits>
#include <map>
#include <optional>
#include <set>
#include <stdexcept>

namespace motioncam {
namespace {
std::string lower(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(),
                   [](unsigned char c) { return char(std::tolower(c)); });
    return value;
}
size_t nearestFrame(const std::vector<int64_t>& timestamps, int64_t time) {
    const auto found = std::lower_bound(timestamps.begin(), timestamps.end(), time);
    size_t index = std::min<size_t>(found - timestamps.begin(), timestamps.size() - 1);
    if (index && std::abs(timestamps[index - 1] - time) < std::abs(timestamps[index] - time)) --index;
    return index;
}
std::string readText(const std::filesystem::path& path) {
    SourceInputStream input(path, std::ios::binary);
    if (!input) throw std::runtime_error("Could not open telemetry: " + path.string());
    std::string result((std::istreambuf_iterator<char>(input)), {});
    if (input.bad()) throw std::runtime_error("Could not read telemetry: " + path.string());
    return result;
}
void shiftTimes(nlohmann::json& value, double delta) {
    if (value.is_object()) {
        for (auto& item : value.items()) {
            if ((item.key() == "sampleTimestampUs" || item.key() == "startTimestampUs" ||
                 item.key() == "endTimestampUs") &&
                item.value().is_number())
                item.value() = item.value().get<double>() + delta;
            else
                shiftTimes(item.value(), delta);
        }
    } else if (value.is_array())
        for (auto& item : value) shiftTimes(item, delta);
}
void shiftFrameTimes(nlohmann::json& frame, double delta) {
    for (const char* name : {"imu", "quaternions", "ois", "ibis", "eis", "gps"}) {
        if (!frame.contains(name)) continue;
        for (auto& sample : frame[name])
            if (!sample.contains("sampleTimestampUs")) sample["sampleTimestampUs"] = 0.0;
    }
    // Proto3 omits scalar zeros. The implicit zero must be shifted too when
    // changing clocks, otherwise a frame with startTimestampUs=0 is corrupted.
    if (!frame.contains("startTimestampUs")) frame["startTimestampUs"] = 0.0;
    if (!frame.contains("endTimestampUs")) frame["endTimestampUs"] = 0.0;
    shiftTimes(frame, delta);
}
std::optional<double> videoClockOrigin(const nlohmann::json& header) {
    if (!header.contains("camera") || !header["camera"].contains("additionalData")) return {};
    auto extra = header["camera"]["additionalData"];
    if (extra.is_string()) extra = nlohmann::json::parse(extra.get<std::string>(), nullptr, false);
    if (extra.is_object() && extra.contains("motioncamVideoClockOriginUs"))
        return extra.at("motioncamVideoClockOriginUs").get<double>();
    return {};
}
double sensorStart(const nlohmann::json& frame) {
    // The protobuf timestamp is the first row's readout latch; Android's
    // SENSOR_TIMESTAMP denotes that row's exposure start.
    return frame.value("startTimestampUs", 0.0) - frame.value("exposureTimeUs", 0.0);
}
double frameCenter(const nlohmann::json& frame) {
    return (frame.value("startTimestampUs", 0.0) + frame.value("endTimestampUs", 0.0) -
            frame.value("exposureTimeUs", 0.0)) /
           2.0;
}
} // namespace

nlohmann::json GyroTelemetry::decodeProtobuf(const uint8_t* bytes, size_t size) {
    if (size > 64 * 1024 * 1024) throw std::runtime_error("Gyroflow protobuf message is too large");
    Main message;
    if (!message.ParseFromArray(bytes, static_cast<int>(size)))
        throw std::runtime_error("Invalid Gyroflow protobuf message");
    if ((!message.magic_string().empty() && message.magic_string() != "GyroflowProtobuf") ||
        message.protocol_version() > 1 || (!message.has_header() && !message.has_frame()))
        throw std::runtime_error("Unsupported Gyroflow protobuf message");
    std::string json;
    const auto status = google::protobuf::util::MessageToJsonString(message, &json);
    if (!status.ok()) throw std::runtime_error("Could not decode Gyroflow protobuf JSON");
    return nlohmann::json::parse(json);
}

void GyroTelemetry::readJsonl(const std::string& path) {
    SourceInputStream input(path);
    if (!input) throw std::runtime_error("Could not open JSONL telemetry: " + path);
    double base = 0;
    std::string line;
    bool first = true;
    bool recognized = false;
    std::vector<int64_t> videoTimestamps;
    size_t frameIndex = 0;
    while (std::getline(input, line)) {
        if (first && line.compare(0, 3, "\xef\xbb\xbf") == 0) line.erase(0, 3);
        first = false;
        if (line.find_first_not_of(" \r\t") == std::string::npos) continue;
        auto message = nlohmann::json::parse(line);
        if (!recognized && message.value("magicString", std::string{}) != "GyroflowProtobuf") return;
        recognized = true;
        if (message.value("magicString", std::string{}) != "GyroflowProtobuf" ||
            message.value("protocolVersion", 1) != 1)
            throw std::runtime_error("Unsupported JSONL telemetry: " + path);
        if (message.contains("header")) {
            const auto& header = message.at("header");
            if (mHeader.is_null()) mHeader = header;
            if (header.contains("camera") && header["camera"].contains("additionalData")) {
                auto extra = header["camera"]["additionalData"];
                if (extra.is_string())
                    extra = nlohmann::json::parse(extra.get<std::string>(), nullptr, false);
                base = extra.is_object() ? extra.value("clock_base_us", 0.0) : 0.0;
                if (extra.is_object() && extra.contains("videoFrameTimestampsNs")) {
                    videoTimestamps = extra["videoFrameTimestampsNs"].get<std::vector<int64_t>>();
                    if (!std::is_sorted(videoTimestamps.begin(), videoTimestamps.end()))
                        throw std::runtime_error("Invalid JSONL video timeline: " + path);
                }
            }
        }
        if (!message.contains("frame")) continue;
        auto& frame = message["frame"];
        const double captureTime = base + sensorStart(frame);
        if (!std::isfinite(captureTime)) throw std::runtime_error("Invalid telemetry timestamp");
        if (!mHaveClockOrigin) {
            mClockOriginUs = videoClockOrigin(mHeader).value_or(captureTime);
            mHaveClockOrigin = true;
        }
        shiftFrameTimes(frame, base - mClockOriginUs);
        if (!videoTimestamps.empty() && frameIndex >= videoTimestamps.size())
            throw std::runtime_error("JSONL video timeline has too few entries: " + path);
        const auto videoTime = videoTimestamps.empty()
                                   ? static_cast<int64_t>(std::llround((captureTime - mClockOriginUs) * 1000))
                                   : videoTimestamps[frameIndex];
        mFrames.push_back({std::move(message), videoTime});
        ++frameIndex;
    }
    if (input.bad()) throw std::runtime_error("Could not read JSONL telemetry: " + path);
    if (!videoTimestamps.empty() && frameIndex != videoTimestamps.size())
        throw std::runtime_error("JSONL video timeline has too many entries: " + path);
    if (recognized && (!mHeader.is_null() || !mFrames.empty())) mFormat = Format::Jsonl;
}

void GyroTelemetry::readContainer(const std::string& path) {
    struct Media {
        AVFormatContext* context = nullptr;
        AVPacket* packet = av_packet_alloc();
        ~Media() {
            av_packet_free(&packet);
            if (context) closeSourceMedia(&context);
        }
    } media;
    if (!media.packet) throw std::bad_alloc();
    if (openSourceMedia(&media.context, path) < 0)
        throw std::runtime_error("Could not open telemetry container: " + path);
    if (avformat_find_stream_info(media.context, nullptr) < 0)
        throw std::runtime_error("Could not inspect telemetry container");
    std::set<int> candidates, confirmed;
    std::map<size_t, std::string> additionalChunks;
    size_t expectedChunks = 0;
    int64_t videoStart = 0;
    for (unsigned i = 0; i < media.context->nb_streams; ++i) {
        const auto* stream = media.context->streams[i];
        if (stream->codecpar->codec_type == AVMEDIA_TYPE_DATA) candidates.insert(static_cast<int>(i));
        if (stream->codecpar->codec_type == AVMEDIA_TYPE_VIDEO && stream->start_time != AV_NOPTS_VALUE)
            videoStart = av_rescale_q(stream->start_time, stream->time_base, AVRational{1, 1000000000});
        if (stream->codecpar->codec_tag == MKTAG('g', 'y', 'r', 'f')) confirmed.insert(static_cast<int>(i));
    }
    if (candidates.empty() && confirmed.empty()) return;
    for (unsigned i = 0; i < media.context->nb_streams; ++i)
        if (!candidates.count(static_cast<int>(i)) && !confirmed.count(static_cast<int>(i)))
            media.context->streams[i]->discard = AVDISCARD_ALL;
    int status;
    while ((status = av_read_frame(media.context, media.packet)) >= 0) {
        auto* packet = media.packet;
        if (candidates.count(packet->stream_index) || confirmed.count(packet->stream_index)) {
            const std::string_view bytes(reinterpret_cast<const char*>(packet->data), packet->size);
            if (bytes.substr(0, 16) == "GYROFLOW IMU LOG" || bytes.substr(0, 14) == "CAMERA IMU LOG" ||
                media.context->streams[packet->stream_index]->codecpar->codec_tag ==
                    MKTAG('g', 'c', 's', 'v')) {
                mGcsv.append(bytes.data(), bytes.size());
                if (mFormat != Format::Jsonl) mFormat = Format::Gcsv;
            } else if (confirmed.count(packet->stream_index) ||
                       bytes.find("GyroflowProtobuf") != std::string_view::npos) {
                confirmed.insert(packet->stream_index);
                auto message = !bytes.empty() && bytes.front() == '{'
                                   ? nlohmann::json::parse(bytes)
                                   : decodeProtobuf(packet->data, packet->size);
                if (message.contains("header")) {
                    const auto& header = message["header"];
                    nlohmann::json extra;
                    if (header.contains("camera") && header["camera"].contains("additionalData")) {
                        extra = header["camera"]["additionalData"];
                        if (extra.is_string())
                            extra = nlohmann::json::parse(extra.get<std::string>(), nullptr, false);
                    }
                    if (extra.is_object() && extra.contains("motioncamTelemetryChunk")) {
                        const auto& chunk = extra.at("motioncamTelemetryChunk");
                        if (chunk.at("version") != 1 || !additionalChunks
                                                             .emplace(chunk.at("index").get<size_t>(),
                                                                      chunk.at("text").get<std::string>())
                                                             .second)
                            throw std::runtime_error("Invalid duplicate/unsupported telemetry chunk");
                        // Extension packets must not replace the main camera header.
                    } else {
                        if (extra.is_object() && extra.contains("motioncamTelemetryChunks")) {
                            const auto& descriptor = extra.at("motioncamTelemetryChunks");
                            if (descriptor.at("version") != 1 ||
                                !(expectedChunks = descriptor.at("count").get<size_t>()))
                                throw std::runtime_error("Invalid telemetry chunk descriptor");
                        }
                        mHeader = header;
                    }
                }
                if (message.contains("frame")) {
                    const auto pts = packet->pts != AV_NOPTS_VALUE ? packet->pts : packet->dts;
                    if (pts == AV_NOPTS_VALUE)
                        throw std::runtime_error("Gyroflow protobuf packet has no timestamp");
                    const auto timestamp =
                        av_rescale_q(pts, media.context->streams[packet->stream_index]->time_base,
                                     AVRational{1, 1000000000}) -
                        videoStart;
                    if (!mHaveClockOrigin) {
                        mClockOriginUs = videoClockOrigin(mHeader).value_or(frameCenter(message["frame"]) -
                                                                            timestamp / 1000.0);
                        mHaveClockOrigin = true;
                    }
                    shiftFrameTimes(message["frame"], -mClockOriginUs);
                    mFrames.push_back({std::move(message), timestamp});
                }
                mFormat = Format::Jsonl;
            }
        }
        av_packet_unref(packet);
    }
    if (status != AVERROR_EOF) throw std::runtime_error("Could not read protobuf telemetry packets");
    if (expectedChunks || !additionalChunks.empty()) {
        if (additionalChunks.size() != expectedChunks)
            throw std::runtime_error("Incomplete extended telemetry metadata");
        std::string restored;
        for (size_t i = 0; i < expectedChunks; ++i) {
            const auto found = additionalChunks.find(i);
            if (found == additionalChunks.end()) throw std::runtime_error("Missing telemetry metadata chunk");
            restored += found->second;
        }
        mHeader["camera"]["additionalData"] = std::move(restored);
    }
}

void GyroTelemetry::readRaw(const std::string& path) {
    Decoder decoder(decoderSource(path));
    const auto& timestamps = decoder.getFrames();
    if (timestamps.empty()) return;
    const auto& container = decoder.getContainerMetadata();
    if (container.contains("gyroflowProtobuf")) {
        for (const auto& record : container["gyroflowProtobuf"]) {
            const auto& payload = record.at("payload").get_binary();
            auto message = decodeProtobuf(payload.data(), payload.size());
            if (message.contains("header")) mHeader = message["header"];
            if (!message.contains("frame")) continue;
            const auto timestamp = record.at("timestampNs").get<int64_t>() - timestamps.front();
            if (!mHaveClockOrigin) {
                mClockOriginUs =
                    videoClockOrigin(mHeader).value_or(sensorStart(message["frame"]) - timestamp / 1000.0);
                mHaveClockOrigin = true;
            }
            shiftFrameTimes(message["frame"], -mClockOriginUs);
            mFrames.push_back({std::move(message), timestamp});
        }
        if (!mFrames.empty()) {
            mFormat = Format::Jsonl;
            return;
        }
    }
    std::vector<MotionSample> gyro;
    decoder.loadGyroData(gyro);
    const bool motionCam = lower(std::filesystem::path(path).extension().string()) != ".mcpro";
    if (mHeader.is_null()) mHeader = container.value("captureTelemetry", nlohmann::json::object());
    if (!mHeader.contains("camera")) mHeader["camera"] = nlohmann::json::object();
    auto& sourceClock = mHeader["camera"]["additionalData"];
    if (sourceClock.is_null()) sourceClock = nlohmann::json::object();
    if (motionCam && !sourceClock.is_object()) sourceClock = {{"sourceAdditionalData", sourceClock}};
    if (sourceClock.is_object()) {
        sourceClock["source_clock_origin_ns"] = timestamps.front();
        if (motionCam) sourceClock["motioncamContainerMetadata"] = container;
    }
    if (mHeader["camera"].value("imuOrientation", std::string{}).empty())
        mHeader["camera"]["imuOrientation"] = "XYZ";
    if (motionCam) {
        const auto& device = container.value("extraData", nlohmann::json::object())
                                 .value("postProcessSettings", nlohmann::json::object())
                                 .value("metadata", nlohmann::json::object());
        mHeader["camera"]["cameraBrand"] = device.value("build.manufacturer", std::string("MotionCam"));
        mHeader["camera"]["cameraModel"] =
            device.value("build.model", container.value("uniqueCameraModel", std::string{}));
        if (timestamps.size() > 1 && timestamps.back() > timestamps.front())
            mHeader["clip"]["fileFrameRate"] =
                (timestamps.size() - 1) * 1e9 / (timestamps.back() - timestamps.front());
    }
    std::vector<OisSample> ois;
    decoder.loadOisData(ois);
    std::stable_sort(ois.begin(), ois.end(),
                     [](const auto& a, const auto& b) { return a.timestampNs < b.timestampNs; });
    auto& camera = mHeader["camera"];
    if (motionCam) {
        // Physical sensor dimensions are millimeters; OIS shifts use full-array
        // pixels, even for a binned/cropped recording. Never infer pitch from the
        // output raster or relabel pixels as nanometers.
        const auto pixels = container.value("pixelArraySize", nlohmann::json::array());
        const auto physical = container.value("sensorPhysicalSize", nlohmann::json::array());
        if (pixels.is_array() && pixels.size() == 2 && physical.is_array() && physical.size() == 2) {
            for (size_t axis = 0; axis < 2; ++axis) {
                if (!pixels[axis].is_number() || !physical[axis].is_number()) continue;
                const auto count = pixels[axis].get<double>(), mm = physical[axis].get<double>();
                if (!(count >= 1 && count <= std::numeric_limits<uint32_t>::max() && mm > 0 &&
                      std::isfinite(count) && std::isfinite(mm)))
                    continue;
                camera[axis ? "sensorPixelHeight" : "sensorPixelWidth"] = static_cast<uint32_t>(count);
                const auto pitch = std::round(mm * 1e6 / count);
                if (pitch > 0 && pitch <= std::numeric_limits<uint32_t>::max())
                    camera[axis ? "pixelPitchYNm" : "pixelPitchXNm"] = static_cast<uint32_t>(pitch);
            }
        }
    }
    const auto pitchX = camera.value("pixelPitchXNm", 0.0), pitchY = camera.value("pixelPitchYNm", 0.0);
    const bool calibratedOis = pitchX > 0 && pitchY > 0 && std::isfinite(pitchX) && std::isfinite(pitchY);
    if (!ois.empty()) {
        if (!sourceClock.is_object()) sourceClock = {{"sourceAdditionalData", sourceClock}};
        // Also retain decoded native pixel units, including when physical sensor
        // calibration is unavailable. These are data, not usable nm calibration.
        auto& native = sourceClock["motioncamOis"];
        native = {{"unit", "active-array pixels"},
                  {"axes", "+X right, +Y down"},
                  {"samples", nlohmann::json::array()}};
        for (const auto& sample : ois)
            native["samples"].push_back(
                {{"sampleTimestampUs", (sample.timestampNs - timestamps.front()) / 1000.0},
                 {"shiftXPixels", sample.xPixels},
                 {"shiftYPixels", sample.yPixels}});
    }
    std::vector<MotionSample> acceleration;
    decoder.loadAccelerometerData(acceleration);
    std::sort(gyro.begin(), gyro.end(),
              [](const auto& a, const auto& b) { return a.timestampNs < b.timestampNs; });
    std::sort(acceleration.begin(), acceleration.end(),
              [](const auto& a, const auto& b) { return a.timestampNs < b.timestampNs; });
    size_t sampleIndex = 0, accelerationIndex = 0, oisIndex = 0;
    const double radiansToDegrees = 180.0 / std::acos(-1.0);
    for (size_t i = 0; i < timestamps.size(); ++i) {
        nlohmann::json metadata;
        decoder.loadFrameMetadata(timestamps[i], metadata);
        if (motionCam)
            sourceClock["motioncamFrameMetadata"].push_back(
                {{"timestampNs", timestamps[i]}, {"metadata", metadata}});
        auto frame = metadata.value("captureTelemetry", nlohmann::json::object());
        const auto relative = timestamps[i] - timestamps.front();
        const double exposure = metadata.value("exposureTime", 0.0) / 1000.0;
        const double readout =
            frame.value("readoutTimeUs", metadata.value("rollingShutterSkewNs", 0.0) / 1000.0);
        frame.erase("readoutTimeUs");
        frame["frameNumber"] = i + 1;
        frame["startTimestampUs"] = relative / 1000.0 + exposure;
        frame["endTimestampUs"] = relative / 1000.0 + exposure + readout;
        frame["exposureTimeUs"] = exposure;
        if (metadata.contains("iso")) frame["iso"] = metadata["iso"];
        if (motionCam) {
            nlohmann::json lens = nlohmann::json::object();
            for (const auto& mapping : {std::pair<const char*, const char*>{"focalLengths", "focalLengthMm"},
                                        {"apertures", "fNumber"}}) {
                if (container.contains(mapping.first) && container[mapping.first].is_array() &&
                    !container[mapping.first].empty())
                    lens[mapping.second] = container[mapping.first].front();
            }
            const auto focus = metadata.value("focusDistance", 0.0);
            if (focus > 0) lens["focusDistanceMm"] = 1000.0 / focus;
            if (!lens.empty()) frame["lens"] = nlohmann::json::array({lens});
        }
        if (motionCam) {
            const double bin = metadata.value("isBinned", false) ? 2.0 : 1.0;
            const auto w = metadata.value("width", 0.0) * bin, h = metadata.value("height", 0.0) * bin;
            const auto rawW = metadata.value("originalWidth", metadata.value("width", 0.0));
            const auto rawH = metadata.value("originalHeight", metadata.value("height", 0.0));
            if (w > 0 && h > 0 && w <= rawW && h <= rawH) {
                const auto active = container.value("preCorrectionActiveArrayRect", nlohmann::json::array());
                const double left = active.is_array() && active.size() == 4 ? active[0].get<double>() : 0;
                const double top = active.is_array() && active.size() == 4 ? active[1].get<double>() : 0;
                frame["cropX"] = left + (rawW - w) / 2;
                frame["cropY"] = top + (rawH - h) / 2;
                frame["cropWidth"] = w;
                frame["cropHeight"] = h;
            }
        }
        frame["imu"] = nlohmann::json::array();
        const auto end = i + 1 < timestamps.size() ? timestamps[i + 1] : std::numeric_limits<int64_t>::max();
        // Merge independently sampled sensors without dropping or resampling either stream.
        while (
            (sampleIndex < gyro.size() && gyro[sampleIndex].timestampNs < end) ||
            (accelerationIndex < acceleration.size() && acceleration[accelerationIndex].timestampNs < end)) {
            const auto gyroTime = sampleIndex < gyro.size() ? gyro[sampleIndex].timestampNs
                                                            : std::numeric_limits<int64_t>::max();
            const auto accelTime = accelerationIndex < acceleration.size()
                                       ? acceleration[accelerationIndex].timestampNs
                                       : std::numeric_limits<int64_t>::max();
            const auto time = std::min(gyroTime, accelTime);
            nlohmann::json imu = {{"sampleTimestampUs", (time - timestamps.front()) / 1000.0}};
            if (gyroTime == time) {
                const auto& sample = gyro[sampleIndex++];
                imu["gyroscopeX"] = sample.x * radiansToDegrees;
                imu["gyroscopeY"] = sample.y * radiansToDegrees;
                imu["gyroscopeZ"] = sample.z * radiansToDegrees;
            }
            if (accelTime == time) {
                const auto& sample = acceleration[accelerationIndex++];
                imu["accelerometerX"] = sample.x;
                imu["accelerometerY"] = sample.y;
                imu["accelerometerZ"] = sample.z;
            }
            frame["imu"].push_back(std::move(imu));
        }
        while (oisIndex < ois.size() && ois[oisIndex].timestampNs < end) {
            const auto& sample = ois[oisIndex++];
            if (calibratedOis)
                frame["ois"].push_back(
                    {{"sampleTimestampUs", (sample.timestampNs - timestamps.front()) / 1000.0},
                     {"shiftXNm", sample.xPixels * pitchX},
                     {"shiftYNm", sample.yPixels * pitchY}});
        }
        mFrames.push_back(
            {{{"magicString", "GyroflowProtobuf"}, {"protocolVersion", 1}, {"frame", frame}}, relative});
        if (i == 0) {
            mHeader["clip"]["frameWidth"] = metadata.value("width", 0);
            mHeader["clip"]["frameHeight"] = metadata.value("height", 0);
            mHeader["clip"]["frameReadoutTimeUs"] = readout;
        }
    }
    mFormat = Format::Jsonl;
}

std::shared_ptr<const GyroTelemetry> GyroTelemetry::load(const std::string& source,
                                                         const std::string& sidecarSource) {
    auto result = std::make_shared<GyroTelemetry>();
    // Archive imports read through a temporary source index, but authoritative
    // overrides remain beside the original archive. Resolve those first.
    if (!sidecarSource.empty() && sidecarSource != source) {
        auto jsonl = std::filesystem::path(sidecarSource);
        jsonl.replace_extension(".jsonl");
        auto gcsv = std::filesystem::path(sidecarSource);
        gcsv.replace_extension(".gcsv");
        if (std::filesystem::is_regular_file(jsonl)) result->readJsonl(jsonl.string());
        if (result->mFormat == Format::None && std::filesystem::is_regular_file(gcsv)) {
            result->mGcsv = readText(gcsv);
            result->mFormat = Format::Gcsv;
        }
        if (result->mFormat != Format::None) {
            std::stable_sort(result->mFrames.begin(), result->mFrames.end(),
                             [](const auto& a, const auto& b) { return a.timestampNs < b.timestampNs; });
            return result;
        }
    }
    const std::filesystem::path path(source);
    const auto suffix = lower(path.extension().string());
    const bool directory = std::filesystem::is_directory(path);
    if (directory || suffix == ".dng") {
        const auto root = directory ? path : path.parent_path();
        std::vector<std::filesystem::path> files, gcsvFiles;
        std::set<std::string> prefixes;
        for (const auto& item : std::filesystem::directory_iterator(root)) {
            if (!item.is_regular_file() || lower(item.path().extension().string()) != ".dng") continue;
            if (!directory && item.path().filename() != path.filename()) continue;
            const auto stem = item.path().stem().string();
            prefixes.insert(stem);
            const auto separator = stem.find_last_of("-_");
            if (separator != std::string::npos && separator + 1 < stem.size() &&
                std::all_of(stem.begin() + separator + 1, stem.end(),
                            [](unsigned char c) { return std::isdigit(c); }))
                prefixes.insert(stem.substr(0, separator));
        }
        for (const auto& item : std::filesystem::directory_iterator(root)) {
            if (!item.is_regular_file()) continue;
            const auto extension = lower(item.path().extension().string());
            if (extension != ".jsonl" && extension != ".gcsv") continue;
            const auto stem = item.path().stem().string();
            const bool matches = std::any_of(prefixes.begin(), prefixes.end(), [&](const auto& prefix) {
                if (stem == prefix || stem.compare(0, prefix.size() + 1, prefix + "-") == 0 ||
                    stem.compare(0, prefix.size() + 1, prefix + "_") == 0)
                    return true;
                // Projected DNG names can append the frame number directly.
                // Match their source companion even after the folder is renamed.
                return !stem.empty() && prefix.size() > stem.size() &&
                       prefix.compare(0, stem.size(), stem) == 0 &&
                       std::all_of(prefix.begin() + stem.size(), prefix.end(),
                                   [](unsigned char c) { return std::isdigit(c); });
            });
            if (matches) (extension == ".jsonl" ? files : gcsvFiles).push_back(item.path());
        }
        std::sort(files.begin(), files.end());
        // Finalized sequences use a directory-named companion; prefer it over
        // any old per-fragment logs retained beside the images.
        const auto companion = root / (root.filename().string() + ".jsonl");
        if (std::filesystem::is_regular_file(companion)) result->readJsonl(companion.string());
        if (result->mFormat == Format::None)
            for (const auto& file : files)
                if (file != companion) result->readJsonl(file.string());
        if (result->mFormat == Format::None) {
            std::vector<std::filesystem::path> candidates;
            if (!directory) {
                auto gcsv = path;
                gcsv.replace_extension(".gcsv");
                candidates.push_back(gcsv);
            }
            for (const auto& prefix : prefixes) candidates.push_back(root / (prefix + ".gcsv"));
            candidates.push_back(root / (root.filename().string() + ".gcsv"));
            std::sort(gcsvFiles.begin(), gcsvFiles.end());
            candidates.insert(candidates.end(), gcsvFiles.begin(), gcsvFiles.end());
            for (const auto& gcsv : candidates)
                if (std::filesystem::is_regular_file(gcsv)) {
                    result->mGcsv = readText(gcsv);
                    result->mFormat = Format::Gcsv;
                    break;
                }
        }
    } else {
        auto jsonl = path;
        jsonl.replace_extension(".jsonl");
        auto gcsv = path;
        gcsv.replace_extension(".gcsv");
        const bool video = suffix == ".mov" || suffix == ".mp4" || suffix == ".mkv";
        // Explicit sidecars override embedded/native telemetry for every input
        // format. JSONL wins when both sidecar formats are present.
        if (std::filesystem::is_regular_file(jsonl)) result->readJsonl(jsonl.string());
        if (result->mFormat == Format::None && std::filesystem::is_regular_file(gcsv)) {
            result->mGcsv = readText(gcsv);
            result->mFormat = Format::Gcsv;
        }
        if (result->mFormat == Format::None && video) result->readContainer(source);
        if (result->mFormat == Format::None &&
            (suffix == ".mcraw" || suffix == ".mcpro" || suffix == ".unspektra"))
            result->readRaw(source);
    }
    std::stable_sort(result->mFrames.begin(), result->mFrames.end(),
                     [](const auto& a, const auto& b) { return a.timestampNs < b.timestampNs; });
    return result;
}

std::string GyroTelemetry::extension() const {
    return mFormat == Format::Jsonl ? ".jsonl" : mFormat == Format::Gcsv ? ".gcsv" : "";
}
const nlohmann::json* GyroTelemetry::frame(size_t index) const {
    return index < mFrames.size() ? &mFrames[index].message.at("frame") : nullptr;
}
std::vector<int64_t> GyroTelemetry::frameTimestamps(size_t expectedFrameCount) const {
    if (mFormat != Format::Jsonl || mFrames.empty() || mFrames.size() != expectedFrameCount) return {};
    std::vector<int64_t> timestamps;
    timestamps.reserve(mFrames.size());
    const auto origin = mFrames.front().timestampNs;
    for (const auto& frame : mFrames) {
        if (frame.timestampNs < origin ||
            (origin < 0 && frame.timestampNs > std::numeric_limits<int64_t>::max() + origin))
            return {};
        const auto time = frame.timestampNs - origin;
        if (!timestamps.empty() && time <= timestamps.back()) return {};
        timestamps.push_back(time);
    }
    return timestamps;
}

std::vector<GyroTelemetry::Frame> GyroTelemetry::outputFrames(const std::vector<int64_t>& timestamps) const {
    if (timestamps.empty()) return mFrames;
    if (!std::is_sorted(timestamps.begin(), timestamps.end()) ||
        std::adjacent_find(timestamps.begin(), timestamps.end()) != timestamps.end())
        throw std::runtime_error("Telemetry output timestamps must be strictly increasing");
    std::vector<Frame> output;
    if (mFrames.empty()) return output;
    std::vector<std::vector<size_t>> buckets(timestamps.size());
    for (size_t i = 0; i < mFrames.size(); ++i) {
        buckets[nearestFrame(timestamps, mFrames[i].timestampNs)].push_back(i);
    }
    size_t sourceIndex = 0;
    for (size_t i = 0; i < timestamps.size(); ++i) {
        if (!buckets[i].empty()) sourceIndex = buckets[i].front();
        auto message = mFrames[sourceIndex].message;
        auto& frame = message["frame"];
        frame["frameNumber"] = i + 1;
        for (const char* name : {"imu", "quaternions", "ois", "ibis", "eis", "gps"}) {
            frame.erase(name);
            for (const auto source : buckets[i]) {
                const auto& original = mFrames[source].message["frame"];
                if (original.contains(name))
                    for (const auto& sample : original[name]) frame[name].push_back(sample);
            }
        }
        output.push_back({std::move(message), timestamps[i]});
    }
    return output;
}

nlohmann::json GyroTelemetry::outputHeader(const std::vector<int64_t>& timestamps, double fps, int width,
                                           int height, int orientation) const {
    auto header = mHeader;
    if (header.is_null()) header = nlohmann::json::object();
    if (fps > 0) header["clip"]["fileFrameRate"] = fps;
    if (width > 0) header["clip"]["frameWidth"] = width;
    if (height > 0) header["clip"]["frameHeight"] = height;
    if (orientation >= 0) header["clip"]["rotationDegrees"] = orientation;
    if (header.contains("camera") && header["camera"].contains("additionalData")) {
        auto& extra = header["camera"]["additionalData"];
        if (extra.is_string()) {
            const auto parsed = nlohmann::json::parse(extra.get<std::string>(), nullptr, false);
            if (parsed.is_object()) extra = parsed;
        }
        if (extra.is_object()) {
            if (extra.contains("clock_base_us") && !extra.contains("source_clock_base_us"))
                extra["source_clock_base_us"] = extra["clock_base_us"];
            if (mHaveClockOrigin && !extra.contains("source_clock_origin_us"))
                extra["source_clock_origin_us"] = mClockOriginUs;
            extra["clock_base_us"] = 0;
        }
    }
    if (!mFrames.empty()) {
        // Capture timestamps belong to the original exposure. Held/interpolated
        // output images have a separate video timeline, which JSONL must retain
        // just as a container retains it in packet PTS.
        auto& extra = header["camera"]["additionalData"];
        if (!extra.is_object()) extra = {{"sourceAdditionalData", extra}};
        // Capture and sensor timestamps are already relative to the video clock.
        extra["motioncamVideoClockOriginUs"] = 0;
        auto& videoTimes = extra["videoFrameTimestampsNs"] = nlohmann::json::array();
        if (!timestamps.empty())
            videoTimes = timestamps;
        else
            for (const auto& source : mFrames) videoTimes.push_back(source.timestampNs);
    }
    std::set<size_t> assigned;
    const bool mergedFrames =
        !timestamps.empty() && std::any_of(mFrames.begin(), mFrames.end(), [&](const auto& source) {
            return !assigned.insert(nearestFrame(timestamps, source.timestampNs)).second;
        });
    if (mergedFrames) {
        // A downsampled video cannot show every source exposure. Retain their
        // camera metadata in the schema's extensible header; motion samples are
        // merged into the corresponding output packets below, never discarded.
        auto& extra = header["camera"]["additionalData"];
        if (!extra.is_object()) extra = {{"sourceAdditionalData", extra}};
        if (!extra.contains("sourceFrameMetadata")) {
            auto& history = extra["sourceFrameMetadata"] = nlohmann::json::array();
            for (const auto& source : mFrames) {
                auto metadata = source.message["frame"];
                for (const char* name : {"imu", "quaternions", "ois", "ibis", "eis", "gps"})
                    metadata.erase(name);
                history.push_back({{"videoTimestampNs", source.timestampNs}, {"frame", metadata}});
            }
        }
    }
    return header;
}

std::string GyroTelemetry::serialize(const std::vector<int64_t>& timestamps, double fps, int width,
                                     int height, int orientation) const {
    if (mFormat == Format::Gcsv) return mGcsv;
    if (mFormat == Format::None) return {};
    const auto rendered = outputFrames(timestamps);
    const auto header = outputHeader(timestamps, fps, width, height, orientation);
    std::string output =
        nlohmann::json{{"magicString", "GyroflowProtobuf"}, {"protocolVersion", 1}, {"header", header}}
            .dump() +
        '\n';
    for (const auto& frame : rendered) {
        auto message = frame.message;
        message.erase("header");
        output += message.dump() + '\n';
    }
    return output;
}
} // namespace motioncam
