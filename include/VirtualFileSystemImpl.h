#pragma once

#include <string>
#include <string_view>
#include <vector>
#include <cstdint>
#include <functional>
#include <memory>
#include <array>
#include <map>
#include <mutex>
#include <unordered_map>
#include <memory>
#include <nlohmann/json_fwd.hpp>

namespace BS { class thread_pool; }
#include "Types.h"
#include "DNGImage.h"
#include "IVirtualFileSystem.h"

namespace motioncam {

struct AudioChunk;
struct GainMap;
struct CalibrationData;
class LRUCache;

struct GyroflowLensProfile {
    int width = 0;
    int height = 0;
    double fx = 0.0;
    double fy = 0.0;
    double cx = 0.0;
    double cy = 0.0;
    std::array<double, 4> distortion{};
    std::array<double, 4> dngRectilinear{};
    double rectilinearRmsPixels = 0.0;
    double rectilinearMaxPixels = 0.0;
};

// Owns the expensive, source-specific preview decoder and its private cache.
// The renderer is reused while settings remain unchanged, so player seeks do
// not repeatedly scan the source and rebuild timing/exposure analysis.
class PreviewRenderer {
public:
    PreviewRenderer(BS::thread_pool& ioThreadPool,
                    BS::thread_pool& processingThreadPool,
                    std::string source, std::string baseName);

    void render(
        const RenderSettings& settings,
        const PreviewOptions& options,
        const std::function<bool(size_t, size_t, const std::string&)>& progress,
        const std::function<void(PreviewFrame&&)>& frameReady);

private:
    struct State;
    BS::thread_pool& mIoThreadPool;
    BS::thread_pool& mProcessingThreadPool;
    std::string mSource;
    std::string mBaseName;
    std::mutex mMutex;
    std::shared_ptr<State> mState;
};

std::unique_ptr<IVirtualFileSystem> createVirtualFileSystem(
    BS::thread_pool& ioThreadPool, BS::thread_pool& processingThreadPool,
    LRUCache& cache, const RenderSettings& settings,
    const std::string& source, const std::string& baseName);

// Shared mounted-DNG shell for every source format. Format implementations
// only render frames; lookup, cache/thread-pool reads, and entry storage are
// deliberately identical across ingest paths.
class MountedDngSource : public IVirtualFileSystem {
public:
    MountedDngSource(LRUCache& cache, BS::thread_pool& processingThreadPool);

    std::vector<Entry> listFiles(const std::string& filter = "") const override;
    std::optional<Entry> findEntry(const std::string& fullPath) const override;
    int readFile(const Entry& entry, size_t pos, size_t len, void* dst,
                 std::function<void(size_t, int)> result,
                 bool async = true) override;

protected:
    virtual int readPriority(const Entry& entry) const;
    virtual std::function<std::shared_ptr<std::vector<char>>()>
        staticMaterializer(const Entry& entry);

