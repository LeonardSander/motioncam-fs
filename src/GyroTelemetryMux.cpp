#include "GyroTelemetry.h"
#include "gyroflow.pb.h"
#include <google/protobuf/util/json_util.h>
extern "C" {
#include <libavformat/avformat.h>
#include <libavutil/error.h>
}
#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>
#include <stdexcept>

namespace rawpreamp {
namespace {
void check(int status, const char* operation) {
    if (status >= 0) return;
    char error[AV_ERROR_MAX_STRING_SIZE]{};
    av_strerror(status, error, sizeof(error));
    throw std::runtime_error(std::string(operation) + ": " + error);
}
uint64_t bigEndian(const unsigned char* p, unsigned size) {
    uint64_t value = 0;
    for (unsigned i = 0; i < size; ++i) value = (value << 8) | p[i];
    return value;
}
// FFmpeg's MOV muxer only writes a data sample entry for registered tags. Use
// its opaque gpmd carrier, then change just our track's stsd sample-entry tag.
// The handler is already 'meta'; packet bytes and timing tables stay untouched.
void setTelemetryTag(const std::filesystem::path& path, int track, const char* tag) {
    std::fstream file(path, std::ios::binary | std::ios::in | std::ios::out);
    if (!file) throw std::runtime_error("Could not update telemetry sample entry");
    const auto fileSize = std::filesystem::file_size(path);
    int trackIndex = -1;
    bool patched = false;
    std::function<void(uint64_t, uint64_t, bool)> walk = [&](uint64_t start, uint64_t end, bool selected) {
        for (uint64_t pos = start; pos + 8 <= end;) {
            std::array<unsigned char, 16> header{};
            file.seekg(pos);
            file.read(reinterpret_cast<char*>(header.data()), 8);
            if (!file) throw std::runtime_error("Truncated telemetry container box");
            uint64_t size = bigEndian(header.data(), 4), head = 8;
            if (size == 1) {
                file.read(reinterpret_cast<char*>(header.data() + 8), 8);
                if (!file) throw std::runtime_error("Truncated extended container box");
                size = bigEndian(header.data() + 8, 8);
                head = 16;
            } else if (!size)
                size = end - pos;
            if (size < head || size > end - pos) throw std::runtime_error("Invalid telemetry container box");
            const std::string type(reinterpret_cast<char*>(header.data() + 4), 4);
            bool current = selected;
            if (type == "trak") current = ++trackIndex == track;
            if (type == "moov" || type == "trak" || type == "mdia" || type == "minf" || type == "stbl")
                walk(pos + head, pos + size, current);
            else if (type == "stsd" && current) {
                if (size < head + 24) throw std::runtime_error("Truncated telemetry sample description");
                std::array<unsigned char, 16> description{};
                file.seekg(pos + head);
                file.read(reinterpret_cast<char*>(description.data()), description.size());
                if (!file || bigEndian(description.data() + 4, 4) != 1 ||
                    std::string(reinterpret_cast<char*>(description.data() + 12), 4) != "gpmd")
                    throw std::runtime_error("Unexpected telemetry sample entry");
                file.seekp(pos + head + 12);
                file.write(tag, 4);
                if (!file) throw std::runtime_error("Could not write telemetry sample entry");
                patched = true;
            }
            pos += size;
        }
    };
    walk(0, fileSize, false);
    file.flush();
    if (!patched || !file) throw std::runtime_error("Telemetry track was not found in completed container");
}
std::string protobuf(nlohmann::json message) {
    if (message.contains("header") && message["header"].contains("camera")) {
        auto& camera = message["header"]["camera"];
        for (const char* name : {"additionalData", "lensProfile"})
            if (camera.contains(name) && !camera[name].is_string()) camera[name] = camera[name].dump();
    }
    Main decoded;
    const auto status = google::protobuf::util::JsonStringToMessage(message.dump(), &decoded);
    if (!status.ok())
        throw std::runtime_error("Telemetry contains fields unsupported by the protobuf schema: " +
                                 status.ToString());
    std::string bytes;
    if (!decoded.SerializeToString(&bytes)) throw std::runtime_error("Could not serialize Gyroflow protobuf");
    return bytes;
}
} // namespace

void GyroTelemetry::embed(const std::string& videoPath, const std::vector<int64_t>& timestamps, double fps,
                          int width, int height, int orientation,
                          const std::function<bool()>& keepGoing, bool excludeGps) const {
    if (mFormat == Format::None) return;
    if (timestamps.empty() || !std::is_sorted(timestamps.begin(), timestamps.end()) ||
        std::adjacent_find(timestamps.begin(), timestamps.end()) != timestamps.end())
        throw std::runtime_error("Embedded telemetry needs the finalized video timeline");
    struct Contexts {
        AVFormatContext* input = nullptr;
        AVFormatContext* output = nullptr;
        AVPacket* packet = av_packet_alloc();
        ~Contexts() {
            av_packet_free(&packet);
            avformat_close_input(&input);
            if (output) {
                if (output->pb) avio_closep(&output->pb);
                avformat_free_context(output);
            }
        }
    } media;
    if (!media.packet) throw std::bad_alloc();
    const auto path = std::filesystem::path(videoPath);
    const auto staged =
        path.parent_path() / (path.stem().string() + ".telemetry" + path.extension().string());
    struct Cleanup {
        std::filesystem::path path;
        AVFormatContext*& output;
        ~Cleanup() {
            // Windows cannot unlink an open file on an error/cancel path.
            if (output && output->pb) avio_closep(&output->pb);
            std::error_code error;
            std::filesystem::remove(path, error);
        }
    } cleanup{staged, media.output};
    check(avformat_open_input(&media.input, videoPath.c_str(), nullptr, nullptr),
          "Open finalized telemetry video");
    check(avformat_find_stream_info(media.input, nullptr), "Read finalized video streams");
    check(avformat_alloc_output_context2(&media.output, nullptr, nullptr, staged.string().c_str()),
          "Allocate telemetry output");
    if (!media.output) throw std::bad_alloc();
    std::vector<int> indices(media.input->nb_streams, -1);
    int64_t videoStart = 0;
    for (unsigned i = 0; i < media.input->nb_streams; ++i) {
        const auto* original = media.input->streams[i];
        // Replace our prior telemetry track on reruns, preserving every other track.
        if (original->codecpar->codec_tag == MKTAG('g', 'y', 'r', 'f') ||
            original->codecpar->codec_tag == MKTAG('g', 'c', 's', 'v'))
            continue;
        auto* copy = avformat_new_stream(media.output, nullptr);
        if (!copy) throw std::bad_alloc();
        indices[i] = copy->index;
        check(avcodec_parameters_copy(copy->codecpar, original->codecpar), "Copy native stream format");
        copy->time_base = original->time_base;
        copy->avg_frame_rate = original->avg_frame_rate;
        copy->r_frame_rate = original->r_frame_rate;
        copy->disposition = original->disposition;
        av_dict_copy(&copy->metadata, original->metadata, 0);
#if LIBAVFORMAT_VERSION_MAJOR < 61
        for (int j = 0; j < original->nb_side_data; ++j) {
            auto* bytes =
                av_stream_new_side_data(copy, original->side_data[j].type, original->side_data[j].size);
            if (!bytes) throw std::bad_alloc();
            std::memcpy(bytes, original->side_data[j].data, original->side_data[j].size);
        }
#endif
        if (original->codecpar->codec_type == AVMEDIA_TYPE_VIDEO && original->start_time != AV_NOPTS_VALUE)
            videoStart = av_rescale_q(original->start_time, original->time_base, AVRational{1, 1000000000});
    }
    av_dict_copy(&media.output->metadata, media.input->metadata, 0);
    auto* telemetry = avformat_new_stream(media.output, nullptr);
    if (!telemetry) throw std::bad_alloc();
    telemetry->codecpar->codec_type = AVMEDIA_TYPE_DATA;
    telemetry->codecpar->codec_id = AV_CODEC_ID_BIN_DATA;
    telemetry->codecpar->codec_tag = MKTAG('g', 'p', 'm', 'd');
    telemetry->time_base = {1, 1000000};
    av_dict_set(&telemetry->metadata, "handler_name", "Gyroflow telemetry", 0);
    check(avio_open(&media.output->pb, staged.string().c_str(), AVIO_FLAG_WRITE),
          "Open telemetry staging output");
    check(avformat_write_header(media.output, nullptr), "Write embedded telemetry header");

    auto frames = mFormat == Format::Jsonl ? outputFrames(timestamps) : std::vector<Frame>{};
    auto header = mFormat == Format::Jsonl ? outputHeader(timestamps, fps, width, height, orientation)
                                           : nlohmann::json{};
    if (excludeGps) {
        stripGps(header);
        for (auto& frame : frames) stripGps(frame.message);
    }
    // Native frame metadata can exceed a single protobuf packet on long clips.
    // Preserve the complete extensible data in bounded header-only packets.
    // All packets still use the official Gyroflow schema; standard camera/clip
    // fields and actual frame/IMU packets remain independently readable.
    constexpr size_t chunkBytes = 1024 * 1024;
    std::string additionalData;
    size_t chunkCount = 0;
    std::vector<size_t> chunkOffsets{0};
    if (header.contains("camera") && header["camera"].contains("additionalData")) {
        const auto& extra = header["camera"]["additionalData"];
        additionalData = extra.is_string() ? extra.get<std::string>() : extra.dump();
        if (additionalData.size() > chunkBytes) {
            while (chunkOffsets.back() < additionalData.size()) {
                auto end = std::min(chunkOffsets.back() + chunkBytes, additionalData.size());
                // Each chunk is a protobuf UTF-8 string; never split a codepoint.
                while (end < additionalData.size() &&
                       (static_cast<unsigned char>(additionalData[end]) & 0xc0) == 0x80)
                    --end;
                chunkOffsets.push_back(end);
            }
            chunkCount = chunkOffsets.size() - 1;
            header["camera"]["additionalData"] = {
                {"rawpreampVideoClockOriginUs", 0},
                {"rawpreampTelemetryChunks", {{"version", 1}, {"count", chunkCount}}}};
        } else
            additionalData.clear();
    }
    // Encode each packet only when it is muxed. Avoid building a full JSONL
    // serialization and a second complete set of encoded payloads in memory.
    const auto packetCount = std::max<size_t>(1, frames.size());
    const auto timeAt = [&](size_t index) { return frames.empty() ? int64_t{0} : frames[index].timestampNs; };
    size_t next = 0, nextChunk = 0;
    int64_t chunkTime = timeAt(0) + 1000;
    const auto packetTime = [&](int64_t time) {
        return av_rescale_q(time + videoStart, AVRational{1, 1000000000}, telemetry->time_base);
    };
    auto writeTelemetry = [&](int64_t until, bool all = false) {
        while (next < packetCount || nextChunk < chunkCount) {
            // Put extension packets between video-frame packets at distinct
            // microsecond ticks. They never change a frame's original PTS.
            if (next < packetCount && packetTime(chunkTime) == packetTime(timeAt(next))) chunkTime += 1000;
            const bool chunk =
                next > 0 && nextChunk < chunkCount && (next == packetCount || chunkTime < timeAt(next));
            const auto timestamp = chunk ? chunkTime : timeAt(next);
            if (!all && timestamp + videoStart > until) break;
            if (keepGoing && !keepGoing()) throw std::runtime_error("Finalization cancelled");
            std::string payload;
            if (chunk) {
                auto chunkHeader = header;
                chunkHeader["camera"]["additionalData"] = {
                    {"rawpreampTelemetryChunk",
                     {{"version", 1},
                      {"index", nextChunk},
                      {"text",
                       additionalData.substr(chunkOffsets[nextChunk],
                                             chunkOffsets[nextChunk + 1] - chunkOffsets[nextChunk])}}}};
                payload = protobuf({{"magicString", "GyroflowProtobuf"},
                                    {"protocolVersion", 1},
                                    {"header", std::move(chunkHeader)}});
            } else if (mFormat == Format::Gcsv)
                payload = mGcsv;
            else {
                auto message = frames.empty() ? nlohmann::json{{"magicString", "GyroflowProtobuf"},
                                                               {"protocolVersion", 1}}
                                              : frames[next].message;
                message.erase("header");
                if (!next) message["header"] = header;
                payload = protobuf(std::move(message));
            }
            if (payload.size() > 64 * 1024 * 1024)
                throw std::runtime_error("Telemetry packet exceeds the supported 64 MiB limit");
            AVPacket* packet = av_packet_alloc();
            if (!packet) throw std::bad_alloc();
            std::unique_ptr<AVPacket, void (*)(AVPacket*)> owned(packet,
                                                                 [](AVPacket* p) { av_packet_free(&p); });
            check(av_new_packet(packet, static_cast<int>(payload.size())), "Allocate telemetry packet");
            std::memcpy(packet->data, payload.data(), payload.size());
            packet->pts = packet->dts = packetTime(timestamp);
            const auto duration = chunk ? int64_t{1000}
                                  : next + 1 < packetCount
                                      ? timeAt(next + 1) - timeAt(next)
                                      : int64_t(std::llround(1e9 / (fps > 0 ? fps : 24)));
            packet->duration =
                std::max<int64_t>(1, av_rescale_q(duration, AVRational{1, 1000000000}, telemetry->time_base));
            packet->stream_index = telemetry->index;
            packet->flags |= AV_PKT_FLAG_KEY;
            check(av_interleaved_write_frame(media.output, packet), "Mux telemetry packet");
            if (chunk) {
                ++nextChunk;
                chunkTime += 1000;
            } else
                ++next;
        }
    };
    int status;
    while ((status = av_read_frame(media.input, media.packet)) >= 0) {
        if (keepGoing && !keepGoing()) throw std::runtime_error("Finalization cancelled");
        auto* packet = media.packet;
        const auto index = packet->stream_index;
        if (indices[index] >= 0) {
            const auto base = media.input->streams[index]->time_base;
            const auto pts = packet->dts != AV_NOPTS_VALUE ? packet->dts : packet->pts;
            if (pts != AV_NOPTS_VALUE) writeTelemetry(av_rescale_q(pts, base, AVRational{1, 1000000000}));
            av_packet_rescale_ts(packet, base, media.output->streams[indices[index]]->time_base);
            packet->stream_index = indices[index];
            packet->pos = -1;
            check(av_interleaved_write_frame(media.output, packet), "Copy finalized video/audio packets");
        }
        av_packet_unref(packet);
    }
    if (status != AVERROR_EOF) check(status, "Read finalized media");
    writeTelemetry(0, true);
    check(av_write_trailer(media.output), "Finish telemetry container");
    check(avio_closep(&media.output->pb), "Close telemetry container");
    avformat_close_input(&media.input);
    setTelemetryTag(staged, telemetry->index, mFormat == Format::Jsonl ? "gyrf" : "gcsv");
    // The video is still inside the caller's unpublished staging transaction.
    const auto backup = path.string() + ".without-telemetry";
    std::filesystem::rename(path, backup);
    try {
        std::filesystem::rename(staged, path);
    } catch (...) {
        std::filesystem::rename(backup, path);
        throw;
    }
    std::filesystem::remove(backup);
}
} // namespace rawpreamp
