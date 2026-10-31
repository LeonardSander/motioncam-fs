#include "GalleryVulkanVideo.h"
#include "SharedVulkanDevice.h"
#include <cstring>
#include "WidgetPaintSignature.h"
#include "VulkanGalleryPreview.h"

#include <QLayout>
#include <QPainter>
#include <QElapsedTimer>
#include <QAbstractButton>
#include <QAbstractSlider>
#include <QLabel>
#include <QVariant>
#include <algorithm>
#include <cmath>

void invalidateGalleryOverlay(QWidget& widget) {
    constexpr auto revision="motioncamGalleryOverlayRevision";
    widget.setProperty(revision,widget.property(revision).toULongLong()+1);
}

// Keep capture outside the Vulkan backend guard so offscreen pixel tests and
// builds without libplacebo use exactly the same layout and fade implementation.
GalleryOverlayCapture captureGalleryOverlay(
        QWidget& viewport, const std::vector<GalleryOverlayRoot>& roots, qreal ratio, GalleryOverlayCache* cache) {
    if(cache)for(auto it=cache->begin();it!=cache->end();){
        if(!it.value().widget)it=cache->erase(it);else ++it;
    }
    // Captured native controls receive input but do not repaint the backing
    // store. Enable paint events only for this synchronous offscreen render;
    // setUpdatesEnabled(true) would also schedule an on-screen repaint.
    struct RestoreUpdates {
        std::vector<QWidget*> widgets;
        RestoreUpdates(QWidget* root,bool children=true) {
            auto list=children?root->findChildren<QWidget*>():QList<QWidget*>{};
            list.prepend(root);
            for(auto* widget:list)if(widget->testAttribute(Qt::WA_UpdatesDisabled)){
                widgets.push_back(widget);widget->setAttribute(Qt::WA_UpdatesDisabled,false);
            }
        }
        ~RestoreUpdates(){for(auto* widget:widgets)widget->setAttribute(Qt::WA_UpdatesDisabled,true);}
    };
    // The overlay and native-window container are siblings in the gallery.
    // Stay in their parent's logical coordinates so native/global window
    // frame offsets cannot enter the capture's viewport coordinates.
    const auto positionInViewport = [&viewport](QWidget* widget) {
        // QWidget::mapTo can cross a native QWindow boundary on Android.
        // Both widgets share the gallery tree, so logical positions suffice.
        auto* anchor=viewport.parentWidget();
        const auto relative=[anchor](QWidget* child){
            QPoint position;
            for(;child&&child!=anchor;child=child->parentWidget())position+=child->pos();
            return position;
        };
        return relative(widget)-relative(&viewport);
    };
    struct Band { QRect area; const GalleryOverlayRoot* root; };
    std::vector<Band> bands;
    for (const auto& root : roots) {
        if (!root.widget->isVisible() || root.opacity <= 0.0) continue;
        root.widget->ensurePolished();
        if (auto* layout = root.widget->layout()) layout->activate();
        const QPoint position = positionInViewport(root.widget);
        const QRect bounds = root.widget->rect().translated(position).intersected(viewport.rect());
        std::vector<QRect> occupied;
        const auto children = root.widget->findChildren<QWidget*>(QString(), Qt::FindDirectChildrenOnly);
        if (children.empty()) {
            if (!bounds.isEmpty()) occupied.push_back(bounds);
        } else for (auto* child : children) {
            if (!child->isVisible() || root.excluded.contains(child->geometry())) continue;
            const QRect area = child->geometry().translated(position).intersected(bounds);
            if (!area.isEmpty()) occupied.push_back(area);
        }
        std::sort(occupied.begin(), occupied.end(), [](const QRect& a, const QRect& b) {
            return a.top() < b.top();
        });
        for (const auto& area : occupied) {
            if (!bands.empty() && bands.back().root == &root &&
                area.top() <= bands.back().area.bottom() + 1)
                bands.back().area = bands.back().area.united(area);
            else bands.push_back({area, &root});
        }
    }
    const auto pixelRect = [](const QRect& band, qreal scale) {
        const int x = int(std::floor(band.x() * scale));
        const int y = int(std::floor(band.y() * scale));
        return QRect(x, y, int(std::ceil((band.x() + band.width()) * scale)) - x,
                     int(std::ceil((band.y() + band.height()) * scale)) - y);
    };
    int width = 0, height = 0;
    for (const auto& band : bands) {
        const auto pixels = pixelRect(band.area, ratio);
        width = std::max(width, pixels.width());
        height += pixels.height();
    }
    GalleryOverlayCapture result;
    result.image = QImage(std::max(1, width), std::max(1, height),
                          QImage::Format_ARGB32_Premultiplied);
    result.image.fill(Qt::transparent);
    QPainter painter(&result.image);
    int y = 0;
    for (const auto& band : bands) {
        const auto& root = *band.root;
        const QRect pixels = pixelRect(band.area, ratio);
        const QPoint position = positionInViewport(root.widget);
        const QRegion region = QRegion(band.area.translated(-position)
            .intersected(root.widget->rect())) - root.excluded;
        if (region.isEmpty()) continue;
        const auto render = [&](QPainter& target, int offsetY) {
            target.save();
            target.setClipRect(QRect(0, offsetY, pixels.width(), pixels.height()));
            target.translate(-pixels.x(), offsetY - pixels.y());
            target.scale(ratio, ratio);
            const auto drawWidget = [&](QWidget* widget, const QRegion& localRegion) {
                const QPoint origin=positionInViewport(widget);
                const bool reusable=cache&&
                    (qobject_cast<QAbstractButton*>(widget)||qobject_cast<QAbstractSlider*>(widget)||
                     qobject_cast<QLabel*>(widget));
                if(!reusable){
                    QElapsedTimer timer;timer.start();++result.paintedWidgets;
                    RestoreUpdates updates(widget);
                    widget->render(&target,localRegion.boundingRect().topLeft()+origin,
                                   localRegion,QWidget::DrawChildren);
                    result.paintNanoseconds+=timer.nsecsElapsed();
                    return;
                }
                const QPointF deviceOrigin=target.worldTransform().map(QPointF(origin));
                const QPoint pixelOrigin(int(std::floor(deviceOrigin.x())),int(std::floor(deviceOrigin.y())));
                const QPointF phase=deviceOrigin-QPointF(pixelOrigin);
                const QPointF deviceEnd=target.worldTransform().map(QPointF(origin+QPoint(widget->width(),widget->height())));
                const QSize pixelSize(int(std::ceil(deviceEnd.x()))-pixelOrigin.x(),
                                      int(std::ceil(deviceEnd.y()))-pixelOrigin.y());
                quint64 signature=0;
                const auto mix=[&](quint64 value){signature^=value+0x9e3779b97f4a7c15ULL+(signature<<6)+(signature>>2);};
                mix(qHash(ratio));mix(qHash(phase.x()));mix(qHash(phase.y()));
                mix(motioncam::widgetPaintSignature(*widget));
                auto& entry=(*cache)[widget];
                if(entry.widget!=widget||entry.signature!=signature||entry.image.isNull()){
                    entry.widget=widget;entry.signature=signature;
                    entry.image=QImage(pixelSize,QImage::Format_ARGB32_Premultiplied);
                    entry.image.fill(Qt::transparent);
                    QElapsedTimer timer;timer.start();++result.paintedWidgets;
                    {RestoreUpdates updates(widget);
                     QPainter capture(&entry.image);capture.translate(phase);capture.scale(ratio,ratio);
                     widget->render(&capture,QPoint(),QRegion(widget->rect()),QWidget::DrawChildren);}
                    result.paintNanoseconds+=timer.nsecsElapsed();
                }else ++result.reusedWidgets;
                target.save();target.setClipRegion(localRegion.translated(origin),Qt::IntersectClip);
                target.resetTransform();
                target.drawImage(pixelOrigin,entry.image);
                target.restore();
            };
            if (root.childrenOnly) {
                // Capture the visible controls directly, without repeatedly
                // traversing a screen-sized transparent layout container.
                for (auto* child : root.widget->findChildren<QWidget*>(
                        QString(), Qt::FindDirectChildrenOnly)) {
                    if (!child->isVisible()) continue;
                    const QRegion childRegion = region.translated(-child->pos()) & child->rect();
                    if (childRegion.isEmpty()) continue;
                    drawWidget(child,childRegion);
                }
            } else {
                drawWidget(root.widget,region);
            }
            target.restore();
        };
        if (root.opacity >= 1.0) {
            render(painter, y);
        } else {
            // QWidget::render with painter opacity allocates an intermediate
            // for the whole widget tree. Fade only the captured band instead.
            QImage layer(pixels.size(), result.image.format());
            layer.fill(Qt::transparent);
            { QPainter capture(&layer); render(capture, 0); }
            painter.setOpacity(root.opacity);
            painter.drawImage(0, y, layer);
            painter.setOpacity(1.0);
        }
        result.parts.push_back({QRectF(0, y, pixels.width(), pixels.height()),
            QRectF(pixelRect(band.area, ratio))});
        y += pixels.height();
    }
    return result;
}

