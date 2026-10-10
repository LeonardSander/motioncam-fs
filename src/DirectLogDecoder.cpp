#include "DirectLogDecoder.h"
#include "DirectLogGpuRgb.h"
#include "CpuWorkerBudget.h"
#include <spdlog/spdlog.h>
#include <boost/algorithm/string.hpp>
#include <stdexcept>
#include <cmath>
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <thread>
#include <unordered_map>
extern "C" {
#include <libavutil/error.h>
}
#ifdef MOTIONCAM_HAS_AVFILTER
extern "C" {
#include <libavfilter/buffersink.h>
#include <libavfilter/buffersrc.h>
}
#endif

namespace {

std::string ffmpegErrorString(int error) {
    char buffer[AV_ERROR_MAX_STRING_SIZE] = {};
    av_strerror(error, buffer, sizeof(buffer));
    return buffer;
}

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
// Version 5 timelines omit packets marked for discard. Some cameras append a
// final, timestamped HEVC access unit which the decoder intentionally emits no
// frame for; retaining it creates a phantom still at the end of the clip.
constexpr uint64_t directLogTimelineCacheMagic = 0x4d4346544c000005ULL;

std::string directLogTimelineCacheKey(const std::string& path) {
    std::error_code error;
    const auto absolute = std::filesystem::absolute(path, error).lexically_normal();
    const auto size = std::filesystem::file_size(absolute, error);
    if (error) return absolute.string();
    const auto modified = std::filesystem::last_write_time(absolute, error);
    if (error) return absolute.string() + ":" + std::to_string(size);
    return absolute.string() + ":" + std::to_string(size) + ":" +
           std::to_string(static_cast<long long>(modified.time_since_epoch().count()));
}

// FFmpeg's timeline scan and hardware probing can allocate several decoder
// surfaces/thread pools.  The gallery may request a second decoder for a
// thumbnail while the mounted decoder is still alive; serialize initialization
// so those peak allocations cannot stack during a long import.
std::mutex directLogInitializationMutex;
std::atomic_uint directLogHardwareDecoderCount{0};

struct SharedHardwareDevice {
    AVBufferRef* reference = nullptr;
    unsigned int users = 0;
};

std::mutex directLogHardwareDeviceMutex;
std::unordered_map<int, SharedHardwareDevice> directLogHardwareDevices;

AVBufferRef* acquireHardwareDevice(AVHWDeviceType type) {
    std::lock_guard lock(directLogHardwareDeviceMutex);
    auto& shared = directLogHardwareDevices[static_cast<int>(type)];
    if (!shared.reference) {
        if (av_hwdevice_ctx_create(&shared.reference, type, nullptr, nullptr, 0) < 0) {
            directLogHardwareDevices.erase(static_cast<int>(type));
            return nullptr;
        }
        spdlog::info("DirectLogDecoder: created shared {} hardware device",
                     av_hwdevice_get_type_name(type));
    }
    AVBufferRef* reference = av_buffer_ref(shared.reference);
    if (!reference) {
        if (shared.users == 0) {
            av_buffer_unref(&shared.reference);
            directLogHardwareDevices.erase(static_cast<int>(type));
        }
        return nullptr;
    }
    ++shared.users;
    return reference;
}

void releaseHardwareDevice(AVBufferRef** reference) {
    if (!reference || !*reference) return;
    const auto* device = reinterpret_cast<const AVHWDeviceContext*>((*reference)->data);
    const int type = device ? static_cast<int>(device->type) : -1;
    std::lock_guard lock(directLogHardwareDeviceMutex);
    av_buffer_unref(reference);
    const auto it = directLogHardwareDevices.find(type);
    if (it == directLogHardwareDevices.end()) return;
    if (--it->second.users == 0) {
        av_buffer_unref(&it->second.reference);
        directLogHardwareDevices.erase(it);
        spdlog::info("DirectLogDecoder: released shared {} hardware device",
                     av_hwdevice_get_type_name(static_cast<AVHWDeviceType>(type)));
    }
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
        !readValue(input, video.orientation) ||
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
    writeValue(output, video.orientation);
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
    // Automatic threading can create a large frame-thread pool for high
    // resolution HEVC/AV1.  A bounded pool keeps multiple imported clips from
    // exhausting system memory while retaining parallel decode.
    context->thread_count = static_cast<int>(std::min(
        4u, std::max(1u, std::thread::hardware_concurrency())));
    context->thread_type = FF_THREAD_FRAME | FF_THREAD_SLICE;
}

template<typename Function>
void parallelPixelRanges(size_t count, Function&& function) {
    constexpr size_t minimumPerWorker = 1u << 20;
    const unsigned int available = motioncam::utils::availableCpuWorkers();
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
      mDecoderInitialized(false),
      mHardwareDecoderActive(false),
      mVideoStreamIndex(-1),
      mLastDecodedFrame(-1) {
    
    spdlog::info("DirectLogDecoder: Initializing for {}", filePath);
    const auto initializationStarted = std::chrono::steady_clock::now();
    std::lock_guard initializationLock(directLogInitializationMutex);
    auto stageStarted = initializationStarted;
    try {
        initFFmpeg();
        if (directLogDiagnosticsEnabled())
            spdlog::info(
                "DirectLog diagnostic: decoder_init stage=demuxer_open latency_ms={:.3f}",
                elapsedMilliseconds(stageStarted));
        stageStarted = std::chrono::steady_clock::now();
        analyzeVideo();
    } catch (...) {
        cleanup();
        throw;
    }
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
    
    mPacket = av_packet_alloc();
    if (!mPacket) throw std::runtime_error("Could not allocate packet");
    mTimeBase = mFormatContext->streams[mVideoStreamIndex]->time_base;
}

void DirectLogDecoder::initDecoder() {
    if (mDecoderInitialized) return;
    AVCodecParameters* codecpar = mFormatContext->streams[mVideoStreamIndex]->codecpar;

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
    const bool forceSoftware = mForceSoftwareDecoder ||
        (forceSoftwareValue && forceSoftwareValue[0] != '\0' &&
         std::string(forceSoftwareValue) != "0");
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
        releaseHardwareDevice(&mHardwareDeviceContext);
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
    if (!mFrame || !mTransferFrame) {
        throw std::runtime_error("Could not allocate frame");
    }
    mDecoderInitialized = true;
    mHardwareDecoderActive = hardwareDecoder;
    if (hardwareDecoder) {
        const unsigned int active = directLogHardwareDecoderCount.fetch_add(1) + 1;
        spdlog::info("DirectLogDecoder: hardware context opened active={} source={}",
                     active, mFilePath);
    }
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
    const AVCodecParameters* codecpar =
        mFormatContext->streams[mVideoStreamIndex]->codecpar;
    mVideoInfo.width = codecpar->width;
    mVideoInfo.height = codecpar->height;
    
    // Determine pixel format
    switch (static_cast<AVPixelFormat>(codecpar->format)) {
        case AV_PIX_FMT_YUV420P:
            mVideoInfo.pixelFormat = "yuv420p";
            break;
        case AV_PIX_FMT_YUVJ420P:
            mVideoInfo.pixelFormat = "yuvj420p";
            break;
        case AV_PIX_FMT_YUV422P:
            mVideoInfo.pixelFormat = "yuv422p";
            break;
        case AV_PIX_FMT_YUVJ422P:
            mVideoInfo.pixelFormat = "yuvj422p";
            break;
        case AV_PIX_FMT_YUV444P:
            mVideoInfo.pixelFormat = "yuv444p";
            break;
        case AV_PIX_FMT_YUVJ444P:
            mVideoInfo.pixelFormat = "yuvj444p";
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
    mVideoInfo.orientation = -1;
    const AVStream* videoStream = mFormatContext->streams[mVideoStreamIndex];
    if (const AVPacketSideData* display = av_packet_side_data_get(
            videoStream->codecpar->coded_side_data,
            videoStream->codecpar->nb_coded_side_data,
            AV_PKT_DATA_DISPLAYMATRIX)) {
        const double counterClockwise = av_display_rotation_get(
            reinterpret_cast<const int32_t*>(display->data));
        if (std::isfinite(counterClockwise)) {
            // av_display_rotation_get() reports counter-clockwise rotation;
            // the rest of the application represents display rotation clockwise.
            const int clockwise = static_cast<int>(std::lround(-counterClockwise));
            mVideoInfo.orientation = ((clockwise % 360) + 360) % 360;
            mVideoInfo.orientation = ((mVideoInfo.orientation + 45) / 90 * 90) % 360;
        }
    }
    
    // Don't rely on container's average framerate - we'll calculate from actual frame timestamps
    // This allows proper CFR conversion handling similar to MCRAW
    mVideoInfo.fps = 0.0; // Will be calculated from actual frame intervals
    
    mVideoInfo.duration = static_cast<double>(mFormatContext->duration) / AV_TIME_BASE;
    
    // DirectLog inputs are frame-based video streams: each video packet is one
    // encoded access unit. Building the timeline by decoding every frame made
    // a cold import take approximately as long as decoding the entire clip.
    // Demuxing preserves exact VFR presentation timestamps and key-frame flags
    // without doing the expensive pixel decode.
    mFrames.clear();
    while (av_read_frame(mFormatContext, mPacket) >= 0) {
        if (mPacket->stream_index == mVideoStreamIndex &&
            !(mPacket->flags & AV_PKT_FLAG_DISCARD)) {
            int64_t pts = mPacket->pts;
            // PTS should be present for MOV/MP4/MKV video. DTS is still a
            // useful fallback for simple streams that omit presentation time.
            if (pts == AV_NOPTS_VALUE) pts = mPacket->dts;
            if (pts != AV_NOPTS_VALUE) {
                DirectLogFrameInfo frameInfo;
                frameInfo.frameNumber = 0;
                frameInfo.pts = pts;
                frameInfo.timestamp = static_cast<Timestamp>(
                    pts * av_q2d(mTimeBase) * 1000000000.0);
                frameInfo.width = mVideoInfo.width;
                frameInfo.height = mVideoInfo.height;
                frameInfo.pixelFormat = mVideoInfo.pixelFormat;
                frameInfo.timeBase = av_q2d(mTimeBase);
                frameInfo.keyFrame = (mPacket->flags & AV_PKT_FLAG_KEY) != 0;
                mFrames.push_back(frameInfo);
            }
        }
        av_packet_unref(mPacket);
    }

    // Packets are stored in decode order when B-frames are present. Sort them
    // into presentation order and discard duplicate timestamps defensively.
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
                                    bool preserveLogEncoded, bool smoothChroma) {
    const int width = outputWidth > 0 ? outputWidth : mVideoInfo.width;
    const int height = outputHeight > 0 ? outputHeight : mVideoInfo.height;
    if (width <= 0 || height <= 0) return false;
    rgbData.resize(static_cast<size_t>(width) * height * 3);
    return extractFrameInto(frameNumber, rgbData.data(), rgbData.size(),
                            width, height, preserveLogEncoded, smoothChroma);
}

bool DirectLogDecoder::extractFrameBayer(int frameNumber,
        std::vector<uint16_t>& bayerData,
        const std::array<uint8_t, 4>& cfaPhase) {
    const int width = mVideoInfo.width;
    const int height = mVideoInfo.height;
    if (width <= 0 || height <= 0) return false;
    bayerData.resize(static_cast<size_t>(width) * height);
    return extractFrameInto(frameNumber, bayerData.data(), bayerData.size(),
                            width, height, false, false, &cfaPhase);
}

bool DirectLogDecoder::extractFrameIntoBytes(int frameNumber,
        std::vector<uint8_t>& bytes, size_t pixelOffset,
        int outputWidth, int outputHeight,
        bool preserveLogEncoded, bool smoothChroma) {
    const int width = outputWidth > 0 ? outputWidth : mVideoInfo.width;
    const int height = outputHeight > 0 ? outputHeight : mVideoInfo.height;
    if (width <= 0 || height <= 0 || (pixelOffset & 1u)) return false;
    const size_t samples = static_cast<size_t>(width) * height * 3;
    if (samples > (std::numeric_limits<size_t>::max() - pixelOffset) / 2) return false;
    bytes.resize(pixelOffset + samples * sizeof(uint16_t));
    return extractFrameInto(frameNumber,
        reinterpret_cast<uint16_t*>(bytes.data() + pixelOffset), samples,
        width, height, preserveLogEncoded, smoothChroma);
}

bool DirectLogDecoder::extractFrameInto(int frameNumber, uint16_t* rgbData,
        size_t sampleCount, int outputWidth, int outputHeight,
        bool preserveLogEncoded, bool smoothChroma,
        const std::array<uint8_t, 4>* cfaPhase) {
    const auto lockStarted = std::chrono::steady_clock::now();
    std::lock_guard<std::mutex> lock(mMutex);
    if (directLogDiagnosticsEnabled())
        spdlog::info("DirectLog diagnostic: frame={} decoder_lock_wait_ms={:.3f}",
                     frameNumber, elapsedMilliseconds(lockStarted));
    return extractFrameIntoLocked(frameNumber, rgbData, sampleCount, outputWidth,
                                  outputHeight, preserveLogEncoded, smoothChroma,
                                  cfaPhase);
}

bool DirectLogDecoder::extractFrameIntoLocked(int frameNumber, uint16_t* rgbData,
        size_t sampleCount, int outputWidth, int outputHeight,
        bool preserveLogEncoded, bool smoothChroma,
        const std::array<uint8_t, 4>* cfaPhase) {
    const bool diagnostics = directLogDiagnosticsEnabled();
    const auto extractStart = std::chrono::steady_clock::now();

    if (frameNumber < 0 || frameNumber >= static_cast<int>(mFrames.size())) {
        return false;
    }
    initDecoder();
    
    const DirectLogFrameInfo& frameInfo = mFrames[frameNumber];
    const char* cacheSetting = std::getenv("MOTIONCAM_DIRECTLOG_FRAME_CACHE");
    const bool cacheEnabled = !cacheSetting || cacheSetting[0] != '0';
    auto convertDecoded = [&](AVFrame* decoded, bool advanceDecoder) {
        const auto conversionStart = std::chrono::steady_clock::now();
        AVFrame* hardwareFrame = decoded;
#ifdef MOTIONCAM_HAS_AVFILTER
        // Keep Vulkan import/render/download inside the decoder lock. A cloned
        // AVFrame still crashed in libplacebo when another request sought and
        // flushed this decoder while its image was being rendered.
        if (AVFrame* scaled = scaledHardwareFrame(decoded, outputWidth, outputHeight))
            hardwareFrame = scaled;
        if (convertVulkanYUVToRGB(hardwareFrame, rgbData, sampleCount,
                outputWidth, outputHeight, preserveLogEncoded, smoothChroma,
                cfaPhase)) {
            if (advanceDecoder) mLastDecodedFrame = frameNumber;
            if (diagnostics)
                spdlog::info("DirectLog diagnostic: frame={} gpu_conversion_ms={:.3f} decoder_total_ms={:.3f}",
                             frameNumber, elapsedMilliseconds(conversionStart),
                             elapsedMilliseconds(extractStart));
            return true;
        }
#endif
        AVFrame* conversionFrame = transferableFrame(hardwareFrame);
        std::vector<uint16_t> fallbackRgb;
        uint16_t* conversionOutput = rgbData;
        size_t conversionSamples = sampleCount;
        if (cfaPhase) {
            fallbackRgb.resize(static_cast<size_t>(outputWidth) * outputHeight * 3);
            conversionOutput = fallbackRgb.data();
            conversionSamples = fallbackRgb.size();
        }
        if (!conversionFrame || !convertYUVToRGB(
                conversionFrame, conversionOutput, conversionSamples,
                outputWidth, outputHeight, preserveLogEncoded, smoothChroma))
            return false;
        if (cfaPhase) {
            for (int y = 0; y < outputHeight; ++y)
                for (int x = 0; x < outputWidth; ++x) {
                    const size_t pixel = static_cast<size_t>(y) * outputWidth + x;
                    rgbData[pixel] = fallbackRgb[pixel * 3 +
                        (*cfaPhase)[(y & 1) * 2 + (x & 1)]];
                }
        }
        if (advanceDecoder) mLastDecodedFrame = frameNumber;
        if (diagnostics)
            spdlog::info("DirectLog diagnostic: frame={} conversion_ms={:.3f} decoder_total_ms={:.3f}",
                         frameNumber, elapsedMilliseconds(conversionStart),
                         elapsedMilliseconds(extractStart));
        return true;
    };
    // The caller can request the same source frame again after changing DNG
    // output settings. The decoded AVFrame remains valid until the next receive
    // or decoder reset, so avoid seeking and decoding its GOP again.
    if (frameNumber == mLastDecodedFrame && mFrame && mFrame->buf[0]) {
        int64_t decodedPts = mFrame->best_effort_timestamp;
        if (decodedPts == AV_NOPTS_VALUE) decodedPts = mFrame->pts;
        if (decodedPts == frameInfo.pts) {
            if (diagnostics)
                spdlog::info("DirectLog diagnostic: frame={} decoder begin mode=current pts={}",
                             frameNumber, frameInfo.pts);
            return convertDecoded(mFrame, false);
        }
    }
    for (auto it = mDecodedFrameCache.begin();
         cacheEnabled && it != mDecodedFrameCache.end(); ++it) {
        if (it->first != frameInfo.pts) continue;
        if (diagnostics)
            spdlog::info("DirectLog diagnostic: frame={} decoder begin mode=cache pts={}",
                         frameNumber, frameInfo.pts);
        const bool converted = convertDecoded(it->second, false);
        av_frame_free(&it->second);
        mDecodedFrameCache.erase(it);
        return converted;
    }
    auto cacheSkippedFrame = [&](int64_t pts) {
        if (!cacheEnabled || pts == AV_NOPTS_VALUE || pts >= frameInfo.pts ||
            pts < mFrames[std::max(0, frameNumber - 8)].pts)
            return;
        for (const auto& cached : mDecodedFrameCache)
            if (cached.first == pts) return;
        AVFrame* copy = av_frame_clone(mFrame);
        if (!copy) return;
        mDecodedFrameCache.emplace_back(pts, copy);
        // Hardware frames retain decoder surfaces and GPU images. Keep only a
        // small look-behind so two mounted clips plus gallery previews do not
        // exhaust VRAM while DaVinci reads ahead.
        const size_t maxCachedFrames = mHardwareDecoderActive ? 2 : 8;
        while (mDecodedFrameCache.size() > maxCachedFrames) {
            av_frame_free(&mDecodedFrameCache.front().second);
            mDecodedFrameCache.pop_front();
        }
    };
    
    // Playback may intentionally skip source frames (for example, displaying
    // 15 fps from a 60 fps sequence). Reuse the current decoder whenever it is
    // already inside the target's GOP; requiring exactly N+1 would restart the
    // GOP for every N+3 request and create progressively longer stalls.
    int precedingKeyFrame = frameNumber;
    while (precedingKeyFrame > 0 && !mFrames[precedingKeyFrame].keyFrame)
        --precedingKeyFrame;
    // Seeking starts decoding again at the preceding keyframe. For long GOPs,
    // continuing from the current decoder position can be far less work even
    // when the caller skips more than a handful of displayed frames.
    const int forwardDistance = frameNumber - mLastDecodedFrame;
    const int seekDistance = frameNumber - precedingKeyFrame;
    const bool sequential = mLastDecodedFrame >= precedingKeyFrame &&
                            forwardDistance > 0 &&
                            forwardDistance <= seekDistance;
    if (diagnostics)
        spdlog::info("DirectLog diagnostic: frame={} decoder begin mode={} pts={}",
                     frameNumber, sequential ? "sequential" : "seek", frameInfo.pts);
    if (!sequential) {
        // libplacebo may still retain imported Vulkan images from the last
        // render. Finish and destroy that renderer before FFmpeg flushes or
        // reuses decoder surfaces after a seek.
#ifdef MOTIONCAM_HAS_AVFILTER
        if (mDirectGpuRgb) {
            mDirectGpuRgb.reset();
            if (diagnostics)
                spdlog::info("DirectLog diagnostic: frame={} gpu_renderer_reset_for_seek=true",
                             frameNumber);
        }
#endif
        av_frame_unref(mFrame);
        for (auto& cached : mDecodedFrameCache)
            av_frame_free(&cached.second);
        mDecodedFrameCache.clear();
#ifdef MOTIONCAM_HAS_AVFILTER
        if (mProxyGpuFrame) av_frame_unref(mProxyGpuFrame);
#endif
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
        int receiveResult = 0;
        while ((receiveResult = avcodec_receive_frame(mCodecContext, mFrame)) == 0) {
            ++framesDecoded;
            int64_t decodedPts = mFrame->best_effort_timestamp;
            if (decodedPts == AV_NOPTS_VALUE) decodedPts = mFrame->pts;
            if (decodedPts == frameInfo.pts) {
                if (diagnostics)
                    spdlog::info("DirectLog diagnostic: frame={} target decoded packets={} decoded_frames={} elapsed_ms={:.3f}; conversion begin",
                                 frameNumber, packetsRead, framesDecoded,
                                 elapsedMilliseconds(extractStart));
                return convertDecoded(mFrame, true) ? 1 : -1;
            }
            // Decoder output is in presentation order. Once it has passed the
            // requested PTS, continuing to EOF cannot find the target and can
            // make a malformed or unusual timeline look like a hung read.
            if (decodedPts != AV_NOPTS_VALUE && decodedPts > frameInfo.pts)
                return -1;
            cacheSkippedFrame(decodedPts);
        }
        if (receiveResult != AVERROR(EAGAIN) && receiveResult != AVERROR_EOF) {
            spdlog::error("DirectLog frame {} receive failed: {}",
                          frameNumber, ffmpegErrorString(receiveResult));
            mLastDecodedFrame = -1;
            return -1;
        }
        return 0;
    };

    if (sequential) {
        const int received = receiveTarget();
        if (received != 0) return received > 0;
    }

    int readResult = 0;
    while ((readResult = av_read_frame(mFormatContext, mPacket)) >= 0) {
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
            if (sendResult < 0 && sendResult != AVERROR(EAGAIN)) {
                spdlog::error("DirectLog frame {} packet submit failed: {}",
                              frameNumber, ffmpegErrorString(sendResult));
                av_packet_unref(mPacket);
                mLastDecodedFrame = -1;
                if (sendResult == AVERROR(ENOMEM) && mHardwareDecoderActive &&
                    !mForceSoftwareDecoder) {
                    spdlog::warn("DirectLogDecoder: GPU decoder ran out of memory; retrying frame {} in software",
                                 frameNumber);
                    mForceSoftwareDecoder = true;
                    cleanup();
                    initFFmpeg();
                    return extractFrameIntoLocked(frameNumber, rgbData, sampleCount,
                        outputWidth, outputHeight, preserveLogEncoded,
                        smoothChroma, cfaPhase);
                }
                return false;
            }
        }
        av_packet_unref(mPacket);
    }

    if (readResult != AVERROR_EOF) {
        spdlog::error("DirectLog frame {} packet read failed: {}",
                      frameNumber, ffmpegErrorString(readResult));
        mLastDecodedFrame = -1;
        return false;
    }
    // Drain delayed frames after the demuxer reaches EOF.
    const int drainResult = avcodec_send_packet(mCodecContext, nullptr);
    if (drainResult < 0 && drainResult != AVERROR_EOF) {
        spdlog::error("DirectLog frame {} drain failed: {}",
                      frameNumber, ffmpegErrorString(drainResult));
        mLastDecodedFrame = -1;
        return false;
    }
    int receiveResult = 0;
    while ((receiveResult = avcodec_receive_frame(mCodecContext, mFrame)) == 0) {
        ++framesDecoded;
        int64_t decodedPts = mFrame->best_effort_timestamp;
        if (decodedPts == AV_NOPTS_VALUE) decodedPts = mFrame->pts;
        if (decodedPts == frameInfo.pts) {
            return convertDecoded(mFrame, true);
        }
        if (decodedPts != AV_NOPTS_VALUE && decodedPts > frameInfo.pts) break;
        cacheSkippedFrame(decodedPts);
    }
    if (receiveResult != AVERROR_EOF && receiveResult != AVERROR(EAGAIN))
        spdlog::error("DirectLog frame {} drain receive failed: {}",
                      frameNumber, ffmpegErrorString(receiveResult));

    mLastDecodedFrame = -1;
    return false;
}

void DirectLogDecoder::overrideTimestamps(const std::vector<Timestamp>& timestamps) {
    if (timestamps.size() != mFrames.size()) return;
    for (size_t i = 1; i < timestamps.size(); ++i)
        if (timestamps[i] <= timestamps[i - 1]) return;
    for (size_t i = 0; i < timestamps.size(); ++i)
        mFrames[i].timestamp = timestamps[i];
}

bool DirectLogDecoder::initHardwareDecoder() {
    std::vector<AVHWDeviceType> preferred;
#ifdef MOTIONCAM_HAS_DIRECTLOG_PLACEBO
    const char* gpuRgb = std::getenv("MOTIONCAM_DIRECTLOG_GPU_RGB");
    const bool preferVulkan = !gpuRgb || gpuRgb[0] != '0';
    if (preferVulkan) preferred.push_back(AV_HWDEVICE_TYPE_VULKAN);
#endif
#ifdef _WIN32
    preferred.insert(preferred.end(), {
        AV_HWDEVICE_TYPE_D3D11VA, AV_HWDEVICE_TYPE_CUDA, AV_HWDEVICE_TYPE_QSV});
#elif defined(__APPLE__)
    preferred.push_back(AV_HWDEVICE_TYPE_VIDEOTOOLBOX);
#else
    preferred.insert(preferred.end(), {
        AV_HWDEVICE_TYPE_CUDA, AV_HWDEVICE_TYPE_VAAPI, AV_HWDEVICE_TYPE_QSV});
#endif
    if (preferred.empty() || preferred.front() != AV_HWDEVICE_TYPE_VULKAN)
        preferred.push_back(AV_HWDEVICE_TYPE_VULKAN);
    for (AVHWDeviceType deviceType : preferred) {
        for (int index = 0;; ++index) {
            const AVCodecHWConfig* config = avcodec_get_hw_config(mCodec, index);
            if (!config) break;
            if (config->device_type != deviceType ||
                !(config->methods & AV_CODEC_HW_CONFIG_METHOD_HW_DEVICE_CTX))
                continue;
            AVBufferRef* device = acquireHardwareDevice(deviceType);
            if (!device) {
                if (directLogDiagnosticsEnabled())
                    spdlog::info(
                        "DirectLog diagnostic: hardware device={} unavailable",
                        av_hwdevice_get_type_name(deviceType));
                continue;
            }
            mHardwareDeviceContext = device;
            mHardwarePixelFormat = config->pix_fmt;
            mCodecContext->hw_device_ctx = av_buffer_ref(mHardwareDeviceContext);
            if (!mCodecContext->hw_device_ctx) {
                releaseHardwareDevice(&mHardwareDeviceContext);
                mHardwarePixelFormat = AV_PIX_FMT_NONE;
                continue;
            }
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

#ifdef MOTIONCAM_HAS_AVFILTER
AVFrame* DirectLogDecoder::scaledHardwareFrame(AVFrame* frame, int width, int height) {
    const char* enabled = std::getenv("MOTIONCAM_DIRECTLOG_GPU_PROXY");
    if ((enabled && enabled[0] == '0') ||
        frame->format != mHardwarePixelFormat || !mHardwareDeviceContext ||
        !frame->hw_frames_ctx ||
        width <= 0 || height <= 0 ||
        (width == frame->width && height == frame->height))
        return nullptr;
    if (mProxyGpuRejected && mProxyGpuRejectedWidth == width &&
        mProxyGpuRejectedHeight == height &&
        mProxyGpuRejectedFormat == frame->format &&
        mProxyGpuRejectedFrames == frame->hw_frames_ctx->data)
        return nullptr;
    mProxyGpuRejected = false;
    auto reject = [&] {
        mProxyGpuRejected = true;
        mProxyGpuRejectedWidth = width;
        mProxyGpuRejectedHeight = height;
        mProxyGpuRejectedFormat = static_cast<AVPixelFormat>(frame->format);
        mProxyGpuRejectedFrames = frame->hw_frames_ctx->data;
    };

    const auto* device = reinterpret_cast<const AVHWDeviceContext*>(
        mHardwareDeviceContext->data);
    const char* scaleName = nullptr;
    const char* quality = nullptr;
    switch (device->type) {
    case AV_HWDEVICE_TYPE_CUDA:
        scaleName = "scale_cuda";
        quality = "interp_algo=bicubic";
        break;
    case AV_HWDEVICE_TYPE_VAAPI:
        scaleName = "scale_vaapi";
        quality = "mode=hq";
        break;
    case AV_HWDEVICE_TYPE_QSV:
        scaleName = "scale_qsv";
        quality = "mode=hq";
        break;
    case AV_HWDEVICE_TYPE_VULKAN:
        scaleName = "scale_vulkan";
        quality = "scaler=bilinear";
        break;
    case AV_HWDEVICE_TYPE_D3D11VA:
        scaleName = "scale_d3d11";
        quality = "";
        break;
    case AV_HWDEVICE_TYPE_VIDEOTOOLBOX:
        scaleName = "scale_vt";
        quality = "";
        break;
    default:
        return nullptr;
    }

    if (mProxyGpuGraph &&
        (mProxyGpuWidth != width || mProxyGpuHeight != height ||
         mProxyGpuInputFrames->data != frame->hw_frames_ctx->data)) {
        av_frame_unref(mProxyGpuFrame);
        avfilter_graph_free(&mProxyGpuGraph);
        mProxyGpuSource = mProxyGpuSink = nullptr;
        av_buffer_unref(&mProxyGpuInputFrames);
    }
    if (!mProxyGpuGraph) {
        auto fail = [&]() -> AVFrame* {
            spdlog::warn("DirectLog GPU proxy unavailable; using CPU scaling");
            av_frame_free(&mProxyGpuFrame);
            avfilter_graph_free(&mProxyGpuGraph);
            mProxyGpuSource = mProxyGpuSink = nullptr;
            av_buffer_unref(&mProxyGpuInputFrames);
            reject();
            return nullptr;
        };
        const AVFilter* sourceFilter = avfilter_get_by_name("buffer");
        const AVFilter* scaleFilter = avfilter_get_by_name(scaleName);
        const AVFilter* sinkFilter = avfilter_get_by_name("buffersink");
        if (!sourceFilter || !scaleFilter || !sinkFilter) return fail();
        mProxyGpuGraph = avfilter_graph_alloc();
        if (!mProxyGpuGraph) return fail();
        mProxyGpuSource = avfilter_graph_alloc_filter(
            mProxyGpuGraph, sourceFilter, "directlog_in");
        if (!mProxyGpuSource) return fail();
        AVBufferSrcParameters* parameters = av_buffersrc_parameters_alloc();
        if (!parameters) return fail();
        parameters->format = frame->format;
        parameters->width = frame->width;
        parameters->height = frame->height;
        parameters->time_base = {1, 1000000};
        parameters->sample_aspect_ratio = {1, 1};
        parameters->hw_frames_ctx = av_buffer_ref(frame->hw_frames_ctx);
        const int setResult = parameters->hw_frames_ctx
            ? av_buffersrc_parameters_set(mProxyGpuSource, parameters) : AVERROR(ENOMEM);
        av_buffer_unref(&parameters->hw_frames_ctx);
        av_free(parameters);
        if (setResult < 0 || avfilter_init_str(mProxyGpuSource, nullptr) < 0)
            return fail();
        AVFilterContext* scaleContext = nullptr;
        const std::string scaleArgs = quality[0]
            ? fmt::format("w={}:h={}:{}", width, height, quality)
            : fmt::format("w={}:h={}", width, height);
        if (avfilter_graph_create_filter(&scaleContext, scaleFilter,
                "directlog_scale", scaleArgs.c_str(), nullptr, mProxyGpuGraph) < 0 ||
            avfilter_graph_create_filter(&mProxyGpuSink, sinkFilter,
                "directlog_out", nullptr, nullptr, mProxyGpuGraph) < 0 ||
            avfilter_link(mProxyGpuSource, 0, scaleContext, 0) < 0 ||
            avfilter_link(scaleContext, 0, mProxyGpuSink, 0) < 0 ||
            avfilter_graph_config(mProxyGpuGraph, nullptr) < 0)
            return fail();
        mProxyGpuInputFrames = av_buffer_ref(frame->hw_frames_ctx);
        if (!mProxyGpuInputFrames) return fail();
        if (!mProxyGpuFrame) mProxyGpuFrame = av_frame_alloc();
        if (!mProxyGpuFrame) return fail();
        mProxyGpuWidth = width;
        mProxyGpuHeight = height;
        spdlog::info("DirectLog GPU proxy initialized backend={} {}x{} -> {}x{}",
            av_hwdevice_get_type_name(device->type),
            frame->width, frame->height, width, height);
    }
    av_frame_unref(mProxyGpuFrame);
    if (av_buffersrc_add_frame_flags(mProxyGpuSource, frame,
            AV_BUFFERSRC_FLAG_KEEP_REF) < 0 ||
        av_buffersink_get_frame(mProxyGpuSink, mProxyGpuFrame) < 0) {
        spdlog::warn("DirectLog GPU proxy failed; using CPU scaling");
        reject();
        return nullptr;
    }
    return mProxyGpuFrame;
}

bool DirectLogDecoder::convertVulkanYUVToRGB(AVFrame* frame, uint16_t* rgbData,
        size_t sampleCount, int outputWidth, int outputHeight,
        bool preserveLogEncoded, bool smoothChroma,
        const std::array<uint8_t, 4>* cfaPhase) {
#ifndef MOTIONCAM_HAS_DIRECTLOG_PLACEBO
    (void)frame; (void)rgbData; (void)sampleCount; (void)outputWidth;
    (void)outputHeight; (void)preserveLogEncoded; (void)smoothChroma;
    (void)cfaPhase;
    return false;
#else
    const char* enabled = std::getenv("MOTIONCAM_DIRECTLOG_GPU_RGB");
    if ((enabled && enabled[0] == '0') || mGpuRgbRejected ||
        frame->format != AV_PIX_FMT_VULKAN || !frame->hw_frames_ctx || !rgbData)
        return false;
    const auto* frames = reinterpret_cast<const AVHWFramesContext*>(
        frame->hw_frames_ctx->data);
    if (!frames || (frames->sw_format != AV_PIX_FMT_P010LE &&
                    frames->sw_format != AV_PIX_FMT_NV12)) return false;
    AVColorRange range = frame->color_range;
    if (range == AVCOL_RANGE_UNSPECIFIED) range = mCodecContext->color_range;
    const bool fullRange = mFullRangeOverride.value_or(range == AVCOL_RANGE_JPEG);
    const int width = outputWidth > 0 ? outputWidth : frame->width;
    const int height = outputHeight > 0 ? outputHeight : frame->height;
    if (width <= 0 || height <= 0 ||
        sampleCount != static_cast<size_t>(width) * height * (cfaPhase ? 1 : 3))
        return false;

    if (!mDirectGpuRgb) mDirectGpuRgb = std::make_unique<DirectLogGpuRgb>();
    const auto curve = mVideoInfo.isHLG ? DirectLogGpuRgb::TransferCurve::HLG
        : mVideoInfo.isLOG60 && !preserveLogEncoded
            ? DirectLogGpuRgb::TransferCurve::LOG60
            : DirectLogGpuRgb::TransferCurve::None;
    if (!mDirectGpuRgb->render(frame, width, height, smoothChroma, curve,
                               fullRange, rgbData, cfaPhase)) {
        spdlog::warn("DirectLog direct GPU RGB conversion failed; using CPU conversion");
        mDirectGpuRgb.reset();
        mGpuRgbRejected = true;
        return false;
    }
    if (!mFullRange.has_value()) mFullRange = fullRange;
    return true;
#endif
}

#endif

AVFrame* DirectLogDecoder::transferableFrame(AVFrame* frame) {
    if (frame->format != mHardwarePixelFormat || !mHardwareDeviceContext) return frame;
    if (mTransferFrame->buf[0] &&
        (mTransferFrame->width != frame->width ||
         mTransferFrame->height != frame->height))
        av_frame_unref(mTransferFrame);
    if (av_hwframe_transfer_data(mTransferFrame, frame, 0) < 0) {
        // A device format change can invalidate a previously allocated
        // download buffer. Retry with a fresh destination in that case.
        av_frame_unref(mTransferFrame);
        if (av_hwframe_transfer_data(mTransferFrame, frame, 0) < 0) {
            spdlog::error("DirectLogDecoder: hardware frame transfer failed");
            return nullptr;
        }
    }
    // The converter only consumes color range from frame properties. Avoid
    // accumulating copied side data while retaining the download allocation.
    mTransferFrame->color_range = frame->color_range;
    return mTransferFrame;
}

void DirectLogDecoder::setFullRangeOverride(std::optional<bool> fullRange) {
    std::lock_guard<std::mutex> lock(mMutex);
    mFullRangeOverride = fullRange;
    mFullRange.reset();
}

bool DirectLogDecoder::convertYUVToRGB(AVFrame* yuvFrame, uint16_t* rgbData,
                                       size_t sampleCount,
                                       int outputWidth, int outputHeight,
                                       bool preserveLogEncoded, bool smoothChroma) {
    const bool diagnostics = directLogDiagnosticsEnabled();
    const auto conversionStart = std::chrono::steady_clock::now();
    const int width = yuvFrame->width;
    const int height = yuvFrame->height;
    if (outputWidth <= 0) outputWidth = width;
    if (outputHeight <= 0) outputHeight = height;
    if (!rgbData || sampleCount != static_cast<size_t>(outputWidth) * outputHeight * 3)
        return false;

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

    // Area filtering keeps the fast proxy path while averaging the source
    // region. The override supports direct comparisons with point/bicubic.
    const char* proxyScaler = std::getenv("MOTIONCAM_DIRECTLOG_PROXY_SCALER");
    const int proxyFlags = proxyScaler && std::string(proxyScaler) == "point"
        ? SWS_POINT : proxyScaler && std::string(proxyScaler) == "bicubic"
            ? SWS_BICUBIC : SWS_AREA;
    mSwsContext = sws_getCachedContext(
        mSwsContext, width, height, static_cast<AVPixelFormat>(yuvFrame->format),
        outputWidth, outputHeight, AV_PIX_FMT_RGB48LE,
        smoothChroma && outputWidth == width && outputHeight == height
            ? SWS_BICUBIC : proxyFlags,
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
    uint8_t* output[] = {reinterpret_cast<uint8_t*>(rgbData)};
    int outputStride[] = {outputWidth * 3 * static_cast<int>(sizeof(uint16_t))};
    const AVPixFmtDescriptor* scaleDescriptor = av_pix_fmt_desc_get(pixelFormat);
    if (mVerifiedBandFormat != pixelFormat || mVerifiedBandSmooth != smoothChroma ||
        mVerifiedBandFullRange != fullRange) {
        mVerifiedBandFormat = pixelFormat;
        mVerifiedBandSmooth = smoothChroma;
        mVerifiedBandFullRange = fullRange;
        mVerifiedBandCounts = 0;
        mBandConversionRejected = false;
    }
    const bool bandCandidate = outputWidth == width &&
        outputHeight == height && scaleDescriptor &&
        (scaleDescriptor->flags & AV_PIX_FMT_FLAG_PLANAR) &&
        !(scaleDescriptor->flags & AV_PIX_FMT_FLAG_RGB) && input[1] &&
        scaleDescriptor->nb_components == 3 &&
        scaleDescriptor->log2_chroma_h <= 1 && height >= 1024;
    const unsigned bandCount = bandCandidate
        ? std::min(8u, motioncam::utils::availableCpuWorkers()) : 1u;
    bool bandSuccess = false;
    const int proxyScale = outputWidth > 0 ? width / outputWidth : 0;
    // Split only integer-scale proxies: aligned source bands then have the
    // same sampling phase as one whole-frame sws_scale call. Four proxy rows
    // of overlap cover the bicubic filter at each internal boundary.
    const bool proxyBandCandidate = outputWidth < width && proxyScale >= 2 &&
        width == outputWidth * proxyScale && height == outputHeight * proxyScale &&
        scaleDescriptor && scaleDescriptor->log2_chroma_h <= 1 &&
        (proxyScale % (1 << scaleDescriptor->log2_chroma_h)) == 0 &&
        (scaleDescriptor->flags & AV_PIX_FMT_FLAG_PLANAR) &&
        scaleDescriptor->nb_components == 3 && input[1] &&
        outputHeight >= 64 && proxyFlags != SWS_POINT;
    if (proxyBandCandidate) {
        const unsigned workers = std::min(8u, motioncam::utils::availableCpuWorkers());
        const std::array<int, 8> key{width, height, outputWidth, outputHeight,
            static_cast<int>(pixelFormat), proxyFlags, fullRange ? 1 : 0,
            static_cast<int>(workers)};
        if (mProxyBandKey != key) {
            mProxyBandKey = key;
            mProxyBandVerified = false;
            mProxyBandRejected = false;
        }
        if (!mProxyBandRejected) {
            for (size_t index = workers; index < mProxySwsContexts.size(); ++index)
                if (mProxySwsContexts[index])
                    sws_freeContext(mProxySwsContexts[index]);
            mProxySwsContexts.resize(workers, nullptr);
            mProxyRgbScratch.resize(workers);
            std::atomic_bool failed{false};
            auto scaleBand = [&](unsigned worker) {
                const int begin = outputHeight * worker / workers;
                const int end = outputHeight * (worker + 1) / workers;
                const int sourceBegin = std::max(0, begin - 4) * proxyScale;
                const int sourceEnd = std::min(outputHeight, end + 4) * proxyScale;
                const int localSourceHeight = sourceEnd - sourceBegin;
                const int localOutputHeight = localSourceHeight / proxyScale;
                auto& context = mProxySwsContexts[worker];
                context = sws_getCachedContext(context, width, localSourceHeight,
                    pixelFormat, outputWidth, localOutputHeight, AV_PIX_FMT_RGB48LE,
                    proxyFlags, nullptr, nullptr, nullptr);
                if (!context || sws_setColorspaceDetails(context, coefficients,
                    fullRange ? 1 : 0, coefficients, 1, 0, 1 << 16, 1 << 16) < 0) {
                    failed.store(true);
                    return;
                }
                const int chromaBegin = sourceBegin >> scaleDescriptor->log2_chroma_h;
                const uint8_t* bandInput[4] = {
                    input[0] + static_cast<ptrdiff_t>(sourceBegin) * inputStride[0],
                    input[1] + static_cast<ptrdiff_t>(chromaBegin) * inputStride[1],
                    input[2] ? input[2] + static_cast<ptrdiff_t>(chromaBegin) * inputStride[2] : nullptr,
                    input[3]};
                auto& scratch = mProxyRgbScratch[worker];
                scratch.resize(static_cast<size_t>(outputWidth) * localOutputHeight * 3);
                uint8_t* bandOutput[4] = {reinterpret_cast<uint8_t*>(scratch.data())};
                if (sws_scale(context, bandInput, inputStride, 0, localSourceHeight,
                    bandOutput, outputStride) != localOutputHeight) {
                    failed.store(true);
                    return;
                }
                const size_t rowSamples = static_cast<size_t>(outputWidth) * 3;
                const int localBegin = begin - sourceBegin / proxyScale;
                std::copy_n(scratch.data() + static_cast<size_t>(localBegin) * rowSamples,
                    static_cast<size_t>(end - begin) * rowSamples,
                    rgbData + static_cast<size_t>(begin) * rowSamples);
            };
            std::vector<std::thread> threads;
            threads.reserve(workers - 1);
            for (unsigned worker = 1; worker < workers; ++worker)
                threads.emplace_back(scaleBand, worker);
            scaleBand(0);
            for (auto& thread : threads) thread.join();
            bandSuccess = !failed.load();
            // Check the first frame for each layout/format/filter. If FFmpeg
            // changes its edge behavior, use the whole-frame path for that
            // configuration instead of risking seams in output images.
            if (bandSuccess && !mProxyBandVerified) {
                std::vector<uint16_t> reference(sampleCount);
                uint8_t* referenceOutput[4] = {reinterpret_cast<uint8_t*>(reference.data())};
                bandSuccess = sws_scale(mSwsContext, input, inputStride, 0, height,
                    referenceOutput, outputStride) == outputHeight &&
                    std::equal(reference.begin(), reference.end(), rgbData);
                mProxyBandVerified = bandSuccess;
                mProxyBandRejected = !bandSuccess;
                spdlog::info("DirectLog proxy band conversion exact_match={}", bandSuccess);
            }
        }
    }
    if (!bandSuccess && bandCount > 1 && !mBandConversionRejected) {
        const int alignment = 1 << scaleDescriptor->log2_chroma_h;
        const int rowsPerBand = ((height + static_cast<int>(bandCount) - 1) /
            static_cast<int>(bandCount) + alignment - 1) / alignment * alignment;
        if (mBandSwsContexts.size() < bandCount)
            mBandSwsContexts.resize(bandCount, nullptr);
        if (mBandRgbScratch.size() < bandCount)
            mBandRgbScratch.resize(bandCount);
        std::atomic_bool failed{false};
        auto scaleBand = [&](unsigned band) {
            const int begin = static_cast<int>(band) * rowsPerBand;
            if (begin >= height) return;
            const int end = std::min(height, begin + rowsPerBand);
            constexpr int haloRows = 16;
            const int sourceBegin = std::max(0, begin - haloRows);
            const int sourceEnd = std::min(height, end + haloRows);
            const int localHeight = sourceEnd - sourceBegin;
            auto& context = mBandSwsContexts[band];
            context = sws_getCachedContext(context, width, localHeight, pixelFormat,
                width, localHeight, AV_PIX_FMT_RGB48LE,
                smoothChroma ? SWS_BICUBIC : SWS_POINT,
                nullptr, nullptr, nullptr);
            if (!context || sws_setColorspaceDetails(context, coefficients,
                fullRange ? 1 : 0, coefficients, 1, 0, 1 << 16, 1 << 16) < 0) {
                failed.store(true);
                return;
            }
            const uint8_t* bandInput[4] = {
                input[0] + static_cast<ptrdiff_t>(sourceBegin) * inputStride[0],
                input[1] + static_cast<ptrdiff_t>(sourceBegin / alignment) * inputStride[1],
                input[2] ? input[2] + static_cast<ptrdiff_t>(sourceBegin / alignment) * inputStride[2] : nullptr,
                input[3]};
            auto& converted = mBandRgbScratch[band];
            converted.resize(static_cast<size_t>(width) * localHeight * 3);
            uint8_t* bandOutput[] = {reinterpret_cast<uint8_t*>(converted.data())};
            if (sws_scale(context, bandInput, inputStride, 0, localHeight,
                          bandOutput, outputStride) != localHeight) {
                failed.store(true);
                return;
            }
            const size_t offset = static_cast<size_t>(begin - sourceBegin) * width * 3;
            std::copy_n(converted.data() + offset, static_cast<size_t>(end - begin) * width * 3,
                rgbData + static_cast<size_t>(begin) * width * 3);
        };
        std::vector<std::thread> threads;
        threads.reserve(bandCount - 1);
        for (unsigned band = 1; band < bandCount; ++band)
            threads.emplace_back(scaleBand, band);
        scaleBand(0);
        for (auto& thread : threads) thread.join();
        bandSuccess = !failed.load();
        if (bandSuccess && !(mVerifiedBandCounts & (1u << bandCount))) {
            std::vector<uint16_t> reference(sampleCount);
            uint8_t* referenceOutput[] = {reinterpret_cast<uint8_t*>(reference.data())};
            bandSuccess = sws_scale(mSwsContext, input, inputStride, 0, height,
                                    referenceOutput, outputStride) == outputHeight;
            const bool matches = bandSuccess &&
                std::equal(reference.begin(), reference.end(), rgbData);
            if (matches) mVerifiedBandCounts |= static_cast<uint16_t>(1u << bandCount);
            else mBandConversionRejected = true;
            if (diagnostics)
                spdlog::info("DirectLog diagnostic: banded_yuv_conversion exact_match={}",
                    matches);
        }
    }
    if (!bandSuccess || mBandConversionRejected) {
        if (sws_scale(mSwsContext, input, inputStride, 0, height,
                      output, outputStride) != outputHeight)
            return false;
    }

    const AVPixFmtDescriptor* inputDescriptor = av_pix_fmt_desc_get(pixelFormat);
    const bool eightBitInput = inputDescriptor && inputDescriptor->nb_components > 0 &&
                               inputDescriptor->comp[0].depth <= 8;
    // This producer's 8-bit limited path clips at RGB code 254. Preserve the
    // original black and shadow transfer response, but promote that clipping
    // code (and only values at or above it) to linear white.
    constexpr uint32_t limitedRgbWhite = 254u * 257u;
    const uint32_t encodedWhite = !fullRange && eightBitInput
        ? limitedRgbWhite : 65535u;
    if (diagnostics)
        spdlog::info("DirectLog diagnostic: swscale_ms={:.3f}; transfer stage begin",
                     elapsedMilliseconds(scaleStart));
    
    // Apply HLG to linear conversion if needed
    if (mVideoInfo.isHLG) {
        applyHLGToLinear(rgbData, sampleCount, encodedWhite);
    } else if (mVideoInfo.isLOG60 && !preserveLogEncoded) {
        applyLOG60ToLinear(rgbData, sampleCount, encodedWhite);
    }
    if (diagnostics)
        spdlog::info("DirectLog diagnostic: transfer_and_conversion_total_ms={:.3f}",
                     elapsedMilliseconds(conversionStart));
    
    return true;
}

void DirectLogDecoder::applyHLGToLinear(
        uint16_t* rgbData, size_t sampleCount, uint32_t encodedWhite) {
    const auto makeLut = [](uint32_t white) {
        std::array<uint16_t, 65536> values{};
        for (size_t i = 0; i < values.size(); ++i) {
            const float encoded = i >= white ? 1.0f : i / 65535.0f;
            const float linear = encoded <= 0.5f
                ? encoded * encoded / 3.0f
                : (std::exp((encoded - 0.55991073f) / 0.17883277f) + 0.28466892f) / 12.0f;
            values[i] = static_cast<uint16_t>(
                std::clamp(std::lround(linear * 65535.0f), 0l, 65535l));
        }
        return values;
    };
    static const auto fullLut = makeLut(65535u);
    static const auto limited8BitLut = makeLut(254u * 257u);
    const auto& lut = encodedWhite == 65535u ? fullLut : limited8BitLut;
    parallelPixelRanges(sampleCount, [&](size_t begin, size_t end) {
        for (size_t i = begin; i < end; ++i) rgbData[i] = lut[rgbData[i]];
    });
}

bool DirectLogDecoder::isHLGVideo(const std::string& filePath) {
    return boost::icontains(filePath, "HLG_NATIVE");
}

void DirectLogDecoder::applyLOG60ToLinear(
        uint16_t* rgbData, size_t sampleCount, uint32_t encodedWhite) {
    const auto makeLut = [](uint32_t white) {
        std::array<uint16_t, 65536> values{};
        for (size_t i = 0; i < values.size(); ++i) {
            const float encoded = i >= white ? 1.0f : i / 65535.0f;
            const float linear = (std::pow(61.0f, encoded) - 1.0f) / 60.0f;
            values[i] = static_cast<uint16_t>(std::clamp(
                std::lround(linear * 65535.0f), 0l, 65535l));
        }
        return values;
    };
    static const auto fullLut = makeLut(65535u);
    static const auto limited8BitLut = makeLut(254u * 257u);
    const auto& lut = encodedWhite == 65535u ? fullLut : limited8BitLut;
    parallelPixelRanges(sampleCount, [&](size_t begin, size_t end) {
        for (size_t i = begin; i < end; ++i) rgbData[i] = lut[rgbData[i]];
    });
}

bool DirectLogDecoder::isLOG60Video(const std::string& filePath) {
    return boost::icontains(filePath, "LOG60_NATIVE");
}

std::pair<uintmax_t, size_t> DirectLogDecoder::timelineCacheUsage() {
    std::lock_guard initializationLock(directLogInitializationMutex);
    uintmax_t bytes = 0;
    size_t files = 0;
    std::error_code error;
    const auto directory = directLogTimelineCachePath("").parent_path();
    std::filesystem::directory_iterator entries(directory, error);
    for (std::filesystem::directory_iterator end; !error && entries != end;
         entries.increment(error)) {
        if (!entries->is_regular_file(error)) continue;
        const auto size = entries->file_size(error);
        if (error) break;
        bytes += size;
        ++files;
    }
    return {bytes, files};
}

void DirectLogDecoder::clearTimelineCache() {
    std::lock_guard initializationLock(directLogInitializationMutex);
    {
        std::lock_guard<std::mutex> cacheLock(directLogTimelineCacheMutex);
        directLogTimelineCache.clear();
    }
    std::error_code error;
    std::filesystem::remove_all(directLogTimelineCachePath("").parent_path(), error);
    if (error)
        spdlog::warn("Could not clear DirectLog timeline cache: {}", error.message());
}

void DirectLogDecoder::cleanup() {
    const size_t cachedFrames = mDecodedFrameCache.size();
    for (auto& cached : mDecodedFrameCache)
        av_frame_free(&cached.second);
    mDecodedFrameCache.clear();
#ifdef MOTIONCAM_HAS_AVFILTER
    mDirectGpuRgb.reset();
    av_frame_free(&mProxyGpuFrame);
    avfilter_graph_free(&mProxyGpuGraph);
    mProxyGpuSource = mProxyGpuSink = nullptr;
    av_buffer_unref(&mProxyGpuInputFrames);
#endif
    for (auto*& context : mBandSwsContexts) {
        if (context) sws_freeContext(context);
        context = nullptr;
    }
    mBandSwsContexts.clear();
    mBandRgbScratch.clear();
    for (auto*& context : mProxySwsContexts) {
        if (context) sws_freeContext(context);
        context = nullptr;
    }
    mProxySwsContexts.clear();
    mProxyRgbScratch.clear();
    mVerifiedBandCounts = 0;
    mBandConversionRejected = false;
    mVerifiedBandFormat = AV_PIX_FMT_NONE;
    mVerifiedBandSmooth = false;
    mVerifiedBandFullRange = false;
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
        releaseHardwareDevice(&mHardwareDeviceContext);
    }
    if (mHardwareDecoderActive) {
        const unsigned int active = directLogHardwareDecoderCount.fetch_sub(1) - 1;
        spdlog::info("DirectLogDecoder: hardware context closed active={} cached_frames_released={} source={}",
                     active, cachedFrames, mFilePath);
        mHardwareDecoderActive = false;
    }
    
    if (mFormatContext) {
        avformat_close_input(&mFormatContext);
    }
    mDecoderInitialized = false;
    mHardwarePixelFormat = AV_PIX_FMT_NONE;
    mLastDecodedFrame = -1;
}

} // namespace motioncam
