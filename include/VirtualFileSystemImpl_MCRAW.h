#pragma once

#include <IVirtualFileSystem.h>
#include <IFuseFileSystem.h>
#include <CalibrationData.h>
#include <VirtualFileSystemImpl.h>
#include <array>
#include <map>
#include <nlohmann/json.hpp>
#include <shared_mutex>
#include <unordered_map>

namespace BS {
class thread_pool;
}

namespace motioncam {

class Decoder;
class LRUCache;

class VirtualFileSystemImpl_MCRAW : public MountedDngSource
{
public:
    VirtualFileSystemImpl_MCRAW(
        BS::thread_pool& ioThreadPool,
        BS::thread_pool& processingThreadPool,
        LRUCache& lruCache,
        const RenderSettings& settings,
        const std::string& file,
        const std::string& baseName);

    ~VirtualFileSystemImpl_MCRAW();

    static std::pair<uintmax_t, size_t> analysisCacheUsage();
    static void clearAnalysisCache();

    void updateOptions(const RenderSettings& settings) override;
    FileInfo getFileInfo() const override;
    std::shared_ptr<std::vector<char>> materializeFile(
        const Entry& entry, bool jpegCompression = false) override;
    bool materializePreviewFrame(const Entry& entry, PreviewFrame& frame) override;

private:
    int readPriority(const Entry& entry) const override;
    std::function<std::shared_ptr<std::vector<char>>()>
        staticMaterializer(const Entry& entry) override;
    void init();

    void attachSidecarGainMapOpcodes(
        std::vector<uint8_t>& dng, size_t frameIndex) const;

private:
    const std::string mSrcPath;
    const std::string mBaseName;
    const boost::filesystem::path mSidecarPath;
    const boost::filesystem::path mGyroflowSidecarPath;
    size_t mTypicalDngSize;
    std::vector<Timestamp> mSourceFrames;
    std::unordered_map<Timestamp, size_t> mFrameIndexByTimestamp;
    std::vector<uint8_t> mAudioFile;
    RenderSettings mSettings;
    float mFps;
    FrameRateInfo mFrameRateInfo;
    FileInfo mFileInfo;
    double mBaselineExpValue;
    std::map<Timestamp, float> mSmoothedExposureOffsets;
    std::map<Timestamp, std::array<float, 3>> mSmoothedAsShotNeutrals;
    std::optional<CalibrationData> mCalibration;
    std::optional<GyroflowLensProfile> mGyroflowLensProfile;
    nlohmann::json mSidecarMetadata;
    vfs::ManualVignetteSidecars mManualVignetteSidecars;
    mutable std::shared_mutex mRenderMutex;
};

} // namespace motioncam
