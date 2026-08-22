#pragma once

#include <IVirtualFileSystem.h>
#include <IFuseFileSystem.h>
#include <CalibrationData.h>
#include <VirtualFileSystemImpl.h>
#include <memory>
#include <array>
#include <map>
#include <shared_mutex>
#include <unordered_map>
#include <nlohmann/json.hpp>

namespace BS {
class thread_pool;
}

namespace motioncam {

class LRUCache;
class DNGDecoder;

class VirtualFileSystemImpl_DNG : public IVirtualFileSystem
{
public:
    VirtualFileSystemImpl_DNG(
        BS::thread_pool& ioThreadPool,
        BS::thread_pool& processingThreadPool,
        LRUCache& lruCache,
        const RenderSettings& config,
        const std::string& file,
        const std::string& baseName);

    ~VirtualFileSystemImpl_DNG();

    std::vector<Entry> listFiles(const std::string& filter = "") const override;
    std::optional<Entry> findEntry(const std::string& fullPath) const override;

    int readFile(
        const Entry& entry,
        const size_t pos,
        const size_t len,
        void* dst,
        std::function<void(size_t, int)> result,
        bool async=true) override;

    void updateOptions(const RenderSettings& config) override;
    FileInfo getFileInfo() const override;
    bool sourceImagePayloadsEqual(const Entry& left, const Entry& right) override;
    std::shared_ptr<std::vector<char>> materializeFile(
        const Entry& entry, bool jpegCompression = false) override;

private:
    void init();
    
    void calculateFrameRateStats();
    std::vector<uint8_t> transformFrame(
        size_t frameIndex, Timestamp outputTimestamp, bool jpegCompression);

private:
    LRUCache& mCache;
    BS::thread_pool& mIoThreadPool;
    BS::thread_pool& mProcessingThreadPool;
    const std::string mSrcPath;
    const std::string mBaseName;
    size_t mTypicalDngSize;
    std::vector<Entry> mFiles;
    RenderSettings mConfig;
    float mFps;
    FrameRateInfo mFrameRateInfo{};
    int mTotalFrames;
    int mDroppedFrames;
    int mDuplicatedFrames;
    int mWidth;
    int mHeight;
    float mSourceWhiteLevel = 65535.0f;
    std::array<float, 4> mSourceBlackLevel{};
    bool mSourceHasGainMap = false;
    bool mHasCfa = false;
    int mCfaSize = 2;
    std::array<uint8_t, 4> mCfaPhase = {0, 1, 1, 2};
    std::unique_ptr<DNGDecoder> mDecoder;
    std::optional<CalibrationData> mCalibration;
    nlohmann::json mSidecarMetadata;
    std::map<Timestamp, float> mNormalizedExposureOffsets;
    std::map<Timestamp, float> mSmoothedExposureOffsets;
    std::map<Timestamp, std::array<float, 3>> mSmoothedAsShotNeutrals;
    std::map<Timestamp, bool> mHasBaselineExposure;
    std::map<Timestamp, bool> mHasAsShotNeutral;
    std::map<Timestamp, double> mExposureTimes;
    std::map<Timestamp, double> mIsoValues;
    mutable std::mutex mMutex;
    mutable std::mutex mPayloadHashMutex;
    mutable std::shared_mutex mRenderMutex;
    std::unordered_map<size_t, uint64_t> mPayloadHashes;
    std::unordered_map<Timestamp, size_t> mFrameIndexByTimestamp;
};

} // namespace motioncam
