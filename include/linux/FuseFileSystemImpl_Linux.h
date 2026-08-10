#pragma once

#include <map>
#include <memory>

#include "IFuseFileSystem.h"

namespace BS { class thread_pool; }

namespace motioncam {

struct LinuxFuseSession;
class LRUCache;

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
    void finalize(MountId, const std::string&, bool,
        const std::function<bool(size_t, size_t, const std::string&)>&) override;

private:
    MountId mNextMountId;
    std::map<MountId, std::unique_ptr<LinuxFuseSession>> mMountedFiles;
    std::unique_ptr<BS::thread_pool> mIoThreadPool;
    std::unique_ptr<BS::thread_pool> mProcessingThreadPool;
    std::unique_ptr<LRUCache> mCache;
};

} // namespace motioncam
