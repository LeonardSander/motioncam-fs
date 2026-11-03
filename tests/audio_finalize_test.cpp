#include "AudioWriter.h"
extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
}
#include <cassert>
#include <algorithm>
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

    for (const int64_t offsetUs : {2500, -2500}) {
        auto shifted = bytes;
        motioncam::AudioWriter::offsetWav(shifted, offsetUs);
        const auto path = (root / "shifted.wav").string();
        { std::ofstream out(path, std::ios::binary);
          out.write(reinterpret_cast<const char*>(shifted.data()), shifted.size()); }
        auto expected = input;
        if (offsetUs > 0) expected.insert(expected.begin(), 120 * 2, 0);
        else expected.erase(expected.begin(), expected.begin() + 120 * 2);
        assert(decode(path, AV_CODEC_ID_PCM_S16LE) == expected);
    }

    // Delay float32 stereo without converting its sample values or bit depth.
    {
        std::vector<uint8_t> floating(44 + 16, 0);
        const auto put = [&](size_t at, uint32_t value, unsigned count = 4) {
            for (unsigned b = 0; b < count; ++b) floating[at + b] = uint8_t(value >> (8 * b));
        };
        const auto label = [&](size_t at, const char* text) {
            for (size_t b = 0; b < 4; ++b) floating[at + b] = text[b];
        };
        label(0, "RIFF"); put(4, floating.size() - 8); label(8, "WAVE");
        label(12, "fmt "); put(16, 16); put(20, 3, 2); put(22, 2, 2);
        put(24, 48000); put(28, 48000 * 8); put(32, 8, 2); put(34, 32, 2);
        label(36, "data"); put(40, 16);
        put(44, 0x3eaaaaab); put(48, 0xbeaaaaab); put(52, 0x3f000001); put(56, 0xbf000001);
        const auto original = floating;
        motioncam::AudioWriter::offsetWav(floating, 2500);
        assert(floating[20] == 3 && floating[34] == 32);
        assert(floating.size() == original.size() + 120 * 8);
        assert(std::all_of(floating.begin() + 44, floating.begin() + 44 + 120 * 8,
                           [](uint8_t value) { return value == 0; }));
        assert(std::equal(original.begin() + 44, original.end(), floating.begin() + 44 + 120 * 8));
    }

    // Extensible PCM and float retain their GUID, channel mask, precision,
    // and exact sample bytes when delayed or trimmed.
    for (const unsigned subtype : {1u, 3u}) {
        const unsigned bits = subtype == 1 ? 24 : 32;
        const unsigned alignment = 2 * bits / 8;
        std::vector<uint8_t> extensible(68 + 3 * alignment, 0);
        const auto put = [&](size_t at, uint32_t value, unsigned count = 4) {
            for (unsigned b = 0; b < count; ++b) extensible[at + b] = uint8_t(value >> (8 * b));
        };
        const auto label = [&](size_t at, const char* text) {
            std::copy_n(text, 4, extensible.begin() + at);
        };
        label(0, "RIFF"); put(4, extensible.size() - 8); label(8, "WAVE");
        label(12, "fmt "); put(16, 40); put(20, 0xfffe, 2); put(22, 2, 2);
        put(24, 48000); put(28, 48000 * alignment); put(32, alignment, 2); put(34, bits, 2);
        put(36, 22, 2); put(38, bits, 2); put(40, 3); put(44, subtype);
        const uint8_t tail[] = {0, 0, 0x10, 0, 0x80, 0, 0, 0xaa, 0, 0x38, 0x9b, 0x71};
        std::copy(std::begin(tail), std::end(tail), extensible.begin() + 48);
        label(60, "data"); put(64, 3 * alignment);
        for (size_t i = 68; i < extensible.size(); ++i) extensible[i] = uint8_t(i);
        const auto original = extensible;
        motioncam::AudioWriter::offsetWav(extensible, 2500);
        assert(std::equal(original.begin() + 8, original.begin() + 64, extensible.begin() + 8));
        assert(extensible.size() == original.size() + 120 * alignment);
        assert(std::all_of(extensible.begin() + 68, extensible.begin() + 68 + 120 * alignment,
                           [](uint8_t value) { return value == 0; }));
        assert(std::equal(original.begin() + 68, original.end(), extensible.begin() + 68 + 120 * alignment));
        motioncam::AudioWriter::offsetWav(extensible, -2500);
        assert(extensible == original);
        for (const size_t corruptAt : {size_t{36}, size_t{38}, size_t{44}, size_t{59}}) {
            auto invalid = original;
            invalid[corruptAt] = 0xff;
            bool rejected = false;
            try { motioncam::AudioWriter::offsetWav(invalid, 2500); }
            catch (const std::runtime_error&) { rejected = true; }
            assert(rejected);
        }
    }

    // Odd-sized 8-bit PCM needs unsigned silence and correct RIFF padding;
    // a fact chunk after data must move and retain the new sample count.
    {
        std::vector<uint8_t> mono(60, 0);
        const auto put = [&](size_t at, uint32_t value, unsigned count = 4) {
            for (unsigned b = 0; b < count; ++b) mono[at + b] = uint8_t(value >> (8 * b));
        };
        const auto label = [&](size_t at, const char* text) {
            for (size_t b = 0; b < 4; ++b) mono[at + b] = text[b];
        };
        label(0, "RIFF"); put(4, 52); label(8, "WAVE"); label(12, "fmt "); put(16, 16);
        put(20, 1, 2); put(22, 1, 2); put(24, 1000); put(28, 1000); put(32, 1, 2); put(34, 8, 2);
        label(36, "data"); put(40, 3); mono[44] = 5; mono[45] = 7; mono[46] = 9;
        label(48, "fact"); put(52, 4); put(56, 3);
        auto delayed = mono;
        motioncam::AudioWriter::offsetWav(delayed, 1000);
        assert(delayed.size() == 60 && delayed[40] == 4 && delayed[56] == 4);
        assert(delayed[44] == 128 && delayed[45] == 5 && delayed[46] == 7 && delayed[47] == 9);
        auto trimmed = mono;
        motioncam::AudioWriter::offsetWav(trimmed, -1000);
        assert(trimmed.size() == 58 && trimmed[40] == 2 && trimmed[54] == 2);
        assert(trimmed[44] == 7 && trimmed[45] == 9);
        motioncam::AudioWriter::offsetWav(mono, -1000000);
        assert(mono.size() == 56 && mono[40] == 0 && mono[52] == 0);
        bool rejected = false;
        try { motioncam::AudioWriter::offsetWav(mono, INT64_MAX); }
        catch (const std::runtime_error&) { rejected = true; }
        assert(rejected);
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
