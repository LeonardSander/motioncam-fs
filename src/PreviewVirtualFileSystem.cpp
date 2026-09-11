#include "VirtualFileSystemImpl.h"
#include "VirtualFileSystemImpl_MCRAW.h"
#include "VirtualFileSystemImpl_DirectLog.h"
#include "VirtualFileSystemImpl_DNG.h"
#include "DNGDecoder.h"
#include "LRUCache.h"
#include <boost/algorithm/string/predicate.hpp>
#include <boost/filesystem.hpp>
#include <chrono>
#include <cstdlib>
#include <spdlog/spdlog.h>

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
           a.vignetteCorrection == b.vignetteCorrection &&
           a.quadBayerOption == b.quadBayerOption &&
           a.cfaPhase == b.cfaPhase &&
           a.jxlDistance == b.jxlDistance &&
           a.cameraNativeStaging == b.cameraNativeStaging &&
           a.streamingPreview == b.streamingPreview;
}

bool galleryDiagnosticsEnabled() {
    static const bool enabled = std::getenv("MOTIONCAM_GALLERY_DIAGNOSTICS") != nullptr;
    return enabled;
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

void PreviewRenderer::render(
    const RenderSettings& settings, const PreviewOptions& options,
    const std::function<bool(size_t, size_t, const std::string&)>& progress,
    const std::function<void(PreviewFrame&&)>& frameReady) {
    const auto requestStarted = std::chrono::steady_clock::now();
    std::shared_ptr<State> state;
    bool rebuilt = false;
    {
        std::lock_guard<std::mutex> lock(mMutex);
        if (!mState || !sameSettings(mState->settings, settings)) {
            const auto initStarted = std::chrono::steady_clock::now();
            state = std::make_shared<State>(settings);
            auto filesystem = createVirtualFileSystem(
                mIoThreadPool, mProcessingThreadPool, state->cache, settings,
                mSource, mBaseName);
            state->filesystem = std::shared_ptr<IVirtualFileSystem>(std::move(filesystem));
            mState = state;
            rebuilt = true;
            if (galleryDiagnosticsEnabled())
                spdlog::info("GALLERY_PERF event=preview_vfs_init source={} latency_ms={:.3f}",
                    mSource, std::chrono::duration<double, std::milli>(
                        std::chrono::steady_clock::now() - initStarted).count());
        } else {
            state = mState;
        }
    }
    const auto waitStarted = std::chrono::steady_clock::now();
    std::lock_guard<std::mutex> executionLock(state->executionMutex);
    const double waitMs = std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - waitStarted).count();
    const auto renderStarted = std::chrono::steady_clock::now();
    auto entries = state->filesystem->listFiles("");
    entries.erase(std::remove_if(entries.begin(), entries.end(), [](const Entry& entry) {
        return entry.type != EntryType::FILE_ENTRY ||
            !boost::iequals(boost::filesystem::path(entry.name).extension().string(), ".dng");
    }), entries.end());
    const size_t firstFrame = std::min(options.firstFrame, entries.size());
    size_t completed = 0;
    for (size_t index = firstFrame; index < entries.size(); ++index) {
        if (progress && !progress(completed, entries.size() - firstFrame,
                "Rendering " + entries[index].name))
            throw std::runtime_error("Preview rendering cancelled");
        if (options.skipFrame && options.skipFrame(index)) {
            ++completed;
            continue;
        }
        PreviewFrame frame;
        if (!state->filesystem->materializePreviewFrame(entries[index], frame)) {
            auto bytes = state->filesystem->materializeFile(entries[index], false);
            if (!bytes) throw std::runtime_error("Failed to render " + entries[index].name);
            std::vector<uint8_t> dng(bytes->begin(), bytes->end());
            if (!DNGDecoder::decodePreview(std::move(dng), settings, frame))
                throw std::runtime_error("Could not decode preview " + entries[index].name);
        }
        frameReady(std::move(frame));
        ++completed;
    }
    if (galleryDiagnosticsEnabled())
        spdlog::info(
            "GALLERY_PERF event=preview_render source={} rebuilt={} queue_ms={:.3f} render_ms={:.3f} total_ms={:.3f}",
            mSource, rebuilt, waitMs,
            std::chrono::duration<double, std::milli>(
                std::chrono::steady_clock::now() - renderStarted).count(),
            std::chrono::duration<double, std::milli>(
                std::chrono::steady_clock::now() - requestStarted).count());
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
