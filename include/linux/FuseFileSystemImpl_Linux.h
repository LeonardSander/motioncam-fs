#pragma once

#include <map>
#include <memory>
#include <mutex>

#include "IFuseFileSystem.h"

namespace BS { class thread_pool; }

namespace motioncam {

struct LinuxFuseSession;
class LRUCache;
class PreviewRenderer;

// Linux counterpart to the Windows projected-filesystem provider.  FUSE3
// exposes generated frames as a read-only, kernel-cached filesystem.
class FuseFileSystemImpl_Linux : public IFuseFileSystem {
public:
    FuseFileSystemImpl_Linux();
    ~FuseFileSystemImpl_Linux();

    MountId mount(const RenderSettings& settings, const std::string& srcFile,
                  const std::string& dstPath) override;
    void unmount(MountId mountId) override;
    void updateOptions(MountId mountId, const RenderSettings& settings) override;
    std::optional<FileInfo> getFileInfo(MountId mountId) override;
    bool generateThumbnail(MountId mountId, const std::string& outputPath,
                           int width = 320, int height = 240) override;
    void finalize(MountId, const std::string&, bool, const FinalizeOptions&,
        const std::function<bool(size_t, size_t, const std::string&)>&,
        const std::function<void(const std::vector<uint8_t>&, Timestamp)>& = {},
        bool = true) override;
    void finalizePreview(MountId, const RenderSettings&, const FinalizeOptions&,
        const std::function<bool(size_t, size_t, const std::string&)>&,
        const std::function<void(const std::vector<uint8_t>&, Timestamp)>&) override;

private:
    MountId mNextMountId;
    // Long-running thumbnail/finalize operations retain their session without
    // holding the map mutex, so unrelated mounts are never serialized behind
    // image processing.
    std::map<MountId, std::shared_ptr<LinuxFuseSession>> mMountedFiles;
    mutable std::mutex mMountedFilesMutex;
    std::unique_ptr<BS::thread_pool> mIoThreadPool;
    std::unique_ptr<BS::thread_pool> mProcessingThreadPool;
    std::unique_ptr<LRUCache> mCache;
    std::map<MountId, std::shared_ptr<PreviewRenderer>> mPreviewRenderers;
};

} // namespace motioncam
