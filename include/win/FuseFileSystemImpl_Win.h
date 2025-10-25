#pragma once

#include <map>
#include <memory>

#include "IFuseFileSystem.h"
#include "RenderConfig.h"

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

    MountId mount(const RenderConfig& config, const std::string& srcFile, const std::string& dstPath) override;
    void unmount(MountId mountId) override;
    void updateOptions(MountId mountId, const RenderConfig& config) override;
    std::optional<FileInfo> getFileInfo(MountId mountId) override;

private:
    MountId mNextMountId;
    std::map<MountId, std::unique_ptr<VirtualizationInstance>> mMountedFiles;
    std::unique_ptr<BS::thread_pool> mIoThreadPool;
    std::unique_ptr<BS::thread_pool> mProcessingThreadPool;
    std::unique_ptr<LRUCache> mCache;

};

} // namespace motioncam
