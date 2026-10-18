#include "GalleryVulkanVideo.h"
#include "VulkanGalleryPreview.h"

#if defined(MOTIONCAM_HAS_DIRECTLOG_PLACEBO) && QT_CONFIG(vulkan)

#include <QEvent>
#include <QGuiApplication>
#include <QScreen>
#include <QTimer>
#include <QVBoxLayout>
#include <QVulkanInstance>
#include <QWindow>
#include <QVersionNumber>

#include <libplacebo/renderer.h>
#include <libplacebo/swapchain.h>
#include <libplacebo/vulkan.h>

#include <spdlog/spdlog.h>

#include <atomic>
#include <array>
#include <chrono>
#include <cstdlib>
#include <condition_variable>
#include <mutex>
#include <thread>
#include <unistd.h>

namespace {
uint64_t samplePackedFrame(pl_gpu gpu, pl_buf source,
                           uint32_t width, uint32_t height) {
    pl_buf_params params{};
    params.size = 64u * sizeof(uint32_t);
    params.host_readable = true;
    params.memory_type = PL_BUF_MEM_HOST;
    pl_buf samples = pl_buf_create(gpu, &params);
    if (!samples) return 0;
    for (uint32_t row = 0; row < 8; ++row)
        for (uint32_t col = 0; col < 8; ++col) {
            const size_t x = (size_t(col) * 2u + 1u) * width / 16u;
            const size_t y = (size_t(row) * 2u + 1u) * height / 16u;
            pl_buf_copy(gpu, samples, (row * 8u + col) * 4u,
                        source, (y * width + x) * 4u, 4u);
        }
    std::array<uint8_t, 64u * 4u> bytes{};
    const bool read = pl_buf_read(gpu, samples, 0, bytes.data(), bytes.size());
    pl_buf_destroy(gpu, &samples);
    if (!read) return 0;
    uint64_t hash = 1469598103934665603ull;
    for (uint8_t byte : bytes) { hash ^= byte; hash *= 1099511628211ull; }
    return hash;
}
}

class GalleryVulkanSurface final : public QWindow {
public:
    explicit GalleryVulkanSurface(QVulkanInstance* instance) {
        setSurfaceType(VulkanSurface);
        setVulkanInstance(instance);
        setProperty("galleryVulkanSurface", true);
        mImage = QImage(1, 1, QImage::Format_RGB888);
        mImage.fill(QColor(96, 96, 96));
        mFrameSerial = 1;
        mSerial = 1;
        connect(this, &QWindow::screenChanged, this, [this] {
            QTimer::singleShot(0, this, [this] { refreshPixelGeometry(); });
        });
    }
    ~GalleryVulkanSurface() override {
        stopRenderer();
        pl_swapchain_destroy(&mPreparedSwapchain);
        pl_vulkan_destroy(&mPreparedVk);
        pl_log_destroy(&mPreparedLog);
    }

    void prewarmDevice() {
        pl_swapchain_destroy(&mPreparedSwapchain);
        pl_vulkan_destroy(&mPreparedVk);
        pl_log_destroy(&mPreparedLog);
        create();
        mPreparedSurface = QVulkanInstance::surfaceForWindow(this);
        if (!mPreparedSurface) return;
        mPreparedLog = pl_log_create(PL_API_VER, nullptr);
        if (!mPreparedLog) return;
        pl_vulkan_params params = pl_vulkan_default_params;
        params.instance = vulkanInstance()->vkInstance();
        params.surface = mPreparedSurface;
        mPreparedVk = pl_vulkan_create(mPreparedLog, &params);
        if (mPreparedVk) {
            pl_vulkan_swapchain_params swapchainParams{};
            swapchainParams.surface = mPreparedSurface;
            swapchainParams.present_mode = VK_PRESENT_MODE_MAILBOX_KHR;
            mPreparedSwapchain = pl_vulkan_create_swapchain(
                mPreparedVk, &swapchainParams);
        }
        mHealthy = bool(mPreparedVk && mPreparedSwapchain);
        if (std::getenv("MOTIONCAM_GALLERY_PERF_PROFILE"))
            spdlog::info("GALLERY_PERF event=native_device_prewarm device={} swapchain={}",
                bool(mPreparedVk), bool(mPreparedSwapchain));
        if (std::getenv("MOTIONCAM_GALLERY_PERF_PROFILE"))
            for (QScreen* screen : QGuiApplication::screens())
                spdlog::info("GALLERY_PERF event=qt_screen name={} dpr={:.3f} logical={}x{}",
                    screen->name().toStdString(), double(screen->devicePixelRatio()),
                    screen->geometry().width(), screen->geometry().height());
    }