    LRUCache& mCache;
    BS::thread_pool& mProcessingThreadPool;
    std::vector<Entry> mFiles;
    mutable std::mutex mMutex;
};

struct FrameRateInfo {
    float minFrameRate;
    float lowerQuartileFrameRate;  // 25th percentile
    float medianFrameRate;         // 50th percentile
    float upperQuartileFrameRate;  // 75th percentile
    float maxFrameRate;
    float averageFrameRate;
};

struct FileInfo {
    FrameRateInfo frameRateInfo;
    float fps;
    int totalFrames;
    int droppedFrames;
    int duplicatedFrames;
    // Output-frame mask used by the gallery timeline. This follows the CFR
    // projection, so inserted holds line up with the player's frame numbers.
    std::shared_ptr<const std::vector<bool>> duplicateFrameMask;
    // Gallery source-frame model. sourceFrameToOutput contains -1 for frames
    // removed by CFR conversion. sourceFrameDuplicated marks a source frame
    // which also supplies one or more inserted hold frames.
    std::shared_ptr<const std::vector<int>> sourceFrameToOutput;
    std::shared_ptr<const std::vector<bool>> sourceFrameDuplicated;
    int width;
    int height;
    // Clockwise display rotation derived from source metadata. Mirrored TIFF
    // orientations currently retain only their rotational component.
    int orientation = -1;
    std::string dataType;        // "Bayer CFA", "Quad Bayer CFA", or "RGB"
    std::string levelsInfo;      // e.g., "1023/64 -> 1023/0 10b"
    float runtimeSeconds;        // Runtime in seconds based on audio track
    std::shared_ptr<const std::vector<uint8_t>> audioWav;
    // Native presentation timestamps for frame-timing visualization. The
    // timestamp unit is timingTimeBaseNum / timingTimeBaseDen seconds.
    std::shared_ptr<const std::vector<std::int64_t>> presentationTimestamps;
    int timingTimeBaseNum = 0;
    int timingTimeBaseDen = 0;
    bool timingUsesCfrMapping = false;
    // Sidecar validation is performed while constructing the mounted source.
    // Expose that result so UI status checks do not parse large sidecars again.
    // -1 means the source implementation does not report sidecar state.
    int sidecarState = -1; // 0 absent, 1 valid, 2 invalid
    bool hasIgnoreForwardMatOverride = false;
    bool ignoreForwardMatOverride = false;
    // False for independent DNG still collections. Such folders may contain
    // mixed dimensions and must not advance into the next mounted clip.
    bool isSequence = true;
};

namespace vfs {

struct ExposureSample {
    Timestamp timestamp = 0;
    double iso = 0.0;
    double exposureSeconds = 0.0;
    double baselineExposure = 0.0;
    std::array<float, 3> asShotNeutral{1.0f, 1.0f, 1.0f};
};

struct ExposureAnalysis {
    std::map<Timestamp, float> normalizedBaseline;
    std::map<Timestamp, float> smoothedBaseline;
    std::map<Timestamp, std::array<float, 3>> smoothedNeutral;
};

ExposureAnalysis analyzeExposureMetadata(
    const std::vector<ExposureSample>& samples, float frameRate);

std::vector<Entry> mapFramesToCfr(
    const std::vector<Entry>& sourceEntries,
    const std::vector<Timestamp>& timestamps,
    const std::string& baseName,
    float frameRate,
    bool convert,
    int& droppedFrames,
    int& duplicatedFrames);

void buildGalleryFrameMap(
    const std::vector<Timestamp>& sourceTimestamps,
    const std::vector<Entry>& mappedEntries,
    std::shared_ptr<const std::vector<int>>& sourceFrameToOutput,
    std::shared_ptr<const std::vector<bool>>& sourceFrameDuplicated);

std::vector<Entry> filterEntries(
    const std::vector<Entry>& entries, const std::string& filter);

// Common tail of every ingest path once it has produced a valid DNG. Keeping
// container timing, sample packing, lens metadata and compression here avoids
// each source subtly inventing its own output order.
struct DngFinalizeOptions {
    float frameRate = 0.0f;
    Timestamp timestamp = 0;
    bool writeTiming = true;
    std::optional<double> isoOverlay;
    bool packToWhiteLevel = false;
    bool compression = false;
    const GyroflowLensProfile* gyroflowLensProfile = nullptr;
    std::string_view sourceName;
};

struct DngRenderPlan {
    int outputFrameNumber = 0;
    Timestamp outputTimestamp = 0;
    int scale = 1;
    bool nativeMetadataFrame = false;
};

DngRenderPlan planDngRender(const Entry& entry, Timestamp sourceTimestamp,
                            Timestamp firstSourceTimestamp,
                            const RenderSettings& settings, float frameRate,
                            bool finalizing, bool numberedSequence = true);

struct DngPixelPipelineOptions {
    int cfaRepeatSize = 2;
    std::array<uint8_t, 4> cfaPhase{0, 1, 1, 2};
    bool hasCfa = false;
    int outputScale = 1;
    uint32_t inputQuantizationWhite = 0;
    // Non-zero for sources which enter this pipeline as linear samples but
    // use KeepInput as a source-specific log ceiling (DirectLog: 12 bits).
    uint32_t linearInputBitDepth = 0;
    const CalibrationData* calibration = nullptr;
    double iso = 0.0;
    double exposureTime = 0.0;
    std::string_view sourceName;
};

size_t projectedDngSize(uint32_t width, uint32_t height, uint32_t channels,
                        uint32_t storedBits, size_t measuredMetadataBytes,
                        size_t transformedMetadataAllowance = 256 * 1024);
size_t projectedGainMapMetadataSize(const std::vector<GainMap>& maps);
size_t projectedSidecarMetadataSize(const nlohmann::json& sidecar);
size_t projectedBadPixelOpcodeSize(const CalibrationData& calibration,
                                   uint32_t sensorWidth, uint32_t sensorHeight);

// Manual flat-field DNGs discovered beside a clip.  The implementation keeps
// the decoded flats in memory, but creates/replaces opcodes on the frame's
// native geometry so all ordinary gain-map processing remains downstream.
struct ManualVignetteSidecars {
    struct Cache {
        std::mutex mutex;
        std::unordered_map<std::string, std::vector<GainMap>> convertedWhiteMaps;
    };
    struct Candidate {
        std::string path;
        std::string illuminant;
        bool whiteImage = false;
        DecodedDNGImage image;
        std::vector<DNGSidecarMetadataEntry> metadata;
    };
    std::vector<Candidate> candidates;
    std::shared_ptr<Cache> cache = std::make_shared<Cache>();
};

ManualVignetteSidecars loadManualVignetteSidecars(
    const std::string& sourcePath, const nlohmann::json* sidecar = nullptr,
    const boost::filesystem::path* sidecarFile = nullptr);
bool applyManualVignetteSidecar(std::vector<uint8_t>& dng,
                                const ManualVignetteSidecars& sidecars);
bool manualVignetteSidecarGainMaps(
    const ManualVignetteSidecars& sidecars, const DNGImageLayout& targetLayout,
    std::vector<GainMap>& opcodeList2, std::vector<GainMap>& opcodeList3);
bool applyManualDngMetadata(std::vector<uint8_t>& dng,
                            const ManualVignetteSidecars& sidecars,
                            const CalibrationData* jsonOverride);
void mergeManualDngMetadata(DNGFrameMetadata& metadata,
                            const ManualVignetteSidecars& sidecars,
                            const CalibrationData* jsonOverride);
std::array<int, 2> manualVignetteSensorResolution(
    const ManualVignetteSidecars& sidecars);

void processDngPixels(std::vector<uint8_t>& dng,
                      const RenderSettings& settings,
                      const DngPixelPipelineOptions& options);
bool decodeProcessedDngPreview(
    const std::shared_ptr<std::vector<char>>& dng, PreviewFrame& preview,
    bool gainMapApplied = false);

void finalizeDng(std::vector<uint8_t>& dng, const RenderSettings& settings,
                 const DngFinalizeOptions& options);

std::optional<Entry> findEntry(
    const std::vector<Entry>& entries, const std::string& fullPath);

int outputFrameNumber(const Entry& entry);

Timestamp outputTimestamp(
    const Entry& entry,
    Timestamp sourceTimestamp,
    Timestamp firstSourceTimestamp,
    float frameRate,
    bool converted);

float configuredExposureOffset(const RenderSettings& settings);

struct CameraIdentity {
    std::string uniqueModel;
    std::string make;
    std::string model;
};

CameraIdentity resolveCameraIdentity(
    const std::string& configuredModel, const std::string& fallbackModel);

void appendDesktopIni(std::vector<Entry>& entries);

std::optional<int> readDesktopIni(
    const Entry& entry, size_t pos, size_t len, void* dst,
    const std::function<void(size_t, int)>& result);

std::vector<GainMap> loadSidecarGainMaps(
    const nlohmann::json& sidecar, size_t frameNumber, const char* field);

bool hasSidecarGainMaps(
    const nlohmann::json& sidecar, size_t frameNumber, const char* field);

void replaceSidecarGainMapOpcodes(
    std::vector<uint8_t>& dng, const nlohmann::json& sidecar,
    size_t frameNumber, bool replaceList2 = true, bool replaceList3 = true);

nlohmann::json loadSidecarMetadataFile(const boost::filesystem::path& path);

boost::filesystem::path sidecarPath(const std::string& sourcePath);

boost::filesystem::path gyroflowSidecarPath(const std::string& sourcePath);

boost::filesystem::path referencedSidecarPath(
    const boost::filesystem::path& discoveredPath,
    const nlohmann::json& sidecar,
    const boost::filesystem::path& sidecarFile,
    const char* field);

std::optional<GyroflowLensProfile> loadGyroflowLensProfile(
    const boost::filesystem::path& path, bool refresh = false);

void applyGyroflowLensProfile(
    std::vector<uint8_t>& dng, const GyroflowLensProfile& profile);

void loadSidecar(
    const boost::filesystem::path& path,
    nlohmann::json& metadata,
    std::optional<CalibrationData>& calibration,
    bool refresh = false);

std::shared_ptr<std::vector<char>> materializeCached(
    LRUCache& cache, const Entry& entry, bool bypassCache,
    const std::function<std::shared_ptr<std::vector<char>>()>& renderer);

int readMountedEntry(
    const Entry& entry, size_t pos, size_t len, void* dst,
    const std::function<void(size_t, int)>& result, bool async,
    BS::thread_pool& processingThreadPool,
    const std::function<std::shared_ptr<std::vector<char>>()>& materializer,
    const std::function<std::shared_ptr<std::vector<char>>()>& staticMaterializer = {},
    int priority = 0);

void finalize(
    IVirtualFileSystem& filesystem,
    const std::string& destination,
    bool jpegCompression,
    const FinalizeOptions& options,
    const std::function<bool(size_t, size_t, const std::string&)>& progress,
    const std::function<void(const std::vector<uint8_t>&, Timestamp)>& fileReady = {},
    bool writeFiles = true);

FrameRateInfo calculateFrameRate(const std::vector<Timestamp>& frames);

FileInfo makeFileInfo(
    const FrameRateInfo& frameRateInfo, float fps, int totalFrames,
    int droppedFrames, int duplicatedFrames, int width, int height);

std::unordered_map<Timestamp, size_t> indexTimestamps(
    const std::vector<Timestamp>& timestamps);

// CFR (Constant Frame Rate) conversion
float determineCFRTarget(
    FrameRateInfo fpsInfo,
    const CFRTarget& cfrTarget,
    bool applyCFRConversion);

int getFrameNumberFromTimestamp(
    Timestamp timestamp,
    Timestamp referenceTimestamp,
    float frameRate);

// File naming utilities
std::string constructFrameFilename(
    const std::string& baseName,
    int frameNumber,
    int padding = 6,
    const std::string& extension = "");

std::string extractFilenameWithoutExtension(const std::string& fullPath);

// Render options utilities
int getScaleFromOptions(FileRenderOptions options, int draftScale);

// Platform-specific constants
#ifdef _WIN32
constexpr std::string_view DESKTOP_INI = R"([.ShellClassInfo]
ConfirmFileOp=0

[ViewState]
Mode=4
Vid={137E7700-3573-11CF-AE69-08002B2E1262}
FolderType=Generic

[{5984FFE0-28D4-11CF-AE66-08002B2E1262}]
Mode=4
LogicalViewMode=1
IconSize=16

[LocalizedFileNames]
)";
#endif

// Audio synchronization
void syncAudio(
    Timestamp videoTimestamp,
    std::vector<AudioChunk>& audioChunks,
    int sampleRate,
    int numChannels);

std::string getDisplayDataType(bool sourceRgb, int cfaSize);

std::string getDisplayDataLevels(
    float dynWhiteLevel, std::array<float, 4> dynBlackLevel, 
    float statWhiteLevel, std::array<float, 4> statBlackLevel, 
    std::string levels, std::string logTransform,
    bool applyShadingMap, bool normalizeShadingMap,
    uint32_t inputBitDepth = 0);

} // namespace vfs
} // namespace motioncam
