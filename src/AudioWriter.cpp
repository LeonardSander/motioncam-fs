#include "SourceMedia.h"
#include "SourceInput.h"
#include "AudioWriter.h"
#include <algorithm>
#include <cerrno>
#include <stdexcept>
#include <cmath>
#include <cstring>
#include <limits>
extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/audio_fifo.h>
#include <libavutil/samplefmt.h>
#include <libswresample/swresample.h>
}

namespace motioncam {
    namespace {
        constexpr auto PROJECT = "RAW Video";
        constexpr auto NOTES = "-";
    
        constexpr auto METADATA =
            "<?xml version=\"1.0\" encoding=\"UTF-8\"?>"
            "<BWFXML>"
            "<IXML_VERSION>1.5</IXML_VERSION>"
            "<PROJECT>%s</PROJECT>"
            "<NOTE>%s</NOTE>"
            "<CIRCLED>FALSE</CIRCLED>"
            "<BLACKMAGIC-KEYWORDS>%s</BLACKMAGIC-KEYWORDS>"
            "<TAPE>%d</TAPE>"
            "<SCENE>%d</SCENE>"
            "<BLACKMAGIC-SHOT>%d</BLACKMAGIC-SHOT>"
            "<TAKE>%d</TAKE>"
            "<BLACKMAGIC-ANGLE>ms</BLACKMAGIC-ANGLE>"
            "<SPEED>"
            "<MASTER_SPEED>%d/%d</MASTER_SPEED>"
            "<CURRENT_SPEED>%d/%d</CURRENT_SPEED>"
            "<TIMECODE_RATE>%d/%d</TIMECODE_RATE>"
            "<TIMECODE_FLAG>NDF</TIMECODE_FLAG>"
            "</SPEED>"
            "</BWFXML>";
    }

    static std::string FormatMetadata(const int fpsNum,
                                      const int fpsDen,
                                      const std::string& project,
                                      const std::string& notes,
                                      const std::string& keywords,
                                      const int& tape,
                                      const int& scene,
                                      const int& shot,
                                      const int& take) {
    
        char result[1024];
        
        snprintf(&result[0],
                 1024,
                 METADATA,
                 project.c_str(),
                 notes.c_str(),
                 keywords.c_str(),
                 tape,
                 scene,
                 shot,
                 take,
                 fpsNum,
                 fpsDen,
                 fpsNum,
                 fpsDen,
                 fpsNum,
                 fpsDen);
        
        return std::string(result);
    }

    static std::shared_ptr<bw64::Chunk> CreateMetadata(int fpsNum, int fpsDen) {
        const int tape = 1;
        const int scene = 1;
        const int shot = 1;
        const int take = 1;
        
        auto metadata = FormatMetadata(fpsNum,
                                       fpsDen,
                                       PROJECT,
                                       NOTES,
                                       "",
                                       tape,
                                       scene,
                                       shot,
                                       take);
        
        return std::make_shared<bw64::iXmlChunk>(metadata);
    }

    AudioWriter::AudioWriter(std::vector<uint8_t>& output, int numChannels, int sampleRate, int fpsNum, int fpsDen, int bitDepth) : mFd(-1) {
        if(numChannels <= 0 || sampleRate <= 0)
            throw std::runtime_error("Invalid format");

        std::vector<std::shared_ptr<bw64::Chunk>> additionalChunks;
        additionalChunks.push_back(CreateMetadata(fpsNum, fpsDen));

        mWriter = std::unique_ptr<bw64::Bw64Writer>(new bw64::Bw64Writer(
                output, numChannels, sampleRate, bitDepth, additionalChunks));
    }

    void AudioWriter::write(const std::vector<int16_t>& data, int numFrames) {
        mWriter->write(data.data(), numFrames);
    }

    void AudioWriter::write(const std::vector<float>& data, int numFrames) {
        mWriter->write(data.data(), numFrames);
    }
}