    void stopRenderer() {
        {
            std::lock_guard lock(mMutex);
            mStopping = true;
        }
        mReady.notify_one();
        if (mThread.joinable()) mThread.join();
        mStopping = false;
    }

    bool usable() const { return mHealthy.load(); }
    qreal pixelRatio() const { return devicePixelRatio(); }

    void updateOutputGeometryLocked() {
        const qreal ratio = devicePixelRatio();
        mOutputWidth = std::max(1, qRound(width() * ratio));
        mOutputHeight = std::max(1, qRound(height() * ratio));
        mTarget = QRectF(mLogicalTarget.x() * ratio,
                         mLogicalTarget.y() * ratio,
                         mLogicalTarget.width() * ratio,
                         mLogicalTarget.height() * ratio);
    }

    void refreshPixelGeometry() {
        std::lock_guard lock(mMutex);
        const int previousWidth = mOutputWidth;
        const int previousHeight = mOutputHeight;
        updateOutputGeometryLocked();
        if (mOutputWidth != previousWidth || mOutputHeight != previousHeight) {
            ++mSerial;
            mReady.notify_one();
            if (std::getenv("MOTIONCAM_GALLERY_PERF_PROFILE"))
                spdlog::info("GALLERY_PERF event=native_pixel_ratio_change logical={}x{} dpr={:.3f} pixels={}x{}",
                    width(), height(), double(devicePixelRatio()),
                    mOutputWidth, mOutputHeight);
        }
    }

    void setFrame(const QImage& image, const QRectF& target, bool smooth) {
        {
            std::lock_guard lock(mMutex);
            mImage = image;
            mGpuBuffer.reset();
            mPlaceholder = false;
            mLogicalTarget = target;
            mSmooth = smooth;
            updateOutputGeometryLocked();
            ++mFrameSerial;
            ++mSerial;
        }
        mReady.notify_one();
    }

    void setGpuFrame(std::shared_ptr<motioncam::VulkanGalleryGpuBuffer> frame,
                     const QRectF& target, bool smooth,
                     const QImage& overlay = {}) {
        {
            std::lock_guard lock(mMutex);
            mImage = {};
            if (mGpuBuffer != frame) {
                mGpuBuffer = std::move(frame);
                ++mFrameSerial;
            }
            if (!overlay.isNull()) {
                if (mOverlay.cacheKey() != overlay.cacheKey()) {
                    mOverlay = overlay;
                    ++mOverlaySerial;
                }
            }
            mPlaceholder = false;
            mLogicalTarget = target;
            mSmooth = smooth;
            updateOutputGeometryLocked();
            ++mSerial;
        }
        mReady.notify_one();
    }

    void setOverlay(const QImage& image) {
        {
            std::lock_guard lock(mMutex);
            mOverlay = image;
            ++mOverlaySerial;
            ++mSerial;
        }
        mReady.notify_one();
    }


    void drainGpuFrames() {
        std::unique_lock lock(mMutex);
        mGpuBuffer.reset();
        ++mSerial;
        mReady.notify_one();
        mFrameDone.wait(lock, [&] { return !mRenderingGpuBuffer; });
    }

protected:
    bool event(QEvent* event) override {
        if (event->type() == QEvent::Hide) stopRenderer();
        const bool pixelRatioChanged = event->type() == QEvent::DevicePixelRatioChange;
        const bool handled = QWindow::event(event);
        if (pixelRatioChanged) refreshPixelGeometry();
        return handled;
    }
    void exposeEvent(QExposeEvent*) override {
        if (!isExposed()) {
            stopRenderer();
            return;
        }
        startRendererIfReady();
        mReady.notify_one();
    }

