#pragma once
#ifdef __ANDROID__
#include <functional>
#include <memory>
#include <string>
struct AVFrame;
struct AVPacket;
namespace motioncam {
// Direct MediaCodec input for P010, absent from FFmpeg 6.1's wrapper.
// Encoded packets are delivered synchronously in a microsecond time base.
class P010VideoEncoder {
public:
    struct Options {
        std::string codecName, mime, bitrateMode;
        int width = 0, height = 0, bitrate = 0, profile = 0;
        double fps = 0;
    };
    explicit P010VideoEncoder(const Options& options);
    ~P010VideoEncoder();
    void writeFrame(const AVFrame* frame, const std::function<void(AVPacket*)>& packetReady);
    void finish(const std::function<void(AVPacket*)>& packetReady);
private:
    struct Impl;
    std::unique_ptr<Impl> m;
};
}
#endif
