#pragma once

#include <string>
#include <vector>
#include <cstdint>
#include <functional>
#include <memory>
#include <array>
#include <map>
#include <nlohmann/json_fwd.hpp>

namespace BS { class thread_pool; }
#include "Types.h"
#include "IVirtualFileSystem.h"

namespace motioncam {

struct AudioChunk;
struct GainMap;
struct CalibrationData;
class LRUCache;

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
    int width;
    int height;
    std::string dataType;        // "Bayer CFA", "Quad Bayer CFA", or "RGB"
    std::string levelsInfo;      // e.g., "1023/64 -> 1023/0 10b"
    float runtimeSeconds;        // Runtime in seconds based on audio track
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

std::vector<Entry> filterEntries(
    const std::vector<Entry>& entries, const std::string& filter);

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

nlohmann::json loadSidecarMetadataFile(const boost::filesystem::path& path);

boost::filesystem::path sidecarPath(const std::string& sourcePath);

void loadSidecar(
    const boost::filesystem::path& path,
    nlohmann::json& metadata,
    std::optional<CalibrationData>& calibration);

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
    bool applyShadingMap, bool normalizeShadingMap);

} // namespace vfs
} // namespace motioncam