    void resizeEvent(QResizeEvent*) override {
        {
            std::lock_guard lock(mMutex);
            updateOutputGeometryLocked();
            ++mSerial;
            mReady.notify_one();
        }
        startRendererIfReady();
    }

private:
    void startRendererIfReady() {
        if (!isExposed() || width() <= 0 || height() <= 0 ||
            mThread.joinable() || !mHealthy) return;
        const VkSurfaceKHR surface = QVulkanInstance::surfaceForWindow(this);
        if (!surface) return;
        if (surface != mPreparedSurface || !mPreparedVk ||
            !mPreparedSwapchain)
            prewarmDevice();
        if (!mPreparedVk || !mPreparedSwapchain) return;
        const qreal ratio = devicePixelRatio();
        int outputWidth = std::max(1, qRound(width() * ratio));
        int outputHeight = std::max(1, qRound(height() * ratio));
        if (std::getenv("MOTIONCAM_GALLERY_PERF_PROFILE"))
            spdlog::info("GALLERY_PERF event=native_surface_pixels logical={}x{} dpr={:.3f} requested={}x{}",
                width(), height(), double(ratio), outputWidth, outputHeight);
        pl_swapchain_frame swapFrame{};
        const bool resized = pl_swapchain_resize(mPreparedSwapchain,
            &outputWidth, &outputHeight);
        if (resized && std::getenv("MOTIONCAM_GALLERY_PERF_PROFILE"))
            spdlog::info("GALLERY_PERF event=native_swapchain_pixels actual={}x{}",
                outputWidth, outputHeight);
        const bool started = resized &&
            pl_swapchain_start_frame(mPreparedSwapchain, &swapFrame);
        if (!started) {
            QTimer::singleShot(16, this, [this] { startRendererIfReady(); });
            return;
        }
        pl_frame output{};
        pl_frame_from_swapchain(&output, &swapFrame);
        const float grey[3] = {96.0f / 255.0f,
            96.0f / 255.0f, 96.0f / 255.0f};
        pl_frame_clear(mPreparedVk->gpu, &output, grey);
        if (!pl_swapchain_submit_frame(mPreparedSwapchain)) {
            mHealthy = false;
            return;
        }
        pl_swapchain_swap_buffers(mPreparedSwapchain);
        if (std::getenv("MOTIONCAM_GALLERY_PERF_PROFILE"))
            spdlog::info("GALLERY_PERF event=native_initial_grey_present");
        const VkInstance instance = vulkanInstance()->vkInstance();
        mThread = std::thread([this, surface, instance] {
            renderLoop(surface, instance);
        });
    }

