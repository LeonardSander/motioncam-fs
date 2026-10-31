#include "AudioWriter.h"
extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
}
#include <cassert>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <vector>

static std::vector<int16_t> decode(const std::string& path, AVCodecID expected) {
    AVFormatContext* input = nullptr;
    assert(avformat_open_input(&input, path.c_str(), nullptr, nullptr) >= 0);
    assert(avformat_find_stream_info(input, nullptr) >= 0);
    const int stream = av_find_best_stream(input, AVMEDIA_TYPE_AUDIO, -1, -1, nullptr, 0);
    assert(stream >= 0 && input->streams[stream]->codecpar->codec_id == expected);
    const AVCodec* codec = avcodec_find_decoder(expected);
    AVCodecContext* decoder = avcodec_alloc_context3(codec);
    assert(avcodec_parameters_to_context(decoder, input->streams[stream]->codecpar) >= 0);
    assert(avcodec_open2(decoder, codec, nullptr) >= 0);
    AVPacket* packet = av_packet_alloc();
    AVFrame* frame = av_frame_alloc();
    std::vector<int16_t> samples;
    auto drain = [&] {
        int result;
        while ((result = avcodec_receive_frame(decoder, frame)) >= 0) {
            assert(frame->ch_layout.nb_channels == 2 && frame->sample_rate == 48000);
            if (expected == AV_CODEC_ID_FLAC || expected == AV_CODEC_ID_PCM_S16LE) {
                assert(frame->format == AV_SAMPLE_FMT_S16);
                const auto* values = reinterpret_cast<const int16_t*>(frame->data[0]);
                samples.insert(samples.end(), values, values + frame->nb_samples * 2);
            } else samples.resize(samples.size() + frame->nb_samples * 2);
            av_frame_unref(frame);
        }
        assert(result == AVERROR_EOF || result == AVERROR(EAGAIN));
    };
    while (av_read_frame(input, packet) >= 0) {
        if (packet->stream_index == stream) {
            assert(avcodec_send_packet(decoder, packet) >= 0);
            drain();
        }
        av_packet_unref(packet);
    }
    assert(avcodec_send_packet(decoder, nullptr) >= 0);
    drain();
    av_frame_free(&frame); av_packet_free(&packet);
    avcodec_free_context(&decoder); avformat_close_input(&input);
    return samples;
}

int main() {
    const auto root = std::filesystem::temp_directory_path() /
        ("mcfuse-audio-finalize-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    std::filesystem::create_directories(root);
    struct Cleanup { std::filesystem::path root; ~Cleanup() { std::filesystem::remove_all(root); } } cleanup{root};
    // A non-frame-aligned duration exercises codec tail and resampler flushing.
    std::vector<int16_t> input(48037 * 2);
    for (size_t i = 0; i < input.size(); ++i) input[i] = int16_t((i * 101u) % 20001u - 10000);
    const auto wav = (root / "audio.wav").string();
    std::vector<uint8_t> bytes;
    {
        motioncam::AudioWriter writer(bytes, 2, 48000, 24, 1, 16);
        writer.write(input, input.size() / 2);
    }
    { std::ofstream out(wav, std::ios::binary); out.write(reinterpret_cast<const char*>(bytes.data()), bytes.size()); }
    const auto flac = (root / "audio.flac").string();
    motioncam::AudioWriter::convertFile(wav, flac, "flac", false);
    assert(decode(flac, AV_CODEC_ID_FLAC) == input);
    const auto copied = (root / "copy.flac").string();
    motioncam::AudioWriter::convertFile(flac, copied, "flac", true);
    assert(decode(copied, AV_CODEC_ID_FLAC) == input);
    for (const auto& source : {wav, flac}) {
        const auto restored = motioncam::AudioWriter::decodeWav(source, 24.0);
        assert(restored && !restored->empty());
        const auto restoredPath = (root / "restored.wav").string();
        { std::ofstream out(restoredPath, std::ios::binary);
          out.write(reinterpret_cast<const char*>(restored->data()), restored->size()); }
        assert(decode(restoredPath, AV_CODEC_ID_PCM_S16LE) == input);
    }

    const auto opus = (root / "audio.opus").string();
    motioncam::AudioWriter::convertFile(wav, opus, "opus", false);
    const auto decodedOpus = decode(opus, AV_CODEC_ID_OPUS);
    assert(decodedOpus.size() >= input.size() && decodedOpus.size() - input.size() < 2000);
    bool cancelled = false;
    try { motioncam::AudioWriter::convertFile(wav, opus, "opus", false, [] { return false; }); }
    catch (const std::runtime_error& e) { cancelled = std::string(e.what()) == "Finalization cancelled"; }
    assert(cancelled);
    int progressCalls = 0;
    cancelled = false;
    try { motioncam::AudioWriter::convertFile(wav, (root / "cancelled.flac").string(), "flac", false,
        [&] { return ++progressCalls < 4; }); }
    catch (const std::runtime_error& e) { cancelled = std::string(e.what()) == "Finalization cancelled"; }
    assert(cancelled && progressCalls == 4);
    bool failed = false;
    try { motioncam::AudioWriter::convertFile(wav, (root / "missing" / "audio.flac").string(), "flac", false); }
    catch (const std::runtime_error&) { failed = true; }
    assert(failed);
}
