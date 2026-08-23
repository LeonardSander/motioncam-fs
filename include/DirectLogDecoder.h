#pragma once

#include <string>
#include <vector>
#include <memory>
#include <cstdint>
#include <mutex>
#include <optional>

extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/imgutils.h>
#include <libavutil/opt.h>
#include <libavutil/pixdesc.h>
#include <libavutil/rational.h>
#include <libswscale/swscale.h>
}

namespace motioncam {

typedef int64_t Timestamp;

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
};

class DirectLogDecoder {
public:
    DirectLogDecoder(const std::string& filePath);
    ~DirectLogDecoder();

    const DirectLogVideoInfo& getVideoInfo() const { return mVideoInfo; }
    const std::vector<DirectLogFrameInfo>& getFrames() const { return mFrames; }
    
    bool extractFrame(int frameNumber, std::vector<uint16_t>& rgbData,
                      int outputWidth = 0, int outputHeight = 0,
                      bool preserveLogEncoded = false);
    bool extractFrameByTimestamp(Timestamp timestamp, std::vector<uint16_t>& rgbData);
    void setFullRangeOverride(std::optional<bool> fullRange);
    
    static bool isHLGVideo(const std::string& filePath);
    static bool isLOG60Video(const std::string& filePath);

private:
    void initFFmpeg();
    bool initHardwareDecoder();
    void analyzeVideo();
    void cleanup();
    bool convertYUVToRGB(AVFrame* yuvFrame, std::vector<uint16_t>& rgbData,
                         int outputWidth, int outputHeight, bool preserveLogEncoded);
    void applyHLGToLinear(std::vector<uint16_t>& rgbData);
    void applyLOG60ToLinear(std::vector<uint16_t>& rgbData);
    AVFrame* transferableFrame(AVFrame* frame);
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
    AVBufferRef* mHardwareDeviceContext;
    AVPixelFormat mHardwarePixelFormat;
    
    int mVideoStreamIndex;
    AVRational mTimeBase;
    std::optional<bool> mFullRange;
    std::optional<bool> mFullRangeOverride;
    int mLastDecodedFrame;
    std::vector<uint16_t> mLimitedRangeScratch;
    mutable std::mutex mMutex;
};

} // namespace motioncam
