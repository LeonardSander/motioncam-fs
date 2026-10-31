#pragma once

#include <vector>
#include <memory>
#include <functional>
#include <string>
#include <bw64/bw64.hpp>

namespace bw64 {
    class Bw64Writer;
}

namespace motioncam {
    class AudioWriter {
    public:
        AudioWriter(std::vector<uint8_t>& output, int numChannels, int sampleRate, int fpsNum, int fpsDen, int bitDepth);

        void write(const std::vector<int16_t>& data, int numFrames);
        void write(const std::vector<float>& data, int numFrames);
        
        static std::shared_ptr<std::vector<uint8_t>> decodeWav(const std::string& source, double fps);

        static void convertFile(const std::string& source, const std::string& destination,
                                const std::string& codec, bool copy,
                                const std::function<bool()>& keepGoing = {});

    private:
        const int mFd;
        std::unique_ptr<bw64::Bw64Writer> mWriter;
    };
}