namespace {
void checkAudio(int status, const char* operation) {
    if (status >= 0) return;
    char message[AV_ERROR_MAX_STRING_SIZE]{};
    av_strerror(status, message, sizeof(message));
    throw std::runtime_error(std::string(operation) + ": " + message);
}
void audioProgress(const std::function<bool()>& keepGoing) {
    if (keepGoing && !keepGoing()) throw std::runtime_error("Finalization cancelled");
}
using AudioCodec = std::unique_ptr<AVCodecContext, void(*)(AVCodecContext*)>;
using AudioFrame = std::unique_ptr<AVFrame, void(*)(AVFrame*)>;
using AudioPacket = std::unique_ptr<AVPacket, void(*)(AVPacket*)>;
void freeAudioFrame(AVFrame* p) { av_frame_free(&p); }
void freeAudioPacket(AVPacket* p) { av_packet_free(&p); }

// FFmpeg reads ZIP companions through the same seekable source interface as
// DNG frames, including compressed audio tracks. No temporary audio file.
struct ZipAudioInput {
    motioncam::SourceInputStream input;
    AVIOContext* io = nullptr;
    explicit ZipAudioInput(const std::string& path) : input(path, std::ios::binary) {
        if (!input) throw std::runtime_error("Could not open ZIP audio source");
        auto* buffer = static_cast<uint8_t*>(av_malloc(65536));
        io = buffer ? avio_alloc_context(buffer, 65536, 0, this, read, nullptr, seek) : nullptr;
        if (!io) { av_free(buffer); throw std::bad_alloc(); }
    }
    ~ZipAudioInput() {
        if (io) { av_freep(&io->buffer); avio_context_free(&io); }
    }
    static int read(void* opaque, uint8_t* bytes, int size) {
        try {
            auto& input = static_cast<ZipAudioInput*>(opaque)->input;
            input.read(reinterpret_cast<char*>(bytes), size);
            if (input.bad()) return AVERROR(EIO);
            return input.gcount() ? static_cast<int>(input.gcount()) : AVERROR_EOF;
        } catch (...) { return AVERROR(EIO); }
    }
    static int64_t seek(void* opaque, int64_t offset, int whence) {
        try {
            auto& input = static_cast<ZipAudioInput*>(opaque)->input;
            input.clear();
            if (whence & AVSEEK_SIZE) {
                const auto position = input.tellg();
                input.seekg(0, std::ios::end);
                const auto size = input.tellg();
                input.seekg(position);
                return static_cast<int64_t>(size);
            }
            whence &= ~AVSEEK_FORCE;
            if (whence != SEEK_SET && whence != SEEK_CUR && whence != SEEK_END) return AVERROR(EINVAL);
            input.seekg(offset, whence == SEEK_SET ? std::ios::beg : whence == SEEK_CUR ? std::ios::cur : std::ios::end);
            return input ? static_cast<int64_t>(input.tellg()) : AVERROR(EIO);
        } catch (...) { return AVERROR(EIO); }
    }
};

// One input/decode/resample implementation for mounted WAV audio and exports.
class AudioInput {
public:
    explicit AudioInput(const std::string& path)
        : mFormat(nullptr, [](AVFormatContext* p) {
              if (p && (p->flags & AVFMT_FLAG_CUSTOM_IO) && p->pb && p->pb->read_packet == ZipAudioInput::read)
                  avformat_close_input(&p);
              else motioncam::closeSourceMedia(&p);
          }),
          mDecoder(nullptr, [](AVCodecContext* p) { avcodec_free_context(&p); }) {
        AVFormatContext* raw = nullptr;
        if (motioncam::isZipSource(path)) {
            mZipInput = std::make_unique<ZipAudioInput>(path);
            raw = avformat_alloc_context();
            if (!raw) throw std::bad_alloc();
            raw->pb = mZipInput->io;
            raw->flags |= AVFMT_FLAG_CUSTOM_IO;
            checkAudio(avformat_open_input(&raw, path.c_str(), nullptr, nullptr), "Open ZIP audio source");
        } else checkAudio(motioncam::openSourceMedia(&raw, path), "Open audio source");
        mFormat.reset(raw);
        checkAudio(avformat_find_stream_info(mFormat.get(), nullptr), "Read audio source");
        mIndex = av_find_best_stream(mFormat.get(), AVMEDIA_TYPE_AUDIO, -1, -1, nullptr, 0);
        if (mIndex != AVERROR_STREAM_NOT_FOUND) checkAudio(mIndex, "Find audio stream");
    }
    AVFormatContext* format() const { return mFormat.get(); }
    AVStream* stream() const { return mIndex >= 0 ? mFormat->streams[mIndex] : nullptr; }
    AVCodecContext* decoder() {
        if (mDecoder) return mDecoder.get();
        if (!stream()) throw std::runtime_error("Audio source has no audio stream");
        const AVCodec* codec = avcodec_find_decoder(stream()->codecpar->codec_id);
        if (!codec) throw std::runtime_error("Audio decoder is unavailable");
        mDecoder.reset(avcodec_alloc_context3(codec));
        if (!mDecoder) throw std::bad_alloc();
        checkAudio(avcodec_parameters_to_context(mDecoder.get(), stream()->codecpar), "Configure audio decoder");
        checkAudio(avcodec_open2(mDecoder.get(), codec, nullptr), "Open audio decoder");
        if (mDecoder->sample_rate <= 0 || mDecoder->ch_layout.nb_channels <= 0)
            throw std::runtime_error("Invalid audio source format");
        if (mDecoder->ch_layout.order == AV_CHANNEL_ORDER_UNSPEC)
            av_channel_layout_default(&mDecoder->ch_layout, mDecoder->ch_layout.nb_channels);
        return mDecoder.get();
    }
    template<class Consume>
    void readPackets(const Consume& consume, const std::function<bool()>& keepGoing) {
        AudioPacket packet(av_packet_alloc(), freeAudioPacket);
        if (!packet) throw std::bad_alloc();
        int status;
        while ((status = av_read_frame(mFormat.get(), packet.get())) >= 0) {
            audioProgress(keepGoing);
            if (packet->stream_index == mIndex) consume(packet.get());
            av_packet_unref(packet.get());
        }
        if (status != AVERROR_EOF) checkAudio(status, "Read audio packet");
    }
    template<class Consume>
    void convert(AVSampleFormat format, int rate, const AVChannelLayout& layout,
                 const Consume& consume, const std::function<bool()>& keepGoing = {}) {
        auto* dec = decoder();
        SwrContext* raw = nullptr;
        const int configured = swr_alloc_set_opts2(&raw, &layout, format, rate,
            &dec->ch_layout, dec->sample_fmt, dec->sample_rate, 0, nullptr);
        std::unique_ptr<SwrContext, void(*)(SwrContext*)> resampler(raw,
            [](SwrContext* p) { swr_free(&p); });
        checkAudio(configured, "Configure audio conversion");
        checkAudio(swr_init(resampler.get()), "Initialize audio conversion");
        auto convertFrame = [&](const AVFrame* frame) {
            AudioFrame converted(av_frame_alloc(), freeAudioFrame);
            if (!converted) throw std::bad_alloc();
            converted->format = format;
            converted->sample_rate = rate;
            checkAudio(av_channel_layout_copy(&converted->ch_layout, &layout), "Set converted audio channels");
            const int capacity = swr_get_out_samples(resampler.get(), frame ? frame->nb_samples : 0);
            checkAudio(capacity, "Size converted audio samples");
            converted->nb_samples = std::max(1, capacity);
            checkAudio(av_frame_get_buffer(converted.get(), 0), "Allocate converted audio samples");
            const int count = swr_convert(resampler.get(), converted->extended_data, converted->nb_samples,
                frame ? const_cast<const uint8_t**>(frame->extended_data) : nullptr,
                frame ? frame->nb_samples : 0);
            checkAudio(count, "Convert audio samples");
            converted->nb_samples = count;
            if (count) consume(*converted);
            return count;
        };
        AudioFrame frame(av_frame_alloc(), freeAudioFrame);
        if (!frame) throw std::bad_alloc();
        auto drain = [&] {
            int status;
            while ((status = avcodec_receive_frame(dec, frame.get())) >= 0) {
                audioProgress(keepGoing);
                convertFrame(frame.get());
                av_frame_unref(frame.get());
            }
            if (status != AVERROR(EAGAIN) && status != AVERROR_EOF) checkAudio(status, "Decode audio");
        };
        readPackets([&](AVPacket* packet) {
            checkAudio(avcodec_send_packet(dec, packet), "Submit audio packet");
            drain();
        }, keepGoing);
        checkAudio(avcodec_send_packet(dec, nullptr), "Flush audio decoder");
        drain();
        while (convertFrame(nullptr)) audioProgress(keepGoing);
    }
private:
    std::unique_ptr<ZipAudioInput> mZipInput;
    std::unique_ptr<AVFormatContext, void(*)(AVFormatContext*)> mFormat;
    AudioCodec mDecoder;
    int mIndex = -1;
};
} // namespace

