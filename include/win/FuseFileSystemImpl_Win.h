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

    MountId mount(FileRenderOptions options, int draftScale, const std::string cfrTarget, const std::string cropTarget, const std::string& srcFile, const std::string& dstPath);
    void unmount(MountId mountId) override;
    void updateOptions(MountId mountId, FileRenderOptions options, int draftScale, const std::string cfrTarget, const std::string cropTarget);
    float getFps(MountId mountId) const override;

private:
    MountId mNextMountId;
    std::map<MountId, std::unique_ptr<VirtualizationInstance>> mMountedFiles;
    std::unique_ptr<BS::thread_pool> mIoThreadPool;
    std::unique_ptr<BS::thread_pool> mProcessingThreadPool;
    std::unique_ptr<LRUCache> mCache;

};

} // namespace motioncam