#if defined(MOTIONCAM_HAS_DIRECTLOG_PLACEBO) && QT_CONFIG(vulkan)

#include <QEvent>
#include <QDebug>
#include <QGuiApplication>
#include <QPlatformSurfaceEvent>
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

namespace {
void galleryVulkanLog(void*, enum pl_log_level level, const char* message) {
#ifdef __ANDROID__
    if (level <= PL_LOG_WARN)
        qWarning().noquote() << "Gallery libplacebo Vulkan:" << message;
#else
    (void) level;
    (void) message;
#endif
}

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
    explicit GalleryVulkanSurface(QVulkanInstance* instance, pl_vulkan sharedDevice)
        : mSharedDevice(sharedDevice) {
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
    ~GalleryVulkanSurface() override { releaseSurfaceResources(); }

    void releaseSurfaceResources() {
        stopRenderer();
        pl_swapchain_destroy(&mPreparedSwapchain);
        if (!mSharedDevice) pl_vulkan_destroy(&mPreparedVk);
        mPreparedVk = nullptr;
        pl_log_destroy(&mPreparedLog);
        mPreparedSurface = VK_NULL_HANDLE;
    }

    void prewarmDevice() {
        releaseSurfaceResources();
        create();
        mPreparedSurface = QVulkanInstance::surfaceForWindow(this);
        if (!mPreparedSurface) return;
        pl_log_params logParams{};
        logParams.log_cb = galleryVulkanLog;
        logParams.log_level = PL_LOG_WARN;
        mPreparedLog = pl_log_create(PL_API_VER, &logParams);
        if (!mPreparedLog) return;
        mPreparedVk = mSharedDevice;
        if (!mPreparedVk) {
            pl_vulkan_params params = pl_vulkan_default_params;
            params.get_proc_addr = motioncam::galleryVulkanGetInstanceProcAddr();
            params.instance = vulkanInstance()->vkInstance();
            params.surface = mPreparedSurface;
            mPreparedVk = pl_vulkan_create(mPreparedLog, &params);
        }
        if (mPreparedVk) {
            pl_vulkan_swapchain_params swapchainParams{};
            swapchainParams.surface = mPreparedSurface;
            swapchainParams.present_mode = VK_PRESENT_MODE_MAILBOX_KHR;
            mPreparedSwapchain = pl_vulkan_create_swapchain(
                mPreparedVk, &swapchainParams);
        }
        // Android can expose the child surface before its native window is
        // ready for a swapchain. Keep the presenter eligible for a retry.
#ifndef __ANDROID__
        mHealthy = bool(mPreparedVk && mPreparedSwapchain);
#endif
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
                     const QImage& overlay = {},
                     const std::vector<GalleryOverlayPart>& parts = {}) {
        {
            std::lock_guard lock(mMutex);
            mImage = {};
            if (mGpuBuffer != frame) {
                mGpuBuffer = std::move(frame);
                ++mFrameSerial;
            }
            if (mOverlay.cacheKey() != overlay.cacheKey() || mOverlayParts != parts) {
                mOverlay = overlay;
                mOverlayParts = parts;
                ++mOverlaySerial;
            }
            mPlaceholder = false;
            mLogicalTarget = target;
            mSmooth = smooth;
            updateOutputGeometryLocked();
            ++mSerial;
        }
        mReady.notify_one();
    }

    void setOverlay(const QImage& image,
                    const std::vector<GalleryOverlayPart>& parts, bool redraw) {
        {
            std::lock_guard lock(mMutex);
            if(mOverlay.cacheKey()==image.cacheKey()&&mOverlayParts==parts)return;
            mOverlay = image;
            mOverlayParts = parts;
            ++mOverlaySerial;
            if (redraw) ++mSerial;
        }
        if (redraw) mReady.notify_one();
    }


    void drainGpuFrames() {
        std::unique_lock lock(mMutex);
        mGpuBuffer.reset();
        mImage = {};
        const uint64_t request = ++mDrainRequested;
        ++mSerial;
        mReady.notify_one();
        mFrameDone.wait(lock, [&] {
            return mDrainCompleted >= request || !mRendererRunning;
        });
    }

protected:
    bool event(QEvent* event) override {
        if (event->type() == QEvent::PlatformSurface &&
            static_cast<QPlatformSurfaceEvent*>(event)->surfaceEventType() ==
                QPlatformSurfaceEvent::SurfaceAboutToBeDestroyed)
            releaseSurfaceResources();
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
        if (!surface) {
            QTimer::singleShot(50, this, [this] { startRendererIfReady(); });
            return;
        }
        if (surface != mPreparedSurface || !mPreparedVk ||
            !mPreparedSwapchain)
            prewarmDevice();
        if (!mPreparedVk || !mPreparedSwapchain) {
            QTimer::singleShot(50, this, [this] { startRendererIfReady(); });
            return;
        }
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
#ifdef __ANDROID__
            qWarning() << "Gallery Vulkan initial swapchain frame unavailable"
                       << "resized" << resized;
#endif
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
        {
            std::lock_guard lock(mMutex);
            mRendererRunning = true;
        }
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
            if (!mSharedDevice) pl_vulkan_destroy(&mPreparedVk);
            mPreparedVk = nullptr;
            pl_log_destroy(&mPreparedLog);
            pl_log_params logParams{};
            logParams.log_cb = galleryVulkanLog;
            logParams.log_level = PL_LOG_WARN;
            log = pl_log_create(PL_API_VER, &logParams);
        }
        pl_renderer renderer = nullptr;
        // Keep several uploads alive while earlier frames are being rendered.
        // Reusing one texture can change the image still sampled by a queued
        // mailbox present, even when the frame serials arrive in order.
        std::array<pl_tex, 3> frameTextures{};
        // Imported buffers and their producer allocations must remain alive
        // until the corresponding texture upload has completed. Keep them
        // per slot so frame uploads can overlap instead of waiting below on
        // every imported buffer.
        std::array<pl_buf, 3> importedFrameBuffers{};
        std::array<std::shared_ptr<motioncam::VulkanGalleryGpuBuffer>, 3>
            importedFrameOwners{};
        unsigned frameTextureSlot = 0;
        bool haveFrameTexture = false;
        pl_tex overlayTexture = nullptr;
        if (log && !vk) {
            vk = mSharedDevice;
            if (!vk) {
                pl_vulkan_params params = pl_vulkan_default_params;
                params.get_proc_addr = motioncam::galleryVulkanGetInstanceProcAddr();
                params.instance = instance;
                params.surface = surface;
                vk = pl_vulkan_create(log, &params);
            }
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
        const pl_fmt bgra8 = vk ? pl_find_named_fmt(vk->gpu,"bgra8") : nullptr;
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
        if (!swapchain || !renderer || !rgba8) {
            spdlog::warn("Gallery native Vulkan presenter initialization failed");
#ifdef __ANDROID__
            qWarning() << "Gallery Vulkan presenter initialization failed"
                       << bool(swapchain) << bool(renderer) << bool(rgb8)
                       << bool(rgba8) << bool(rgb10a2);
#endif
            mHealthy = false;
        }

        // Release imports before their producer/device leases, on the thread
        // that owns the presenter. Reuse this for drains, stills and shutdown.
        auto releaseSlot = [&](size_t slot, bool destroyTexture) {
            if (frameTextures[slot])
                while (pl_tex_poll(vk->gpu, frameTextures[slot], UINT64_MAX)) {}
            if (destroyTexture || (importedFrameOwners[slot] &&
                                   importedFrameOwners[slot]->texture))
                pl_tex_destroy(vk->gpu, &frameTextures[slot]);
            pl_buf_destroy(vk->gpu, &importedFrameBuffers[slot]);
            importedFrameOwners[slot].reset();
        };

        uint64_t displayedSerial = 0;
        uint64_t uploadedFrameSerial = 0;
        uint64_t uploadedOverlaySerial = 0;
        int lastSubmittedFrameIndex = -1;
        unsigned presentCount = 0;
        unsigned sharedImageFrames = 0, bufferUploadFrames = 0, hostUploadFrames = 0;
        bool placeholderLogged = false;
        auto presentWindow = std::chrono::steady_clock::now();
        double setupTotal = 0.0, uploadTotal = 0.0, startTotal = 0.0,
            drawTotal = 0.0, submitTotal = 0.0, swapTotal = 0.0,
            releaseTotal = 0.0;
        while (mHealthy) {
            QImage image;
            QImage overlayImage;
            std::vector<GalleryOverlayPart> capturedOverlayParts;
            uint64_t overlaySerial = 0;
            uint64_t frameSerial = 0;
            uint64_t drainRequest = 0;
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
                drainRequest = mDrainRequested;
                image = mImage;
                overlayImage = mOverlay;
                capturedOverlayParts = mOverlayParts;
                overlaySerial = mOverlaySerial;
                frameSerial = mFrameSerial;
                gpuBuffer = mGpuBuffer;
                target = mTarget;
                smooth = mSmooth;
                placeholder = mPlaceholder;
                outputWidth = mOutputWidth;
                outputHeight = mOutputHeight;
            }
            if (drainRequest > mDrainCompleted) {
                if (vk)
                    for (size_t slot = 0; slot < frameTextures.size(); ++slot)
                        releaseSlot(slot, true);
                haveFrameTexture = false;
                uploadedFrameSerial = 0;
                std::lock_guard lock(mMutex);
                mDrainCompleted = drainRequest;
                mFrameDone.notify_all();
            }
            if ((!gpuBuffer && (image.isNull() ||
                 (image.format() != QImage::Format_RGB888 &&
                  image.format() != QImage::Format_RGBA8888))) ||
                outputWidth <= 0 || outputHeight <= 0) continue;
            if (placeholder) target = QRectF(0, 0, outputWidth, outputHeight);

            const auto setupStarted = std::chrono::steady_clock::now();
            if (overlaySerial != uploadedOverlaySerial) {
                if (!overlayImage.isNull()) {
                    // Keep Qt's native raster format through upload. Painting
                    // directly into RGBA makes Qt swizzle pixels repeatedly.
                    QImage upload = overlayImage;
                    pl_fmt format = nullptr;
#if Q_BYTE_ORDER == Q_LITTLE_ENDIAN
                    if (upload.format() == QImage::Format_ARGB32_Premultiplied &&
                        bgra8 && (bgra8->caps & PL_FMT_CAP_SAMPLEABLE))
                        format = bgra8;
#endif
                    if (!format) {
                        format = rgba8;
                        if (upload.format() != QImage::Format_RGBA8888_Premultiplied)
                            upload = upload.convertToFormat(QImage::Format_RGBA8888_Premultiplied);
                    }
                    if(uploadedOverlaySerial==0&&std::getenv("MOTIONCAM_GALLERY_PERF_PROFILE"))
                        spdlog::info("GALLERY_PERF event=native_overlay_format name={} converted={}",
                            format->name,upload.cacheKey()!=overlayImage.cacheKey());
                    pl_tex_params overlayParams{};
                    overlayParams.w = overlayImage.width();
                    overlayParams.h = overlayImage.height();
                    overlayParams.format = format;
                    overlayParams.sampleable = true;
                    overlayParams.host_writable = true;
                    if (!pl_tex_recreate(vk->gpu, &overlayTexture, &overlayParams)) {
                        mHealthy = false;
                        break;
                    }
                    pl_tex_transfer_params overlayTransfer{};
                    overlayTransfer.tex = overlayTexture;
                    overlayTransfer.ptr = const_cast<uchar*>(upload.constBits());
                    overlayTransfer.row_pitch = upload.bytesPerLine();
                    if (!pl_tex_upload(vk->gpu, &overlayTransfer)) {
                        mHealthy = false;
                        break;
                    }
                } else {
                    pl_tex_destroy(vk->gpu, &overlayTexture);
                }
                uploadedOverlaySerial = overlaySerial;
            }

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
                if (gpuBuffer && gpuBuffer->staticFrame)
                    for (size_t slot = 1; slot < frameTextures.size(); ++slot)
                        releaseSlot(slot, true);
                releaseSlot(nextSlot, gpuBuffer && gpuBuffer->texture);
                if (gpuBuffer && gpuBuffer->texture) {
                    const auto producer = pl_vulkan_get(static_cast<pl_gpu>(gpuBuffer->gpu));
                    if (!producer || producer->device != vk->device) {
                        mHealthy = false;
                        break;
                    }
                    VkFormat format{};
                    VkImageUsageFlags usage{};
                    pl_vulkan_wrap_params wrap{};
                    wrap.image = pl_vulkan_unwrap(static_cast<pl_gpu>(gpuBuffer->gpu),
                        static_cast<pl_tex>(gpuBuffer->texture), &format, &usage);
                    wrap.width = gpuBuffer->width;
                    wrap.height = gpuBuffer->height;
                    wrap.format = format;
                    wrap.usage = usage;
                    uploadTexture = pl_vulkan_wrap(vk->gpu, &wrap);
                    if (!uploadTexture) {
                        if (gpuBuffer->rejectSharedImage) {
                            gpuBuffer->rejectSharedImage();
                            continue;
                        }
                        mHealthy = false;
                        break;
                    }
                    pl_vulkan_release_params release{};
                    release.tex = uploadTexture;
                    release.layout = VK_IMAGE_LAYOUT_GENERAL;
                    release.qf = VK_QUEUE_FAMILY_IGNORED;
                    VkSemaphore ready{};
                    static_assert(sizeof(ready) == sizeof(gpuBuffer->readySemaphore));
                    std::memcpy(&ready, &gpuBuffer->readySemaphore, sizeof(ready));
                    release.semaphore = {ready, 1};
                    pl_vulkan_release_ex(vk->gpu, &release);
                    importedFrameOwners[nextSlot] = gpuBuffer;
                    ++sharedImageFrames;
                    frameTextureSlot = nextSlot;
                    haveFrameTexture = true;
                    uploadedFrameSerial = frameSerial;
                } else {
                if (gpuBuffer) ++bufferUploadFrames;
                else if (!placeholder) ++hostUploadFrames;
                QImage uploadImage = image;
                if (!gpuBuffer && (!rgb8 ||
                    (uploadImage.format() != QImage::Format_RGB888 &&
                     uploadImage.format() != QImage::Format_RGBA8888)))
                    uploadImage = uploadImage.convertToFormat(QImage::Format_RGBA8888);
                pl_tex_params tex{};
                tex.w = gpuBuffer ? int(gpuBuffer->width) : image.width();
                tex.h = gpuBuffer ? int(gpuBuffer->height) : image.height();
                tex.format = gpuBuffer && gpuBuffer->rgb10a2 ? rgb10a2 :
                    uploadImage.format() == QImage::Format_RGBA8888 ? rgba8 : rgb8;
                tex.sampleable = true;
                tex.host_writable = true;
                if (!tex.format || !pl_tex_recreate(vk->gpu, &uploadTexture, &tex)) {
                    mHealthy = false;
                    break;
                }
                pl_tex_transfer_params transfer{};
                transfer.tex = uploadTexture;
                int importFd = -1;
                if (gpuBuffer) {
                    importFd = gpuBuffer->fd;
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
                    transfer.ptr = const_cast<uchar*>(uploadImage.constBits());
                    transfer.row_pitch = uploadImage.bytesPerLine();
                }
                const bool uploaded = (!gpuBuffer || imported) &&
                    pl_tex_upload(vk->gpu, &transfer);
                if (!uploaded) {
                    spdlog::warn("Gallery native Vulkan frame upload failed: gpu_buffer={} import_fd={} imported={}",
                        bool(gpuBuffer), importFd, bool(imported));
#ifdef __ANDROID__
                    qWarning() << "Gallery Vulkan frame upload failed"
                               << bool(gpuBuffer) << importFd << bool(imported);
#endif
                    pl_buf_destroy(vk->gpu, &imported);
                    mHealthy = false;
                    break;
                }
                if (imported && gpuBuffer->debugSampleHash) {
                    const uint64_t importedHash = samplePackedFrame(vk->gpu,
                        imported, gpuBuffer->width, gpuBuffer->height);
                    if (std::getenv("MOTIONCAM_GALLERY_PERF_PROFILE"))
                        spdlog::info("GALLERY_PERF event=native_buffer_compare frame={} producer_hash={} presenter_hash={}",
                            gpuBuffer->sourceFrameIndex, gpuBuffer->debugSampleHash, importedHash);
                    if (importedHash != gpuBuffer->debugSampleHash)
                        spdlog::error("GALLERY_PERF event=native_buffer_mismatch frame={} producer_hash={} presenter_hash={}",
                            gpuBuffer->sourceFrameIndex, gpuBuffer->debugSampleHash,
                            importedHash);
                }
                if (imported) {
                    importedFrameBuffers[nextSlot] = imported;
                    importedFrameOwners[nextSlot] = gpuBuffer;
                    imported = nullptr;
                }
                frameTextureSlot = nextSlot;
                haveFrameTexture = true;
                uploadedFrameSerial = frameSerial;
                }
            }
            const auto uploadedAt = std::chrono::steady_clock::now();

            if (!pl_swapchain_resize(swapchain, &outputWidth, &outputHeight)) {
#ifdef __ANDROID__
                qWarning() << "Gallery Vulkan frame resize failed"
                           << outputWidth << outputHeight;
#endif
                pl_buf_destroy(vk->gpu, &imported);
                mHealthy = false;
                break;
            }
            pl_swapchain_frame swapFrame{};
            if (!pl_swapchain_start_frame(swapchain, &swapFrame)) {
#ifdef __ANDROID__
                qWarning() << "Gallery Vulkan frame acquire failed";
#endif
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
            std::array<pl_overlay,1> overlays{};
            std::array<pl_overlay_part,1> overlayParts{};
            std::vector<pl_overlay_part> controlParts;
            if (overlayTexture) {
                auto& overlay=overlays[output.num_overlays];
                auto& overlayPart=overlayParts[output.num_overlays++];
                overlay.tex = overlayTexture;
                overlay.mode = PL_OVERLAY_NORMAL;
                overlay.coords = PL_OVERLAY_COORDS_DST_FRAME;
                overlay.repr = pl_color_repr_rgb;
                overlay.color.primaries = PL_COLOR_PRIM_BT_709;
                overlay.color.transfer = PL_COLOR_TRC_SRGB;
                if (capturedOverlayParts.empty()) {
                    overlayPart.src = {0, 0, float(overlayImage.width()), float(overlayImage.height())};
                    overlayPart.dst = {0, 0, float(outputWidth), float(outputHeight)};
                    overlay.parts = &overlayPart;
                    overlay.num_parts = 1;
                } else {
                    controlParts.reserve(capturedOverlayParts.size());
                    for (const auto& part : capturedOverlayParts) {
                        pl_overlay_part mapped{};
                        mapped.src = {float(part.source.x()), float(part.source.y()),
                            float(part.source.x() + part.source.width()),
                            float(part.source.y() + part.source.height())};
                        mapped.dst = {float(part.target.x()), float(part.target.y()),
                            float(part.target.x() + part.target.width()),
                            float(part.target.y() + part.target.height())};
                        controlParts.push_back(mapped);
                    }
                    overlay.parts = controlParts.data();
                    overlay.num_parts = int(controlParts.size());
                }
            }
            output.overlays=overlays.data();
            pl_frame input{};
            input.num_planes = 1;
            input.planes[0].texture = frameTextures[frameTextureSlot];
            input.planes[0].components = std::min(4,
                frameTextures[frameTextureSlot]->params.format->num_components);
            for (int channel = 0; channel < input.planes[0].components; ++channel)
                input.planes[0].component_mapping[channel] = channel;
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
                spdlog::info("GALLERY_PERF event=native_present frames={} fps={:.1f} setup_ms={:.2f} upload_ms={:.2f} start_ms={:.2f} draw_ms={:.2f} submit_ms={:.2f} swap_ms={:.2f} release_ms={:.2f} shared_images={} buffer_uploads={} host_uploads={}",
                    presentCount, presentCount / seconds, setupTotal / presentCount,
                    uploadTotal / presentCount,
                    startTotal / presentCount,
                    drawTotal / presentCount, submitTotal / presentCount,
                    swapTotal / presentCount,
                    releaseTotal / presentCount,
                    sharedImageFrames, bufferUploadFrames, hostUploadFrames);
                presentCount = 0;
                sharedImageFrames = bufferUploadFrames = hostUploadFrames = 0;
                presentWindow = now;
                setupTotal = uploadTotal = startTotal = drawTotal = submitTotal =
                    swapTotal = releaseTotal = 0.0;
            }
        }
        if (vk)
            for (size_t slot = 0; slot < frameTextures.size(); ++slot)
                releaseSlot(slot, true);
        if (vk) pl_tex_destroy(vk->gpu, &overlayTexture);
        if (renderer) pl_renderer_destroy(&renderer);
        pl_swapchain_destroy(&swapchain);
        if (!mSharedDevice) pl_vulkan_destroy(&vk);
        pl_log_destroy(&log);
        {
            std::lock_guard lock(mMutex);
            mRendererRunning = false;
            mFrameDone.notify_all();
        }
    }

    std::atomic_bool mHealthy{true};
    VkSurfaceKHR mPreparedSurface = VK_NULL_HANDLE;
    pl_log mPreparedLog = nullptr;
    pl_vulkan mPreparedVk = nullptr;
    pl_vulkan mSharedDevice = nullptr;
    pl_swapchain mPreparedSwapchain = nullptr;
    std::mutex mMutex;
    std::condition_variable mReady;
    std::condition_variable mFrameDone;
    std::thread mThread;
    QImage mImage;
    QImage mOverlay;
    std::vector<GalleryOverlayPart> mOverlayParts;
    uint64_t mOverlaySerial = 0;
    uint64_t mFrameSerial = 0;
    std::shared_ptr<motioncam::VulkanGalleryGpuBuffer> mGpuBuffer;
    QRectF mTarget;
    QRectF mLogicalTarget;
    bool mSmooth = true;
    bool mPlaceholder = true;
    bool mStopping = false;
    bool mRendererRunning = false;
    uint64_t mDrainRequested = 0;
    uint64_t mDrainCompleted = 0;
    uint64_t mSerial = 0;
    int mOutputWidth = 0, mOutputHeight = 0;
};

