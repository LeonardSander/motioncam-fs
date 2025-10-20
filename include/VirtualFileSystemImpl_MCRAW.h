#pragma once

#include <IVirtualFileSystem.h>
#include <IFuseFileSystem.h>
#include <CalibrationData.h>
#include <ExposureKeyframes.h>

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

    void updateOptions(FileRenderOptions options, int draftScale, const std::string& cfrTarget, const std::string& cropTarget, const std::string& cameraModel, const std::string& levels, const std::string& logTransform, const std::string& exposureCompensation, const std::string& quadBayerOption) override;
    FileInfo getFileInfo() const;

private:
    void init(FileRenderOptions options);

    size_t generateFrame(
        const Entry& entry,
        const size_t pos,
        const size_t len,
        void* dst,
        std::function<void(size_t, int)> result,
        bool async);

    size_t generateAudio(
        const Entry& entry,
        const size_t pos,
        const size_t len,
        void* dst,
        std::function<void(size_t, int)> result,
        bool async);

private:
    LRUCache& mCache;
    BS::thread_pool& mIoThreadPool;
    BS::thread_pool& mProcessingThreadPool;
    const std::string mSrcPath;
    const std::string mBaseName;
    size_t mTypicalDngSize;
    std::vector<Entry> mFiles;
    std::vector<uint8_t> mAudioFile;
    int mDraftScale;
    std::string mCFRTarget;
    std::string mCropTarget;
    std::string mCameraModel;    
    std::string mLevels;
    std::string mLogTransform;
    std::string mExposureCompensation;
    std::optional<ExposureKeyframes> mExposureKeyframes;
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
    double mBaselineExpValue;
    std::optional<CalibrationData> mCalibration;
    std::mutex mMutex;
};

} // namespace motioncam
