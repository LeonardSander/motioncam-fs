#pragma once

#include <IVirtualFileSystem.h>
#include <IFuseFileSystem.h>
#include <VirtualFileSystemImpl.h>
#include <CalibrationData.h>
#include <DNGDecoder.h>
#include <Types.h>
#include <nlohmann/json.hpp>
#include <memory>
#include <shared_mutex>
#include <unordered_map>

namespace BS {
class thread_pool;
}

namespace motioncam {

class LRUCache;
class DirectLogDecoder;

class VirtualFileSystemImpl_DirectLog : public MountedDngSource
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

    void updateOptions(const RenderSettings& config) override;
    FileInfo getFileInfo() const override;
    std::shared_ptr<std::vector<uint8_t>> materializeFile(
        const Entry& entry, bool jpegCompression = false) override;
    bool materializePreviewFrame(const Entry& entry, PreviewFrame& frame,
                                 bool retainSourceSamples = false) override;

private:
    std::function<std::shared_ptr<std::vector<uint8_t>>()>
        staticMaterializer(const Entry& entry) override;
    struct FrameMetadata {
        double iso = 0.0;
        double shutterSpeed = 0.0;
        double baselineExposure = 0.0;
        std::optional<std::array<float, 3>> asShotNeutral;
        uint16_t tiffOrientation = 0;
        std::optional<std::array<float, 9>> colorMatrix1, colorMatrix2;
        std::optional<std::array<float, 9>> forwardMatrix1, forwardMatrix2;
        std::optional<std::array<float, 9>> cameraCalibration1, cameraCalibration2;
        uint16_t calibrationIlluminant1 = 0, calibrationIlluminant2 = 0;
    };

    void init();
    
    bool isHLGVideo() const;
    void calculateFrameRateStats();
    bool convertRGBToDNG(std::vector<uint16_t> rgbData, std::vector<uint8_t>& dngData, int frameNumber, motioncam::Timestamp timestamp,
                         double iso = 0.0, double shutterSpeed = 0.0,
                         double baselineExposure = 0.0,
                         const std::optional<std::array<float, 3>>& asShotNeutral = std::nullopt,
                         uint16_t tiffOrientation = 0,
                         const FrameMetadata* sourceMetadata = nullptr,
                         const std::vector<GainMap>& opcodeList2 = {},
                         const std::vector<GainMap>& opcodeList3 = {},
                         int decodedWidth = 0, int decodedHeight = 0,
                         bool prepopulatedImage = false,
                         bool inputBayer = false);
    std::vector<GainMap> loadSidecarGainMaps(int frameNumber, const char* field) const;
    struct PreparedSidecarGainMaps {
        std::vector<GainMap> opcodeList2, opcodeList3;
    };
    struct ProcessedFrame {
        std::vector<uint16_t> rgb;
        std::vector<uint8_t> dngStripBuffer;
        FrameMetadata metadata;
        PreparedSidecarGainMaps gainMaps;
        Timestamp timestamp = 0;
        int frameNumber = 0;
        int width = 0;
        int height = 0;
        bool bayer = false;
    };
    ProcessedFrame processFrame(const Entry& entry, int previewScale = 1,
                                bool prepopulateDng = false,
                                bool outputBayer = false);
    PreparedSidecarGainMaps prepareSidecarGainMaps(int frameNumber) const;
    void analyzeSidecarExposure();
    FrameMetadata frameMetadata(int frameNumber) const;


private:
    const std::string mSrcPath;
    const std::string mBaseName;
    size_t mTypicalDngSize;
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
    std::shared_ptr<std::vector<uint8_t>> mAudioWav;
    std::optional<CalibrationData> mCalibration;
    std::optional<GyroflowLensProfile> mGyroflowLensProfile;
    nlohmann::json mSidecarMetadata;
    vfs::ManualVignetteSidecars mManualVignetteSidecars;
    std::unordered_map<Timestamp, size_t> mFrameIndexByTimestamp;
    std::map<Timestamp, float> mNormalizedExposureOffsets;
    std::map<Timestamp, float> mSmoothedExposureOffsets;
    std::map<Timestamp, std::array<float, 3>> mSmoothedAsShotNeutrals;
    mutable std::shared_mutex mRenderMutex;
};

} // namespace motioncam
