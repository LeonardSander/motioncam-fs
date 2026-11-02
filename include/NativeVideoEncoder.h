#pragma once

#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace motioncam {
// In-process Camera Native encoding. Input is full-range linear RGB48LE;
// timestamps and durations are in nanoseconds, relative to the clip start.
class NativeVideoEncoder {
public:
#ifdef __ANDROID__
    struct AndroidHardwareEncoder {
        std::string name, pixelFormat, bitrateMode;
        int profile = 0;
    };
#endif
    struct Options {
        std::string outputPath, audioPath, encoder, pixelFormat;
        std::map<std::string, std::string> codecOptions;
        int width = 0, height = 0, orientation = 0;
        double fps = 24;
        bool constantFrameRate = false;
        bool inBandHeaders = false;
#ifdef __ANDROID__
        std::optional<AndroidHardwareEncoder> androidHardware;
        int hardwareBitrate = 0;
#endif
    };
    explicit NativeVideoEncoder(const Options& options);
    ~NativeVideoEncoder();
    void writeFrame(const std::vector<uint8_t>& rgb, int64_t pts, int64_t duration);
    void finish();
#ifdef __ANDROID__
    static AndroidHardwareEncoder selectAndroidHardwareEncoder(const std::string& mime,
        int width, int height, double fps, int bitrate, int bitDepth);
#endif
    static bool supports(const std::string& encoder, const std::string& pixelFormat);
private:
    struct Impl;
    std::unique_ptr<Impl> m;
};
}
