#include "DirectLogDecoder.h"
#include <spdlog/spdlog.h>
#include <boost/algorithm/string.hpp>
#include <stdexcept>
#include <cmath>
#include <algorithm>
#include <array>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <thread>
#include <unordered_map>

namespace {

bool directLogDiagnosticsEnabled() {
    static const bool enabled = [] {
        const char* value = std::getenv("MOTIONCAM_DIRECTLOG_DIAGNOSTICS");
        return value && value[0] != '\0' && std::string(value) != "0";
    }();
    return enabled;
}

double elapsedMilliseconds(const std::chrono::steady_clock::time_point start) {
    return std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - start).count();
}

struct CachedDirectLogTimeline {
    motioncam::DirectLogVideoInfo videoInfo;
    std::vector<motioncam::DirectLogFrameInfo> frames;
};

std::mutex directLogTimelineCacheMutex;
std::unordered_map<std::string, CachedDirectLogTimeline> directLogTimelineCache;
constexpr uint64_t directLogTimelineCacheMagic = 0x4d4346544c000002ULL;

std::string directLogTimelineCacheKey(const std::string& path) {
    std::error_code error;
    const auto absolute = std::filesystem::absolute(path, error).lexically_normal();
    const auto size = std::filesystem::file_size(absolute, error);
    if (error) return absolute.string();
    const auto modified = std::filesystem::last_write_time(absolute, error);
    if (error) return absolute.string() + ":" + std::to_string(size);
    return absolute.string() + ":" + std::to_string(size) + ":" +
           std::to_string(modified.time_since_epoch().count());
}

std::filesystem::path directLogTimelineCachePath(const std::string& key) {
    const char* cacheRoot = std::getenv("XDG_CACHE_HOME");
    std::filesystem::path root;
    if (cacheRoot && cacheRoot[0] != '\0') root = cacheRoot;
    else if (const char* home = std::getenv("HOME")) root = std::filesystem::path(home) / ".cache";
    else root = std::filesystem::temp_directory_path();
    return root / "motioncam-fuse" / "directlog-timelines" /
           (std::to_string(std::hash<std::string>{}(key)) + ".bin");
}

template<typename T> bool readValue(std::istream& input, T& value) {
    return static_cast<bool>(input.read(reinterpret_cast<char*>(&value), sizeof(value)));
}
template<typename T> void writeValue(std::ostream& output, const T& value) {
    output.write(reinterpret_cast<const char*>(&value), sizeof(value));
}
bool readString(std::istream& input, std::string& value, uint32_t maximum = 16384) {
    uint32_t size = 0;
    if (!readValue(input, size) || size > maximum) return false;
    value.resize(size);
    return size == 0 || static_cast<bool>(input.read(value.data(), size));
}
void writeString(std::ostream& output, const std::string& value) {
    const uint32_t size = static_cast<uint32_t>(value.size());
    writeValue(output, size);
    output.write(value.data(), size);
}

bool loadPersistentTimeline(const std::string& key, CachedDirectLogTimeline& cached) {
    std::ifstream input(directLogTimelineCachePath(key), std::ios::binary);
    uint64_t magic = 0;
    std::string storedKey;
    if (!readValue(input, magic) || magic != directLogTimelineCacheMagic ||
        !readString(input, storedKey) || storedKey != key) return false;
    auto& video = cached.videoInfo;
    uint8_t hlg = 0, log60 = 0;
    uint64_t count = 0;
    if (!readValue(input, video.width) || !readValue(input, video.height) ||
        !readValue(input, video.fps) || !readValue(input, video.totalFrames) ||
        !readString(input, video.pixelFormat, 256) || !readValue(input, hlg) ||
        !readValue(input, log60) || !readValue(input, video.duration) ||
        !readValue(input, count) || count > 10000000) return false;
    video.isHLG = hlg != 0;
    video.isLOG60 = log60 != 0;
    cached.frames.resize(static_cast<size_t>(count));
    for (size_t index = 0; index < cached.frames.size(); ++index) {
        auto& frame = cached.frames[index];
        uint8_t keyFrame = 0;
        if (!readValue(input, frame.pts) || !readValue(input, frame.timestamp) ||
            !readValue(input, frame.width) || !readValue(input, frame.height) ||
            !readValue(input, frame.timeBase) || !readValue(input, keyFrame)) return false;
        frame.frameNumber = static_cast<int>(index);
        frame.pixelFormat = video.pixelFormat;
        frame.keyFrame = keyFrame != 0;
    }
    return video.totalFrames == static_cast<int64_t>(cached.frames.size()) &&
           !cached.frames.empty() && input.peek() == std::char_traits<char>::eof();
}

