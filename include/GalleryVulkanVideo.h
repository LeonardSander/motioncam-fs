#pragma once

#include <QImage>
#include <QRectF>
#include <QWidget>

#include <memory>
#include <functional>

class QVulkanInstance;
class GalleryVulkanSurface;
namespace motioncam { struct VulkanGalleryGpuBuffer; }

// Native Vulkan presentation keeps swapchain pacing off the GUI thread.
// The widget owns the window container and destroys it before the instance.
class GalleryVulkanVideo final : public QWidget {
public:
    explicit GalleryVulkanVideo(QWidget* parent = nullptr);
    ~GalleryVulkanVideo() override;

    bool usable() const;
    qreal pixelRatio() const;
    void setFrame(const QImage& image, const QRectF& target, bool smooth);
    void setGpuFrame(std::shared_ptr<motioncam::VulkanGalleryGpuBuffer> frame,
                     const QRectF& target, bool smooth,
                     const QImage& overlay = {});
    void setOverlay(const QImage& image);
    void drainGpuFrames();

private:
#if defined(MOTIONCAM_HAS_DIRECTLOG_PLACEBO) && QT_CONFIG(vulkan)
    std::unique_ptr<QVulkanInstance> mInstance;
#endif
    GalleryVulkanSurface* mSurface = nullptr;
    QWidget* mContainer = nullptr;
};
