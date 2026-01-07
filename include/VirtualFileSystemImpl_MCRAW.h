#pragma once

#include <IVirtualFileSystem.h>
#include <IFuseFileSystem.h>
#include "CameraFrameMetadata.h"
#include "CameraMetadata.h"
#include <chrono>
#include <cstdint>
#include <deque>
#include <future>
#include <memory>
#include <optional>
#include <tuple>
#include <unordered_map>
#include <utility>
#include <vector>

namespace BS {
class thread_pool;
}

namespace motioncam {

using Timestamp = std::int64_t;

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
    FileInfo getFileInfo() const;
    const std::string& getSourcePath() const { return mSrcPath; }
    const std::string& getLevels() const { return mLevels; }
    const std::string& getExposureCompensation() const { return mExposureCompensation; }
    std::vector<Entry> getExpiredDngEntries(std::chrono::seconds ttl);
    std::vector<std::pair<Entry, std::chrono::steady_clock::time_point>> getDngAccessEntries() const;
    void forgetDngAccess(const Entry& entry);

    struct MatrixOverrideProfile {
        std::array<float, 9> colorMatrix1{};
        std::array<float, 9> colorMatrix2{};
        std::array<float, 9> forwardMatrix1{};
        std::array<float, 9> forwardMatrix2{};
        std::array<float, 9> calibrationMatrix1{};
        std::array<float, 9> calibrationMatrix2{};
        bool hasColor1{false};
        bool hasColor2{false};
        bool hasForward1{false};
        bool hasForward2{false};
        bool hasCalibration1{false};
        bool hasCalibration2{false};
    };

private:
    using FrameData = std::tuple<size_t, CameraConfiguration, CameraFrameMetadata, std::shared_ptr<std::vector<uint8_t>>>;

    void init(FileRenderOptions options);
    void recordDngAccess(const Entry& entry);
    size_t getEntryIndex(const Entry& entry) const;
    std::shared_future<FrameData> scheduleDecode(const Entry& entry, std::chrono::high_resolution_clock::time_point requestStartTime);
    std::shared_future<FrameData> getOrSchedulePrefetch(const Entry& entry, std::chrono::high_resolution_clock::time_point requestStartTime);
    void scheduleReadahead(size_t currentIndex);
    void consumePrefetch(Timestamp timestamp);
    void trimPrefetchLocked();
    void applyMatrixOverride(CameraConfiguration& cameraConfig) const;

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
    size_t mFirstFrameDngSize;
    std::vector<Entry> mFiles;
    std::unordered_map<std::string, Entry> mFileIndex;
    std::unordered_map<std::string, size_t> mPathIndex;
    std::unordered_map<Timestamp, size_t> mTimestampIndex;
    std::vector<uint8_t> mAudioFile;
    int mDraftScale;
    CFRTarget mCFRTarget;
    std::string mCropTarget;
    std::string mCameraModel;
    std::string mLevels;
    LogTransformMode mLogTransform;
    std::string mExposureCompensation;
    QuadBayerMode mQuadBayerOption;
    std::optional<MatrixOverrideProfile> mMatrixOverrideProfile;
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
    std::mutex mMutex;
    mutable std::mutex mAccessMutex;
    std::unordered_map<Entry, std::chrono::steady_clock::time_point, Entry::Hash> mLastAccessTimes;
    size_t mPrefetchWindow;
    std::unordered_map<Timestamp, std::shared_future<FrameData>> mPrefetchFutures;
    std::deque<Timestamp> mPrefetchOrder;
    std::mutex mPrefetchMutex;
};

} // namespace motioncam