    void renderLoop(VkSurfaceKHR surface, VkInstance instance) {
        pl_log log = nullptr;
        pl_vulkan vk = nullptr;
        pl_swapchain swapchain = nullptr;
        if (mPreparedSurface == surface && mPreparedVk) {
            log = mPreparedLog;
            vk = mPreparedVk;
            swapchain = mPreparedSwapchain;
            mPreparedLog = nullptr;
            mPreparedVk = nullptr;
            mPreparedSwapchain = nullptr;
        } else {
            pl_swapchain_destroy(&mPreparedSwapchain);
            pl_vulkan_destroy(&mPreparedVk);
            pl_log_destroy(&mPreparedLog);
            log = pl_log_create(PL_API_VER, nullptr);
        }
        pl_renderer renderer = nullptr;
        // Keep several uploads alive while earlier frames are being rendered.
        // Reusing one texture can change the image still sampled by a queued
        // mailbox present, even when the frame serials arrive in order.
        std::array<pl_tex, 3> frameTextures{};
        unsigned frameTextureSlot = 0;
        bool haveFrameTexture = false;
        pl_tex overlayTexture = nullptr;
        if (log && !vk) {
            pl_vulkan_params params = pl_vulkan_default_params;
            params.instance = instance;
            params.surface = surface;
            vk = pl_vulkan_create(log, &params);
        }
        if (vk) {
            pl_vulkan_swapchain_params params{};
            params.surface = surface;
            params.present_mode = VK_PRESENT_MODE_MAILBOX_KHR;
            if (!swapchain) swapchain = pl_vulkan_create_swapchain(vk, &params);
            renderer = pl_renderer_create(log, vk->gpu);
        }
        const pl_fmt rgb8 = vk ? pl_find_fmt(vk->gpu, PL_FMT_UNORM, 3, 8, 8,
            PL_FMT_CAP_SAMPLEABLE) : nullptr;
        const pl_fmt rgba8 = vk ? pl_find_fmt(vk->gpu, PL_FMT_UNORM, 4, 8, 8,
            PL_FMT_CAP_SAMPLEABLE) : nullptr;
        pl_fmt rgb10a2 = nullptr;
        if (vk) for (int index = 0; index < vk->gpu->num_formats; ++index) {
            const pl_fmt candidate = vk->gpu->formats[index];
            if (candidate->type == PL_FMT_UNORM && !candidate->opaque &&
                !candidate->emulated && candidate->num_components == 4 &&
                candidate->texel_size == 4 && candidate->texel_align <= 4 &&
                candidate->host_bits[0] == 10 && candidate->host_bits[1] == 10 &&
                candidate->host_bits[2] == 10 && candidate->host_bits[3] == 2 &&
                (candidate->caps & PL_FMT_CAP_SAMPLEABLE) &&
                pl_fmt_is_ordered(candidate)) {
                rgb10a2 = candidate;
                break;
            }
        }
        if(rgb8&&std::getenv("MOTIONCAM_GALLERY_PERF_PROFILE"))
            spdlog::info("GALLERY_PERF event=native_format name={} texel_size={} texel_align={} emulated={}",
                rgb8->name,rgb8->texel_size,rgb8->texel_align,rgb8->emulated);
        if(rgb10a2&&std::getenv("MOTIONCAM_GALLERY_PERF_PROFILE"))
            spdlog::info("GALLERY_PERF event=native_packed_format name={} texel_size={} emulated={}",
                rgb10a2->name,rgb10a2->texel_size,rgb10a2->emulated);
        if (!swapchain || !renderer || !rgb8 || !rgba8 || !rgb10a2) {
            spdlog::warn("Gallery native Vulkan presenter initialization failed");
            mHealthy = false;
        }

        uint64_t displayedSerial = 0;
        uint64_t uploadedFrameSerial = 0;
        uint64_t uploadedOverlaySerial = 0;
        int lastSubmittedFrameIndex = -1;
        unsigned presentCount = 0;
        bool placeholderLogged = false;
        auto presentWindow = std::chrono::steady_clock::now();
        double setupTotal = 0.0, uploadTotal = 0.0, startTotal = 0.0,
            drawTotal = 0.0, submitTotal = 0.0, swapTotal = 0.0,
            releaseTotal = 0.0;
        while (mHealthy) {
            QImage image;
            QImage overlayImage;
            uint64_t overlaySerial = 0;
            uint64_t frameSerial = 0;
            std::shared_ptr<motioncam::VulkanGalleryGpuBuffer> gpuBuffer;
            QRectF target;
            bool smooth = true;
            bool placeholder = false;
            int outputWidth = 0, outputHeight = 0;
            {
                std::unique_lock lock(mMutex);
                mReady.wait(lock, [&] { return mStopping || mSerial != displayedSerial; });
                if (mStopping) break;
                displayedSerial = mSerial;
                image = mImage;
                overlayImage = mOverlay;
                overlaySerial = mOverlaySerial;
                frameSerial = mFrameSerial;
                gpuBuffer = mGpuBuffer;
                mRenderingGpuBuffer = bool(gpuBuffer);
                target = mTarget;
                smooth = mSmooth;
                placeholder = mPlaceholder;
                outputWidth = mOutputWidth;
                outputHeight = mOutputHeight;
            }
            std::shared_ptr<void> frameCompletion(nullptr,
                [this, &gpuBuffer](void*) {
                    gpuBuffer.reset();
                    std::lock_guard lock(mMutex);
                    mRenderingGpuBuffer = false;
                    mFrameDone.notify_all();
                });
            if ((!gpuBuffer && (image.isNull() ||
                 image.format() != QImage::Format_RGB888)) ||
                outputWidth <= 0 || outputHeight <= 0) continue;
            if (placeholder) target = QRectF(0, 0, outputWidth, outputHeight);

            if (overlaySerial != uploadedOverlaySerial) {
                if (!overlayImage.isNull() &&
                    overlayImage.format() == QImage::Format_RGBA8888_Premultiplied) {
                    pl_tex_params overlayParams{};
                    overlayParams.w = overlayImage.width();
                    overlayParams.h = overlayImage.height();
                    overlayParams.format = rgba8;
                    overlayParams.sampleable = true;
                    overlayParams.host_writable = true;
                    if (!pl_tex_recreate(vk->gpu, &overlayTexture, &overlayParams)) {
                        mHealthy = false;
                        break;
                    }
                    pl_tex_transfer_params overlayTransfer{};
                    overlayTransfer.tex = overlayTexture;
                    overlayTransfer.ptr = const_cast<uchar*>(overlayImage.constBits());
                    overlayTransfer.row_pitch = overlayImage.bytesPerLine();
                    if (!pl_tex_upload(vk->gpu, &overlayTransfer)) {
                        mHealthy = false;
                        break;
                    }
                } else {
                    pl_tex_destroy(vk->gpu, &overlayTexture);
                }
                uploadedOverlaySerial = overlaySerial;
            }

            const auto setupStarted = std::chrono::steady_clock::now();
            pl_buf imported = nullptr;
            const auto uploadStarted = std::chrono::steady_clock::now();
            if (frameSerial != uploadedFrameSerial || !haveFrameTexture) {
                // Stills arrive far apart and do not need three resident
                // full-resolution textures. Reuse the first slot after its
                // last queued use completes.
                const unsigned nextSlot = gpuBuffer && gpuBuffer->staticFrame
                    ? 0 : haveFrameTexture
                        ? (frameTextureSlot + 1) % frameTextures.size() : 0;
                pl_tex& uploadTexture = frameTextures[nextSlot];
                if (uploadTexture)
                    while (pl_tex_poll(vk->gpu, uploadTexture, UINT64_MAX)) {}
                pl_tex_params tex{};
                tex.w = gpuBuffer ? int(gpuBuffer->width) : image.width();
                tex.h = gpuBuffer ? int(gpuBuffer->height) : image.height();
                tex.format = gpuBuffer && gpuBuffer->rgb10a2 ? rgb10a2 : rgb8;
                tex.sampleable = true;
                tex.host_writable = true;
                if (!pl_tex_recreate(vk->gpu, &uploadTexture, &tex)) {
                    mHealthy = false;
                    break;
                }
                pl_tex_transfer_params transfer{};
                transfer.tex = uploadTexture;
                int importFd = -1;
                if (gpuBuffer) {
                    importFd = ::dup(gpuBuffer->fd);
                    if (importFd >= 0) {
                        pl_buf_params params{};
                        params.size = gpuBuffer->bytes;
                        params.import_handle = PL_HANDLE_DMA_BUF;
                        params.shared_mem.handle.fd = importFd;
                        params.shared_mem.size = gpuBuffer->allocationBytes;
                        params.shared_mem.offset = gpuBuffer->allocationOffset;
                        params.shared_mem.drm_format_mod = gpuBuffer->drmFormatModifier;
                        imported = pl_buf_create(vk->gpu, &params);
                    }
                    transfer.buf = imported;
                    transfer.row_pitch = size_t(gpuBuffer->width) *
                        (gpuBuffer->rgb10a2 ? 4u : 3u);
                } else {
                    transfer.ptr = const_cast<uchar*>(image.constBits());
                    transfer.row_pitch = image.bytesPerLine();
                }
                const bool uploaded = (!gpuBuffer || imported) &&
                    pl_tex_upload(vk->gpu, &transfer);
                if (!uploaded) {
                    spdlog::warn("Gallery native Vulkan frame upload failed: gpu_buffer={} import_fd={} imported={}",
                        bool(gpuBuffer), importFd, bool(imported));
                    if (importFd >= 0 && !imported) ::close(importFd);
                    pl_buf_destroy(vk->gpu, &imported);
                    mHealthy = false;
                    break;
                }
                // The producer and presenter use separate Vulkan devices.
                // Do not let the producer reuse this allocation while the
                // upload is still reading from it.
                if (imported)
                    while (pl_buf_poll(vk->gpu, imported, UINT64_MAX)) {}
                if (imported && gpuBuffer->debugSampleHash) {
                    const uint64_t importedHash = samplePackedFrame(vk->gpu,
                        imported, gpuBuffer->width, gpuBuffer->height);
                    if (importedHash != gpuBuffer->debugSampleHash)
                        spdlog::error("GALLERY_PERF event=native_buffer_mismatch frame={} producer_hash={} presenter_hash={}",
                            gpuBuffer->sourceFrameIndex, gpuBuffer->debugSampleHash,
                            importedHash);
                }
                frameTextureSlot = nextSlot;
                haveFrameTexture = true;
                uploadedFrameSerial = frameSerial;
            }
            const auto uploadedAt = std::chrono::steady_clock::now();

            if (!pl_swapchain_resize(swapchain, &outputWidth, &outputHeight)) {
                pl_buf_destroy(vk->gpu, &imported);
                mHealthy = false;
                break;
            }
            pl_swapchain_frame swapFrame{};
            if (!pl_swapchain_start_frame(swapchain, &swapFrame)) {
                pl_buf_destroy(vk->gpu, &imported);
                continue;
            }
            const auto startedAt = std::chrono::steady_clock::now();
            pl_frame output{};
            pl_frame_from_swapchain(&output, &swapFrame);
            const float background[3] = {96.0f / 255.0f,
                96.0f / 255.0f, 96.0f / 255.0f};
            pl_frame_clear(vk->gpu, &output, background);
            output.crop = {
                float(target.left()), float(target.top()),
                float(target.right()), float(target.bottom())
            };
            pl_overlay overlay{};
            pl_overlay_part overlayPart{};
            if (overlayTexture) {
                overlay.tex = overlayTexture;
                overlay.mode = PL_OVERLAY_NORMAL;
                overlay.coords = PL_OVERLAY_COORDS_DST_FRAME;
                overlay.repr = pl_color_repr_rgb;
                overlay.color.primaries = PL_COLOR_PRIM_BT_709;
                overlay.color.transfer = PL_COLOR_TRC_SRGB;
                overlayPart.src = {0, 0, float(overlayImage.width()),
                    float(overlayImage.height())};
                overlayPart.dst = {0, 0, float(outputWidth), float(outputHeight)};
                overlay.parts = &overlayPart;
                overlay.num_parts = 1;
                output.overlays = &overlay;
                output.num_overlays = 1;
            }
            pl_frame input{};
            input.num_planes = 1;
            input.planes[0].texture = frameTextures[frameTextureSlot];
            input.planes[0].components = gpuBuffer && gpuBuffer->rgb10a2 ? 4 : 3;
            input.planes[0].component_mapping[0] = 0;
            input.planes[0].component_mapping[1] = 1;
            input.planes[0].component_mapping[2] = 2;
            if (gpuBuffer && gpuBuffer->rgb10a2)
                input.planes[0].component_mapping[3] = 3;
            input.repr = pl_color_repr_rgb;
            input.rotation = gpuBuffer
                ? gpuBuffer->orientation == 90 ? PL_ROTATION_90
                : gpuBuffer->orientation == 180 ? PL_ROTATION_180
                : gpuBuffer->orientation == 270 ? PL_ROTATION_270
                : PL_ROTATION_0
                : PL_ROTATION_0;
            input.color.primaries = PL_COLOR_PRIM_BT_709;
            input.color.transfer = PL_COLOR_TRC_SRGB;
            auto renderParams = pl_render_fast_params;
            renderParams.upscaler = smooth ? &pl_filter_bicubic : &pl_filter_nearest;
            renderParams.downscaler = smooth ? &pl_filter_bicubic : &pl_filter_nearest;
            renderParams.correct_subpixel_offsets = true;
            renderParams.border = PL_CLEAR_COLOR;
            renderParams.background = PL_CLEAR_COLOR;
            renderParams.background_color[0] = 96.0f / 255.0f;
            renderParams.background_color[1] = 96.0f / 255.0f;
            renderParams.background_color[2] = 96.0f / 255.0f;
            const bool rendered = pl_render_image(renderer, &input, &output,
                &renderParams);
            const auto drawnAt = std::chrono::steady_clock::now();
            const bool submitted = pl_swapchain_submit_frame(swapchain);
            const auto renderedAt = std::chrono::steady_clock::now();
            if (std::getenv("MOTIONCAM_GALLERY_PERF_DRAW_TRACE"))
                spdlog::info("GALLERY_PERF event=native_draw_trace draw_ms={:.3f} submit_ms={:.3f} target={}x{} output={}x{} smooth={}",
                    std::chrono::duration<double, std::milli>(drawnAt-startedAt).count(),
                    std::chrono::duration<double, std::milli>(renderedAt-drawnAt).count(),
                    target.width(), target.height(), outputWidth, outputHeight, smooth);
            if (!rendered || !submitted) {
                spdlog::warn("Gallery native Vulkan frame render failed: rendered={} submitted={}",
                    rendered, submitted);
                mHealthy = false;
                break;
            }
            pl_swapchain_swap_buffers(swapchain);
            const auto swappedAt = std::chrono::steady_clock::now();
            if (gpuBuffer && uploadedFrameSerial == frameSerial &&
                std::getenv("MOTIONCAM_GALLERY_PERF_PRESENT_TRACE")) {
                const int frameIndex = gpuBuffer->sourceFrameIndex;
                if (frameIndex <= lastSubmittedFrameIndex &&
                    frameIndex != lastSubmittedFrameIndex)
                    spdlog::error("GALLERY_PERF event=native_frame_order_error previous={} current={} serial={}",
                        lastSubmittedFrameIndex, frameIndex, frameSerial);
                spdlog::info("GALLERY_PERF event=native_frame_submit frame={} serial={} slot={} time_ns={}",
                    frameIndex, frameSerial, frameTextureSlot,
                    std::chrono::duration_cast<std::chrono::nanoseconds>(
                        swappedAt.time_since_epoch()).count());
                lastSubmittedFrameIndex = frameIndex;
            }
            if (placeholder && !placeholderLogged &&
                std::getenv("MOTIONCAM_GALLERY_PERF_PROFILE")) {
                spdlog::info("GALLERY_PERF event=native_placeholder_present");
                placeholderLogged = true;
            }
            pl_buf_destroy(vk->gpu, &imported);
            const auto releasedAt = std::chrono::steady_clock::now();
            ++presentCount;
            setupTotal += std::chrono::duration<double, std::milli>(uploadStarted - setupStarted).count();
            uploadTotal += std::chrono::duration<double, std::milli>(uploadedAt - uploadStarted).count();
            startTotal += std::chrono::duration<double, std::milli>(startedAt - uploadedAt).count();
            drawTotal += std::chrono::duration<double, std::milli>(drawnAt - startedAt).count();
            submitTotal += std::chrono::duration<double, std::milli>(renderedAt - drawnAt).count();
            swapTotal += std::chrono::duration<double, std::milli>(swappedAt - renderedAt).count();
            releaseTotal += std::chrono::duration<double, std::milli>(releasedAt - swappedAt).count();
            const auto now = releasedAt;
            const double seconds = std::chrono::duration<double>(now - presentWindow).count();
            if (seconds >= 1.0) {
                spdlog::info("GALLERY_PERF event=native_present frames={} fps={:.1f} setup_ms={:.2f} upload_ms={:.2f} start_ms={:.2f} draw_ms={:.2f} submit_ms={:.2f} swap_ms={:.2f} release_ms={:.2f}",
                    presentCount, presentCount / seconds, setupTotal / presentCount,
                    uploadTotal / presentCount,
                    startTotal / presentCount,
                    drawTotal / presentCount, submitTotal / presentCount,
                    swapTotal / presentCount,
                    releaseTotal / presentCount);
                presentCount = 0;
                presentWindow = now;
                setupTotal = uploadTotal = startTotal = drawTotal = submitTotal =
                    swapTotal = releaseTotal = 0.0;
            }
        }
        if (vk)
            for (auto& texture : frameTextures)
                pl_tex_destroy(vk->gpu, &texture);
        if (vk) pl_tex_destroy(vk->gpu, &overlayTexture);
        if (renderer) pl_renderer_destroy(&renderer);
        pl_swapchain_destroy(&swapchain);
        pl_vulkan_destroy(&vk);
        pl_log_destroy(&log);
    }

