#pragma once

#include <string>
#include <vector>
#include <memory>
#include <cstdint>
#include <unordered_map>
#include <array>
#include "Types.h"

namespace motioncam {

typedef int64_t Timestamp;

struct DNGFrameInfo {
    int frameNumber;
    std::string filePath;
    Timestamp timestamp;
    int width;
    int height;
    bool hasGainMap;
    bool hasExactPresentationTimestamp = false;
    bool hasTimeCodeTimestamp = false;
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
    std::array<float, 4> blackLevel{};
    std::array<float, 4> whiteLevel{};
    uint32_t blackLevelCount = 0;
    uint32_t whiteLevelCount = 0;
    std::array<float, 9> colorMatrix1{};
    std::array<float, 9> colorMatrix2{};
    std::array<float, 9> forwardMatrix1{};
    std::array<float, 9> forwardMatrix2{};
    bool hasExposure = false;
    bool hasBaselineExposure = false;
    bool hasAsShotNeutral = false;
    bool hasColorMatrix1 = false;
    bool hasColorMatrix2 = false;
    bool hasForwardMatrix1 = false;
    bool hasForwardMatrix2 = false;
};

struct GainMap {
    uint32_t top, left, bottom, right;
    uint32_t coordinateWidth = 0, coordinateHeight = 0;
    uint32_t plane, planes;
    uint32_t rowPitch, colPitch;
    uint32_t width, height, channels;
    double spacingV, spacingH, originV, originH;
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
    bool getGainMaps(int frameNumber, std::vector<GainMap>& gainMaps);
    bool getFrameMetadata(int frameNumber, DNGFrameMetadata& metadata);
    static bool getColorMetadata(const std::vector<uint8_t>& dngData,
                                 DNGFrameMetadata& metadata);
    static bool getGainMaps(const std::vector<uint8_t>& dngData,
                            int opcodeList, std::vector<GainMap>& gainMaps);
    bool getCFAMetadata(int frameNumber, int& repeatSize, std::array<uint8_t, 4>& phase);
    static bool updateMetadata(std::vector<uint8_t>& dngData,
                               const double* baselineExposure,
                               const std::array<float, 3>* asShotNeutral);
    static bool setTimingMetadata(std::vector<uint8_t>& dngData,
                                  double frameRate,
                                  Timestamp timestampNs);
    static bool getTimingMetadata(const std::vector<uint8_t>& dngData,
                                  Timestamp& timestampNs);
    static bool repairExposureTime(std::vector<uint8_t>& dngData, double exposureTime);
    static bool ensureUncompressed(std::vector<uint8_t>& dngData);
    static bool overrideDataLevels(std::vector<uint8_t>& dngData,
                                   const std::string& levels);
    static bool packUncompressedToWhiteLevel(std::vector<uint8_t>& dngData);
    static bool bakeIsoOverlay(std::vector<uint8_t>& dngData, double iso);
    static bool extractUncompressedRGB16(const std::vector<uint8_t>& dngData,
                                         std::vector<uint8_t>& rgbData,
                                         uint32_t& width,
                                         uint32_t& height);
    static bool replaceUncompressedRGB16(std::vector<uint8_t>& dngData,
                                         const std::vector<uint8_t>& rgbData,
                                         uint32_t width, uint32_t height);
    static bool markSyntheticFrame(std::vector<uint8_t>& dngData);
    static bool interpolateFrameMetadata(std::vector<uint8_t>& dngData,
                                         const std::vector<uint8_t>& leftDng,
                                         const std::vector<uint8_t>& rightDng,
                                         double ratio);
    static bool getCFAMetadata(const std::vector<uint8_t>& dngData,
                               int& repeatSize, std::array<uint8_t, 4>& phase);
    static bool compressJPEGXL(std::vector<uint8_t>& dngData, float distance);
    static bool compressLosslessJPEG(std::vector<uint8_t>& dngData);
    static bool bakeGainMaps(std::vector<uint8_t>& dngData,
                             bool normalizeGainMaps,
                             bool colorOnly,
                             bool optimizeGainMaps = false);
    static bool transformGainMaps(std::vector<uint8_t>& dngData,
                                  bool normalizeGainMaps,
                                  bool colorOnly,
                                  bool optimizeGainMaps);
    static bool processHigherCFA(std::vector<uint8_t>& dngData,
                                 int repeatSize,
                                 const std::array<uint8_t, 4>& phase,
                                 QuadBayerMode mode,
                                 bool remosaic,
                                 int proxyScale = 1,
                                 bool higherCfaHq = true);
    
    static bool isDNGSequence(const std::string& path);

private:
    void analyzeSequence();
    void findDNGFiles();
    void extractTimestampsFromFilenames();
    bool readDNGFile(const std::string& filePath, std::vector<uint8_t>& data);
    bool readDNGGainMap(const std::string& dngPath, GainMap& gainMap);
    static bool parseOpcodeGainMaps(const uint8_t* opcodeData, size_t opcodeSize,
                                    std::vector<GainMap>& gainMaps);

private:
    std::string mSequencePath;
    DNGSequenceInfo mSequenceInfo;
    std::vector<DNGFrameInfo> mFrames;
    std::unordered_map<int, GainMap> mGainMapCache;
};

} // namespace motioncam
