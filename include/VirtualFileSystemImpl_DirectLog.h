#pragma once

#include <IVirtualFileSystem.h>
#include <IFuseFileSystem.h>
#include <VirtualFileSystemImpl.h>
#include <CalibrationData.h>
#include <DNGDecoder.h>
#include <Types.h>
#include <nlohmann/json.hpp>
#include <memory>

namespace BS {
class thread_pool;
}

namespace motioncam {

class LRUCache;
class DirectLogDecoder;

class VirtualFileSystemImpl_DirectLog : public IVirtualFileSystem
{
public:
    VirtualFileSystemImpl_DirectLog(
        BS::thread_pool& ioThreadPool,
        BS::thread_pool& processingThreadPool,
        LRUCache& lruCache,
        const RenderSettings& config,
        const std::string& file,
        const std::string& baseName);

    ~VirtualFileSystemImpl_DirectLog();

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
    std::shared_ptr<std::vector<char>> materializeFile(
        const Entry& entry, bool jpegCompression = false) override;

private:
    void init();
    
    size_t generateFrame(
        const Entry& entry,
        const size_t pos,
        const size_t len,
        void* dst,
        std::function<void(size_t, int)> result,
        bool async);

    bool isHLGVideo() const;
    void calculateFrameRateStats();
    bool convertRGBToDNG(const std::vector<uint16_t>& rgbData, std::vector<uint8_t>& dngData, int frameNumber, motioncam::Timestamp timestamp, bool jpegCompression = false,
                         float gainMapExposureOffset = 0.0f,
                         const std::array<float, 3>& gainMapNeutralScale = {1.0f, 1.0f, 1.0f},
                         double iso = 0.0, double shutterSpeed = 0.0,
                         double baselineExposure = 0.0,
                         const std::optional<std::array<float, 3>>& asShotNeutral = std::nullopt,
                         const std::vector<GainMap>& opcodeList2 = {},
                         const std::vector<GainMap>& opcodeList3 = {});
    std::vector<GainMap> loadSidecarGainMaps(int frameNumber, const char* field) const;
    void applySidecarGainMaps(std::vector<uint16_t>& rgbData, int frameNumber,
                              float& exposureOffset,
                              std::array<float, 3>& neutralScale) const;
    void loadSidecarMetadata(const boost::filesystem::path& path);


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
    FrameRateInfo mFrameRateInfo;
    int mTotalFrames;
    int mDroppedFrames;
    int mDuplicatedFrames;
    int mWidth;
    int mHeight;
    std::string mPixelFormat;
    bool mIsHLG;
    std::unique_ptr<DirectLogDecoder> mDecoder;
    std::optional<CalibrationData> mCalibration;
    nlohmann::json mSidecarMetadata;
    mutable std::mutex mMutex;
};

} // namespace motioncam
