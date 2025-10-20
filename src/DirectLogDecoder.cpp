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
      mFrame(nullptr),
      mPacket(nullptr),
      mVideoStreamIndex(-1) {
    
    spdlog::info("DirectLogDecoder: Initializing for {}", filePath);
    
    initFFmpeg();
    analyzeVideo();
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
    mPacket = av_packet_alloc();
    
    if (!mFrame || !mPacket) {
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
    
    // Don't rely on container's average framerate - we'll calculate from actual frame timestamps
    // This allows proper CFR conversion handling similar to MCRAW
    mVideoInfo.fps = 0.0; // Will be calculated from actual frame intervals
    
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
    if (mCodecContext) {
        avcodec_flush_buffers(mCodecContext);
    }
    
    spdlog::info("DirectLogDecoder: Analyzed video - {}x{} @ {:.2f}fps, {} frames, format: {}, HLG: {}", 
                 mVideoInfo.width, mVideoInfo.height, mVideoInfo.fps, mVideoInfo.totalFrames, 
                 mVideoInfo.pixelFormat, mVideoInfo.isHLG);
}



bool DirectLogDecoder::extractFrame(int frameNumber, std::vector<uint16_t>& rgbData) {
    std::lock_guard<std::mutex> lock(mMutex);
    
    if (frameNumber < 0 || frameNumber >= static_cast<int>(mFrames.size())) {
        return false;
    }
    
    const DirectLogFrameInfo& frameInfo = mFrames[frameNumber];
    
    // Seek to frame
    if (av_seek_frame(mFormatContext, mVideoStreamIndex, frameInfo.pts, AVSEEK_FLAG_BACKWARD) < 0) {
        spdlog::error("Failed to seek to frame {}", frameNumber);
        return false;
    }
    
    if (mCodecContext) {
        avcodec_flush_buffers(mCodecContext);
    }
    
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

bool DirectLogDecoder::extractFrameByTimestamp(Timestamp timestamp, std::vector<uint16_t>& rgbData) {
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

bool DirectLogDecoder::convertYUVToRGB(AVFrame* yuvFrame, std::vector<uint16_t>& rgbData) {
    // Manual YUV to RGB conversion using Rec.2020 color space
    // Input: YUV with limited range (16-235 for Y, 16-240 for UV in 8-bit, scaled for 10-bit)
    // Output: RGB 16-bit per channel with full range (0-65535)
    
    const int width = mVideoInfo.width;
    const int height = mVideoInfo.height;
    
    // Allocate output buffer (3 channels * 16-bit)
    rgbData.resize(width * height * 3);
    
    // Rec.2020 YCbCr to RGB conversion matrix coefficients
    const double Kr = 0.2627;
    const double Kg = 0.6780;
    const double Kb = 0.0593;
    
    // Determine bit depth and scaling factors
    bool is10bit = (mCodecContext->pix_fmt == AV_PIX_FMT_YUV420P10LE || 
                    mCodecContext->pix_fmt == AV_PIX_FMT_YUV422P10LE);
    int bitDepth = is10bit ? 10 : 8;
    int maxInput = (1 << bitDepth) - 1;  // 255 for 8-bit, 1023 for 10-bit
    
    // Limited range parameters
    double yMin = 16.0 * (maxInput / 255.0);
    double yMax = 235.0 * (maxInput / 255.0);
    double cMin = 16.0 * (maxInput / 255.0);
    double cMax = 240.0 * (maxInput / 255.0);
    
    // Get plane pointers and strides
    uint8_t* yPlane = yuvFrame->data[0];
    uint8_t* uPlane = yuvFrame->data[1];
    uint8_t* vPlane = yuvFrame->data[2];
    
    int yStride = yuvFrame->linesize[0];
    int uStride = yuvFrame->linesize[1];
    int vStride = yuvFrame->linesize[2];
    
    // Determine chroma subsampling
    int chromaHeightDiv = (mCodecContext->pix_fmt == AV_PIX_FMT_YUV422P10LE ? 1 : 2);
    int chromaWidthDiv = 2;
    
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            // Read Y value
            double yVal;
            if (is10bit) {
                uint16_t* yPtr = reinterpret_cast<uint16_t*>(yPlane + y * yStride);
                yVal = yPtr[x];
            } else {
                yVal = yPlane[y * yStride + x];
            }
            
            // Read U and V values (with chroma subsampling)
            int chromaX = x / chromaWidthDiv;
            int chromaY = y / chromaHeightDiv;
            
            double uVal, vVal;
            if (is10bit) {
                uint16_t* uPtr = reinterpret_cast<uint16_t*>(uPlane + chromaY * uStride);
                uint16_t* vPtr = reinterpret_cast<uint16_t*>(vPlane + chromaY * vStride);
                uVal = uPtr[chromaX];
                vVal = vPtr[chromaX];
            } else {
                uVal = uPlane[chromaY * uStride + chromaX];
                vVal = vPlane[chromaY * vStride + chromaX];
            }
            
            // Convert from limited range to full range [0, 1]
            double yNorm = (yVal - yMin) / (yMax - yMin);
            double uNorm = (uVal - cMin) / (cMax - cMin) - 0.5;
            double vNorm = (vVal - cMin) / (cMax - cMin) - 0.5;
            
            // Clamp normalized values
            yNorm = std::clamp(yNorm, 0.0, 1.0);
            
            // Rec.2020 YCbCr to RGB conversion
            double r = yNorm + 2.0 * (1.0 - Kr) * vNorm;
            double g = yNorm - 2.0 * Kb * (1.0 - Kb) / Kg * uNorm - 2.0 * Kr * (1.0 - Kr) / Kg * vNorm;
            double b = yNorm + 2.0 * (1.0 - Kb) * uNorm;
            
            // Clamp to [0, 1] and convert to 16-bit full range
            r = std::clamp(r, 0.0, 1.0);
            g = std::clamp(g, 0.0, 1.0);
            b = std::clamp(b, 0.0, 1.0);
            
            int idx = (y * width + x) * 3;
            rgbData[idx + 0] = static_cast<uint16_t>(r * 65535.0 + 0.5);
            rgbData[idx + 1] = static_cast<uint16_t>(g * 65535.0 + 0.5);
            rgbData[idx + 2] = static_cast<uint16_t>(b * 65535.0 + 0.5);
        }
    }
    
    // Apply HLG to linear conversion if needed
    if (mVideoInfo.isHLG) {
        applyHLGToLinear(rgbData);
    }
    
    return true;
}

void DirectLogDecoder::applyHLGToLinear(std::vector<uint16_t>& rgbData) {
    // Convert HLG to linear
    // HLG OECF inverse (simplified)
    for (size_t i = 0; i < rgbData.size(); i += 3) {
        for (int c = 0; c < 3; ++c) {
            float normalized = rgbData[i + c] / 65535.0f;
            
            // HLG inverse OECF
            float linear;
            if (normalized <= 0.5f) {
                linear = normalized * normalized / 3.0f;
            } else {
                linear = (std::exp((normalized - 0.55991073f) / 0.17883277f) + 0.28466892f) / 12.0f;
            }
            
            rgbData[i + c] = static_cast<uint16_t>(std::clamp(linear * 65535.0f, 0.0f, 65535.0f));
        }
    }
}

bool DirectLogDecoder::isHLGVideo(const std::string& filePath) {
    return boost::icontains(filePath, "HLG_NATIVE");
}

void DirectLogDecoder::cleanup() {
    if (mFrame) {
        av_frame_free(&mFrame);
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