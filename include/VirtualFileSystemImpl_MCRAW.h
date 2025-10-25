#pragma once

#include <IVirtualFileSystem.h>
#include <IFuseFileSystem.h>
#include <CalibrationData.h>
#include <ExposureKeyframes.h>
#include <RenderConfig.h>

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
        const RenderConfig& config,
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

    void updateOptions(const RenderConfig& config) override;
    FileInfo getFileInfo() const override;

private:
    void init();

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
    RenderConfig mConfig;
    std::optional<ExposureKeyframes> mExposureKeyframes;
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
    bool mNeedRemosaic;
    float mSrcWhiteLevel;
    std::array<float, 4> mSrcBlackLevel;
    float mAudioDurationSeconds;
};

} // namespace motioncam
