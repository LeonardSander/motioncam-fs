#pragma once

#include <string>
#include <vector>
#include <memory>
#include <cstdint>
#include <unordered_map>
#include <array>

namespace motioncam {

typedef int64_t Timestamp;

struct DNGFrameInfo {
    int frameNumber;
    std::string filePath;
    Timestamp timestamp;
    int width;
    int height;
    bool hasGainMap;
};

struct DNGSequenceInfo {
    std::string basePath;
    int width;
    int height;
    double fps;
    int64_t totalFrames;
};

struct DNGFrameMetadata {
    double exposureTime = 0.0;
    double iso = 0.0;
    double baselineExposure = 0.0;
    std::array<float, 3> asShotNeutral = {1.0f, 1.0f, 1.0f};
    bool hasExposure = false;
    bool hasBaselineExposure = false;
    bool hasAsShotNeutral = false;
};

struct GainMap {
    float top, left, bottom, right;
    uint32_t width, height;
    int channels;
    std::vector<float> data;
};

class DNGDecoder {
public:
    DNGDecoder(const std::string& sequencePath);
    ~DNGDecoder();

    const DNGSequenceInfo& getSequenceInfo() const { return mSequenceInfo; }
    const std::vector<DNGFrameInfo>& getFrames() const { return mFrames; }
    
    bool extractFrame(int frameNumber, std::vector<uint8_t>& dngData);
    bool extractFrameByTimestamp(Timestamp timestamp, std::vector<uint8_t>& dngData);
    bool getGainMap(int frameNumber, GainMap& gainMap);
    bool getFrameMetadata(int frameNumber, DNGFrameMetadata& metadata);
    static bool updateMetadata(std::vector<uint8_t>& dngData,
                               const double* baselineExposure,
                               const std::array<float, 3>* asShotNeutral);
    
    static bool isDNGSequence(const std::string& path);

private:
    void analyzeSequence();
    void findDNGFiles();
    void extractTimestampsFromFilenames();
    bool readDNGFile(const std::string& filePath, std::vector<uint8_t>& data);
    bool readDNGGainMap(const std::string& dngPath, GainMap& gainMap);
    bool parseOpcodeGainMap(const uint8_t* opcodeData, size_t opcodeSize, GainMap& gainMap);

private:
    std::string mSequencePath;
    DNGSequenceInfo mSequenceInfo;
    std::vector<DNGFrameInfo> mFrames;
    std::unordered_map<int, GainMap> mGainMapCache;
};

} // namespace motioncam
