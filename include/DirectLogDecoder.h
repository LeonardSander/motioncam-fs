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
    
    bool extractFrame(int frameNumber, std::vector<uint16_t>& rgbData);
    bool extractFrameByTimestamp(Timestamp timestamp, std::vector<uint16_t>& rgbData);
    void setFullRangeOverride(std::optional<bool> fullRange);
    
    static bool isHLGVideo(const std::string& filePath);
    static bool isLOG60Video(const std::string& filePath);

private:
    void initFFmpeg();
    void analyzeVideo();
    void cleanup();
    bool convertYUVToRGB(AVFrame* yuvFrame, std::vector<uint16_t>& rgbData);
    void applyHLGToLinear(std::vector<uint16_t>& rgbData);
    void applyLOG60ToLinear(std::vector<uint16_t>& rgbData);

private:
    std::string mFilePath;
    DirectLogVideoInfo mVideoInfo;
    std::vector<DirectLogFrameInfo> mFrames;
    
    AVFormatContext* mFormatContext;
    AVCodecContext* mCodecContext;
    const AVCodec* mCodec;
    AVFrame* mFrame;
    AVPacket* mPacket;
    
    int mVideoStreamIndex;
    AVRational mTimeBase;
    std::optional<bool> mFullRange;
    std::optional<bool> mFullRangeOverride;

    mutable std::mutex mMutex;
};

} // namespace motioncam