std::shared_ptr<std::vector<uint8_t>> motioncam::AudioWriter::decodeWav(
        const std::string& source, double fps) {
    AudioInput input(source);
    if (!input.stream()) return {};
    auto* decoder = input.decoder();
    const auto timing = av_d2q(fps > 0.0 ? fps : 24.0, 100000);
    auto bytes = std::make_shared<std::vector<uint8_t>>();
    {
        AudioWriter writer(*bytes, decoder->ch_layout.nb_channels,
            decoder->sample_rate, timing.num, timing.den, 16);
        input.convert(AV_SAMPLE_FMT_S16, decoder->sample_rate, decoder->ch_layout,
            [&](const AVFrame& frame) {
                writer.mWriter->write(reinterpret_cast<const int16_t*>(frame.data[0]), frame.nb_samples);
            });
    }
    return bytes;
}

void motioncam::AudioWriter::offsetWav(std::vector<uint8_t>& bytes, int64_t offsetUs) {
    if (!offsetUs) return;
    const auto read16 = [&](size_t p) { return unsigned(bytes[p]) | unsigned(bytes[p + 1]) << 8; };
    const auto read32 = [&](size_t p) { return read16(p) | uint32_t(read16(p + 2)) << 16; };
    const auto write32 = [&](size_t p, uint32_t value) {
        for (unsigned i = 0; i < 4; ++i) bytes[p + i] = static_cast<uint8_t>(value >> (8 * i));
    };
    if (bytes.size() < 12 || std::memcmp(bytes.data(), "RIFF", 4) ||
        std::memcmp(bytes.data() + 8, "WAVE", 4))
        throw std::runtime_error("Audio offset requires a PCM/float RIFF WAV");
    uint32_t rate = 0, alignment = 0, bits = 0, format = 0, channels = 0;
    size_t dataHeader = 0, dataSize = 0, fact = 0;
    for (size_t p = 12; p + 8 <= bytes.size();) {
        const auto size = read32(p + 4);
        if (size > bytes.size() - p - 8)
            throw std::runtime_error("Truncated WAV chunk");
        if (!std::memcmp(bytes.data() + p, "fmt ", 4) && size >= 16) {
            format = read16(p + 8);
            channels = read16(p + 10);
            rate = read32(p + 12);
            alignment = read16(p + 20);
            bits = read16(p + 22);
            if (format == 0xfffe) {
                // Resolve KSDATAFORMAT_SUBTYPE_PCM/IEEE_FLOAT without changing
                // the original extensible header or sample representation.
                static constexpr uint8_t guidTail[] = {
                    0x00, 0x00, 0x10, 0x00, 0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71
                };
                if (size < 40 || read16(p + 24) < 22 || read16(p + 24) > size - 18 ||
                    std::memcmp(bytes.data() + p + 36, guidTail, sizeof(guidTail)))
                    throw std::runtime_error("Unsupported extensible WAV format for audio offset");
                format = read32(p + 32);
                const auto validBits = read16(p + 26);
                if (!validBits || validBits > bits || (format == 3 && validBits != bits))
                    throw std::runtime_error("Invalid extensible WAV sample precision");
            }
        }
        if (!std::memcmp(bytes.data() + p, "fact", 4) && size >= 4) fact = p + 8;
        if (!std::memcmp(bytes.data() + p, "data", 4)) {
            dataHeader = p;
            dataSize = size;
        }
        p += 8 + size_t(size) + (size & 1u);
    }
    if (!dataHeader || !rate || !channels || !alignment || !bits || bits % 8 ||
        alignment != channels * (bits / 8) || dataSize % alignment ||
        (format == 1 ? bits > 32 : format != 3 || (bits != 32 && bits != 64)))
        throw std::runtime_error("Unsupported WAV format for audio offset");
    const long double frames = std::round(std::abs(static_cast<long double>(offsetUs)) * rate / 1000000);
    if (frames > std::numeric_limits<uint32_t>::max() / alignment)
        throw std::runtime_error("Audio offset exceeds WAV size limits");
    const size_t shift = static_cast<size_t>(frames) * alignment;
    const size_t removed = offsetUs < 0 ? std::min(shift, dataSize) : 0;
    const size_t inserted = offsetUs > 0 ? shift : 0;
    const uint64_t newDataSize = uint64_t(dataSize) - removed + inserted;
    const size_t oldEnd = dataHeader + 8 + dataSize + (dataSize & 1u);
    if (oldEnd > bytes.size()) throw std::runtime_error("Truncated WAV data padding");
    const uint64_t newFileSize = uint64_t(bytes.size()) - dataSize - (dataSize & 1u) +
        newDataSize + (newDataSize & 1u);
    if (newDataSize > UINT32_MAX || newFileSize - 8 > UINT32_MAX || newFileSize > bytes.max_size())
        throw std::runtime_error("Aligned audio exceeds WAV size limits");
    const auto newSize = static_cast<size_t>(newDataSize);
    // Update optional sample count before splicing, since it can follow data.
    if (fact) write32(fact, static_cast<uint32_t>(newSize / alignment));
    bytes.erase(bytes.begin() + dataHeader + 8, bytes.begin() + dataHeader + 8 + removed);
    bytes.insert(bytes.begin() + dataHeader + 8, inserted,
                 format == 1 && bits == 8 ? uint8_t{128} : uint8_t{0});
    if ((dataSize & 1u) != (newSize & 1u)) {
        const auto padding = bytes.begin() + dataHeader + 8 + newSize;
        if (newSize & 1u) bytes.insert(padding, 0);
        else bytes.erase(padding);
    }
    write32(dataHeader + 4, static_cast<uint32_t>(newSize));
    write32(4, static_cast<uint32_t>(bytes.size() - 8));
}

