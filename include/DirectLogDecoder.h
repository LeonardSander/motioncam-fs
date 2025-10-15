#pragma once

#include <string>
#include <vector>
#include <memory>
#include <cstdint>

extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libswscale/swscale.h>
#include <libavutil/imgutils.h>
#include <libavutil/opt.h>
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
    double duration;
};

class DirectLogDecoder {
public:
    DirectLogDecoder(const std::string& filePath);
    ~DirectLogDecoder();

    const DirectLogVideoInfo& getVideoInfo() const { return mVideoInfo; }
    const std::vector<DirectLogFrameInfo>& getFrames() const { return mFrames; }
    
    bool extractFrame(int frameNumber, std::vector<uint8_t>& rgbData);
    bool extractFrameByTimestamp(Timestamp timestamp, std::vector<uint8_t>& rgbData);
    
    static bool isHLGVideo(const std::string& filePath);

private:
    void initFFmpeg();
    void analyzeVideo();
    void setupScaler();
    void cleanup();
    bool convertYUVToRGB(AVFrame* yuvFrame, std::vector<uint8_t>& rgbData);
    void applyHLGToLinear(std::vector<uint8_t>& rgbData);

private:
    std::string mFilePath;
    DirectLogVideoInfo mVideoInfo;
    std::vector<DirectLogFrameInfo> mFrames;
    
    AVFormatContext* mFormatContext;
    AVCodecContext* mCodecContext;
    const AVCodec* mCodec;
    SwsContext* mSwsContext;
    AVFrame* mFrame;
    AVFrame* mRGBFrame;
    AVPacket* mPacket;
    
    int mVideoStreamIndex;
    uint8_t* mRGBBuffer;
    int mRGBBufferSize;
    AVRational mTimeBase;
};

} // namespace motioncam