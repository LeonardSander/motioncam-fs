#pragma once

#include <cstdint>
#include <vector>
#include <memory>
#include <functional>
#include <string>
#include <bw64/bw64.hpp>

namespace bw64 {
    class Bw64Writer;
}

namespace rawpreamp {
    class AudioWriter {
    public:
        AudioWriter(std::vector<uint8_t>& output, int numChannels, int sampleRate, int fpsNum, int fpsDen, int bitDepth);

        void write(const std::vector<int16_t>& data, int numFrames);
        void write(const std::vector<float>& data, int numFrames);
        
        static std::shared_ptr<std::vector<uint8_t>> decodeWav(const std::string& source, double fps);

        // Positive offsets delay the track with silence; negative offsets trim its start.
        // PCM and float samples retain their original precision.
        static void offsetWav(std::vector<uint8_t>& bytes, int64_t offsetUs);

        // Extracts the original audio packets, preserving their codec and precision.
        // Returns the staged filename, or an empty string if no audio is present.
        static std::string extractOriginalTrack(const std::string& source,
            const std::string& outputBase);

        static void convertFile(const std::string& source, const std::string& destination,
                                const std::string& codec, bool copy,
                                const std::function<bool()>& keepGoing = {});

    private:
        const int mFd;
        std::unique_ptr<bw64::Bw64Writer> mWriter;
    };
}
