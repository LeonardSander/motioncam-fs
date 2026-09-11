#pragma once

#include <map>
#include <memory>
#include <mutex>

#include "IFuseFileSystem.h"

namespace BS {
    class thread_pool;
}

namespace motioncam {

struct Session;
class LRUCache;
class PreviewRenderer;

class FuseFileSystemImpl_MacOs : public IFuseFileSystem
{
public:
    FuseFileSystemImpl_MacOs();
    ~FuseFileSystemImpl_MacOs();

    MountId mount(
        const RenderSettings& settings,
        const std::string& srcFile,
        const std::string& dstPath) override;

    void unmount(MountId mountId) override;
    void updateOptions(
        MountId mountId,
        const RenderSettings& settings) override;
    std::optional<FileInfo> getFileInfo(MountId mountId) override;
    void setCachePolicy(CachePolicy) override {}
    void setCacheQuotaBytes(std::uint64_t) override {}
    void cleanupCacheExpired() override {}
    void finalize(MountId, const std::string&, bool, const FinalizeOptions&,
        const std::function<bool(size_t, size_t, const std::string&)>&,
        const std::function<void(const std::vector<uint8_t>&, Timestamp)>& = {},
        bool = true) override;
    void renderPreview(MountId, const RenderSettings&, const PreviewOptions&,
        const std::function<bool(size_t, size_t, const std::string&)>&,
        const std::function<void(PreviewFrame&&)>&) override;

private:
    MountId mNextMountId;
    std::map<MountId, std::shared_ptr<Session>> mMountedFiles;
    mutable std::mutex mMountedFilesMutex;
    std::unique_ptr<BS::thread_pool> mIoThreadPool;
    std::unique_ptr<BS::thread_pool> mProcessingThreadPool;
    std::unique_ptr<LRUCache> mCache;
    std::map<MountId, std::shared_ptr<PreviewRenderer>> mPreviewRenderers;
};

} // namespace motioncam
