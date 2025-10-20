#pragma once

#include <IVirtualFileSystem.h>
#include <IFuseFileSystem.h>
#include <CalibrationData.h>
#include <memory>

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
        FileRenderOptions options,
        int draftScale,
        const std::string& cfrTarget,
        const std::string& cropTarget,
        const std::string& file,
        const std::string& baseName,
        const std::string& cameraModel,
        const std::string& levels,
        const std::string& logTransform,
        const std::string& exposureCompensation = "0ev",
        const std::string& quadBayerOption = "Remosaic");

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

    void updateOptions(FileRenderOptions options, int draftScale, const std::string& cfrTarget, const std::string& cropTarget, const std::string& cameraModel, const std::string& levels, const std::string& logTransform, const std::string& exposureCompensation, const std::string& quadBayerOption) override;
    FileInfo getFileInfo() const override;

private:
    void init(FileRenderOptions options);
    
    size_t generateFrame(
        const Entry& entry,
        const size_t pos,
        const size_t len,
        void* dst,
        std::function<void(size_t, int)> result,
        bool async);

    void calculateFrameRateStats();

private:
    LRUCache& mCache;
    BS::thread_pool& mIoThreadPool;
    BS::thread_pool& mProcessingThreadPool;
    const std::string mSrcPath;
    const std::string mBaseName;
    size_t mTypicalDngSize;
    std::vector<Entry> mFiles;
    int mDraftScale;
    std::string mCFRTarget;
    std::string mCropTarget;
    std::string mCameraModel;    
    std::string mLevels;
    std::string mLogTransform;
    std::string mExposureCompensation;
    std::string mQuadBayerOption;
    FileRenderOptions mOptions;
    float mFps;
    float mMedFps;
    float mAvgFps;
    int mTotalFrames;
    int mDroppedFrames;
    int mDuplicatedFrames;
    int mWidth;
    int mHeight;
    std::unique_ptr<DNGDecoder> mDecoder;
    std::optional<CalibrationData> mCalibration;
    mutable std::mutex mMutex;
};

} // namespace motioncam