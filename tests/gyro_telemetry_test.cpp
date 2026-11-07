#include "GyroTelemetry.h"
#include "ArchiveImport.h"
#include "gyroflow.pb.h"
#include <motioncam/Container.hpp>
#include <motioncam/Decoder.hpp>
#include <motioncam/MotionSample.hpp>
#include <QCoreApplication>
#include <QDir>
#include <QTemporaryDir>
#include <archive.h>
#include <archive_entry.h>
extern "C" {
#include <libavformat/avformat.h>
#include <libavutil/pixfmt.h>
}
#include <cstring>
#include <cmath>
#include <limits>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <stdexcept>

namespace {
using Json = nlohmann::json;
void require(bool value, const char* text) {
    if (!value) throw std::runtime_error(text);
}
void write(const std::filesystem::path& path, const std::string& text) {
    std::ofstream out(path, std::ios::binary);
    out.write(text.data(), text.size());
    require(bool(out), "write fixture");
}
void makeRaw(const std::filesystem::path& path, bool calibration = true, bool badOisCount = false,
             bool nonFiniteOis = false) {
    using namespace motioncam;
    std::ofstream out(path, std::ios::binary);
    auto put = [&](const auto& value) { out.write(reinterpret_cast<const char*>(&value), sizeof(value)); };
    auto item = [&](Type type, const std::string& bytes) {
        put(Item{type, static_cast<uint32_t>(bytes.size())});
        out.write(bytes.data(), bytes.size());
    };
    motioncam::Header h{};
    std::memcpy(h.ident, CONTAINER_ID, 7);
    h.version = CONTAINER_VERSION;
    put(h);
    Json camera = {{"model", "Native camera"}, {"calibration", {1, 2, 3}}};
    if (calibration) {
        camera["pixelArraySize"] = {32, 64};
        camera["sensorPhysicalSize"] = {0.032, 0.128};
        camera["preCorrectionActiveArrayRect"] = {2, 4, 34, 68};
    }
    item(Type::METADATA, camera.dump());
    const auto offset = static_cast<int64_t>(out.tellp());
    item(Type::BUFFER, "unused pixels");
    item(Type::METADATA, Json{{"width", 16},
                              {"height", 16},
                              {"iso", 200},
                              {"exposureTime", 1000000},
                              {"lensState", "preserve"},
                              {"isBinned", true},
                              {"originalWidth", 32},
                              {"originalHeight", 64},
                              {"rollingShutterSkewNs", 2000000}}
                             .dump());
    auto motion = [&](Type type, int64_t timestamp, float x) {
        const auto position = static_cast<int64_t>(out.tellp());
        put(Item{type, static_cast<uint32_t>(sizeof(GyroDataHeader) + sizeof(MotionSample))});
        put(GyroDataHeader{1, 1});
        put(MotionSample{timestamp, x, 2, 3, 0});
        return position;
    };
    const auto gyro = motion(Type::GYRO_DATA, 1000000100, 1);
    put(Item{Type::GYRO_INDEX, static_cast<uint32_t>(sizeof(GyroIndex) + sizeof(BufferOffset))});
    put(GyroIndex{1, 1});
    put(BufferOffset{gyro, 1000000100});
    item(Type::OIS_DATA, std::string("\x01\x00\xfe", 3));
    put(Item{Type::OIS_DATA, static_cast<uint32_t>(8 + 2 * sizeof(OisSample))});
    put(uint32_t{1});
    put(uint32_t{badOisCount ? 3u : 2u});
    put(OisSample{999999900, nonFiniteOis ? std::numeric_limits<float>::quiet_NaN() : 0.25f, -0.5f});
    put(OisSample{1000000300, -1.0f, 2.0f});
    const auto accel = motion(Type::ACCELEROMETER_DATA, 1000000200, 9);
    put(Item{Type::ACCELEROMETER_INDEX,
             static_cast<uint32_t>(sizeof(AccelerometerIndex) + sizeof(BufferOffset))});
    put(AccelerometerIndex{1, 1});
    put(BufferOffset{accel, 1000000200});
    put(Item{Type::BUFFER_INDEX_DATA, sizeof(BufferOffset)});
    const auto indexOffset = static_cast<int64_t>(out.tellp());
    put(BufferOffset{offset, 1000000000});
    put(Item{Type::BUFFER_INDEX, sizeof(BufferIndex)});
    put(BufferIndex{static_cast<int32_t>(INDEX_MAGIC_NUMBER), 1, indexOffset});
    require(bool(out), "write MCRAW fixture");
}
void makeVideo(const std::filesystem::path& path) {
    AVFormatContext* format = nullptr;
    require(avformat_alloc_output_context2(&format, nullptr, "mov", path.string().c_str()) >= 0 && format,
            "allocate video fixture");
    auto* stream = avformat_new_stream(format, nullptr);
    require(stream, "video fixture stream");
    stream->codecpar->codec_type = AVMEDIA_TYPE_VIDEO;
    stream->codecpar->codec_id = AV_CODEC_ID_RAWVIDEO;
    stream->codecpar->codec_tag = MKTAG('r', 'a', 'w', ' ');
    stream->codecpar->format = AV_PIX_FMT_RGB24;
    stream->codecpar->width = 16;
    stream->codecpar->height = 16;
    stream->codecpar->bits_per_coded_sample = 24;
    stream->time_base = {1, 30};
    require(avio_open(&format->pb, path.string().c_str(), AVIO_FLAG_WRITE) >= 0, "open video fixture");
    require(avformat_write_header(format, nullptr) >= 0, "write video fixture header");
    auto* packet = av_packet_alloc();
    require(packet, "video fixture packet");
    for (int i = 0; i < 3; ++i) {
        require(av_new_packet(packet, 16 * 16 * 3) >= 0, "allocate video frame");
        std::memset(packet->data, 64 + i, packet->size);
        packet->pts = packet->dts = av_rescale_q(i, AVRational{1, 30}, stream->time_base);
        packet->duration = av_rescale_q(1, AVRational{1, 30}, stream->time_base);
        packet->stream_index = stream->index;
        packet->flags = AV_PKT_FLAG_KEY;
        require(av_interleaved_write_frame(format, packet) >= 0, "write video fixture frame");
        av_packet_unref(packet);
    }
    av_packet_free(&packet);
    require(av_write_trailer(format) >= 0, "finish video fixture");
    avio_closep(&format->pb);
    avformat_free_context(format);
}
Json header(int64_t base) {
    return {{"camera",
             {{"cameraBrand", "Test"},
              {"cameraModel", "Camera"},
              {"imuOrientation", "XYZ"},
              {"additionalData", {{"clock_base_us", base}, {"preserve", "rich metadata"}}}}},
            {"clip",
             {{"frameWidth", 16}, {"frameHeight", 16}, {"fileFrameRate", 30}, {"frameReadoutTimeUs", 1000}}}};
}
Json frame(int number, double start, double gyro) {
    return {{"magicString", "GyroflowProtobuf"},
            {"protocolVersion", 1},
            {"frame",
             {{"frameNumber", number},
              {"startTimestampUs", start},
              {"endTimestampUs", start + 1000},
              {"exposureTimeUs", 2000},
              {"iso", 400},
              {"lens", Json::array({{{"focalLengthMm", 6.06},
                                     {"fNumber", 1.6},
                                     {"focusDistanceMm", 500},
                                     {"opencvStandard", {{"coefficients", {0, 0, 0, 0, 0}}}}}})},
              {"imu",
               Json::array(
                   {{{"sampleTimestampUs", start - 2000}, {"gyroscopeX", gyro}, {"accelerometerY", 9.81}}})},
              {"quaternions", Json::array({{{"sampleTimestampUs", start}, {"quat", {{"w", 1.0}}}}})}}}};
}
std::string jsonl(const Json& h, const std::vector<Json>& frames) {
    std::string result =
        Json{{"magicString", "GyroflowProtobuf"}, {"protocolVersion", 1}, {"header", h}}.dump() + '\n';
    for (const auto& frame : frames) result += frame.dump() + '\n';
    return result;
}
void zip(const std::filesystem::path& path, const std::string& content) {
    auto* output = archive_write_new();
    require(output, "allocate zip");
    if (path.extension() == ".7z")
        archive_write_set_format_7zip(output);
    else
        archive_write_set_format_zip(output);
    require(archive_write_open_filename(output, path.string().c_str()) == ARCHIVE_OK, "open zip");
    for (const auto& file : std::vector<std::pair<std::string, std::string>>{
             {"clip-00000000.dng", "fixture"}, {"clip-00000000.jsonl", content}}) {
        auto* entry = archive_entry_new();
        archive_entry_set_pathname(entry, file.first.c_str());
        archive_entry_set_filetype(entry, AE_IFREG);
        archive_entry_set_perm(entry, 0600);
        archive_entry_set_size(entry, file.second.size());
        require(archive_write_header(output, entry) == ARCHIVE_OK, "zip header");
        require(archive_write_data(output, file.second.data(), file.second.size()) ==
                    static_cast<la_ssize_t>(file.second.size()),
                "zip data");
        archive_entry_free(entry);
    }
    require(archive_write_close(output) == ARCHIVE_OK, "close zip");
    archive_write_free(output);
}
} // namespace
int main(int argc, char** argv) {
    // Android console tests have no JVM; QCoreApplication initializes JNI.
#ifndef __ANDROID__
    QCoreApplication app(argc, argv);
#endif
    try {
        QTemporaryDir temporary;
        require(temporary.isValid(), "temporary directory");
        const auto root = std::filesystem::path(temporary.path().toStdString());
        std::filesystem::create_directory(root / "sequence");
        write(root / "sequence" / "clip-00000000.dng", "fixture");
        write(root / "sequence" / "clip-00000002.dng", "fixture");
        write(root / "sequence" / "clip-00000000.jsonl", jsonl(header(1000000), {frame(1, 500, 1.25)}));
        write(root / "sequence" / "clip-00000002.jsonl", jsonl(header(1066667), {frame(2, 500, 2.5)}));
        auto rich = motioncam::GyroTelemetry::load((root / "sequence").string());
        require(rich->format() == motioncam::GyroTelemetry::Format::Jsonl, "JSONL format");
        require(rich->frames().size() == 2 && rich->frames()[1].timestampNs == 66667000,
                "clock base alignment");
        require(rich->frame(0)->at("imu")[0].at("gyroscopeX") == 1.25, "gyro retained");
        write(root / "sequence" / "unrelated.jsonl", jsonl(header(0), {frame(1, 0, 999)}));
        require(motioncam::GyroTelemetry::load((root / "sequence").string())->frames().size() == 2,
                "unrelated JSONL ignored");
        write(root / "sequence" / "sequence.jsonl", jsonl(header(0), {frame(1, 0, 777)}));
        const auto finalized = motioncam::GyroTelemetry::load((root / "sequence").string());
        require(finalized->frames().size() == 1 && finalized->frame(0)->at("imu")[0]["gyroscopeX"] == 777,
                "finalized directory companion overrides old fragments on reimport");
        std::filesystem::remove(root / "sequence" / "sequence.jsonl");
        write(root / "frame-only.jsonl", frame(1, 0, 12).dump() + '\n');
        require(motioncam::GyroTelemetry::load((root / "frame-only.jsonl").string())->frames().size() == 1,
                "frame-only JSONL recognized as telemetry");
        auto gpsFrame = frame(1, 0, 12);
        gpsFrame["gps"] = Json::array({{{"latitude", 51.5}, {"longitude", 13.4}}});
        write(root / "gps.jsonl", jsonl(header(0), {gpsFrame}));
        const auto gpsTelemetry = motioncam::GyroTelemetry::load((root / "gps.jsonl").string());
        require(gpsTelemetry->serialize().find("latitude") != std::string::npos,
                "GPS retained by default");
        const auto privateTelemetry = gpsTelemetry->serialize({}, 0, 0, 0, -1, true);
        require(privateTelemetry.find("latitude") == std::string::npos &&
                privateTelemetry.find("gyroscopeX") != std::string::npos,
                "GPS excluded while gyro retained");
        const auto merged = rich->serialize({0, 33333333, 66667000}, 30);
        write(root / "held.jsonl", merged);
        const auto held = motioncam::GyroTelemetry::load((root / "held.jsonl").string());
        require(held->frames().size() == 3 && held->frames()[1].timestampNs == 33333333 &&
                    held->frames()[2].timestampNs == 66667000,
                "JSONL preserves video timing independently of held exposures");
        std::istringstream lines(merged);
        std::string line;
        std::getline(lines, line);
        auto h = Json::parse(line);
        require(h["header"]["camera"]["additionalData"]["clock_base_us"] == 0, "normalized clock base");
        std::getline(lines, line);
        auto f = Json::parse(line);
        require(f["frame"].contains("quaternions") && f["frame"].contains("lens"), "rich fields retained");
        std::getline(lines, line);
        require(Json::parse(line)["frame"]["frameNumber"] == 2, "held output frame mapping");
        std::getline(lines, line);
        require(Json::parse(line)["frame"]["frameNumber"] == 3, "source output frame mapping");
        require(held->frameTimestamps(3) == std::vector<int64_t>({0, 33333333, 66667000}),
                "finalized presentation timeline takes precedence over held exposure timestamps");
        auto changedExposure = frame(2, 34000, 2);
        changedExposure["frame"]["exposureTimeUs"] = 6000;
        write(root / "sensor-timing.jsonl",
              jsonl(header(1000000), {frame(1, 0, 1), changedExposure, frame(3, 66000, 3)}));
        const auto sensorTiming = motioncam::GyroTelemetry::load((root / "sensor-timing.jsonl").string());
        require(sensorTiming->frameTimestamps(3) == std::vector<int64_t>({0, 30000000, 66000000}),
                "protobuf timing uses sensor exposure start rather than variable exposure center");
        require(sensorTiming->frames()[1].timestampNs == 30000000 &&
                    sensorTiming->frame(0)->at("imu")[0]["sampleTimestampUs"] == 0,
                "capture and IMU clocks share the first sensor exposure origin");
        require(sensorTiming->frameTimestamps(4).empty(), "incomplete protobuf timing permits CSV fallback");
        auto duplicateExposure = frame(2, 0, 2);
        write(root / "invalid-timing.jsonl", jsonl(header(0), {frame(1, 0, 1), duplicateExposure}));
        require(motioncam::GyroTelemetry::load((root / "invalid-timing.jsonl").string())
                    ->frameTimestamps(2)
                    .empty(),
                "non-increasing protobuf capture timing permits CSV fallback");
        auto implicitZero = frame(1, 0, 1);
        implicitZero["frame"].erase("startTimestampUs");
        auto notesHeader = header(0);
        notesHeader["camera"]["additionalData"] = "Plain text notes are valid protobuf strings";
        write(root / "implicit-zero.jsonl", jsonl(notesHeader, {implicitZero}));
        const auto zero = motioncam::GyroTelemetry::load((root / "implicit-zero.jsonl").string());
        require(zero->frame(0)->at("startTimestampUs") == 2000, "implicit proto3 zero is clock-rebased");
        require(zero->serialize().find("Plain text notes") != std::string::npos,
                "plain additional data preserved");
        auto implicitSample = frame(1, 1000, 1);
        implicitSample["frame"]["imu"][0].erase("sampleTimestampUs");
        implicitSample["frame"]["quaternions"][0].erase("sampleTimestampUs");
        write(root / "implicit-sample.jsonl", jsonl(header(1000000), {implicitSample}));
        const auto sampleZero = motioncam::GyroTelemetry::load((root / "implicit-sample.jsonl").string());
        require(sampleZero->frame(0)->at("imu")[0]["sampleTimestampUs"] == 1000 &&
                    sampleZero->frame(0)->at("quaternions")[0]["sampleTimestampUs"] == 1000,
                "implicit proto3 sample timestamps are rebased with explicit timestamps");
        const auto renamed = root / "renamed-finalized";
        std::filesystem::create_directory(renamed);
        write(renamed / "clip-mounted000000.dng", "fixture");
        write(renamed / "clip-mounted000001.dng", "fixture");
        const auto csvCompanion = "GYROFLOW IMU LOG\nversion,1.2\nt,gx,gy,gz\n0,1,2,3\n";
        write(renamed / "clip-mounted.gcsv", csvCompanion);
        write(renamed / "clip-mounted.jsonl", jsonl(header(0), {frame(1, 0, 42)}));
        require(motioncam::GyroTelemetry::load(renamed.string())->frame(0)->at("imu")[0]["gyroscopeX"] == 42,
                "projected DNG source companion survives a directory rename");
        std::filesystem::remove(renamed / "clip-mounted.jsonl");
        require(motioncam::GyroTelemetry::load(renamed.string())->serialize() == csvCompanion,
                "projected DNG GCSV companion survives a directory rename");
        const auto zipPath = root / "capture.zip";
        zip(zipPath, jsonl(header(1000000), {frame(1, 500, 3.75)}));
        const auto imported = motioncam::extractReviewArchive(QString::fromStdString(zipPath.string()));
        auto zipped = motioncam::GyroTelemetry::load(imported.sourcePath.toStdString());
        require(zipped->format() == motioncam::GyroTelemetry::Format::Jsonl && zipped->frames().size() == 1,
                "ZIP-backed JSONL");
        write(root / "capture.gcsv", "GYROFLOW IMU LOG\nversion,1.2\nt,gx,gy,gz\n0,9,8,7\n");
        const auto archiveCsv =
            motioncam::GyroTelemetry::load(imported.sourcePath.toStdString(), zipPath.string());
        require(archiveCsv->extension() == ".gcsv", "GCSV beside ZIP overrides archived JSONL");
        write(root / "capture.jsonl", jsonl(header(0), {frame(1, 0, 888)}));
        const auto archiveJsonl =
            motioncam::GyroTelemetry::load(imported.sourcePath.toStdString(), zipPath.string());
        require(archiveJsonl->frame(0)->at("imu")[0]["gyroscopeX"] == 888,
                "JSONL beside ZIP overrides archived JSONL and GCSV");
        std::filesystem::remove(root / "capture.gcsv");
        std::filesystem::remove(root / "capture.jsonl");
        const auto sevenZip = root / "capture.7z";
        zip(sevenZip, jsonl(header(0), {frame(1, 0, 12)}));
        const auto extracted7z = motioncam::extractReviewArchive(QString::fromStdString(sevenZip.string()));
        write(root / "capture.gcsv", "GYROFLOW IMU LOG\nversion,1.2\nt,gx,gy,gz\n0,9,8,7\n");
        require(motioncam::GyroTelemetry::load(extracted7z.sourcePath.toStdString(), sevenZip.string())
                        ->extension() == ".gcsv",
                "GCSV beside 7z overrides extracted telemetry");
        write(root / "capture.jsonl", jsonl(header(0), {frame(1, 0, 999)}));
        require(motioncam::GyroTelemetry::load(extracted7z.sourcePath.toStdString(), sevenZip.string())
                        ->frame(0)
                        ->at("imu")[0]["gyroscopeX"] == 999,
                "JSONL beside 7z overrides extracted telemetry");
        std::filesystem::remove(root / "capture.gcsv");
        std::filesystem::remove(root / "capture.jsonl");
        motioncam::removeReviewArchive(extracted7z.temporaryRoot);
        motioncam::removeReviewArchive(imported.temporaryRoot);
        makeRaw(root / "native.mcraw");
        auto raw = motioncam::GyroTelemetry::load((root / "native.mcraw").string());
        require(raw->extension() == ".jsonl", "native MCRAW exports JSONL");
        const auto& imu = raw->frames().front().message["frame"]["imu"];
        require(imu.size() == 2 && imu[0].contains("gyroscopeX") && !imu[0].contains("accelerometerX") &&
                    imu[1]["accelerometerX"] == 9 && !imu[1].contains("gyroscopeX"),
                "independent IMU timestamps retained");
        require(std::abs(imu[0]["gyroscopeX"].get<double>() - 180.0 / std::acos(-1.0)) < 1e-6,
                "gyro converted to protobuf degrees per second");
        const auto& extra = raw->header()["camera"]["additionalData"];
        require(extra["motioncamContainerMetadata"]["motioncamOisRecords"][0]["payload"] ==
                    Json::binary(std::vector<uint8_t>{1, 0, 254}),
                "opaque OIS preserved");
        require(extra["motioncamFrameMetadata"][0]["metadata"]["lensState"] == "preserve",
                "native frame metadata preserved");
        {
            motioncam::Decoder decoder((root / "native.mcraw").string());
            std::vector<motioncam::OisSample> samples{{123, 4, 5}};
            decoder.loadOisData(samples);
            require(samples.size() == 3 && samples[0].timestampNs == 123 &&
                        samples[1].timestampNs == 999999900 && samples[1].xPixels == 0.25f &&
                        samples[1].yPixels == -0.5f,
                    "decoder appends native OIS samples exactly");
        }
        const auto& rawFrame = raw->frames().front().message["frame"];
        require(rawFrame["ois"].size() == 2 && rawFrame["ois"][0]["sampleTimestampUs"] == -0.1 &&
                    rawFrame["ois"][0]["shiftXNm"] == 250 && rawFrame["ois"][0]["shiftYNm"] == -1000 &&
                    rawFrame["ois"][1]["shiftYNm"] == 4000,
                "OIS clock, signs, anisotropic pitch and tail samples");
        require(rawFrame["cropX"] == 2 && rawFrame["cropY"] == 20 && rawFrame["cropWidth"] == 32 &&
                    rawFrame["cropHeight"] == 32,
                "OIS full-array crop and binning geometry");
        require(rawFrame["endTimestampUs"].get<double>() - rawFrame["startTimestampUs"].get<double>() == 2000,
                "MCRAW rolling shutter mapped");
        makeRaw(root / "uncalibrated.mcraw", false);
        auto uncalibrated = motioncam::GyroTelemetry::load((root / "uncalibrated.mcraw").string());
        require(!uncalibrated->frames()[0].message["frame"].contains("ois") &&
                    uncalibrated->header()["camera"]["additionalData"]["motioncamOis"]["samples"].size() == 2,
                "uncalibrated OIS remains decoded pixels, never invented nanometers");
        for (bool nonFinite : {false, true}) {
            makeRaw(root / "invalid-ois.mcraw", true, !nonFinite, nonFinite);
            motioncam::Decoder decoder((root / "invalid-ois.mcraw").string());
            std::vector<motioncam::OisSample> samples{{123, 4, 5}};
            bool failed = false;
            try {
                decoder.loadOisData(samples);
            } catch (const motioncam::IOException&) {
                failed = true;
            }
            require(failed && samples.size() == 1, "invalid OIS rejected without modifying output");
        }
        makeVideo(root / "raw-native.mov");
        raw->embed((root / "raw-native.mov").string(), {0, 33333333, 66666667}, 30, 16, 16, 90);
        auto rawRoundtrip = motioncam::GyroTelemetry::load((root / "raw-native.mov").string());
        const auto checkCaptureClock = [&](const auto& restored) {
            const auto* capture = restored->frame(0);
            require(capture && capture->at("startTimestampUs") == rawFrame["startTimestampUs"] &&
                        capture->at("endTimestampUs") == rawFrame["endTimestampUs"],
                    "capture timestamps survive export without rebasing");
            for (const char* sensor : {"imu", "ois"}) {
                require(capture->at(sensor).size() == rawFrame[sensor].size(),
                        "sensor sample count retained");
                for (size_t i = 0; i < rawFrame[sensor].size(); ++i)
                    require(std::abs(capture->at(sensor)[i]["sampleTimestampUs"].template get<double>() -
                                     rawFrame[sensor][i]["sampleTimestampUs"].get<double>()) < 1e-9,
                            "sensor timestamps survive export without rebasing");
            }
        };
        checkCaptureClock(rawRoundtrip);
        require(rawRoundtrip->serialize().find("motioncamOisRecords") != std::string::npos,
                "native MCRAW extras survive protobuf embedding");
        require(rawRoundtrip->header()["clip"]["rotationDegrees"] == 90,
                "OIS axes independent of output rotation");
        size_t oisCount = 0;
        for (const auto& frame : rawRoundtrip->frames())
            if (frame.message["frame"].contains("ois")) {
                for (const auto& sample : frame.message["frame"]["ois"]) {
                    if (!oisCount)
                        require(sample["shiftXNm"] == 250 && sample["shiftYNm"] == -1000,
                                "OIS shifts survive protobuf embedding");
                    ++oisCount;
                }
            }
        require(oisCount == 2, "held output frames do not repeat or lose OIS samples");
        write(root / "raw-jsonl.jsonl", raw->serialize());
        auto rawJsonl = motioncam::GyroTelemetry::load((root / "raw-jsonl.jsonl").string());
        require(rawJsonl->frames()[0].message["frame"]["ois"].size() == 2, "OIS JSONL readback");
        checkCaptureClock(rawJsonl);
        write(root / "raw-jsonl-twice.jsonl", rawJsonl->serialize());
        checkCaptureClock(motioncam::GyroTelemetry::load((root / "raw-jsonl-twice.jsonl").string()));
        write(root / "legacy.mcraw", "unused fixture");
        const std::string gcsv =
            "GYROFLOW IMU "
            "LOG\nversion,1.2\nid,original\norientation,XYZ\ntscale,1\ngscale,1\nt,gx,gy,gz\n0,1,2,3\n";
        write(root / "legacy.gcsv", gcsv);
        auto legacy = motioncam::GyroTelemetry::load((root / "legacy.mcraw").string());
        require(legacy->extension() == ".gcsv" && legacy->serialize() == gcsv, "GCSV copied byte-for-byte");
        for (const char* suffix : {".mcraw", ".mcpro", ".unspektra", ".mov", ".mp4", ".mkv"}) {
            const auto path = root / (std::string("override") + suffix);
            write(path, "Sidecars must override without reading the clip");
            write(root / "override.gcsv", gcsv);
            require(motioncam::GyroTelemetry::load(path.string())->serialize() == gcsv,
                    "GCSV override for every clip type");
            write(root / "override.jsonl", raw->serialize());
            require(motioncam::GyroTelemetry::load(path.string())->extension() == ".jsonl",
                    "JSONL override for every clip type");
            std::filesystem::remove(root / "override.gcsv");
            std::filesystem::remove(root / "override.jsonl");
        }
        {
            const auto video = root / "native.mov";
            makeVideo(video);
            rich->embed(video.string(), {0, 33333333, 66667000}, 30, 16, 16, 90);
            auto embedded = motioncam::GyroTelemetry::load(video.string());
            require(embedded->format() == motioncam::GyroTelemetry::Format::Jsonl &&
                        embedded->frames().size() == 3,
                    "read embedded protobuf");
            size_t imu = 0, quaternions = 0;
            for (const auto& frame : embedded->frames()) {
                const auto& value = frame.message["frame"];
                if (value.contains("imu")) imu += value["imu"].size();
                if (value.contains("quaternions")) quaternions += value["quaternions"].size();
            }
            require(imu == 2 && quaternions == 2,
                    "all motion samples survive embedding and frame duplication");
            require(embedded->header()["clip"]["rotationDegrees"] == 90, "container telemetry orientation");
            require(embedded->header()["camera"]["additionalData"].get<std::string>().find("rich metadata") !=
                        std::string::npos,
                    "additional data preserved");
            require(!std::filesystem::exists(root / "native.jsonl") &&
                        !std::filesystem::exists(root / "native.gcsv"),
                    "no gyro sidecars");
            write(root / "native.gcsv", gcsv);
            require(motioncam::GyroTelemetry::load(video.string())->serialize() == gcsv,
                    "GCSV sidecar overrides embedded protobuf");
            write(root / "native.jsonl", jsonl(header(0), {frame(1, 0, 999)}));
            const auto override = motioncam::GyroTelemetry::load(video.string());
            require(override->frames().size() == 1 && override->frame(0)->at("imu")[0]["gyroscopeX"] == 999,
                    "JSONL sidecar overrides embedded protobuf and GCSV sidecar");
            std::filesystem::remove(root / "native.jsonl");
            std::filesystem::remove(root / "native.gcsv");
            const auto originalSize = std::filesystem::file_size(video);
            bool cancelled = false;
            try {
                embedded->embed(video.string(), {0, 33333333, 66667000}, 30, 16, 16, 90,
                                [] { return false; });
            } catch (const std::runtime_error&) {
                cancelled = true;
            }
            require(cancelled && std::filesystem::file_size(video) == originalSize &&
                        !std::filesystem::exists(root / "native.telemetry.mov") &&
                        motioncam::GyroTelemetry::load(video.string())->frames().size() == 3,
                    "cancelled embedding preserves original and removes staging file");
            // Re-embedding replaces its own track, without accumulating duplicate telemetry.
            embedded->embed(video.string(), {0, 33333333, 66667000}, 30, 16, 16, 90);
            require(motioncam::GyroTelemetry::load(video.string())->frames().size() == 3,
                    "replace embedded track");
            legacy->embed(video.string(), {0, 33333333, 66667000}, 30, 16, 16, 0);
            const auto storedCsv = motioncam::GyroTelemetry::load(video.string());
            require(storedCsv->format() == motioncam::GyroTelemetry::Format::Gcsv &&
                        storedCsv->serialize() == gcsv,
                    "GCSV never converted to JSONL");
            {
                std::ifstream input(video, std::ios::binary);
                const std::string bytes((std::istreambuf_iterator<char>(input)), {});
                input.close();
                const auto tag = bytes.rfind("gcsv");
                require(tag != std::string::npos, "find GCSV sample entry");
                std::fstream output(video, std::ios::binary | std::ios::in | std::ios::out);
                output.seekp(tag);
                output.write("gpmd", 4);
                output.close();
                require(motioncam::GyroTelemetry::load(video.string())->serialize() == gcsv,
                        "GCSV recognized in a generic metadata track");
            }
        }
        write(root / "header-only.jsonl", jsonl(header(0), {}));
        const auto headerOnly = motioncam::GyroTelemetry::load((root / "header-only.jsonl").string());
        makeVideo(root / "header-only.mov");
        headerOnly->embed((root / "header-only.mov").string(), {0, 33333333, 66666667}, 30, 16, 16, 0);
        std::filesystem::remove(root / "header-only.jsonl");
        const auto headerRoundtrip = motioncam::GyroTelemetry::load((root / "header-only.mov").string());
        require(headerRoundtrip->extension() == ".jsonl" &&
                    headerRoundtrip->header()["camera"]["cameraBrand"] == "Test",
                "header-only telemetry survives container export");
        {
            // Simulate long MCRAW native frame metadata exceeding the former
            // single-packet limit. Include Unicode across chunk boundaries.
            constexpr size_t metadataBytes = 65 * 1024 * 1024;
            auto bigHeader = header(0);
            bigHeader["camera"]["additionalData"]["motioncamVideoClockOriginUs"] = 0;
            std::string payload(metadataBytes, 'x');
            for (size_t i = 1; i + 4 < payload.size(); i += 1048573)
                payload.replace(i, 4, "\xf0\x9f\x93\xb7");
            payload.back() = 'z';
            bigHeader["camera"]["additionalData"]["motioncamFrameMetadata"] = {
                {"preservedPayload", std::move(payload)}};
            write(root / "large.jsonl",
                  jsonl(bigHeader, {frame(1, 0, 1), frame(2, 33333, 2), frame(3, 66667, 3)}));
            bigHeader.clear();
            auto large = motioncam::GyroTelemetry::load((root / "large.jsonl").string());
            makeVideo(root / "large.mov");
            large->embed((root / "large.mov").string(), {0, 33333333, 66667000}, 30, 16, 16, 0);
            std::filesystem::remove(root / "large.jsonl");
            const auto restored = motioncam::GyroTelemetry::load((root / "large.mov").string());
            require(restored->frame(0)->at("startTimestampUs") == large->frame(0)->at("startTimestampUs") &&
                        restored->frame(0)->at("imu")[0]["sampleTimestampUs"] ==
                            large->frame(0)->at("imu")[0]["sampleTimestampUs"],
                    "chunked headers retain the video clock before frame packets are read");
            const auto extra = Json::parse(restored->header()["camera"]["additionalData"].get<std::string>());
            const auto& value =
                extra["motioncamFrameMetadata"]["preservedPayload"].get_ref<const std::string&>();
            require(value.size() == metadataBytes && value.back() == 'z' &&
                        value == large
                                     ->header()["camera"]["additionalData"]["motioncamFrameMetadata"]
                                               ["preservedPayload"]
                                     .get_ref<const std::string&>(),
                    "metadata beyond 64 MiB survives bounded native protobuf packets exactly");
            require(restored->frames().size() == 3 && restored->frames()[0].timestampNs == 0 &&
                        restored->frames()[1].timestampNs == 33333000 &&
                        restored->frames()[2].timestampNs == 66667000,
                    "metadata extension packets preserve image count and container PTS");
        }
        std::cout << "Container roundtrip and format preservation checked\n";
        // Real fixtures can be checked without writing beside the user's captures.
        for (int i = 1; i < argc; ++i) {
            std::string sourcePath = argv[i];
            std::string archiveRoot;
            if (std::filesystem::path(sourcePath).extension() == ".zip") {
                const auto imported = motioncam::extractReviewArchive(QString::fromStdString(sourcePath));
                sourcePath = imported.sourcePath.toStdString();
                archiveRoot = imported.temporaryRoot.toStdString();
            }
            const auto source = motioncam::GyroTelemetry::load(sourcePath);
            require(source->format() == motioncam::GyroTelemetry::Format::Jsonl, "native capture telemetry");
            size_t samples = 0;
            for (const auto& frame : source->frames())
                if (frame.message["frame"].contains("imu")) samples += frame.message["frame"]["imu"].size();
            require(samples > 0, "native capture IMU");
            // Exercise the complete schema writer against real camera telemetry,
            // retaining all motion data even when the output has fewer images.
            const auto native = root / "fixture-roundtrip.mov";
            makeVideo(native);
            source->embed(native.string(), {0, 33333333, 66667000}, 30, 16, 16, 0);
            const auto roundtrip = motioncam::GyroTelemetry::load(native.string());
            size_t restoredSamples = 0;
            for (const auto& packet : roundtrip->frames())
                if (packet.message["frame"].contains("imu"))
                    restoredSamples += packet.message["frame"]["imu"].size();
            require(restoredSamples == samples,
                    "every real-capture IMU sample survives native-container export");
            if (!archiveRoot.empty()) motioncam::removeReviewArchive(QString::fromStdString(archiveRoot));
            std::cout << argv[i] << ": " << source->frames().size() << " capture frames, " << samples
                      << " IMU samples\n";
        }
        std::cout << "Telemetry tests passed\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