    std::atomic_bool mHealthy{true};
    VkSurfaceKHR mPreparedSurface = VK_NULL_HANDLE;
    pl_log mPreparedLog = nullptr;
    pl_vulkan mPreparedVk = nullptr;
    pl_swapchain mPreparedSwapchain = nullptr;
    std::mutex mMutex;
    std::condition_variable mReady;
    std::condition_variable mFrameDone;
    std::thread mThread;
    QImage mImage;
    QImage mOverlay;
    uint64_t mOverlaySerial = 0;
    uint64_t mFrameSerial = 0;
    std::shared_ptr<motioncam::VulkanGalleryGpuBuffer> mGpuBuffer;
    QRectF mTarget;
    QRectF mLogicalTarget;
    bool mSmooth = true;
    bool mPlaceholder = true;
    bool mStopping = false;
    bool mRenderingGpuBuffer = false;
    uint64_t mSerial = 0;
    int mOutputWidth = 0, mOutputHeight = 0;
};

GalleryVulkanVideo::GalleryVulkanVideo(QWidget* parent) : QWidget(parent) {
    QPalette background=palette();
    background.setColor(QPalette::Window,QColor(96,96,96));
    setPalette(background);
    setAutoFillBackground(true);
    mInstance = std::make_unique<QVulkanInstance>();
    mInstance->setApiVersion(QVersionNumber(1, 2));
    if (!mInstance->create()) {
        spdlog::warn("Gallery native Vulkan instance unavailable: {}",
            int(mInstance->errorCode()));
        return;
    }
    mSurface = new GalleryVulkanSurface(mInstance.get());
    mContainer = QWidget::createWindowContainer(mSurface, this);
    mSurface->prewarmDevice();
    mContainer->setPalette(background);
    mContainer->setAutoFillBackground(true);
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->addWidget(mContainer);
}

