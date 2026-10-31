#pragma once

#include "IFuseFileSystem.h"

#include <map>
#include <memory>
#include <mutex>

namespace BS { class thread_pool; }

namespace motioncam {
class IVirtualFileSystem;
class LRUCache;
class PreviewRenderer;

// Android has no app-accessible FUSE mount. Keep processing sessions in-process
// so the gallery and finalizer can use the same source decoders as desktop.
class ProcessingFileSystem final : public IFuseFileSystem {
public:
    ProcessingFileSystem();
    ~ProcessingFileSystem() override;

    MountId mount(const RenderSettings&, const std::string&, const std::string&,
                  bool projectFiles = true) override;
    void unmount(MountId) override;
    void setProjectionEnabled(MountId, bool) override;
    void updateOptions(MountId, const RenderSettings&) override;
    std::optional<FileInfo> getFileInfo(MountId) override;
    void finalize(MountId, const std::string&, bool, const FinalizeOptions&,
                  const std::function<bool(size_t, size_t, const std::string&)>&,
                  const std::function<void(const std::vector<uint8_t>&, Timestamp)>& = {},
                  bool = true) override;
    void finalizeToEntries(MountId, bool, const FinalizeOptions&,
        const std::function<bool(size_t, size_t, const std::string&)>&,
        const std::function<void(const std::string&, const std::vector<uint8_t>&)>&);
    void renderPreview(MountId, const RenderSettings&, const PreviewOptions&,
                       const std::function<bool(size_t, size_t, const std::string&)>&,
                       const std::function<void(PreviewFrame&&)>&) override;
    void releasePreviewResources(MountId) override;

private:
    struct Session;
    std::shared_ptr<Session> session(MountId) const;
    MountId mNextMountId = 0;
    mutable std::mutex mMutex;
    std::map<MountId, std::shared_ptr<Session>> mSessions;
    std::map<MountId, std::shared_ptr<PreviewRenderer>> mPreviews;
    std::unique_ptr<BS::thread_pool> mIoPool;
    std::unique_ptr<BS::thread_pool> mProcessingPool;
    std::unique_ptr<LRUCache> mCache;
};
}
