#include "android/ProcessingFileSystem.h"

#include "LRUCache.h"
#include "VirtualFileSystemImpl.h"
#include "IVirtualFileSystem.h"

#include <BS_thread_pool.hpp>
#include <algorithm>
#include <filesystem>
#include <stdexcept>
#include <thread>

namespace motioncam {
namespace {
constexpr std::size_t cacheBytes = 256u * 1024u * 1024u;
}

struct ProcessingFileSystem::Session {
    std::string source;
    std::unique_ptr<IVirtualFileSystem> fs;
};

ProcessingFileSystem::ProcessingFileSystem()
    : mIoPool(std::make_unique<BS::thread_pool>(2)),
      mProcessingPool(std::make_unique<BS::thread_pool>(
          std::max(1u, std::min(4u, std::thread::hardware_concurrency())))),
      mCache(std::make_unique<LRUCache>(cacheBytes)) {}

ProcessingFileSystem::~ProcessingFileSystem() {
    mSessions.clear();
    mIoPool->wait();
    mProcessingPool->wait();
}

std::shared_ptr<ProcessingFileSystem::Session>
ProcessingFileSystem::session(MountId id) const {
    std::lock_guard<std::mutex> lock(mMutex);
    const auto it = mSessions.find(id);
    if (it == mSessions.end()) throw std::runtime_error("Clip not found");
    return it->second;
}

MountId ProcessingFileSystem::mount(const RenderSettings& settings,
                                   const std::string& source,
                                   const std::string& destination, bool) {
    RenderSettings local = settings;
    local.projectedFileSizes = false;
    auto mounted = std::make_shared<Session>();
    mounted->source = source;
    mounted->fs = createVirtualFileSystem(*mIoPool, *mProcessingPool, *mCache,
        local, source, std::filesystem::path(destination).filename().string());
    std::lock_guard<std::mutex> lock(mMutex);
    const MountId id = mNextMountId++;
    mSessions.emplace(id, std::move(mounted));
    return id;
}

void ProcessingFileSystem::unmount(MountId id) {
    std::lock_guard<std::mutex> lock(mMutex);
    mPreviews.erase(id);
    mSessions.erase(id);
}

void ProcessingFileSystem::setProjectionEnabled(MountId, bool) {}

void ProcessingFileSystem::updateOptions(MountId id, const RenderSettings& settings) {
    auto mounted = session(id);
    RenderSettings local = settings;
    local.projectedFileSizes = false;
    mounted->fs->updateOptions(local);
    std::lock_guard<std::mutex> lock(mMutex);
    mPreviews.erase(id);
}

std::optional<FileInfo> ProcessingFileSystem::getFileInfo(MountId id) {
    std::lock_guard<std::mutex> lock(mMutex);
    const auto it = mSessions.find(id);
    if (it == mSessions.end()) return std::nullopt;
    return it->second->fs->getFileInfo();
}

void ProcessingFileSystem::finalize(MountId id, const std::string& destination,
    bool jpegCompression, const FinalizeOptions& options,
    const std::function<bool(size_t, size_t, const std::string&)>& progress,
    const std::function<void(const std::vector<uint8_t>&, Timestamp)>& fileReady,
    bool writeFiles) {
    auto mounted = session(id);
    vfs::finalize(*mounted->fs, destination, jpegCompression, options, progress,
                  fileReady, writeFiles);
}

void ProcessingFileSystem::finalizeToEntries(MountId id, bool jpegCompression,
    const FinalizeOptions& options,
    const std::function<bool(size_t, size_t, const std::string&)>& progress,
    const std::function<void(const std::string&, const std::vector<uint8_t>&)>& entryReady) {
    auto mounted = session(id);
    vfs::finalize(*mounted->fs, {}, jpegCompression, options, progress, {}, false,
                  entryReady);
}

void ProcessingFileSystem::renderPreview(MountId id, const RenderSettings& settings,
    const PreviewOptions& options,
    const std::function<bool(size_t, size_t, const std::string&)>& progress,
    const std::function<void(PreviewFrame&&)>& frameReady) {
    std::shared_ptr<PreviewRenderer> preview;
    {
        std::lock_guard<std::mutex> lock(mMutex);
        const auto it = mSessions.find(id);
        if (it == mSessions.end()) throw std::runtime_error("Clip not found");
        auto& cached = mPreviews[id];
        if (!cached)
            cached = std::make_shared<PreviewRenderer>(*mIoPool, *mProcessingPool,
                it->second->source, "gallery-preview-" + std::to_string(id));
        preview = cached;
    }
    preview->render(settings, options, progress, frameReady);
}

void ProcessingFileSystem::releasePreviewResources(MountId id) {
    std::lock_guard<std::mutex> lock(mMutex);
    mPreviews.erase(id);
}
}