GalleryVulkanVideo::~GalleryVulkanVideo() {
    delete mContainer;
    mSurface = nullptr;
    mInstance.reset();
}

bool GalleryVulkanVideo::usable() const {
    return mSurface && mSurface->usable();
}

qreal GalleryVulkanVideo::pixelRatio() const {
    return mSurface ? mSurface->pixelRatio() : devicePixelRatioF();
}


void GalleryVulkanVideo::setFrame(const QImage& image, const QRectF& target,
                                 bool smooth) {
    if (mSurface) mSurface->setFrame(image, target, smooth);
}

void GalleryVulkanVideo::setGpuFrame(
        std::shared_ptr<motioncam::VulkanGalleryGpuBuffer> frame,
        const QRectF& target, bool smooth, const QImage& overlay) {
    if (mSurface) mSurface->setGpuFrame(std::move(frame), target, smooth, overlay);
}

void GalleryVulkanVideo::setOverlay(const QImage& image) {
    if (mSurface) mSurface->setOverlay(image);
}



void GalleryVulkanVideo::drainGpuFrames() {
    if (mSurface) mSurface->drainGpuFrames();
}

#else

GalleryVulkanVideo::GalleryVulkanVideo(QWidget* parent) : QWidget(parent) {}
GalleryVulkanVideo::~GalleryVulkanVideo() = default;
bool GalleryVulkanVideo::usable() const { return false; }
qreal GalleryVulkanVideo::pixelRatio() const { return devicePixelRatioF(); }
void GalleryVulkanVideo::setFrame(const QImage&, const QRectF&, bool) {}
void GalleryVulkanVideo::setGpuFrame(
    std::shared_ptr<motioncam::VulkanGalleryGpuBuffer>, const QRectF&, bool,
    const QImage&) {}
void GalleryVulkanVideo::setOverlay(const QImage&) {}
void GalleryVulkanVideo::drainGpuFrames() {}

#endif
