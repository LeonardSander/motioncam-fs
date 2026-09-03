#include "VirtualFileSystemImpl.h"
#include "VirtualFileSystemImpl_MCRAW.h"
#include "VirtualFileSystemImpl_DirectLog.h"
#include "VirtualFileSystemImpl_DNG.h"
#include "DNGDecoder.h"
#include "LRUCache.h"
#include <boost/algorithm/string/predicate.hpp>
#include <boost/filesystem.hpp>

namespace motioncam {
namespace {
bool sameSettings(const RenderSettings& a, const RenderSettings& b) {
    return a.options == b.options &&
           a.draftScale == b.draftScale &&
           a.cfrTarget.mode == b.cfrTarget.mode &&
           a.cfrTarget.customValue == b.cfrTarget.customValue &&
           a.cropTarget == b.cropTarget &&
           a.cameraModel == b.cameraModel &&
           a.levels == b.levels &&
           a.logTransform == b.logTransform &&
           a.exposureCompensation == b.exposureCompensation &&
           a.badPixelTreatment == b.badPixelTreatment &&
           a.quadBayerOption == b.quadBayerOption &&
           a.cfaPhase == b.cfaPhase &&
           a.jxlDistance == b.jxlDistance &&
           a.cameraNativeStaging == b.cameraNativeStaging &&
           a.streamingPreview == b.streamingPreview;
}
}

struct PreviewRenderer::State {
    explicit State(const RenderSettings& value)
        : settings(value), cache(64 * 1024 * 1024) {}

    RenderSettings settings;
    LRUCache cache;
    std::shared_ptr<IVirtualFileSystem> filesystem;
    // Source decoders are stateful. A cancelled gallery generation may still
    // be completing its current frame when a seek starts another finalization
    // with identical settings, so access to a reused filesystem must remain
    // single-threaded.
    std::mutex executionMutex;
};

PreviewRenderer::PreviewRenderer(
    BS::thread_pool& ioThreadPool, BS::thread_pool& processingThreadPool,
    std::string source, std::string baseName)
    : mIoThreadPool(ioThreadPool),
      mProcessingThreadPool(processingThreadPool),
      mSource(std::move(source)),
      mBaseName(std::move(baseName)) {}

void PreviewRenderer::finalize(
    const RenderSettings& settings, const std::string& destination,
    const FinalizeOptions& options,
    const std::function<bool(size_t, size_t, const std::string&)>& progress,
    const std::function<void(const std::vector<uint8_t>&, Timestamp)>& fileReady) {
    std::shared_ptr<State> state;
    {
        std::lock_guard<std::mutex> lock(mMutex);
        if (!mState || !sameSettings(mState->settings, settings)) {
            state = std::make_shared<State>(settings);
            auto filesystem = createVirtualFileSystem(
                mIoThreadPool, mProcessingThreadPool, state->cache, settings,
                mSource, mBaseName);
            state->filesystem = std::shared_ptr<IVirtualFileSystem>(std::move(filesystem));
            mState = state;
        } else {
            state = mState;
        }
    }
    std::lock_guard<std::mutex> executionLock(state->executionMutex);
    vfs::finalize(*state->filesystem, destination, false, options,
                  progress, fileReady, false);
}

std::unique_ptr<IVirtualFileSystem> createVirtualFileSystem(
    BS::thread_pool& ioThreadPool, BS::thread_pool& processingThreadPool,
    LRUCache& cache, const RenderSettings& settings,
    const std::string& source, const std::string& baseName) {
    const boost::filesystem::path path(source);
    const std::string extension = path.extension().string();
    const std::string filename = path.filename().string();
    if (boost::iequals(extension, ".mcraw"))
        return std::make_unique<VirtualFileSystemImpl_MCRAW>(
            ioThreadPool, processingThreadPool, cache, settings, source, baseName);
    if ((boost::iequals(extension, ".mov") || boost::iequals(extension, ".mp4") ||
         boost::iequals(extension, ".mkv")) && boost::icontains(filename, "NATIVE"))
        return std::make_unique<VirtualFileSystemImpl_DirectLog>(
            ioThreadPool, processingThreadPool, cache, settings, source, baseName);
    if (boost::iequals(extension, ".dng") || DNGDecoder::isDNGSequence(source))
        return std::make_unique<VirtualFileSystemImpl_DNG>(
            ioThreadPool, processingThreadPool, cache, settings, source, baseName);
    throw std::runtime_error("Invalid preview source format");
}
}
