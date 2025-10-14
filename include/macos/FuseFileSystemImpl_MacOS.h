#pragma once

#include <map>
#include <memory>

#include "IFuseFileSystem.h"

namespace BS {
    class thread_pool;
}

namespace motioncam {

struct Session;
class LRUCache;

class FuseFileSystemImpl_MacOs : public IFuseFileSystem
{
public:
    FuseFileSystemImpl_MacOs();
    ~FuseFileSystemImpl_MacOs();

    MountId mount(FileRenderOptions options, int draftScale, const std::string cfrTarget, const std::string cropTarget, const std::string cameraModel, const std::string levels, const std::string logTransform, const std::string exposureCompensation, const std::string quadBayerOption, const std::string& srcFile, const std::string& dstPath) override;
    void unmount(MountId mountId) override;
    void updateOptions(MountId mountId, FileRenderOptions options, int draftScale, std::string cfrTarget, std::string cropTarget, std::string cameraModel, std::string levels, std::string logTransform) override;
    std::optional<FileInfo> getFileInfo(MountId mountId) override;

private:
    MountId mNextMountId;
    std::map<MountId, std::unique_ptr<Session>> mMountedFiles;
    std::unique_ptr<BS::thread_pool> mIoThreadPool;
    std::unique_ptr<BS::thread_pool> mProcessingThreadPool;
    std::unique_ptr<LRUCache> mCache;
};

} // namespace motioncam
