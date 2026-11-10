#include "NativeVideoEncoder.h"

#include <algorithm>
#include <cstring>
#include <cmath>
#include <limits>
#include <stdexcept>
#ifdef __ANDROID__
#include <QCoreApplication>
#include <QJniObject>
#include "android/P010VideoEncoder.h"
#endif

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavcodec/bsf.h>
#include <libavformat/avformat.h>
#include <libavutil/display.h>
#include <libavutil/error.h>
#include <libavutil/opt.h>
#include <libavutil/pixdesc.h>
#ifdef MOTIONCAM_HAS_AVFILTER
#include <libavfilter/avfilter.h>
#include <libavfilter/buffersrc.h>
#include <libavfilter/buffersink.h>
#endif
}

namespace motioncam {
namespace {
void check(int result, const char* operation) {
    if (result >= 0) return;
    char message[AV_ERROR_MAX_STRING_SIZE]{};
    av_strerror(result, message, sizeof(message));
    throw std::runtime_error(std::string(operation) + ": " + message);
}
struct Dictionary {
    AVDictionary* value = nullptr;
    ~Dictionary() { av_dict_free(&value); }
};
}

#ifdef __ANDROID__
NativeVideoEncoder::AndroidHardwareEncoder NativeVideoEncoder::selectAndroidHardwareEncoder(
        const std::string& mime, int width, int height, double fps, int bitrate, int bitDepth) {
    const auto activity = QNativeInterface::QAndroidApplication::context();
    if (!activity.isValid()) throw std::runtime_error("Android activity is unavailable");
    const auto result = activity.callMethod<QString>("selectNativeHardwareEncoder",
        QString::fromStdString(mime), jint(width), jint(height), jdouble(fps), jint(bitrate), jint(bitDepth));
    const auto parts = result.split('|');
    if (parts.size() != 4 || parts[0].isEmpty())
        throw std::runtime_error("No hardware encoder supports this codec, requested bit depth/input layout, size, frame rate, and bitrate on this device");
    bool validProfile = false;
    const int profile = parts[3].toInt(&validProfile);
    if (!validProfile || (bitDepth == 10 && profile <= 0))
        throw std::runtime_error("Android returned an invalid hardware encoder profile");
    return {parts[0].toStdString(), parts[1].toStdString(), parts[2].toStdString(), profile};
}
#endif

bool NativeVideoEncoder::supports(const std::string& name, const std::string& format) {
    const auto* codec = avcodec_find_encoder_by_name(name.c_str());
    const auto pixel = av_get_pix_fmt(format.c_str());
    if (!codec || pixel == AV_PIX_FMT_NONE) return false;
    const AVPixelFormat* formats = nullptr;
#if LIBAVCODEC_VERSION_MAJOR >= 61
    if (avcodec_get_supported_config(nullptr, codec, AV_CODEC_CONFIG_PIX_FORMAT, 0,
                                    reinterpret_cast<const void**>(&formats), nullptr) < 0) return false;
#else
    formats = codec->pix_fmts;
#endif
    if (!formats) return true;
    for (; *formats != AV_PIX_FMT_NONE; ++formats)
        if (*formats == pixel) return true;
    return false;
}

struct NativeVideoEncoder::Impl {
    Options options;
#ifdef __ANDROID__
    std::unique_ptr<P010VideoEncoder> p010Encoder;
#endif
    AVFormatContext* output = nullptr;
    AVFormatContext* audio = nullptr;
    AVCodecContext* codec = nullptr;
    AVBSFContext* extractHeaders = nullptr;
    bool headerWritten = false;
    AVStream* videoStream = nullptr;
    AVStream* audioStream = nullptr;
    int audioIndex = -1;
    AVPacket* packet = av_packet_alloc();
    AVPacket* audioPacket = av_packet_alloc();
    AVFrame* frame = av_frame_alloc();
    bool audioPending = false, audioDone = false, finished = false;
    int64_t lastEnd = 0;
    std::map<int64_t, int64_t> frameDurations;
#ifdef MOTIONCAM_HAS_AVFILTER
    AVFilterGraph* graph = nullptr;
    AVFilterContext* source = nullptr;
    AVFilterContext* sink = nullptr;
#endif
    ~Impl() {
#ifdef __ANDROID__
        p010Encoder.reset();
#endif
#ifdef MOTIONCAM_HAS_AVFILTER
        avfilter_graph_free(&graph);
#endif
        av_frame_free(&frame);
        av_packet_free(&packet);
        av_packet_free(&audioPacket);
        av_bsf_free(&extractHeaders);
        avcodec_free_context(&codec);
        avformat_close_input(&audio);
        if (output) {
            if (output->pb) avio_closep(&output->pb);
            avformat_free_context(output);
        }
    }
    void drainAudio(int64_t until, AVRational timeBase, bool all = false) {
        while (audio && !audioDone) {
            if (!audioPending) {
                int status = av_read_frame(audio, audioPacket);
                if (status == AVERROR_EOF) { audioDone = true; break; }
                check(status, "Read native audio");
                if (audioPacket->stream_index != audioIndex) {
                    av_packet_unref(audioPacket);
                    continue;
                }
                audioPending = true;
            }
            const auto inputBase = audio->streams[audioIndex]->time_base;
            const int64_t dts = audioPacket->dts != AV_NOPTS_VALUE ? audioPacket->dts : audioPacket->pts;
            if (!all && dts != AV_NOPTS_VALUE && av_compare_ts(dts, inputBase, until, timeBase) > 0)
                break;
            av_packet_rescale_ts(audioPacket, inputBase, audioStream->time_base);
            audioPacket->stream_index = audioStream->index;
            audioPacket->pos = -1;
            check(av_interleaved_write_frame(output, audioPacket), "Mux native audio");
            av_packet_unref(audioPacket);
            audioPending = false;
        }
    }
    void writeHeader() {
        if (headerWritten) return;
        Dictionary muxOptions;
        av_dict_set(&muxOptions.value, "movflags", "+write_colr", 0);
        av_dict_set(&muxOptions.value, "video_track_timescale", "1000000", 0);
        check(avformat_write_header(output, &muxOptions.value), "Write native container header");
        headerWritten = true;
    }
    void writePacket(AVPacket* encoded) {
        if (!headerWritten && extractHeaders) {
            size_t size = 0;
            const auto* extra = av_packet_get_side_data(encoded, AV_PKT_DATA_NEW_EXTRADATA, &size);
            if (!extra || !size)
                throw std::runtime_error("Hardware encoder did not supply the video headers required for MP4");
            auto* parameters = videoStream->codecpar;
            av_freep(&parameters->extradata);
            parameters->extradata = static_cast<uint8_t*>(av_mallocz(size + AV_INPUT_BUFFER_PADDING_SIZE));
            if (!parameters->extradata) throw std::bad_alloc();
            std::memcpy(parameters->extradata, extra, size);
            parameters->extradata_size = static_cast<int>(size);
        }
        const auto duration = frameDurations.find(encoded->pts);
        if (duration != frameDurations.end()) {
            encoded->duration = duration->second;
            frameDurations.erase(duration);
        }
        writeHeader();
        drainAudio(encoded->dts, codec->time_base);
        av_packet_rescale_ts(encoded, codec->time_base, videoStream->time_base);
        encoded->stream_index = videoStream->index;
        check(av_interleaved_write_frame(output, encoded), "Mux native video");
    }
    void drainHeaders() {
        AVPacket* filtered = av_packet_alloc();
        if (!filtered) throw std::bad_alloc();
        std::unique_ptr<AVPacket, void(*)(AVPacket*)> owned(filtered, [](AVPacket* p) { av_packet_free(&p); });
        int status;
        while ((status = av_bsf_receive_packet(extractHeaders, filtered)) >= 0) {
            writePacket(filtered);
            av_packet_unref(filtered);
        }
        if (status != AVERROR(EAGAIN) && status != AVERROR_EOF) check(status, "Extract native video headers");
    }
    void consumePacket(AVPacket* encoded) {
        if (extractHeaders) {
            check(av_bsf_send_packet(extractHeaders, encoded), "Read native video headers");
            drainHeaders();
        } else writePacket(encoded);
    }
    void drainPackets() {
        int status;
        while ((status = avcodec_receive_packet(codec, packet)) >= 0) {
            consumePacket(packet);
            av_packet_unref(packet);
        }
        if (status != AVERROR(EAGAIN) && status != AVERROR_EOF) check(status, "Receive native video");
    }
#ifdef MOTIONCAM_HAS_AVFILTER
    void drainFrames() {
        int status;
        while ((status = av_buffersink_get_frame(sink, frame)) >= 0) {
            const auto base = av_buffersink_get_time_base(sink);
            frame->pts = av_rescale_q(frame->pts, base, codec->time_base);
            frame->duration = av_rescale_q(frame->duration, base, codec->time_base);
            frame->pict_type = AV_PICTURE_TYPE_NONE;
            if (frame->format != codec->pix_fmt || frame->color_range != AVCOL_RANGE_JPEG)
                throw std::runtime_error("Native conversion did not preserve the requested full-range pixel layout");
            frameDurations[frame->pts] = std::max<int64_t>(1, frame->duration);
#ifdef __ANDROID__
            if (p010Encoder)
                p010Encoder->writeFrame(frame, [&](AVPacket* encoded) { consumePacket(encoded); });
            else
#endif
            {
                check(avcodec_send_frame(codec, frame), "Encode native video");
                drainPackets();
            }
            av_frame_unref(frame);
        }
        if (status != AVERROR(EAGAIN) && status != AVERROR_EOF) check(status, "Convert native RGB");
    }
#endif
};

NativeVideoEncoder::NativeVideoEncoder(const Options& options) : m(std::make_unique<Impl>()) {
#ifndef MOTIONCAM_HAS_AVFILTER
    throw std::runtime_error("Camera Native encoding requires linked FFmpeg avfilter support");
#else
    auto& s = *m;
    s.options = options;
    if (!s.packet || !s.audioPacket || !s.frame) throw std::bad_alloc();
    if (options.width <= 0 || options.height <= 0 || !std::isfinite(options.fps) || !(options.fps > 0) || options.fps > std::numeric_limits<int>::max() / 2.0)
        throw std::runtime_error("Invalid native video dimensions or frame rate");
    if (options.pixelFormat.rfind("yuvj", 0) == 0)
        throw std::runtime_error("Deprecated yuvj pixel layouts are not permitted for native encoding; use explicit full-range metadata");
    bool directP010 = false;
#ifdef __ANDROID__
    if (options.androidHardware && (options.hardwareBitrate <= 0 ||
        options.androidHardware->pixelFormat != options.pixelFormat || options.androidHardware->name.empty()))
        throw std::runtime_error("Invalid Android hardware encoder configuration");
    directP010 = options.androidHardware.has_value() && options.pixelFormat == "p010le";
#endif
    if (!directP010 && !supports(options.encoder, options.pixelFormat))
        throw std::runtime_error("The linked FFmpeg build cannot encode " + options.encoder + " in " + options.pixelFormat);
    const auto* encoder = avcodec_find_encoder_by_name(options.encoder.c_str());
    if (!encoder) throw std::runtime_error("The linked FFmpeg build does not recognize the requested encoder");
    check(avformat_alloc_output_context2(&s.output, nullptr, nullptr, options.outputPath.c_str()), "Create native video container");
    if (!s.output) throw std::bad_alloc();
    s.codec = avcodec_alloc_context3(encoder);
    if (!s.codec) throw std::bad_alloc();
    s.codec->thread_count = 4;
    s.codec->width = options.width;
    s.codec->height = options.height;
    s.codec->pix_fmt = av_get_pix_fmt(options.pixelFormat.c_str());
    s.codec->time_base = {1, 1000000};
    s.codec->framerate = av_d2q(options.fps, 1000000);
    s.codec->gop_size = std::max(1, static_cast<int>(options.fps * 2));
    s.codec->max_b_frames = 0;
    s.codec->color_range = AVCOL_RANGE_JPEG;
    s.codec->color_primaries = AVCOL_PRI_BT2020;
    s.codec->color_trc = AVCOL_TRC_UNSPECIFIED;
    const auto* description = av_pix_fmt_desc_get(s.codec->pix_fmt);
    s.codec->colorspace = description->flags & AV_PIX_FMT_FLAG_RGB ? AVCOL_SPC_RGB : AVCOL_SPC_BT2020_NCL;
    const bool inBandHeaders = options.inBandHeaders || options.encoder.find("_mediacodec") != std::string::npos;
    if (!inBandHeaders && (s.output->oformat->flags & AVFMT_GLOBALHEADER))
        s.codec->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
#ifdef __ANDROID__
    if (directP010) {
        const auto& hardware = *options.androidHardware;
        P010VideoEncoder::Options nativeOptions;
        nativeOptions.codecName = hardware.name;
        nativeOptions.mime = s.codec->codec_id == AV_CODEC_ID_H264 ? "video/avc"
            : s.codec->codec_id == AV_CODEC_ID_HEVC ? "video/hevc" : "video/av01";
        nativeOptions.bitrateMode = hardware.bitrateMode;
        nativeOptions.profile = hardware.profile;
        nativeOptions.bitrate = options.hardwareBitrate;
        nativeOptions.width = options.width; nativeOptions.height = options.height;
        nativeOptions.fps = options.fps;
        s.codec->profile = s.codec->codec_id == AV_CODEC_ID_H264 ? AV_PROFILE_H264_HIGH_10
            : s.codec->codec_id == AV_CODEC_ID_HEVC ? AV_PROFILE_HEVC_MAIN_10 : AV_PROFILE_AV1_MAIN;
        s.codec->bits_per_raw_sample = 10;
        s.p010Encoder = std::make_unique<P010VideoEncoder>(nativeOptions);
    } else
#endif
    {
        Dictionary codecOptions;
#ifdef __ANDROID__
        if (options.androidHardware) {
            const auto& hardware = *options.androidHardware;
            s.codec->bit_rate = options.hardwareBitrate;
            av_dict_set(&codecOptions.value, "codec_name", hardware.name.c_str(), 0);
            av_dict_set(&codecOptions.value, "ndk_codec", "1", 0);
            av_dict_set(&codecOptions.value, "bitrate_mode", hardware.bitrateMode.c_str(), 0);
        }
#endif
        for (const auto& [key, value] : options.codecOptions)
            av_dict_set(&codecOptions.value, key.c_str(), value.c_str(), 0);
        check(avcodec_open2(s.codec, encoder, &codecOptions.value), "Open native video encoder");
        if (av_dict_count(codecOptions.value))
            throw std::runtime_error("Unsupported native encoder option: " + std::string(av_dict_get(codecOptions.value, "", nullptr, AV_DICT_IGNORE_SUFFIX)->key));
    }
    s.videoStream = avformat_new_stream(s.output, nullptr);
    if (!s.videoStream) throw std::bad_alloc();
    s.videoStream->time_base = s.codec->time_base;
    s.videoStream->avg_frame_rate = s.codec->framerate;
    check(avcodec_parameters_from_context(s.videoStream->codecpar, s.codec), "Set native video format");
    const int orientation = options.orientation < 0 ? 0 : options.orientation % 360;
    if (orientation % 90) throw std::runtime_error("Native video orientation must be a multiple of 90 degrees");
    if (orientation) {
#if LIBAVCODEC_VERSION_MAJOR >= 60
        auto* side = av_packet_side_data_new(&s.videoStream->codecpar->coded_side_data,
            &s.videoStream->codecpar->nb_coded_side_data, AV_PKT_DATA_DISPLAYMATRIX, 9 * sizeof(int32_t), 0);
        auto* matrix = side ? reinterpret_cast<int32_t*>(side->data) : nullptr;
#else
        auto* matrix = reinterpret_cast<int32_t*>(av_stream_new_side_data(s.videoStream, AV_PKT_DATA_DISPLAYMATRIX, 9 * sizeof(int32_t)));
#endif
        if (!matrix) throw std::bad_alloc();
        // The setter takes clockwise degrees; the getter reports the inverse.
        // Match the clockwise orientation used by Fuse and DirectLog imports.
        av_display_rotation_set(matrix, orientation);
    }
    if (!options.audioPath.empty()) {
        check(avformat_open_input(&s.audio, options.audioPath.c_str(), nullptr, nullptr), "Open native audio");
        check(avformat_find_stream_info(s.audio, nullptr), "Read native audio format");
        s.audioIndex = av_find_best_stream(s.audio, AVMEDIA_TYPE_AUDIO, -1, -1, nullptr, 0);
        check(s.audioIndex, "Find native audio stream");
        s.audioStream = avformat_new_stream(s.output, nullptr);
        if (!s.audioStream) throw std::bad_alloc();
        check(avcodec_parameters_copy(s.audioStream->codecpar, s.audio->streams[s.audioIndex]->codecpar), "Set native audio format");
        s.audioStream->codecpar->codec_tag = 0;
        s.audioStream->time_base = s.audio->streams[s.audioIndex]->time_base;
    }
    s.graph = avfilter_graph_alloc();
    if (!s.graph) throw std::bad_alloc();
    // Avoid a second, unbounded thread pool alongside the renderer and codec.
    s.graph->nb_threads = 2;
    const std::string input = "video_size=" + std::to_string(options.width) + "x" + std::to_string(options.height) +
        ":pix_fmt=" + std::to_string(AV_PIX_FMT_RGB48LE) + ":time_base=1/1000000000:pixel_aspect=1/1";
    check(avfilter_graph_create_filter(&s.source, avfilter_get_by_name("buffer"), "rgb", input.c_str(), nullptr, s.graph), "Create native RGB source");
    AVBufferSrcParameters* sourceParameters = av_buffersrc_parameters_alloc();
    if (!sourceParameters) throw std::bad_alloc();
    sourceParameters->format = AV_PIX_FMT_RGB48LE;
    sourceParameters->width = options.width;
    sourceParameters->height = options.height;
    sourceParameters->time_base = {1, 1000000000};
    sourceParameters->sample_aspect_ratio = {1, 1};
#if LIBAVFILTER_VERSION_MAJOR >= 10
    sourceParameters->color_space = AVCOL_SPC_RGB;
    sourceParameters->color_range = AVCOL_RANGE_JPEG;
#endif
    const int sourceStatus = av_buffersrc_parameters_set(s.source, sourceParameters);
    av_free(sourceParameters);
    check(sourceStatus, "Set native RGB input properties");
    check(avfilter_graph_create_filter(&s.sink, avfilter_get_by_name("buffersink"), "video", nullptr, nullptr, s.graph), "Create native video sink");
    // swscale is part of every supported Android build. Keep full-range
    // BT.2020 matrix conversion and ordered dithering explicit.
    std::string filters = "lutrgb=r=log(1+60*val/maxval)/log(61)*maxval:g=log(1+60*val/maxval)/log(61)*maxval:b=log(1+60*val/maxval)/log(61)*maxval,"
        "scale=in_range=full:out_range=full:out_color_matrix=bt2020:flags=accurate_rnd+full_chroma_int:sws_dither=bayer,format=" + options.pixelFormat;
    if (options.constantFrameRate)
        filters += ",fps=fps=" + std::to_string(s.codec->framerate.num) + "/" + std::to_string(s.codec->framerate.den) + ":round=near";
    AVFilterInOut* inputs = avfilter_inout_alloc();
    AVFilterInOut* outputs = avfilter_inout_alloc();
    if (!inputs || !outputs) {
        avfilter_inout_free(&inputs); avfilter_inout_free(&outputs); throw std::bad_alloc();
    }
    inputs->name = av_strdup("out"); inputs->filter_ctx = s.sink; inputs->pad_idx = 0;
    outputs->name = av_strdup("in"); outputs->filter_ctx = s.source; outputs->pad_idx = 0;
    const int parsed = avfilter_graph_parse_ptr(s.graph, filters.c_str(), &inputs, &outputs, nullptr);
    avfilter_inout_free(&inputs); avfilter_inout_free(&outputs);
    check(parsed, "Build native color conversion");
    check(avfilter_graph_config(s.graph, nullptr), "Configure native color conversion");
    check(avio_open(&s.output->pb, options.outputPath.c_str(), AVIO_FLAG_WRITE), "Open native output");
    if (inBandHeaders) {
        const auto* filter = av_bsf_get_by_name("extract_extradata");
        if (!filter) throw std::runtime_error("Linked FFmpeg is missing the extract_extradata filter");
        check(av_bsf_alloc(filter, &s.extractHeaders), "Create native header extractor");
        check(avcodec_parameters_from_context(s.extractHeaders->par_in, s.codec), "Set native header format");
        s.extractHeaders->time_base_in = s.codec->time_base;
        check(av_bsf_init(s.extractHeaders), "Initialize native header extractor");
    } else s.writeHeader();
#endif
}
NativeVideoEncoder::~NativeVideoEncoder() = default;

void NativeVideoEncoder::writeFrame(const std::vector<uint8_t>& rgb, int64_t pts, int64_t duration) {
#ifdef MOTIONCAM_HAS_AVFILTER
    auto& s = *m;
    if (s.finished || rgb.size() != static_cast<size_t>(s.options.width) * s.options.height * 6 || duration <= 0)
        throw std::runtime_error("Invalid native RGB frame");
    AVFrame* input = av_frame_alloc();
    if (!input) throw std::bad_alloc();
    std::unique_ptr<AVFrame, void(*)(AVFrame*)> owned(input, [](AVFrame* p) { av_frame_free(&p); });
    input->format = AV_PIX_FMT_RGB48LE;
    input->width = s.options.width; input->height = s.options.height;
    input->pts = pts; input->duration = duration;
    input->color_range = AVCOL_RANGE_JPEG;
    input->color_primaries = AVCOL_PRI_BT2020;
    input->colorspace = AVCOL_SPC_RGB;
    check(av_frame_get_buffer(input, 0), "Allocate native RGB frame");
    for (int row = 0; row < input->height; ++row)
        std::memcpy(input->data[0] + row * input->linesize[0], rgb.data() + static_cast<size_t>(row) * input->width * 6, input->width * 6);
    check(av_buffersrc_add_frame_flags(s.source, input, AV_BUFFERSRC_FLAG_KEEP_REF), "Submit native RGB frame");
    s.lastEnd = pts + duration;
    s.drainFrames();
#endif
}
void NativeVideoEncoder::finish() {
#ifdef MOTIONCAM_HAS_AVFILTER
    auto& s = *m;
    if (s.finished) return;
    check(av_buffersrc_close(s.source, s.lastEnd, 0), "Finish native RGB input");
    s.drainFrames();
#ifdef __ANDROID__
    if (s.p010Encoder)
        s.p010Encoder->finish([&](AVPacket* encoded) { s.consumePacket(encoded); });
    else
#endif
    {
        check(avcodec_send_frame(s.codec, nullptr), "Flush native video encoder");
        s.drainPackets();
    }
    if (s.extractHeaders) {
        check(av_bsf_send_packet(s.extractHeaders, nullptr), "Finish native video headers");
        s.drainHeaders();
    }
    if (!s.headerWritten) throw std::runtime_error("The encoder produced no video packets");
    if (!s.frameDurations.empty()) throw std::runtime_error("The encoder did not preserve every video frame timestamp");
    s.drainAudio(0, {1, 1}, true);
    check(av_write_trailer(s.output), "Finish native video container");
    check(avio_closep(&s.output->pb), "Close native video output");
    // Hardware components can accept a format and then negotiate a different
    // layout or omit color signaling. Validate the actual bitstream before
    // the caller publishes a LOG60/full-range sidecar with it.
    if (s.options.encoder.find("_mediacodec") != std::string::npos) {
        AVFormatContext* input = nullptr;
        check(avformat_open_input(&input, s.options.outputPath.c_str(), nullptr, nullptr), "Verify hardware output");
        std::unique_ptr<AVFormatContext, void(*)(AVFormatContext*)> owned(input,
            [](AVFormatContext* p) { avformat_close_input(&p); });
        check(avformat_find_stream_info(input, nullptr), "Read hardware output format");
        const int index = av_find_best_stream(input, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
        check(index, "Find hardware output video");
        const auto* actual = input->streams[index]->codecpar;
        const auto* layout = av_pix_fmt_desc_get(static_cast<AVPixelFormat>(actual->format));
        const auto* requested = av_pix_fmt_desc_get(s.codec->pix_fmt);
        if (!layout || layout->comp[0].depth != requested->comp[0].depth || layout->log2_chroma_w != 1 || layout->log2_chroma_h != 1 ||
            actual->width != s.options.width || actual->height != s.options.height ||
            actual->color_range != AVCOL_RANGE_JPEG || actual->color_primaries != AVCOL_PRI_BT2020 ||
            actual->color_space != AVCOL_SPC_BT2020_NCL || actual->codec_id != s.codec->codec_id)
            throw std::runtime_error("This hardware encoder did not preserve the requested bit depth and 4:2:0 full-range BT.2020 video format. Use ProRes or CineForm on this device.");
    }
    s.finished = true;
#endif
}
}
