#include "android/P010VideoEncoder.h"
#include "P010Packing.h"
#ifdef __ANDROID__
#include <media/NdkMediaCodec.h>
#include <media/NdkMediaFormat.h>
#include <chrono>
#include <cstring>
#include <cmath>
#include <limits>
#include <stdexcept>
#include <vector>
#include <utility>
extern "C" {
#include <libavcodec/packet.h>
#include <libavutil/frame.h>
#include <libavutil/pixfmt.h>
}
namespace motioncam {
namespace {
void checkMedia(media_status_t status, const char* operation) {
    if (status != AMEDIA_OK)
        throw std::runtime_error(std::string(operation) + " (MediaCodec status " + std::to_string(status) + ")");
}
// Release dequeued buffers on every allocation, validation, and callback error.
class OutputBuffer {
public:
    OutputBuffer(AMediaCodec* codec, ssize_t index) : mCodec(codec), mIndex(index) {}
    ~OutputBuffer() {
        if (mIndex >= 0) AMediaCodec_releaseOutputBuffer(mCodec, mIndex, false);
    }
    void release() {
        const ssize_t index = std::exchange(mIndex, -1);
        checkMedia(AMediaCodec_releaseOutputBuffer(mCodec, index, false), "Release P010 output buffer");
    }
private:
    AMediaCodec* mCodec;
    ssize_t mIndex;
};
using MediaFormat = std::unique_ptr<AMediaFormat, decltype(&AMediaFormat_delete)>;
constexpr int p010ColorFormat = 54;
constexpr int64_t waitUs = 10000;
constexpr auto timeout = std::chrono::seconds(30);
}
struct P010VideoEncoder::Impl {
    Options options;
    AMediaCodec* codec = nullptr;
    bool started = false, finished = false, outputEos = false;
    int strideBytes = 0, sliceHeight = 0;
    int64_t endTimestamp = 0;
    std::vector<uint8_t> headers;
    ~Impl() {
        if (codec) {
            if (started) AMediaCodec_stop(codec);
            AMediaCodec_delete(codec);
        }
    }
    bool drain(const std::function<void(AVPacket*)>& ready, int64_t wait = 0) {
        AMediaCodecBufferInfo info{};
        const ssize_t index = AMediaCodec_dequeueOutputBuffer(codec, &info, wait);
        if (index == AMEDIACODEC_INFO_TRY_AGAIN_LATER) return false;
        if (index == AMEDIACODEC_INFO_OUTPUT_BUFFERS_CHANGED) return true;
        if (index == AMEDIACODEC_INFO_OUTPUT_FORMAT_CHANGED) {
            MediaFormat format(AMediaCodec_getOutputFormat(codec), AMediaFormat_delete);
            if (!format) throw std::runtime_error("Could not read P010 encoder output format");
            // The emitted format must honor full-range BT.2020 before any
            // container metadata is attached. Transfer stays unspecified for LLOG60.
            int32_t range = 0, standard = 0;
            if (!AMediaFormat_getInt32(format.get(), "color-range", &range) || range != 1 ||
                !AMediaFormat_getInt32(format.get(), "color-standard", &standard) || standard != 6)
                throw std::runtime_error("The P010 hardware encoder did not honor full-range BT.2020 output");
            // Some codecs expose codec-specific data through the format only.
            if (headers.empty()) {
                for (const char* key : {"csd-0", "csd-1", "csd-2"}) {
                    void* data = nullptr; size_t size = 0;
                    if (AMediaFormat_getBuffer(format.get(), key, &data, &size) && size)
                        headers.insert(headers.end(), static_cast<uint8_t*>(data), static_cast<uint8_t*>(data) + size);
                }
            }
            return true;
        }
        if (index < 0) throw std::runtime_error("Could not dequeue P010 encoded video");
        OutputBuffer outputBuffer(codec, index);
        size_t capacity = 0;
        uint8_t* buffer = AMediaCodec_getOutputBuffer(codec, index, &capacity);
        if (info.offset < 0 || info.size < 0 || static_cast<size_t>(info.offset) > capacity ||
            static_cast<size_t>(info.size) > capacity - info.offset || (info.size && !buffer)) {
            throw std::runtime_error("Invalid P010 encoded output buffer");
        }
        if (info.flags & AMEDIACODEC_BUFFER_FLAG_PARTIAL_FRAME) {
            throw std::runtime_error("Partial P010 encoded access units are not supported");
        }
        if (info.flags & AMEDIACODEC_BUFFER_FLAG_CODEC_CONFIG) {
            if (info.size) headers.assign(buffer + info.offset, buffer + info.offset + info.size);
        } else if (info.size) {
            AVPacket* packet = av_packet_alloc();
            if (!packet) { throw std::bad_alloc(); }
            std::unique_ptr<AVPacket, void(*)(AVPacket*)> owned(packet, [](AVPacket* p) { av_packet_free(&p); });
            if (headers.size() > static_cast<size_t>(std::numeric_limits<int>::max() - info.size)) {
                throw std::runtime_error("P010 encoder headers are too large");
            }
            const int status = av_new_packet(packet, static_cast<int>(headers.size()) + info.size);
            if (status < 0) { throw std::bad_alloc(); }
            if (!headers.empty()) std::memcpy(packet->data, headers.data(), headers.size());
            std::memcpy(packet->data + headers.size(), buffer + info.offset, info.size);
            headers.clear();
            packet->pts = packet->dts = info.presentationTimeUs;
            if (info.flags & 1) packet->flags |= AV_PKT_FLAG_KEY;
            outputBuffer.release();
            // No MediaCodec buffer is retained while the muxer runs.
            ready(packet);
            if (info.flags & AMEDIACODEC_BUFFER_FLAG_END_OF_STREAM) outputEos = true;
            return true;
        }
        outputBuffer.release();
        if (info.flags & AMEDIACODEC_BUFFER_FLAG_END_OF_STREAM) outputEos = true;
        return true;
    }
    ssize_t inputBuffer(const std::function<void(AVPacket*)>& ready) {
        const auto deadline = std::chrono::steady_clock::now() + timeout;
        while (std::chrono::steady_clock::now() < deadline) {
            const ssize_t index = AMediaCodec_dequeueInputBuffer(codec, waitUs);
            if (index >= 0) return index;
            if (index != AMEDIACODEC_INFO_TRY_AGAIN_LATER)
                throw std::runtime_error("Could not dequeue P010 encoder input");
            while (drain(ready)) {}
        }
        throw std::runtime_error("Timed out waiting for P010 encoder input");
    }
};
P010VideoEncoder::P010VideoEncoder(const Options& options) : m(std::make_unique<Impl>()) {
    auto& s = *m; s.options = options;
    if (options.width <= 0 || options.width > std::numeric_limits<int>::max() / 2 ||
        options.height <= 0 || options.width % 2 || options.height % 2 ||
        !std::isfinite(options.fps) || options.fps <= 0 || options.bitrate <= 0)
        throw std::runtime_error("Invalid P010 hardware encoder configuration");
    s.codec = AMediaCodec_createCodecByName(options.codecName.c_str());
    if (!s.codec) throw std::runtime_error("Could not create the selected P010 hardware encoder");
    MediaFormat format(AMediaFormat_new(), AMediaFormat_delete);
    if (!format) throw std::bad_alloc();
    AMediaFormat_setString(format.get(), "mime", options.mime.c_str());
    AMediaFormat_setInt32(format.get(), "width", options.width);
    AMediaFormat_setInt32(format.get(), "height", options.height);
    AMediaFormat_setInt32(format.get(), "color-format", p010ColorFormat);
    AMediaFormat_setInt32(format.get(), "profile", options.profile);
    AMediaFormat_setInt32(format.get(), "bitrate", options.bitrate);
    AMediaFormat_setInt32(format.get(), "bitrate-mode", options.bitrateMode == "vbr" ? 1 : 2);
    AMediaFormat_setFloat(format.get(), "frame-rate", static_cast<float>(options.fps));
    AMediaFormat_setInt32(format.get(), "i-frame-interval", 2);
    AMediaFormat_setInt32(format.get(), "max-bframes", 0);
    AMediaFormat_setInt32(format.get(), "color-range", 1); // COLOR_RANGE_FULL
    AMediaFormat_setInt32(format.get(), "color-standard", 6); // COLOR_STANDARD_BT2020
    // LLOG60 has no standardized transfer identifier. Never signal PQ or HLG.
    checkMedia(AMediaCodec_configure(s.codec, format.get(), nullptr, nullptr, AMEDIACODEC_CONFIGURE_FLAG_ENCODE), "Configure P010 hardware encoder");
    checkMedia(AMediaCodec_start(s.codec), "Start P010 hardware encoder");
    s.started = true;
    MediaFormat input(AMediaCodec_getInputFormat(s.codec), AMediaFormat_delete);
    int32_t color = 0, width = 0, height = 0;
    if (!input || !AMediaFormat_getInt32(input.get(), "color-format", &color) || color != p010ColorFormat ||
        !AMediaFormat_getInt32(input.get(), "width", &width) || width != options.width ||
        !AMediaFormat_getInt32(input.get(), "height", &height) || height != options.height)
        throw std::runtime_error("Hardware encoder did not accept the requested P010 input dimensions and format");
    // Android's raw video stride is measured in bytes, including for P010.
    int32_t stride = options.width * 2, slice = options.height;
    AMediaFormat_getInt32(input.get(), "stride", &stride);
    AMediaFormat_getInt32(input.get(), "slice-height", &slice);
    if (stride < options.width * 2 || stride % 2 || slice < options.height || slice % 2)
        throw std::runtime_error("Unsupported P010 hardware input stride or slice height");
    s.strideBytes = stride;
    s.sliceHeight = slice;
}
P010VideoEncoder::~P010VideoEncoder() = default;
void P010VideoEncoder::writeFrame(const AVFrame* frame, const std::function<void(AVPacket*)>& ready) {
    auto& s = *m;
    if (!frame || s.finished || frame->format != AV_PIX_FMT_P010LE || frame->width != s.options.width || frame->height != s.options.height)
        throw std::runtime_error("Invalid P010 hardware frame");
    const ssize_t index = s.inputBuffer(ready);
    size_t capacity = 0;
    uint8_t* buffer = AMediaCodec_getInputBuffer(s.codec, index, &capacity);
    const size_t bytes = packP010Input(*frame, buffer, capacity, s.strideBytes, s.sliceHeight);
    checkMedia(AMediaCodec_queueInputBuffer(s.codec, index, 0, bytes, frame->pts, 0), "Queue P010 hardware frame");
    s.endTimestamp = frame->pts + frame->duration;
    while (s.drain(ready)) {}
}
void P010VideoEncoder::finish(const std::function<void(AVPacket*)>& ready) {
    auto& s = *m;
    if (s.finished) return;
    const ssize_t index = s.inputBuffer(ready);
    checkMedia(AMediaCodec_queueInputBuffer(s.codec, index, 0, 0, s.endTimestamp,
        AMEDIACODEC_BUFFER_FLAG_END_OF_STREAM), "Finish P010 hardware input");
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (!s.outputEos) {
        if (std::chrono::steady_clock::now() > deadline)
            throw std::runtime_error("Timed out flushing P010 hardware encoder");
        s.drain(ready, waitUs);
    }
    s.finished = true;
}
}
#endif
