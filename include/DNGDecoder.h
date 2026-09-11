#pragma once

#include <string>
#include <vector>
#include <memory>
#include <optional>
#include <cstdint>
#include <unordered_map>
#include <array>
#include "DNGImage.h"

namespace motioncam {

struct DNGFrameInfo {
    int frameNumber;
    std::string filePath;
    Timestamp timestamp;
    int width;
    int height;
    bool hasGainMap;
    bool hasExactPresentationTimestamp = false;
    bool hasTimeCodeTimestamp = false;
    bool duplicateFrame = false;
    bool syntheticFrame = false;
};

struct DNGSequenceInfo {
    std::string basePath;
    int width = 0;
    int height = 0;
    double fps = 0.0;
    int64_t totalFrames = 0;
    bool hasFrameNumberSequence = false;
};

class DNGDecoder {
public:
    DNGDecoder(const std::string& sequencePath);
    ~DNGDecoder();

    const DNGSequenceInfo& getSequenceInfo() const { return mSequenceInfo; }
    const std::vector<DNGFrameInfo>& getFrames() const { return mFrames; }
    
    bool extractFrame(int frameNumber, std::vector<uint8_t>& dngData);
    bool getGainMap(int frameNumber, GainMap& gainMap);
    bool getFrameMetadata(int frameNumber, DNGFrameMetadata& metadata);
    static bool getColorMetadata(const std::vector<uint8_t>& dngData,
                                 DNGFrameMetadata& metadata);
    static bool setOrientation(std::vector<uint8_t>& dngData, int clockwiseDegrees);
    static bool getGainMaps(const std::vector<uint8_t>& dngData,
                            int opcodeList, std::vector<GainMap>& gainMaps);
    static bool hasOnlySinglePlaneGainMap(const std::vector<uint8_t>& dngData,
                                          int opcodeList);
    static bool replaceGainMaps(std::vector<uint8_t>& dngData,
                                int opcodeList, const std::vector<GainMap>& gainMaps);
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
    static bool ensureUncompressed(std::vector<uint8_t>& dngData,
                                   bool backgroundWork = false);
    static void beginForegroundWork();
    static void endForegroundWork();
    static bool removeThumbnails(std::vector<uint8_t>& dngData);
    static bool overrideDataLevels(std::vector<uint8_t>& dngData,
                                   const std::string& levels);
    static bool packUncompressedToWhiteLevel(std::vector<uint8_t>& dngData);
    static bool applyLogTransform(std::vector<uint8_t>& dngData, LogTransformMode mode,
                                  uint32_t quantizationWhite = 0);
    static bool bakeIsoOverlay(std::vector<uint8_t>& dngData, double iso);
    static bool getImageLayout(const std::vector<uint8_t>& dngData,
                               DNGImageLayout& layout);
    static bool decodeImage(std::vector<uint8_t> dngData,
                            DecodedDNGImage& image,
                            bool backgroundWork = false,
                            bool applyLinearization = true);
    // Writes unpacked samples into an existing DNG metadata template. The
    // template's image topology must match; container canonicalization and
    // endian encoding are handled here.
    static bool encodeImage(std::vector<uint8_t>& dngTemplate,
                            const DecodedDNGImage& image);
    static bool decodePreview(std::vector<uint8_t> dngData,
                              const RenderSettings& settings,
                              PreviewFrame& frame,
                              bool applyPreviewScale = false);
    static bool replaceNormalizedRGB16(std::vector<uint8_t>& dngData,
                                       const std::vector<uint8_t>& rgbData,
                                       uint32_t width, uint32_t height);
    static bool markSyntheticFrame(std::vector<uint8_t>& dngData);
    static bool markDuplicateFrame(std::vector<uint8_t>& dngData);
    static bool isSyntheticFrame(const std::vector<uint8_t>& dngData);
    static bool isDuplicateFrame(const std::vector<uint8_t>& dngData);
    static bool interpolateFrameMetadata(std::vector<uint8_t>& dngData,
                                         const std::vector<uint8_t>& leftDng,
                                         const std::vector<uint8_t>& rightDng,
                                         double ratio);
    static bool getCFAMetadata(const std::vector<uint8_t>& dngData,
                               int& repeatSize, std::array<uint8_t, 4>& phase);
    static bool compressJPEGXL(std::vector<uint8_t>& dngData, float distance);
    static bool compressLossyJPEG(std::vector<uint8_t>& dngData, int quality = 90);
    static bool compressLosslessJPEG(std::vector<uint8_t>& dngData);
    static bool bakeGainMaps(std::vector<uint8_t>& dngData,
                             bool normalizeGainMaps,
                             bool colorOnly,
                             bool optimizeGainMaps = false,
                             bool debugGainMap = false,
                             int cfaRepeatSizeOverride = 0);
    static bool transformGainMaps(std::vector<uint8_t>& dngData,
                                  bool normalizeGainMaps,
                                  bool colorOnly,
                                  bool optimizeGainMaps);
    static bool canonicalizeGainMapOpcodes(std::vector<uint8_t>& dngData);
    static bool repairGainMapCfaPhase(
        std::vector<uint8_t>& dngData,
        std::optional<bool> sidecarOverride = std::nullopt);
    static bool cropGainMapsToFullSensor(
        std::vector<uint8_t>& dngData, uint32_t fullWidth, uint32_t fullHeight);
    static bool processHigherCFA(std::vector<uint8_t>& dngData,
                                 int repeatSize,
                                 const std::array<uint8_t, 4>& phase,
                                 QuadBayerMode mode,
                                 bool remosaic,
                                 int proxyScale = 1,
                                 bool higherCfaHq = true,
                                 bool nearestNeighborDemosaic = false,
                                 bool topologyOnly = false);
    
    static bool isDNGSequence(const std::string& path);
    static bool imagePayloadsEqual(const std::vector<uint8_t>& left,
                                   const std::vector<uint8_t>& right);
    static bool imagePayloadHash(const std::vector<uint8_t>& data, uint64_t& hash);

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