void savePersistentTimeline(const std::string& key, const CachedDirectLogTimeline& cached) {
    const auto path = directLogTimelineCachePath(key);
    std::error_code error;
    std::filesystem::create_directories(path.parent_path(), error);
    if (error) return;
    const auto temporary = path.string() + ".tmp";
    std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
    if (!output) return;
    writeValue(output, directLogTimelineCacheMagic);
    writeString(output, key);
    const auto& video = cached.videoInfo;
    writeValue(output, video.width); writeValue(output, video.height);
    writeValue(output, video.fps); writeValue(output, video.totalFrames);
    writeString(output, video.pixelFormat);
    writeValue(output, static_cast<uint8_t>(video.isHLG));
    writeValue(output, static_cast<uint8_t>(video.isLOG60));
    writeValue(output, video.duration);
    writeValue(output, static_cast<uint64_t>(cached.frames.size()));
    for (const auto& frame : cached.frames) {
        writeValue(output, frame.pts); writeValue(output, frame.timestamp);
        writeValue(output, frame.width); writeValue(output, frame.height);
        writeValue(output, frame.timeBase);
        writeValue(output, static_cast<uint8_t>(frame.keyFrame));
    }
    output.close();
    if (!output) { std::filesystem::remove(temporary, error); return; }
    std::filesystem::rename(temporary, path, error);
    if (error) {
        std::filesystem::remove(path, error);
        error.clear();
        std::filesystem::rename(temporary, path, error);
    }
}

void configureSoftwareThreading(AVCodecContext* context) {
    context->thread_count = 0;
    context->thread_type = FF_THREAD_FRAME | FF_THREAD_SLICE;
}

template<typename Function>
void parallelPixelRanges(size_t count, Function&& function) {
    constexpr size_t minimumPerWorker = 1u << 20;
    const unsigned int available = std::max(1u, std::thread::hardware_concurrency());
    const unsigned int workers = static_cast<unsigned int>(std::min<size_t>(
        std::min<unsigned int>(available, 4),
        std::max<size_t>(1, (count + minimumPerWorker - 1) / minimumPerWorker)));
    if (workers == 1) {
        function(0, count);
        return;
    }
    std::vector<std::thread> threads;
    threads.reserve(workers - 1);
    for (unsigned int worker = 1; worker < workers; ++worker) {
        const size_t begin = count * worker / workers;
        const size_t end = count * (worker + 1) / workers;
        threads.emplace_back([&, begin, end] { function(begin, end); });
    }
    function(0, count / workers);
    for (auto& thread : threads) thread.join();
}

} // namespace

