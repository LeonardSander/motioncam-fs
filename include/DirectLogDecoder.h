#pragma once

#include <string>
#include <vector>
#include <memory>
#include <cstdint>
#include <mutex>
#include <optional>
#include <utility>
#include <array>
#include <deque>

#include "Types.h"

extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/imgutils.h>
#include <libavutil/opt.h>
#include <libavutil/hwcontext.h>
#include <libavutil/pixdesc.h>
#include <libavutil/rational.h>
#include <libavutil/display.h>
#include <libswscale/swscale.h>
#ifdef MOTIONCAM_HAS_AVFILTER
#include <libavfilter/avfilter.h>
#endif
}

namespace motioncam {
class DirectLogGpuRgb;

struct DirectLogFrameInfo {
    int frameNumber;
    int64_t pts;           
    Timestamp timestamp;   
    int width;
    int height;
    std::string pixelFormat;
    double timeBase;
    bool keyFrame;
};

struct DirectLogVideoInfo {
    int width;
    int height;
    double fps;
    int64_t totalFrames;
    std::string pixelFormat;
    bool isHLG;
    bool isLOG60;
    double duration;
    int orientation = -1;
};

class DirectLogDecoder {
public:
    DirectLogDecoder(const std::string& filePath);
    ~DirectLogDecoder();

    const DirectLogVideoInfo& getVideoInfo() const { return mVideoInfo; }
    const std::vector<DirectLogFrameInfo>& getFrames() const { return mFrames; }
    void overrideTimestamps(const std::vector<Timestamp>& timestamps);
    
    bool extractFrame(int frameNumber, std::vector<uint16_t>& rgbData,
                      int outputWidth = 0, int outputHeight = 0,
                      bool preserveLogEncoded = false,
                      bool smoothChroma = true);
    bool extractFrameBayer(int frameNumber, std::vector<uint16_t>& bayerData,
                           const std::array<uint8_t, 4>& cfaPhase);
    bool extractFrameIntoBytes(int frameNumber, std::vector<uint8_t>& bytes,
                               size_t pixelOffset, int outputWidth = 0,
                               int outputHeight = 0,
                               bool preserveLogEncoded = false,
                               bool smoothChroma = true);
    void setFullRangeOverride(std::optional<bool> fullRange);
    
    static bool isHLGVideo(const std::string& filePath);
    static bool isLOG60Video(const std::string& filePath);
    static std::pair<uintmax_t, size_t> timelineCacheUsage();
    static void clearTimelineCache();

private:
    void initFFmpeg();
    void initDecoder();
    bool initHardwareDecoder();
    void analyzeVideo();
    void cleanup();
    bool extractFrameInto(int frameNumber, uint16_t* rgbData, size_t sampleCount,
                          int outputWidth, int outputHeight,
                          bool preserveLogEncoded, bool smoothChroma,
                          const std::array<uint8_t, 4>* cfaPhase = nullptr);
    bool extractFrameIntoLocked(int frameNumber, uint16_t* rgbData, size_t sampleCount,
                                int outputWidth, int outputHeight,
                                bool preserveLogEncoded, bool smoothChroma,
                                const std::array<uint8_t, 4>* cfaPhase);
    bool convertYUVToRGB(AVFrame* yuvFrame, uint16_t* rgbData, size_t sampleCount,
                         int outputWidth, int outputHeight, bool preserveLogEncoded,
                         bool smoothChroma);
    void applyHLGToLinear(uint16_t* rgbData, size_t sampleCount, uint32_t encodedWhite);
    void applyLOG60ToLinear(uint16_t* rgbData, size_t sampleCount, uint32_t encodedWhite);
    AVFrame* transferableFrame(AVFrame* frame);
#ifdef MOTIONCAM_HAS_AVFILTER
    AVFrame* scaledHardwareFrame(AVFrame* frame, int width, int height);
    bool convertVulkanYUVToRGB(AVFrame* frame, uint16_t* rgbData,
                               size_t sampleCount, int outputWidth,
                               int outputHeight, bool preserveLogEncoded,
                               bool smoothChroma,
                               const std::array<uint8_t, 4>* cfaPhase);
#endif
    static AVPixelFormat selectPixelFormat(AVCodecContext* context,
                                           const AVPixelFormat* formats);

private:
    std::string mFilePath;
    DirectLogVideoInfo mVideoInfo;
    std::vector<DirectLogFrameInfo> mFrames;
    
    AVFormatContext* mFormatContext;
    AVCodecContext* mCodecContext;
    const AVCodec* mCodec;
    AVFrame* mFrame;
    AVFrame* mTransferFrame;
    AVPacket* mPacket;
    SwsContext* mSwsContext;
#ifdef MOTIONCAM_HAS_AVFILTER
    AVFilterGraph* mProxyGpuGraph = nullptr;
    AVFilterContext* mProxyGpuSource = nullptr;
    AVFilterContext* mProxyGpuSink = nullptr;
    AVFrame* mProxyGpuFrame = nullptr;
    int mProxyGpuWidth = 0;
    int mProxyGpuHeight = 0;
    AVBufferRef* mProxyGpuInputFrames = nullptr;
    bool mProxyGpuRejected = false;
    int mProxyGpuRejectedWidth = 0;
    int mProxyGpuRejectedHeight = 0;
    AVPixelFormat mProxyGpuRejectedFormat = AV_PIX_FMT_NONE;
    const void* mProxyGpuRejectedFrames = nullptr;
    bool mGpuRgbRejected = false;
    std::unique_ptr<DirectLogGpuRgb> mDirectGpuRgb;
#endif
    std::vector<SwsContext*> mBandSwsContexts;
    std::vector<std::vector<uint16_t>> mBandRgbScratch;
    std::vector<SwsContext*> mProxySwsContexts;
    std::vector<std::vector<uint16_t>> mProxyRgbScratch;
    std::array<int, 8> mProxyBandKey{};
    bool mProxyBandVerified = false;
    bool mProxyBandRejected = false;
    uint16_t mVerifiedBandCounts = 0;
    bool mBandConversionRejected = false;
    AVPixelFormat mVerifiedBandFormat = AV_PIX_FMT_NONE;
    bool mVerifiedBandSmooth = false;
    bool mVerifiedBandFullRange = false;
    AVBufferRef* mHardwareDeviceContext;
    AVPixelFormat mHardwarePixelFormat;
    bool mDecoderInitialized;
    bool mHardwareDecoderActive;
    bool mForceSoftwareDecoder = false;
    
    int mVideoStreamIndex;
    AVRational mTimeBase;
    std::optional<bool> mFullRange;
    std::optional<bool> mFullRangeOverride;
    int mLastDecodedFrame;
    std::deque<std::pair<int64_t, AVFrame*>> mDecodedFrameCache;
    std::vector<uint16_t> mLimitedRangeScratch;
    mutable std::mutex mMutex;
};

} // namespace motioncam
