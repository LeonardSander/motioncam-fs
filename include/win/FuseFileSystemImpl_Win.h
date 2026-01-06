#pragma once

#include <map>
#include <memory>


#include "IFuseFileSystem.h"

namespace BS {
    class thread_pool;
}

namespace motioncam {

class VirtualizationInstance;
class LRUCache;

class FuseFileSystemImpl_Win : public IFuseFileSystem
{
public:
    FuseFileSystemImpl_Win();

    MountId mount(FileRenderOptions options, int draftScale, const std::string cfrTarget, const std::string cropTarget, const std::string cameraModel, const std::string levels, const std::string logTransform, const std::string exposureCompensation, const std::string quadBayerOption, bool matrixOverrideEnabled, const std::string& matrixProfile, const std::string& matrixFilePath, const std::string& srcFile, const std::string& dstPath) override;
    void unmount(MountId mountId) override;
    void updateOptions(MountId mountId, FileRenderOptions options, int draftScale, std::string cfrTarget, std::string cropTarget, std::string cameraModel, std::string levels, std::string logTransform, std::string exposureCompensation, std::string quadBayerOption, bool matrixOverrideEnabled, const std::string& matrixProfile, const std::string& matrixFilePath) override;
    std::optional<FileInfo> getFileInfo(MountId mountId) override;
    bool generateThumbnail(MountId mountId, const std::string& outputPath, int width = 320, int height = 240) override;
    void setCachePolicy(CachePolicy policy) override;
    void setCacheQuotaBytes(std::uint64_t bytes) override;
    void cleanupCacheExpired() override;

private:
    void evictMaterializedByGlobalQuota(std::uint64_t quotaBytes);

    MountId mNextMountId;
    std::map<MountId, std::unique_ptr<VirtualizationInstance>> mMountedFiles;
    std::unique_ptr<BS::thread_pool> mIoThreadPool;
    std::unique_ptr<BS::thread_pool> mProcessingThreadPool;
    std::unique_ptr<LRUCache> mCache;
    CachePolicy mCachePolicy;
    std::uint64_t mCacheQuotaBytes;

};

} // namespace motioncam