namespace motioncam {

DirectLogDecoder::DirectLogDecoder(const std::string& filePath)
    : mFilePath(filePath),
      mFormatContext(nullptr),
      mCodecContext(nullptr),
      mCodec(nullptr),
      mFrame(nullptr),
      mTransferFrame(nullptr),
      mPacket(nullptr),
      mSwsContext(nullptr),
      mHardwareDeviceContext(nullptr),
      mHardwarePixelFormat(AV_PIX_FMT_NONE),
      mVideoStreamIndex(-1),
      mLastDecodedFrame(-1) {
    
    spdlog::info("DirectLogDecoder: Initializing for {}", filePath);
    const auto initializationStarted = std::chrono::steady_clock::now();
    auto stageStarted = initializationStarted;
    initFFmpeg();
    if (directLogDiagnosticsEnabled())
        spdlog::info("DirectLog diagnostic: decoder_init stage=ffmpeg_open latency_ms={:.3f}",
                     elapsedMilliseconds(stageStarted));
    stageStarted = std::chrono::steady_clock::now();
    analyzeVideo();
    if (directLogDiagnosticsEnabled())
        spdlog::info(
            "DirectLog diagnostic: decoder_init stage=timeline latency_ms={:.3f} total_ms={:.3f}",
            elapsedMilliseconds(stageStarted), elapsedMilliseconds(initializationStarted));
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
    
    // Start with FFmpeg's default decoder for every codec. This exposes native
    // hardware configurations for AVC, HEVC, and AV1 while retaining the
    // software ProRes and CineForm decoders when no hardware path exists.
    mCodec = avcodec_find_decoder(codecpar->codec_id);
    if (!mCodec) {
        throw std::runtime_error("Unsupported codec");
    }
    // Decoder registration order can resolve a codec ID to an external
    // software decoder (notably AV1 -> libdav1d). Probe FFmpeg's native
    // decoder first because it advertises the platform hardware configs;
    // external decoders remain the explicit software fallback below.
    const char* nativeDecoderName = nullptr;
    switch (codecpar->codec_id) {
        case AV_CODEC_ID_AV1: nativeDecoderName = "av1"; break;
        case AV_CODEC_ID_HEVC: nativeDecoderName = "hevc"; break;
        case AV_CODEC_ID_H264: nativeDecoderName = "h264"; break;
        default: break;
    }
    if (nativeDecoderName) {
        if (const AVCodec* nativeDecoder = avcodec_find_decoder_by_name(nativeDecoderName))
            mCodec = nativeDecoder;
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
    
    const char* forceSoftwareValue = std::getenv("MOTIONCAM_DIRECTLOG_FORCE_SOFTWARE");
    const bool forceSoftware = forceSoftwareValue && forceSoftwareValue[0] != '\0' &&
                               std::string(forceSoftwareValue) != "0";
    bool hardwareDecoder = !forceSoftware && initHardwareDecoder();
    if (forceSoftware)
        spdlog::info("DirectLogDecoder: software decode forced by environment");
    if (!hardwareDecoder && codecpar->codec_id == AV_CODEC_ID_AV1) {
        if (const AVCodec* dav1d = avcodec_find_decoder_by_name("libdav1d")) {
            avcodec_free_context(&mCodecContext);
            mCodec = dav1d;
            mCodecContext = avcodec_alloc_context3(mCodec);
            if (!mCodecContext || avcodec_parameters_to_context(mCodecContext, codecpar) < 0)
                throw std::runtime_error("Could not configure libdav1d decoder");
        }
    }
    // Explicitly request the decoder's automatic thread configuration. The
    // API default can otherwise leave external decoders effectively serial.
    configureSoftwareThreading(mCodecContext);
    if (!hardwareDecoder)
        spdlog::info("DirectLogDecoder: using {} software decoding with automatic threading",
                     mCodec->name);

    // Open codec. A device being available does not guarantee support for the
    // clip's AV1 profile, bit depth, dimensions, or the installed driver. Retry
    // with the software decoder before rejecting an otherwise valid source.
    if (avcodec_open2(mCodecContext, mCodec, nullptr) < 0) {
        if (!hardwareDecoder)
            throw std::runtime_error("Could not open codec");
        spdlog::warn(
            "DirectLogDecoder: hardware decoder could not open clip; retrying in software");
        avcodec_free_context(&mCodecContext);
        av_buffer_unref(&mHardwareDeviceContext);
        mHardwarePixelFormat = AV_PIX_FMT_NONE;
        hardwareDecoder = false;
        mCodec = codecpar->codec_id == AV_CODEC_ID_AV1
            ? avcodec_find_decoder_by_name("libdav1d")
            : avcodec_find_decoder(codecpar->codec_id);
        if (!mCodec) mCodec = avcodec_find_decoder(codecpar->codec_id);
        mCodecContext = mCodec ? avcodec_alloc_context3(mCodec) : nullptr;
        if (!mCodecContext ||
            avcodec_parameters_to_context(mCodecContext, codecpar) < 0)
            throw std::runtime_error("Could not configure software decoder fallback");
        configureSoftwareThreading(mCodecContext);
        if (avcodec_open2(mCodecContext, mCodec, nullptr) < 0)
            throw std::runtime_error("Could not open software decoder fallback");
        spdlog::info("DirectLogDecoder: using {} software decoding with automatic threading",
                     mCodec->name);
    }
    
    // Allocate frames and packet
    mFrame = av_frame_alloc();
    mTransferFrame = av_frame_alloc();
    mPacket = av_packet_alloc();

    if (!mFrame || !mTransferFrame || !mPacket) {
        throw std::runtime_error("Could not allocate frame or packet");
    }
    
    mTimeBase = mFormatContext->streams[mVideoStreamIndex]->time_base;
}

void DirectLogDecoder::analyzeVideo() {
    const std::string cacheKey = directLogTimelineCacheKey(mFilePath);
    {
        std::lock_guard<std::mutex> lock(directLogTimelineCacheMutex);
        if (const auto cached = directLogTimelineCache.find(cacheKey);
            cached != directLogTimelineCache.end()) {
            mVideoInfo = cached->second.videoInfo;
            mFrames = cached->second.frames;
            spdlog::info("DirectLogDecoder: reused cached timeline for {} ({} frames)",
                         mFilePath, mFrames.size());
            return;
        }
        CachedDirectLogTimeline persistent;
        if (loadPersistentTimeline(cacheKey, persistent)) {
            mVideoInfo = persistent.videoInfo;
            mFrames = persistent.frames;
            directLogTimelineCache.emplace(cacheKey, std::move(persistent));
            spdlog::info("DirectLogDecoder: reused persistent timeline for {} ({} frames)",
                         mFilePath, mFrames.size());
            return;
        }
    }
    mVideoInfo.width = mCodecContext->width;
    mVideoInfo.height = mCodecContext->height;
    
    // Determine pixel format
    switch (mCodecContext->pix_fmt) {
        case AV_PIX_FMT_YUV420P:
            mVideoInfo.pixelFormat = "yuv420p";
            break;
        case AV_PIX_FMT_YUV422P:
            mVideoInfo.pixelFormat = "yuv422p";
            break;
        case AV_PIX_FMT_YUV444P:
            mVideoInfo.pixelFormat = "yuv444p";
            break;
        case AV_PIX_FMT_YUV420P10LE:
            mVideoInfo.pixelFormat = "yuv420p10le";
            break;
        case AV_PIX_FMT_YUV422P10LE:
            mVideoInfo.pixelFormat = "yuv422p10le";
            break;
        case AV_PIX_FMT_YUV444P10LE:
            mVideoInfo.pixelFormat = "yuv444p10le";
            break;
        default:
            mVideoInfo.pixelFormat = "unknown";
            break;
    }
    
    // Check if HLG based on filename
    mVideoInfo.isHLG = boost::icontains(mFilePath, "HLG_NATIVE");
    mVideoInfo.isLOG60 = boost::icontains(mFilePath, "LOG60_NATIVE");
    
    // Don't rely on container's average framerate - we'll calculate from actual frame timestamps
    // This allows proper CFR conversion handling similar to MCRAW
    mVideoInfo.fps = 0.0; // Will be calculated from actual frame intervals
    
    mVideoInfo.duration = static_cast<double>(mFormatContext->duration) / AV_TIME_BASE;
    
    // Decode frames to build the presentation timeline. Packet timestamps and
    // packet counts are not guaranteed to map one-to-one to displayed frames.
    mFrames.clear();
    auto appendDecodedFrames = [&]() {
        while (avcodec_receive_frame(mCodecContext, mFrame) == 0) {
            int64_t pts = mFrame->best_effort_timestamp;
            if (pts == AV_NOPTS_VALUE) pts = mFrame->pts;
            if (pts == AV_NOPTS_VALUE) continue;
            DirectLogFrameInfo frameInfo;
            frameInfo.frameNumber = 0;
            frameInfo.pts = pts;
            frameInfo.timestamp = static_cast<Timestamp>(
                pts * av_q2d(mTimeBase) * 1000000000.0);
            frameInfo.width = mFrame->width;
            frameInfo.height = mFrame->height;
            frameInfo.pixelFormat = mVideoInfo.pixelFormat;
            frameInfo.timeBase = av_q2d(mTimeBase);
            frameInfo.keyFrame = (mFrame->flags & AV_FRAME_FLAG_KEY) != 0;
            mFrames.push_back(frameInfo);
        }
    };
    while (av_read_frame(mFormatContext, mPacket) >= 0) {
        if (mPacket->stream_index == mVideoStreamIndex) {
            int sendResult = avcodec_send_packet(mCodecContext, mPacket);
            if (sendResult == AVERROR(EAGAIN)) {
                appendDecodedFrames();
                sendResult = avcodec_send_packet(mCodecContext, mPacket);
            }
            if (sendResult == 0) appendDecodedFrames();
        }
        av_packet_unref(mPacket);
    }
    avcodec_send_packet(mCodecContext, nullptr);
    appendDecodedFrames();

    // Sort by PTS to obtain display order for decoders that emit frames in a
    // different order, and discard duplicate timestamps defensively.
    std::sort(mFrames.begin(), mFrames.end(), [](const auto& a, const auto& b) {
        return a.pts < b.pts;
    });
    mFrames.erase(std::unique(mFrames.begin(), mFrames.end(), [](const auto& a, const auto& b) {
        return a.pts == b.pts;
    }), mFrames.end());
    for (size_t i = 0; i < mFrames.size(); ++i)
        mFrames[i].frameNumber = static_cast<int>(i);
    mVideoInfo.totalFrames = mFrames.size();
    
    // Seek back to beginning
    av_seek_frame(mFormatContext, mVideoStreamIndex, 0, AVSEEK_FLAG_BACKWARD);
    if (mCodecContext) {
        avcodec_flush_buffers(mCodecContext);
    }
    {
        std::lock_guard<std::mutex> lock(directLogTimelineCacheMutex);
        CachedDirectLogTimeline cached{mVideoInfo, mFrames};
        directLogTimelineCache[cacheKey] = cached;
        savePersistentTimeline(cacheKey, cached);
    }
    
    spdlog::info("DirectLogDecoder: Analyzed video - {}x{} @ {:.2f}fps, {} frames, format: {}, HLG: {}, LOG60: {}",
                 mVideoInfo.width, mVideoInfo.height, mVideoInfo.fps, mVideoInfo.totalFrames,
                 mVideoInfo.pixelFormat, mVideoInfo.isHLG, mVideoInfo.isLOG60);
}



bool DirectLogDecoder::extractFrame(int frameNumber, std::vector<uint16_t>& rgbData,
                                    int outputWidth, int outputHeight,
                                    bool preserveLogEncoded) {
    std::lock_guard<std::mutex> lock(mMutex);
    const bool diagnostics = directLogDiagnosticsEnabled();
    const auto extractStart = std::chrono::steady_clock::now();
    
    if (frameNumber < 0 || frameNumber >= static_cast<int>(mFrames.size())) {
        return false;
    }
    
    const DirectLogFrameInfo& frameInfo = mFrames[frameNumber];
    
    // Playback may intentionally skip source frames (for example, displaying
    // 15 fps from a 60 fps sequence). Reuse the current decoder whenever it is
    // already inside the target's GOP; requiring exactly N+1 would restart the
    // GOP for every N+3 request and create progressively longer stalls.
    int precedingKeyFrame = frameNumber;
    while (precedingKeyFrame > 0 && !mFrames[precedingKeyFrame].keyFrame)
        --precedingKeyFrame;
    // Staying in the current GOP is only beneficial for the small skips made
    // by preview frame-rate limiting. A user seek can land hundreds of frames
    // ahead while still sharing a (missing or very distant) keyframe; treating
    // that as sequential made the decoder walk every intervening frame.
    constexpr int maxSequentialSkip = 8;
    const int forwardDistance = frameNumber - mLastDecodedFrame;
    const bool sequential = mLastDecodedFrame >= precedingKeyFrame &&
                            forwardDistance > 0 &&
                            forwardDistance <= maxSequentialSkip;
    if (diagnostics)
        spdlog::info("DirectLog diagnostic: frame={} decoder begin mode={} pts={}",
                     frameNumber, sequential ? "sequential" : "seek", frameInfo.pts);
    if (!sequential) {
        if (av_seek_frame(mFormatContext, mVideoStreamIndex, frameInfo.pts,
                          AVSEEK_FLAG_BACKWARD) < 0) {
            spdlog::error("Failed to seek to frame {}", frameNumber);
            mLastDecodedFrame = -1;
            return false;
        }
        avcodec_flush_buffers(mCodecContext);
        if (diagnostics)
            spdlog::info("DirectLog diagnostic: frame={} seek+flush complete elapsed_ms={:.3f}",
                         frameNumber, elapsedMilliseconds(extractStart));
    }

    size_t packetsRead = 0;
    size_t framesDecoded = 0;
    auto receiveTarget = [&]() -> int {
        while (avcodec_receive_frame(mCodecContext, mFrame) == 0) {
            ++framesDecoded;
            int64_t decodedPts = mFrame->best_effort_timestamp;
            if (decodedPts == AV_NOPTS_VALUE) decodedPts = mFrame->pts;
            if (decodedPts == frameInfo.pts) {
                const auto conversionStart = std::chrono::steady_clock::now();
                if (diagnostics)
                    spdlog::info("DirectLog diagnostic: frame={} target decoded packets={} decoded_frames={} elapsed_ms={:.3f}; conversion begin",
                                 frameNumber, packetsRead, framesDecoded,
                                 elapsedMilliseconds(extractStart));
                AVFrame* conversionFrame = transferableFrame(mFrame);
                if (!conversionFrame || !convertYUVToRGB(
                        conversionFrame, rgbData, outputWidth, outputHeight,
                        preserveLogEncoded)) return -1;
                mLastDecodedFrame = frameNumber;
                if (diagnostics)
                    spdlog::info("DirectLog diagnostic: frame={} conversion_ms={:.3f} decoder_total_ms={:.3f}",
                                 frameNumber, elapsedMilliseconds(conversionStart),
                                 elapsedMilliseconds(extractStart));
                return 1;
            }
            // Decoder output is in presentation order. Once it has passed the
            // requested PTS, continuing to EOF cannot find the target and can
            // make a malformed or unusual timeline look like a hung read.
            if (decodedPts != AV_NOPTS_VALUE && decodedPts > frameInfo.pts)
                return -1;
        }
        return 0;
    };

    if (sequential) {
        const int received = receiveTarget();
        if (received != 0) return received > 0;
    }

    while (av_read_frame(mFormatContext, mPacket) >= 0) {
        ++packetsRead;
        if (mPacket->stream_index == mVideoStreamIndex) {
            int sendResult = avcodec_send_packet(mCodecContext, mPacket);
            if (sendResult == AVERROR(EAGAIN)) {
                const int received = receiveTarget();
                if (received != 0) {
                    av_packet_unref(mPacket);
                    return received > 0;
                }
                sendResult = avcodec_send_packet(mCodecContext, mPacket);
            }
            if (sendResult == 0) {
                const int received = receiveTarget();
                if (received != 0) {
                    av_packet_unref(mPacket);
                    return received > 0;
                }
            }
        }
        av_packet_unref(mPacket);
    }

    // Drain delayed frames after the demuxer reaches EOF.
    avcodec_send_packet(mCodecContext, nullptr);
    while (avcodec_receive_frame(mCodecContext, mFrame) == 0) {
        ++framesDecoded;
        int64_t decodedPts = mFrame->best_effort_timestamp;
        if (decodedPts == AV_NOPTS_VALUE) decodedPts = mFrame->pts;
        if (decodedPts == frameInfo.pts) {
            const auto conversionStart = std::chrono::steady_clock::now();
            AVFrame* conversionFrame = transferableFrame(mFrame);
            if (!conversionFrame || !convertYUVToRGB(
                    conversionFrame, rgbData, outputWidth, outputHeight,
                    preserveLogEncoded)) break;
            mLastDecodedFrame = frameNumber;
            if (diagnostics)
                spdlog::info("DirectLog diagnostic: frame={} EOF-drain conversion_ms={:.3f} packets={} decoded_frames={} total_ms={:.3f}",
                             frameNumber, elapsedMilliseconds(conversionStart), packetsRead,
                             framesDecoded, elapsedMilliseconds(extractStart));
            return true;
        }
        if (decodedPts != AV_NOPTS_VALUE && decodedPts > frameInfo.pts) break;
    }

    mLastDecodedFrame = -1;
    return false;
}

bool DirectLogDecoder::initHardwareDecoder() {
    const AVHWDeviceType preferred[] = {
#ifdef _WIN32
        AV_HWDEVICE_TYPE_D3D11VA,
        AV_HWDEVICE_TYPE_CUDA,
        AV_HWDEVICE_TYPE_QSV,
#elif defined(__APPLE__)
        AV_HWDEVICE_TYPE_VIDEOTOOLBOX,
#else
        AV_HWDEVICE_TYPE_CUDA,
        AV_HWDEVICE_TYPE_VAAPI,
        AV_HWDEVICE_TYPE_QSV,
#endif
        AV_HWDEVICE_TYPE_VULKAN
    };
    for (AVHWDeviceType deviceType : preferred) {
        for (int index = 0;; ++index) {
            const AVCodecHWConfig* config = avcodec_get_hw_config(mCodec, index);
            if (!config) break;
            if (config->device_type != deviceType ||
                !(config->methods & AV_CODEC_HW_CONFIG_METHOD_HW_DEVICE_CTX))
                continue;
            AVBufferRef* device = nullptr;
            const int createResult = av_hwdevice_ctx_create(
                &device, deviceType, nullptr, nullptr, 0);
            if (createResult < 0) {
                if (directLogDiagnosticsEnabled())
                    spdlog::info(
                        "DirectLog diagnostic: hardware device={} unavailable error={}",
                        av_hwdevice_get_type_name(deviceType), createResult);
                continue;
            }
            mHardwareDeviceContext = device;
            mHardwarePixelFormat = config->pix_fmt;
            mCodecContext->hw_device_ctx = av_buffer_ref(mHardwareDeviceContext);
            mCodecContext->opaque = this;
            mCodecContext->get_format = &DirectLogDecoder::selectPixelFormat;
            spdlog::info("DirectLogDecoder: using {} hardware decoding",
                         av_hwdevice_get_type_name(deviceType));
            return true;
        }
    }
    return false;
}

AVPixelFormat DirectLogDecoder::selectPixelFormat(
        AVCodecContext* context, const AVPixelFormat* formats) {
    const auto* decoder = static_cast<const DirectLogDecoder*>(context->opaque);
    for (const AVPixelFormat* format = formats; *format != AV_PIX_FMT_NONE; ++format)
        if (*format == decoder->mHardwarePixelFormat) return *format;
    spdlog::warn("DirectLogDecoder: requested hardware pixel format unavailable");
    // A device can exist while not supporting this codec profile. Select an
    // offered software format instead of accidentally choosing an unrelated
    // hardware format that has no matching device context.
    for (const AVPixelFormat* format = formats; *format != AV_PIX_FMT_NONE; ++format) {
        const AVPixFmtDescriptor* descriptor = av_pix_fmt_desc_get(*format);
        if (descriptor && !(descriptor->flags & AV_PIX_FMT_FLAG_HWACCEL)) return *format;
    }
    return formats[0];
}

AVFrame* DirectLogDecoder::transferableFrame(AVFrame* frame) {
    if (frame->format != mHardwarePixelFormat || !mHardwareDeviceContext) return frame;
    av_frame_unref(mTransferFrame);
    if (av_hwframe_transfer_data(mTransferFrame, frame, 0) < 0) {
        spdlog::error("DirectLogDecoder: hardware frame transfer failed");
        return nullptr;
    }
    av_frame_copy_props(mTransferFrame, frame);
    return mTransferFrame;
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

void DirectLogDecoder::setFullRangeOverride(std::optional<bool> fullRange) {
    std::lock_guard<std::mutex> lock(mMutex);
    mFullRangeOverride = fullRange;
    mFullRange.reset();
}

bool DirectLogDecoder::convertYUVToRGB(AVFrame* yuvFrame, std::vector<uint16_t>& rgbData,
                                       int outputWidth, int outputHeight,
                                       bool preserveLogEncoded) {
    const bool diagnostics = directLogDiagnosticsEnabled();
    const auto conversionStart = std::chrono::steady_clock::now();
    const int width = mVideoInfo.width;
    const int height = mVideoInfo.height;
    if (outputWidth <= 0) outputWidth = width;
    if (outputHeight <= 0) outputHeight = height;
    rgbData.resize(static_cast<size_t>(outputWidth) * outputHeight * 3);

    if (!mFullRange.has_value()) {
        AVColorRange colorRange = yuvFrame->color_range;
        if (colorRange == AVCOL_RANGE_UNSPECIFIED)
            colorRange = mCodecContext->color_range;
        mFullRange = mFullRangeOverride.value_or(colorRange == AVCOL_RANGE_JPEG);
        spdlog::info(
            "DirectLogDecoder: treating clip as {} range (metadata: {}, override: {})",
            *mFullRange ? "full" : "limited", av_color_range_name(colorRange),
            mFullRangeOverride.has_value() ? (*mFullRangeOverride ? "Full" : "Limited") : "Auto");
    }
    const bool fullRange = *mFullRange;

    mSwsContext = sws_getCachedContext(
        mSwsContext, width, height, static_cast<AVPixelFormat>(yuvFrame->format),
        outputWidth, outputHeight, AV_PIX_FMT_RGB48LE,
        outputWidth == width && outputHeight == height ? SWS_BILINEAR : SWS_POINT,
        nullptr, nullptr, nullptr);
    if (!mSwsContext) return false;

    const int* coefficients = sws_getCoefficients(SWS_CS_BT2020);
    if (sws_setColorspaceDetails(mSwsContext, coefficients, fullRange ? 1 : 0,
                                 coefficients, 1, 0, 1 << 16, 1 << 16) < 0)
        return false;

    const uint8_t* input[] = {
        yuvFrame->data[0], yuvFrame->data[1], yuvFrame->data[2], yuvFrame->data[3]};
    int inputStride[] = {
        yuvFrame->linesize[0], yuvFrame->linesize[1],
        yuvFrame->linesize[2], yuvFrame->linesize[3]};

    // DirectLog scales nominal 8-bit limited-range codes over all 1023 values:
    // Y 16..235 becomes 64.19..942.53 rather than conventional 64..940, and
    // chroma 16..240 becomes 64.19..962.82 rather than 64..960. Multiplying
    // 10-bit samples by 1020/1023 maps that endpoint interpretation into
    // swscale's conventional limited-range domain.
    const auto pixelFormat = static_cast<AVPixelFormat>(yuvFrame->format);
    const bool tenBit = pixelFormat == AV_PIX_FMT_YUV420P10LE ||
                        pixelFormat == AV_PIX_FMT_YUV422P10LE;
    if (!fullRange && tenBit) {
        const auto rangeStart = std::chrono::steady_clock::now();
        const AVPixFmtDescriptor* descriptor = av_pix_fmt_desc_get(pixelFormat);
        if (!descriptor) return false;
        const int chromaWidth = AV_CEIL_RSHIFT(width, descriptor->log2_chroma_w);
        const int chromaHeight = AV_CEIL_RSHIFT(height, descriptor->log2_chroma_h);
        const size_t lumaSamples = static_cast<size_t>(width) * height;
        const size_t chromaSamples = static_cast<size_t>(chromaWidth) * chromaHeight;
        mLimitedRangeScratch.resize(lumaSamples + 2 * chromaSamples);

        auto remapPlane = [](const uint8_t* source, int sourceStride,
                             uint16_t* destination, int planeWidth, int planeHeight) {
            for (int y = 0; y < planeHeight; ++y) {
                const auto* row = reinterpret_cast<const uint16_t*>(
                    source + static_cast<ptrdiff_t>(y) * sourceStride);
                for (int x = 0; x < planeWidth; ++x)
                    destination[static_cast<size_t>(y) * planeWidth + x] =
                        static_cast<uint16_t>((static_cast<unsigned int>(row[x]) * 1020 + 511) / 1023);
            }
        };
        uint16_t* luma = mLimitedRangeScratch.data();
        uint16_t* chromaU = luma + lumaSamples;
        uint16_t* chromaV = chromaU + chromaSamples;
        remapPlane(yuvFrame->data[0], yuvFrame->linesize[0], luma, width, height);
        remapPlane(yuvFrame->data[1], yuvFrame->linesize[1], chromaU, chromaWidth, chromaHeight);
        remapPlane(yuvFrame->data[2], yuvFrame->linesize[2], chromaV, chromaWidth, chromaHeight);
        input[0] = reinterpret_cast<const uint8_t*>(luma);
        input[1] = reinterpret_cast<const uint8_t*>(chromaU);
        input[2] = reinterpret_cast<const uint8_t*>(chromaV);
        inputStride[0] = width * static_cast<int>(sizeof(uint16_t));
        inputStride[1] = inputStride[2] = chromaWidth * static_cast<int>(sizeof(uint16_t));
        if (diagnostics)
            spdlog::info("DirectLog diagnostic: limited-range remap_ms={:.3f}",
                         elapsedMilliseconds(rangeStart));
    }

    const auto scaleStart = std::chrono::steady_clock::now();
    uint8_t* output[] = {reinterpret_cast<uint8_t*>(rgbData.data())};
    int outputStride[] = {outputWidth * 3 * static_cast<int>(sizeof(uint16_t))};
    if (sws_scale(mSwsContext, input, inputStride, 0, height,
                  output, outputStride) != outputHeight)
        return false;
    if (diagnostics)
        spdlog::info("DirectLog diagnostic: swscale_ms={:.3f}; transfer stage begin",
                     elapsedMilliseconds(scaleStart));
    
    // Apply HLG to linear conversion if needed
    if (mVideoInfo.isHLG) {
        applyHLGToLinear(rgbData);
    } else if (mVideoInfo.isLOG60 && !preserveLogEncoded) {
        applyLOG60ToLinear(rgbData);
    }
    if (diagnostics)
        spdlog::info("DirectLog diagnostic: transfer_and_conversion_total_ms={:.3f}",
                     elapsedMilliseconds(conversionStart));
    
    return true;
}

void DirectLogDecoder::applyHLGToLinear(std::vector<uint16_t>& rgbData) {
    static const auto lut = [] {
        std::array<uint16_t, 65536> values{};
        for (size_t i = 0; i < values.size(); ++i) {
            const float encoded = i / 65535.0f;
            const float linear = encoded <= 0.5f
                ? encoded * encoded / 3.0f
                : (std::exp((encoded - 0.55991073f) / 0.17883277f) + 0.28466892f) / 12.0f;
            values[i] = static_cast<uint16_t>(
                std::clamp(std::lround(linear * 65535.0f), 0l, 65535l));
        }
        return values;
    }();
    parallelPixelRanges(rgbData.size(), [&](size_t begin, size_t end) {
        for (size_t i = begin; i < end; ++i) rgbData[i] = lut[rgbData[i]];
    });
}

bool DirectLogDecoder::isHLGVideo(const std::string& filePath) {
    return boost::icontains(filePath, "HLG_NATIVE");
}

void DirectLogDecoder::applyLOG60ToLinear(std::vector<uint16_t>& rgbData) {
    static const auto lut = [] {
        std::array<uint16_t, 65536> values{};
        for (size_t i = 0; i < values.size(); ++i) {
            const float encoded = i / 65535.0f;
            const float linear = (std::pow(61.0f, encoded) - 1.0f) / 60.0f;
            values[i] = static_cast<uint16_t>(std::clamp(
                std::lround(linear * 65535.0f), 0l, 65535l));
        }
        return values;
    }();
    parallelPixelRanges(rgbData.size(), [&](size_t begin, size_t end) {
        for (size_t i = begin; i < end; ++i) rgbData[i] = lut[rgbData[i]];
    });
}

bool DirectLogDecoder::isLOG60Video(const std::string& filePath) {
    return boost::icontains(filePath, "LOG60_NATIVE");
}

void DirectLogDecoder::cleanup() {
    if (mSwsContext) {
        sws_freeContext(mSwsContext);
        mSwsContext = nullptr;
    }
    if (mFrame) {
        av_frame_free(&mFrame);
    }
    if (mTransferFrame) {
        av_frame_free(&mTransferFrame);
    }
    
    if (mPacket) {
        av_packet_free(&mPacket);
    }
    
    if (mCodecContext) {
        avcodec_free_context(&mCodecContext);
    }
    if (mHardwareDeviceContext) {
        av_buffer_unref(&mHardwareDeviceContext);
    }
    
    if (mFormatContext) {
        avformat_close_input(&mFormatContext);
    }
}

} // namespace motioncam
