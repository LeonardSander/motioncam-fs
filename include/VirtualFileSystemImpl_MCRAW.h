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

class VirtualFileSystemImpl_MCRAW : public IVirtualFileSystem
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

    std::vector<Entry> listFiles(const std::string& filter = "") const override;
    std::optional<Entry> findEntry(const std::string& fullPath) const override;

    int readFile(
        const Entry& entry,
        const size_t pos,
        const size_t len,
        void* dst,
        std::function<void(size_t, int)> result,
        bool async=true) override;

    void updateOptions(const RenderSettings& settings) override;
    FileInfo getFileInfo() const override;
    std::shared_ptr<std::vector<char>> materializeFile(
        const Entry& entry, bool jpegCompression = false) override;

private:
    void init();

    void applySidecarGainMapOpcodes(std::vector<uint8_t>& dng, size_t frameIndex) const;

private:
    LRUCache& mCache;
    BS::thread_pool& mIoThreadPool;
    BS::thread_pool& mProcessingThreadPool;
    const std::string mSrcPath;
    const std::string mBaseName;
    size_t mTypicalDngSize;
    std::vector<Entry> mFiles;
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
    nlohmann::json mSidecarMetadata;
    mutable std::mutex mMutex;
    mutable std::shared_mutex mRenderMutex;
};

} // namespace motioncam
