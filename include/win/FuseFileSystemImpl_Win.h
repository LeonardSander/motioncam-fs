#pragma once

#include <map>
#include <memory>
#include <mutex>

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
    ~FuseFileSystemImpl_Win() override;

    MountId mount(const RenderSettings& settings, const std::string& srcFile, const std::string& dstPath) override;
    void unmount(MountId mountId) override;
    void updateOptions(MountId mountId, const RenderSettings& settings) override;
    std::optional<FileInfo> getFileInfo(MountId mountId) override;
    bool generateThumbnail(MountId mountId, const std::string& outputPath,
                           int width = 320, int height = 240) override;
    void setCachePolicy(CachePolicy policy) override;
    void setCacheQuotaBytes(std::uint64_t bytes) override;
    void cleanupCacheExpired() override;
    void finalize(MountId, const std::string&, bool, const FinalizeOptions&,
        const std::function<bool(size_t, size_t, const std::string&)>&,
        const std::function<void(const std::vector<uint8_t>&, Timestamp)>& = {},
        bool = true) override;

private:
    MountId mNextMountId;
    std::map<MountId, std::unique_ptr<VirtualizationInstance>> mMountedFiles;
    mutable std::mutex mMountedFilesMutex;
    std::unique_ptr<BS::thread_pool> mIoThreadPool;
    std::unique_ptr<BS::thread_pool> mProcessingThreadPool;
    std::unique_ptr<LRUCache> mCache;
    CachePolicy mCachePolicy{CachePolicy::Quota};
    std::uint64_t mCacheQuotaBytes{30ULL * 1024 * 1024 * 1024};

};

} // namespace motioncam