// Use the already linked FFmpeg libraries, including on Android where an
// external ffmpeg executable cannot be assumed to exist.
void motioncam::AudioWriter::convertFile(const std::string& source,
        const std::string& destination, const std::string& codec, bool copy,
        const std::function<bool()>& keepGoing) {
    const auto check = checkAudio;
    auto progress = [&] { audioProgress(keepGoing); };
    progress();
    AudioInput input(source);
    auto* inStream = input.stream();
    if (!inStream) throw std::runtime_error("Audio source has no audio stream");
    AVFormatContext* rawOutput = nullptr;
    check(avformat_alloc_output_context2(&rawOutput, nullptr, nullptr, destination.c_str()),
          "Create finalized audio output");
    std::unique_ptr<AVFormatContext, void(*)(AVFormatContext*)> output(rawOutput,
        [](AVFormatContext* p) { if (p->pb) avio_closep(&p->pb); avformat_free_context(p); });
    AVStream* outStream = avformat_new_stream(output.get(), nullptr);
    if (!outStream) throw std::bad_alloc();
    AudioCodec encoder(nullptr, [](AVCodecContext* p) { avcodec_free_context(&p); });
    if (copy) {
        check(avcodec_parameters_copy(outStream->codecpar, inStream->codecpar), "Copy audio format");
        outStream->codecpar->codec_tag = 0;
        outStream->time_base = inStream->time_base;
    } else {
        auto* decoder = input.decoder();
        const AVCodec* enc = codec == "opus" ? avcodec_find_encoder_by_name("libopus")
                                             : avcodec_find_encoder(AV_CODEC_ID_FLAC);
        if (!enc && codec == "opus") enc = avcodec_find_encoder(AV_CODEC_ID_OPUS);
        if (!enc) throw std::runtime_error("Finalized audio codec is unavailable: " + codec);
        encoder.reset(avcodec_alloc_context3(enc));
        if (!encoder) throw std::bad_alloc();
        encoder->sample_rate = codec == "opus" ? 48000 : decoder->sample_rate;
#if LIBAVCODEC_VERSION_MAJOR >= 62
        const void* supported = nullptr;
        check(avcodec_get_supported_config(encoder.get(), enc, AV_CODEC_CONFIG_SAMPLE_FORMAT,
            0, &supported, nullptr), "Query audio sample formats");
        const auto* formats = static_cast<const AVSampleFormat*>(supported);
#else
        const auto* formats = enc->sample_fmts;
#endif
        encoder->sample_fmt = formats ? formats[0] : AV_SAMPLE_FMT_FLTP;
        if (codec == "flac" && av_get_bytes_per_sample(decoder->sample_fmt) > 2 && formats)
            for (const auto* f = formats; *f != AV_SAMPLE_FMT_NONE; ++f)
                if (*f == AV_SAMPLE_FMT_S32) encoder->sample_fmt = *f;
        check(av_channel_layout_copy(&encoder->ch_layout, &decoder->ch_layout), "Copy audio channels");
        encoder->time_base = AVRational{1, encoder->sample_rate};
        if (codec == "opus") encoder->bit_rate = 288000;
        encoder->strict_std_compliance = FF_COMPLIANCE_EXPERIMENTAL;
        if (output->oformat->flags & AVFMT_GLOBALHEADER) encoder->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
        check(avcodec_open2(encoder.get(), enc, nullptr), "Open audio encoder");
        check(avcodec_parameters_from_context(outStream->codecpar, encoder.get()), "Set audio output format");
        outStream->time_base = encoder->time_base;
    }
    av_dict_copy(&output->metadata, input.format()->metadata, 0);
    check(avio_open(&output->pb, destination.c_str(), AVIO_FLAG_WRITE), "Open audio output file");
    check(avformat_write_header(output.get(), nullptr), "Write audio header");
    AudioPacket encoded(av_packet_alloc(), freeAudioPacket);
    if (!encoded) throw std::bad_alloc();
    if (copy) {
        input.readPackets([&](AVPacket* packet) {
            av_packet_rescale_ts(packet, inStream->time_base, outStream->time_base);
            packet->stream_index = outStream->index;
            packet->pos = -1;
            check(av_interleaved_write_frame(output.get(), packet), "Write copied audio packet");
        }, keepGoing);
    } else {
        std::unique_ptr<AVAudioFifo, void(*)(AVAudioFifo*)> fifo(
            av_audio_fifo_alloc(encoder->sample_fmt, encoder->ch_layout.nb_channels, 1), av_audio_fifo_free);
        if (!fifo) throw std::bad_alloc();
        int64_t samplesWritten = 0;
        auto drainEncoder = [&] {
            int status;
            while ((status = avcodec_receive_packet(encoder.get(), encoded.get())) >= 0) {
                av_packet_rescale_ts(encoded.get(), encoder->time_base, outStream->time_base);
                encoded->stream_index = outStream->index;
                check(av_interleaved_write_frame(output.get(), encoded.get()), "Write encoded audio packet");
                av_packet_unref(encoded.get());
            }
            if (status != AVERROR(EAGAIN) && status != AVERROR_EOF) check(status, "Encode finalized audio");
        };
        const int chunk = encoder->frame_size > 0 ? encoder->frame_size : 1024;
        auto drainFifo = [&](bool flush) {
            while (av_audio_fifo_size(fifo.get()) >= chunk || (flush && av_audio_fifo_size(fifo.get()))) {
                progress();
                const int available = std::min(chunk, av_audio_fifo_size(fifo.get()));
                AudioFrame frame(av_frame_alloc(), freeAudioFrame);
                if (!frame) throw std::bad_alloc();
                frame->format = encoder->sample_fmt;
                frame->sample_rate = encoder->sample_rate;
                check(av_channel_layout_copy(&frame->ch_layout, &encoder->ch_layout), "Set encoded audio channels");
                frame->nb_samples = available < chunk && !(encoder->codec->capabilities &
                    (AV_CODEC_CAP_SMALL_LAST_FRAME | AV_CODEC_CAP_VARIABLE_FRAME_SIZE)) ? chunk : available;
                check(av_frame_get_buffer(frame.get(), 0), "Allocate encoded audio samples");
                check(av_samples_set_silence(frame->extended_data, 0, frame->nb_samples,
                    encoder->ch_layout.nb_channels, encoder->sample_fmt), "Initialize encoded audio samples");
                if (av_audio_fifo_read(fifo.get(), reinterpret_cast<void**>(frame->extended_data), available) != available)
                    throw std::runtime_error("Could not read converted audio samples");
                frame->pts = samplesWritten;
                samplesWritten += frame->nb_samples;
                check(avcodec_send_frame(encoder.get(), frame.get()), "Submit finalized audio samples");
                drainEncoder();
            }
        };
        input.convert(encoder->sample_fmt, encoder->sample_rate, encoder->ch_layout,
            [&](const AVFrame& frame) {
                check(av_audio_fifo_realloc(fifo.get(), av_audio_fifo_size(fifo.get()) + frame.nb_samples),
                      "Grow converted audio queue");
                if (av_audio_fifo_write(fifo.get(), reinterpret_cast<void**>(frame.extended_data),
                                        frame.nb_samples) != frame.nb_samples)
                    throw std::runtime_error("Could not queue converted audio samples");
                drainFifo(false);
            }, keepGoing);
        drainFifo(true);
        check(avcodec_send_frame(encoder.get(), nullptr), "Flush audio encoder");
        drainEncoder();
    }
    progress();
    check(av_write_trailer(output.get()), "Finish finalized audio output");
    check(avio_closep(&output->pb), "Close finalized audio output");
}