GalleryVulkanVideo::GalleryVulkanVideo(QWidget* parent) : QWidget(parent) {
    QPalette background=palette();
    background.setColor(QPalette::Window,QColor(96,96,96));
    setPalette(background);
    setAutoFillBackground(true);
    mInstance = std::make_unique<QVulkanInstance>();
    const auto shared = motioncam::sharedVulkanDevice();
    if (shared) mInstance->setVkInstance(shared->instance);
    mInstance->setApiVersion(QVersionNumber(1, 2));
    if (!mInstance->create()) {
        spdlog::warn("Gallery native Vulkan instance unavailable: {}",
            int(mInstance->errorCode()));
#ifdef __ANDROID__
        qWarning() << "Gallery Vulkan instance unavailable"
                   << int(mInstance->errorCode());
#endif
        return;
    }
    mSurface = new GalleryVulkanSurface(mInstance.get(), shared);
    mContainer = QWidget::createWindowContainer(mSurface, this);
#ifndef __ANDROID__
    mSurface->prewarmDevice();
#endif
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
        const QRectF& target, bool smooth, const QImage& overlay,
        const std::vector<GalleryOverlayPart>& parts) {
    if (mSurface) mSurface->setGpuFrame(std::move(frame), target, smooth, overlay, parts);
}

void GalleryVulkanVideo::setOverlay(const QImage& image,
                                    const std::vector<GalleryOverlayPart>& parts, bool redraw) {
    if (mSurface) mSurface->setOverlay(image,parts,redraw);
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
    const QImage&, const std::vector<GalleryOverlayPart>&) {}
void GalleryVulkanVideo::setOverlay(const QImage&,
                                  const std::vector<GalleryOverlayPart>&, bool) {}
void GalleryVulkanVideo::drainGpuFrames() {}

#endif
