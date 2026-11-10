#include "RawLogTransform.h"
#include "NativeVideoEncoder.h"
#include "AudioWriter.h"
#include "P010Packing.h"
#include <QTemporaryDir>
#include <QFile>
#include <cmath>
#include <cstring>
#include <cstdio>
#include <stdexcept>

extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/display.h>
#include <libavutil/pixdesc.h>
}

namespace {
void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}
void checkP010Packing() {
    AVFrame* frame = av_frame_alloc();
    require(frame != nullptr, "Allocate P010 test frame");
    std::unique_ptr<AVFrame, void(*)(AVFrame*)> owned(frame, [](AVFrame* p) { av_frame_free(&p); });
    frame->format = AV_PIX_FMT_P010LE;
    frame->width = 32; frame->height = 16;
    require(av_frame_get_buffer(frame, 64) >= 0, "Allocate aligned P010 planes");
    // Include adjacent 10-bit levels that would collapse to the same 8-bit
    // value. Packing must preserve them, including the six low padding bits.
    const uint16_t levels[] = {0, 1, 511, 512, 1022, 1023};
    for (int plane = 0; plane < 2; ++plane) {
        for (int y = 0; y < (plane ? 8 : 16); ++y) {
            for (int x = 0; x < 32; ++x) {
                const uint16_t value = levels[(x + y) % 6] << 6;
                auto* at = frame->data[plane] + y * frame->linesize[plane] + x * 2;
                at[0] = value & 255; at[1] = value >> 8;
            }
        }
    }
    constexpr int stride = 96, slice = 32;
    constexpr size_t size = stride * slice * 3 / 2;
    std::vector<uint8_t> packed(size + 16, 0xab);
    require(motioncam::packP010Input(*frame, packed.data(), size, stride, slice) == size, "P010 packed byte count");
    for (int plane = 0; plane < 2; ++plane) {
        for (int y = 0; y < (plane ? slice / 2 : slice); ++y) {
            for (int x = 0; x < stride / 2; ++x) {
                const size_t offset = (plane ? stride * slice : 0) + y * stride + x * 2;
                const int sample = packed[offset] | (packed[offset + 1] << 8);
                const int expected = y < (plane ? 8 : 16) && x < 32 ? levels[(x + y) % 6] << 6 : plane ? 512 << 6 : 0;
                require(sample == expected, "P010 retains 10-bit samples and neutral padding with padded stride/slice height");
            }
        }
    }
    for (size_t i = size; i < packed.size(); ++i) require(packed[i] == 0xab, "P010 buffer bounds");
    try {
        motioncam::packP010Input(*frame, packed.data(), size - 1, stride, slice);
        require(false, "Reject undersized P010 buffers");
    } catch (const std::runtime_error& e) {
        require(std::string(e.what()).find("too small") != std::string::npos, "P010 bounds validation");
    }
}
void encodeAndCheck(const QString& path, bool cfr, const std::string& codec = "prores_ks",
                    const std::string& pixelFormat = "yuv444p10le", const QString& audio = {}, int orientation = 90) {
    const bool shoulder = path.contains("LLOG60_NATIVE");
    motioncam::NativeVideoEncoder::Options options;
    options.outputPath = path.toStdString();
    options.encoder = codec;
    options.inBandHeaders = codec == "libx265" || codec == "libaom-av1" || codec == "libx264";
    options.pixelFormat = pixelFormat;
    options.audioPath = audio.toStdString();
    if (codec == "libx264")
        options.codecOptions = {{"preset", "ultrafast"}, {"tune", "zerolatency"}, {"x264-params", "range=pc"}};
    else if (codec == "libx265")
        options.codecOptions = {{"preset", "ultrafast"}, {"x265-params", "pools=none:frame-threads=1:bframes=0:log-level=error:range=full"}};
    else if (codec == "libaom-av1")
        options.codecOptions = {{"cpu-used", "8"}, {"crf", "20"}, {"b", "0"},
            {"profile", pixelFormat == "yuv420p10le" ? "0" : pixelFormat == "yuv422p10le" ? "2" : "1"}};
    else if (codec == "cfhd")
        options.codecOptions = {{"quality", "film3+"}};
    else
        options.codecOptions = {{"profile", pixelFormat == "yuv422p10le" ? "3" : "4"}, {"alpha_bits", "0"}};
    options.width = options.height = 64;
    options.orientation = orientation;
    options.fps = 25;
    options.constantFrameRate = cfr;
    {
        motioncam::NativeVideoEncoder encoder(options);
        const int64_t timestamps[] = {0, 40000000, 120000000};
        const int64_t durations[] = {40000000, 80000000, 40000000};
        std::vector<uint8_t> rgb(64 * 64 * 6);
        // Neutral bands expose both LOG60 midtones and the black/white
        // endpoints needed to distinguish full-range from limited-range YUV.
        for (int y = 0; y < 64; ++y) {
            const uint16_t value = y < 16 ? 0 : y < 32 ? 6554 : y < 48 ? 65535 : shoulder ? 64552 : 32768;
            for (int x = 0; x < 64 * 3; ++x) {
                const size_t at = (static_cast<size_t>(y) * 64 * 3 + x) * 2;
                rgb[at] = value & 255; rgb[at + 1] = value >> 8;
            }
        }
        for (int i = 0; i < 3; ++i) encoder.writeFrame(rgb, timestamps[i], durations[i]);
        encoder.finish();
        encoder.finish();
    }
    AVFormatContext* input = nullptr;
    require(avformat_open_input(&input, options.outputPath.c_str(), nullptr, nullptr) >= 0, "Open encoded video");
    std::unique_ptr<AVFormatContext, void(*)(AVFormatContext*)> owned(input, [](AVFormatContext* p) { avformat_close_input(&p); });
    require(avformat_find_stream_info(input, nullptr) >= 0, "Read encoded video");
    auto* stream = input->streams[0];
    require(stream->codecpar->width == 64 && stream->codecpar->height == 64, "Video dimensions");
    // MOV's nclc atom has no range bit and ProRes decoders infer limited
    // range. Check the actual full-range samples for that codec below.
    if (codec == "libx265" || codec == "libaom-av1" || codec == "libx264")
        require(stream->codecpar->color_range == AVCOL_RANGE_JPEG, "Full-range metadata");
    require(stream->codecpar->color_primaries == AVCOL_PRI_BT2020, "BT.2020 primaries");
    if (pixelFormat != "gbrp12le")
        require(stream->codecpar->color_space == AVCOL_SPC_BT2020_NCL, "BT.2020 matrix");
#if LIBAVCODEC_VERSION_MAJOR >= 60
    const auto* side = av_packet_side_data_get(stream->codecpar->coded_side_data, stream->codecpar->nb_coded_side_data, AV_PKT_DATA_DISPLAYMATRIX);
    const auto* matrix = side ? reinterpret_cast<const int32_t*>(side->data) : nullptr;
#else
    size_t size = 0;
    const auto* matrix = reinterpret_cast<const int32_t*>(av_stream_get_side_data(stream, AV_PKT_DATA_DISPLAYMATRIX, &size));
#endif
    // Import the track using the same clockwise convention as DirectLogDecoder.
    const int importedOrientation = matrix ? ((static_cast<int>(std::lround(-av_display_rotation_get(matrix))) % 360) + 360) % 360 : 0;
    require(importedOrientation == (orientation < 0 ? 0 : orientation), "Round-trip clockwise display orientation");
    const auto* decoder = avcodec_find_decoder(stream->codecpar->codec_id);
    AVCodecContext* context = avcodec_alloc_context3(decoder);
    std::unique_ptr<AVCodecContext, void(*)(AVCodecContext*)> ownedCodec(context, [](AVCodecContext* p) { avcodec_free_context(&p); });
    require(avcodec_parameters_to_context(context, stream->codecpar) >= 0 && avcodec_open2(context, decoder, nullptr) >= 0, "Open decoder");
    AVPacket* packet = av_packet_alloc();
    AVFrame* frame = av_frame_alloc();
    int count = 0, decoded = 0;
    size_t audioBytes = 0;
    int64_t previous = -1;
    auto drain = [&] {
        while (avcodec_receive_frame(context, frame) >= 0) {
            const auto* format = av_pix_fmt_desc_get(static_cast<AVPixelFormat>(frame->format));
            require(format && format->comp[0].depth >= (pixelFormat == "nv12" ? 8 : 10), "Preserve requested bit depth");
            require(format->log2_chroma_w == (pixelFormat == "yuv444p10le" || pixelFormat == "gbrp12le" ? 0 : 1) &&
                    format->log2_chroma_h == (pixelFormat == "yuv420p10le" || pixelFormat == "nv12" ? 1 : 0), "Preserve chroma layout");
            auto luma = [&](int y) {
                const auto* row = frame->data[0] + y * frame->linesize[0];
                const int sample = format->comp[0].depth == 8 ? row[32] : row[64] | (row[65] << 8);
                return double(sample) / ((1 << format->comp[0].depth) - 1);
            };
            const double expected = motioncam::LLog60::encode(0.1);
            require(std::abs(luma(24) - expected) < .015, "Native transfer full-range pixel value");
            if (shoulder) {
                require(std::abs(luma(56) - motioncam::LLog60::encode(64552. / 65535)) < .006,
                    "AV1 retains modified highlight transition");
                require(std::abs(motioncam::LLog60::decode(luma(56)) - 64552. / 65535) < .007,
                    "AV1 highlights round-trip through modified inverse");
            }
            require(luma(8) < .02 && luma(40) > .98, "Full-range black and white endpoints (no 16-235/64-940 scaling)");
            ++decoded;
            av_frame_unref(frame);
        }
    };
    while (av_read_frame(input, packet) >= 0) {
        if (packet->stream_index == stream->index) {
            const int64_t timestamp = av_rescale_q(packet->pts, stream->time_base, AVRational{1, 1000000000});
            const int64_t expected = cfr ? count * 40000000LL : (count == 2 ? 120000000LL : count * 40000000LL);
            require(std::abs(timestamp - expected) <= 1000 && timestamp > previous, "Video frame timestamps");
            const int64_t duration = av_rescale_q(packet->duration, stream->time_base, AVRational{1, 1000000000});
            require(std::abs(duration - (!cfr && count == 1 ? 80000000LL : 40000000LL)) <= 1000, "Preserve video frame durations");
            previous = timestamp;
            require(avcodec_send_packet(context, packet) >= 0, "Decode video packet");
            drain();
            ++count;
        } else if (input->streams[packet->stream_index]->codecpar->codec_type == AVMEDIA_TYPE_AUDIO) {
            const bool flac = audio.endsWith(".flac");
            require(input->streams[packet->stream_index]->codecpar->codec_id ==
                (flac ? AV_CODEC_ID_FLAC : AV_CODEC_ID_PCM_S16LE), "Preserve audio codec");
            audioBytes += packet->size;
            if (!flac)
                for (int i = 0; i < packet->size; ++i) require(packet->data[i] == 0, "Preserve audio samples");
        }
        av_packet_unref(packet);
    }
    require(avcodec_send_packet(context, nullptr) >= 0, "Flush decoder");
    drain();
    require(decoded == count, "Decode every encoded frame including delayed frames");
    require(audio.endsWith(".flac") ? audioBytes > 0 : audioBytes == (audio.isEmpty() ? 0 : 15360), "Preserve audio payload");
    av_packet_free(&packet); av_frame_free(&frame);
    require(count == (cfr ? 4 : 3), "VFR/CFR output frame count");
}
}
int main() {
    // No event loop or Java activity is needed for local-file codec fixtures.
    try {
        checkP010Packing();
        using Curve = motioncam::LLog60;
        double previousCode = -1;
        for (int i = 0; i <= 65535; ++i) {
            const double x = i / 65535., y = Curve::encode(x);
            require(y > previousCode, "Shoulder curve is strictly monotonic");
            require(std::abs(Curve::decode(y) - x) < 1e-12, "Shoulder inverse round-trip");
            previousCode = y;
        }
        for (double x : {Curve::start, Curve::end}) {
            constexpr double h = 1e-7;
            const double left = (Curve::encode(x) - Curve::encode(x-h))/h;
            const double right = (Curve::encode(x+h) - Curve::encode(x))/h;
            require(std::abs(left-right) < 2e-6, "Continuous slope at shoulder joins");
        }
        require(Curve::encode(0) == 0 && Curve::encode(1) == 1 &&
            Curve::encode(.995) == .995, "Fixed endpoints and linear white segment");
        QTemporaryDir directory;
        require(directory.isValid(), "Temporary directory");
        try {
            motioncam::NativeVideoEncoder::Options legacy;
            legacy.width = legacy.height = 64;
            legacy.encoder = "prores_ks";
            legacy.pixelFormat = "yuvj420p";
            motioncam::NativeVideoEncoder encoder(legacy);
            require(false, "Reject deprecated JPEG-range pixel formats");
        } catch (const std::runtime_error& e) {
            require(std::string(e.what()).find("Deprecated yuvj") != std::string::npos, "Reject yuvj before opening an encoder");
        }
        require(!motioncam::NativeVideoEncoder::supports("missing_encoder", "yuv420p10le"), "Reject missing encoders");
        require(!motioncam::NativeVideoEncoder::supports("prores_ks", "yuv420p"), "Reject unsupported pixel formats");
        const QString audio = directory.filePath("audio.wav");
        std::vector<uint8_t> wav(44 + 15360, 0);
        auto le32 = [&](size_t at, uint32_t value) {
            for (int i = 0; i < 4; ++i) wav[at + i] = (value >> (8 * i)) & 255;
        };
        std::memcpy(wav.data(), "RIFF", 4); le32(4, wav.size() - 8);
        std::memcpy(wav.data() + 8, "WAVEfmt ", 8); le32(16, 16);
        wav[20] = wav[22] = 1; le32(24, 48000); le32(28, 96000);
        wav[32] = 2; wav[34] = 16;
        std::memcpy(wav.data() + 36, "data", 4); le32(40, 15360);
        QFile audioFile(audio);
        require(audioFile.open(QIODevice::WriteOnly) && audioFile.write(reinterpret_cast<const char*>(wav.data()), wav.size()) == static_cast<qint64>(wav.size()), "Create audio fixture");
        audioFile.close();
        const QString flacAudio = directory.filePath("audio.flac");
        motioncam::AudioWriter::convertFile(audio.toStdString(), flacAudio.toStdString(), "flac", false);
        encodeAndCheck(directory.filePath("vfr.mov"), false, "prores_ks", "yuv444p10le", audio);
        encodeAndCheck(directory.filePath("cfr.mov"), true);
        encodeAndCheck(directory.filePath("prores422.mov"), false, "prores_ks", "yuv422p10le");
        encodeAndCheck(directory.filePath("cineform422.mov"), false, "cfhd", "yuv422p10le");
        encodeAndCheck(directory.filePath("cineformRGB.mov"), false, "cfhd", "gbrp12le");
        for (int rotation : {-1, 0, 180, 270})
            encodeAndCheck(directory.filePath(QString("rotation-%1.mov").arg(rotation)), false,
                "cfhd", "yuv422p10le", {}, rotation);
        if (motioncam::NativeVideoEncoder::supports("libx264", "nv12"))
            encodeAndCheck(directory.filePath("nv12.mov"), false, "libx264", "nv12", audio);
        if (motioncam::NativeVideoEncoder::supports("libaom-av1", "yuv420p10le"))
            encodeAndCheck(directory.filePath("clip_LLOG60_NATIVE.mp4"), false,
                "libaom-av1", "yuv420p10le", flacAudio);
        for (const char* codec : {"libx265", "libaom-av1"}) {
            for (const char* format : {"yuv420p10le", "yuv422p10le", "yuv444p10le"}) {
                if (!motioncam::NativeVideoEncoder::supports(codec, format)) continue;
                const bool av1 = std::string(codec) == "libaom-av1";
                encodeAndCheck(directory.filePath(QString::fromLatin1(codec) + format + (av1 ? ".mp4" : ".mov")),
                    false, codec, format, av1 ? flacAudio : audio);
            }
        }
    } catch (const std::exception& e) {
        std::fprintf(stderr, "%s\n", e.what()); return 1;
    }
}
