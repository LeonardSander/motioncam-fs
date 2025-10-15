#include "DirectLogDecoder.h"
#include <spdlog/spdlog.h>
#include <boost/algorithm/string.hpp>
#include <stdexcept>
#include <cmath>

namespace motioncam {

DirectLogDecoder::DirectLogDecoder(const std::string& filePath) 
    : mFilePath(filePath),
      mFormatContext(nullptr),
      mCodecContext(nullptr),
      mCodec(nullptr),
      mSwsContext(nullptr),
      mFrame(nullptr),
      mRGBFrame(nullptr),
      mPacket(nullptr),
      mVideoStreamIndex(-1),
      mRGBBuffer(nullptr),
      mRGBBufferSize(0) {
    
    spdlog::info("DirectLogDecoder: Initializing for {}", filePath);
    
    initFFmpeg();
    analyzeVideo();
    setupScaler();
}

DirectLogDecoder::~DirectLogDecoder() {
    cleanup();
}

void DirectLogDecoder::initFFmpeg() {
    // Open input file
    if (avformat_open_input(&mFormatContext, mFilePath.c_str(), nullptr, nullptr) < 0) {
        throw std::runtime_error("Could not open video file: " + mFilePath);
    }
    
    // Retrieve stream information
    if (avformat_find_stream_info(mFormatContext, nullptr) < 0) {
        throw std::runtime_error("Could not find stream information");
    }
    
    // Find video stream
    for (unsigned int i = 0; i < mFormatContext->nb_streams; i++) {
        if (mFormatContext->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
            mVideoStreamIndex = i;
            break;
        }
    }
    
    if (mVideoStreamIndex == -1) {
        throw std::runtime_error("Could not find video stream");
    }
    
    // Get codec parameters
    AVCodecParameters* codecpar = mFormatContext->streams[mVideoStreamIndex]->codecpar;
    
    // Find decoder
    mCodec = avcodec_find_decoder(codecpar->codec_id);
    if (!mCodec) {
        throw std::runtime_error("Unsupported codec");
    }
    
    // Allocate codec context
    mCodecContext = avcodec_alloc_context3(mCodec);
    if (!mCodecContext) {
        throw std::runtime_error("Could not allocate codec context");
    }
    
    // Copy codec parameters to context
    if (avcodec_parameters_to_context(mCodecContext, codecpar) < 0) {
        throw std::runtime_error("Could not copy codec parameters");
    }
    
    // Open codec
    if (avcodec_open2(mCodecContext, mCodec, nullptr) < 0) {
        throw std::runtime_error("Could not open codec");
    }
    
    // Allocate frames and packet
    mFrame = av_frame_alloc();
    mRGBFrame = av_frame_alloc();
    mPacket = av_packet_alloc();
    
    if (!mFrame || !mRGBFrame || !mPacket) {
        throw std::runtime_error("Could not allocate frame or packet");
    }
    
    mTimeBase = mFormatContext->streams[mVideoStreamIndex]->time_base;
}

void DirectLogDecoder::analyzeVideo() {
    mVideoInfo.width = mCodecContext->width;
    mVideoInfo.height = mCodecContext->height;
    
    // Determine pixel format
    switch (mCodecContext->pix_fmt) {
        case AV_PIX_FMT_YUV420P:
            mVideoInfo.pixelFormat = "yuv420p";
            break;
        case AV_PIX_FMT_YUV420P10LE:
            mVideoInfo.pixelFormat = "yuv420p10le";
            break;
        case AV_PIX_FMT_YUV422P10LE:
            mVideoInfo.pixelFormat = "yuv422p10le";
            break;
        default:
            mVideoInfo.pixelFormat = "unknown";
            break;
    }
    
    // Check if HLG based on filename
    mVideoInfo.isHLG = boost::icontains(mFilePath, "HLG_NATIVE");
    
    // Calculate FPS and duration
    AVRational frameRate = mFormatContext->streams[mVideoStreamIndex]->r_frame_rate;
    if (frameRate.den != 0) {
        mVideoInfo.fps = static_cast<double>(frameRate.num) / frameRate.den;
    } else {
        mVideoInfo.fps = 30.0; // Default
    }
    
    mVideoInfo.duration = static_cast<double>(mFormatContext->duration) / AV_TIME_BASE;
    
    // Read all frames to get timestamps
    mFrames.clear();
    
    int frameNumber = 0;
    while (av_read_frame(mFormatContext, mPacket) >= 0) {
        if (mPacket->stream_index == mVideoStreamIndex) {
            DirectLogFrameInfo frameInfo;
            frameInfo.frameNumber = frameNumber++;
            frameInfo.pts = mPacket->pts;
            frameInfo.timestamp = static_cast<Timestamp>(mPacket->pts * av_q2d(mTimeBase) * 1000000000.0);
            frameInfo.width = mVideoInfo.width;
            frameInfo.height = mVideoInfo.height;
            frameInfo.pixelFormat = mVideoInfo.pixelFormat;
            frameInfo.timeBase = av_q2d(mTimeBase);
            
            mFrames.push_back(frameInfo);
        }
        av_packet_unref(mPacket);
    }
    
    mVideoInfo.totalFrames = mFrames.size();
    
    // Seek back to beginning
    av_seek_frame(mFormatContext, mVideoStreamIndex, 0, AVSEEK_FLAG_BACKWARD);
    avcodec_flush_buffers(mCodecContext);
    
    spdlog::info("DirectLogDecoder: Analyzed video - {}x{} @ {:.2f}fps, {} frames, format: {}, HLG: {}", 
                 mVideoInfo.width, mVideoInfo.height, mVideoInfo.fps, mVideoInfo.totalFrames, 
                 mVideoInfo.pixelFormat, mVideoInfo.isHLG);
}

void DirectLogDecoder::setupScaler() {
    // Setup scaler to convert YUV to RGB
    mSwsContext = sws_getContext(
        mVideoInfo.width, mVideoInfo.height, mCodecContext->pix_fmt,
        mVideoInfo.width, mVideoInfo.height, AV_PIX_FMT_RGB24,
        SWS_BILINEAR, nullptr, nullptr, nullptr);
    
    if (!mSwsContext) {
        throw std::runtime_error("Could not initialize scaler context");
    }
    
    // Allocate RGB buffer
    mRGBBufferSize = av_image_get_buffer_size(AV_PIX_FMT_RGB24, mVideoInfo.width, mVideoInfo.height, 1);
    mRGBBuffer = static_cast<uint8_t*>(av_malloc(mRGBBufferSize));
    
    if (!mRGBBuffer) {
        throw std::runtime_error("Could not allocate RGB buffer");
    }
    
    // Setup RGB frame
    av_image_fill_arrays(mRGBFrame->data, mRGBFrame->linesize, mRGBBuffer, 
                        AV_PIX_FMT_RGB24, mVideoInfo.width, mVideoInfo.height, 1);
}

bool DirectLogDecoder::extractFrame(int frameNumber, std::vector<uint8_t>& rgbData) {
    if (frameNumber < 0 || frameNumber >= static_cast<int>(mFrames.size())) {
        return false;
    }
    
    const DirectLogFrameInfo& frameInfo = mFrames[frameNumber];
    
    // Seek to frame
    if (av_seek_frame(mFormatContext, mVideoStreamIndex, frameInfo.pts, AVSEEK_FLAG_BACKWARD) < 0) {
        spdlog::error("Failed to seek to frame {}", frameNumber);
        return false;
    }
    
    avcodec_flush_buffers(mCodecContext);
    
    // Read packets until we find our frame
    while (av_read_frame(mFormatContext, mPacket) >= 0) {
        if (mPacket->stream_index == mVideoStreamIndex) {
            if (avcodec_send_packet(mCodecContext, mPacket) == 0) {
                while (avcodec_receive_frame(mCodecContext, mFrame) == 0) {
                    if (mFrame->pts == frameInfo.pts) {
                        // Convert to RGB
                        if (convertYUVToRGB(mFrame, rgbData)) {
                            av_packet_unref(mPacket);
                            return true;
                        }
                    }
                }
            }
        }
        av_packet_unref(mPacket);
    }
    
    return false;
}

bool DirectLogDecoder::extractFrameByTimestamp(Timestamp timestamp, std::vector<uint8_t>& rgbData) {
    // Find frame with closest timestamp
    auto it = std::lower_bound(mFrames.begin(), mFrames.end(), timestamp,
                              [](const DirectLogFrameInfo& frame, Timestamp ts) {
                                  return frame.timestamp < ts;
                              });
    
    if (it == mFrames.end()) {
        it = mFrames.end() - 1;
    }
    
    int frameNumber = static_cast<int>(std::distance(mFrames.begin(), it));
    return extractFrame(frameNumber, rgbData);
}

bool DirectLogDecoder::convertYUVToRGB(AVFrame* yuvFrame, std::vector<uint8_t>& rgbData) {
    // Scale YUV to RGB
    int result = sws_scale(mSwsContext, 
                          yuvFrame->data, yuvFrame->linesize, 0, mVideoInfo.height,
                          mRGBFrame->data, mRGBFrame->linesize);
    
    if (result != mVideoInfo.height) {
        spdlog::error("Failed to scale frame");
        return false;
    }
    
    // Copy RGB data
    rgbData.resize(mRGBBufferSize);
    std::memcpy(rgbData.data(), mRGBBuffer, mRGBBufferSize);
    
    // Apply HLG to linear conversion if needed
    if (mVideoInfo.isHLG) {
        applyHLGToLinear(rgbData);
    }
    
    return true;
}

void DirectLogDecoder::applyHLGToLinear(std::vector<uint8_t>& rgbData) {
    // Convert HLG to linear
    // HLG OECF inverse (simplified)
    for (size_t i = 0; i < rgbData.size(); i += 3) {
        for (int c = 0; c < 3; ++c) {
            float normalized = rgbData[i + c] / 255.0f;
            
            // HLG inverse OECF
            float linear;
            if (normalized <= 0.5f) {
                linear = normalized * normalized / 3.0f;
            } else {
                linear = (std::exp((normalized - 0.55991073f) / 0.17883277f) + 0.28466892f) / 12.0f;
            }
            
            rgbData[i + c] = static_cast<uint8_t>(std::clamp(linear * 255.0f, 0.0f, 255.0f));
        }
    }
}

bool DirectLogDecoder::isHLGVideo(const std::string& filePath) {
    return boost::icontains(filePath, "HLG_NATIVE");
}

void DirectLogDecoder::cleanup() {
    if (mRGBBuffer) {
        av_free(mRGBBuffer);
        mRGBBuffer = nullptr;
    }
    
    if (mSwsContext) {
        sws_freeContext(mSwsContext);
        mSwsContext = nullptr;
    }
    
    if (mFrame) {
        av_frame_free(&mFrame);
    }
    
    if (mRGBFrame) {
        av_frame_free(&mRGBFrame);
    }
    
    if (mPacket) {
        av_packet_free(&mPacket);
    }
    
    if (mCodecContext) {
        avcodec_free_context(&mCodecContext);
    }
    
    if (mFormatContext) {
        avformat_close_input(&mFormatContext);
    }
}

} // namespace motioncam