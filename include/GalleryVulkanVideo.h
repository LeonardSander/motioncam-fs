#pragma once

#include <QImage>
#include <QRectF>
#include <QRegion>
#include <vector>
#include <QWidget>
#include <QHash>
#include <QPointer>

#include <memory>
#include <functional>

struct GalleryOverlayPart {
    QRectF source;
    QRectF target;
    bool operator==(const GalleryOverlayPart& other) const {
        return source==other.source&&target==other.target;
    }
};

struct GalleryOverlayRoot {
    QWidget* widget;
    qreal opacity;
    QRegion excluded;
    bool childrenOnly = false; // For transparent layout containers.
};

struct GalleryOverlayCapture {
    QImage image;
    std::vector<GalleryOverlayPart> parts;
    int paintedWidgets = 0, reusedWidgets = 0;
    qint64 paintNanoseconds = 0;
};

struct GalleryOverlayCacheEntry {
    QPointer<QWidget> widget;
    quint64 signature = 0;
    QImage image;
};
using GalleryOverlayCache = QHash<QWidget*, GalleryOverlayCacheEntry>;

// Call when custom paint state changes beyond the standard widget properties.
void invalidateGalleryOverlay(QWidget& widget);

// Capture occupied control bands for native presentation, without requiring a GPU.
GalleryOverlayCapture captureGalleryOverlay(QWidget& viewport,
    const std::vector<GalleryOverlayRoot>& roots, qreal ratio,
    GalleryOverlayCache* cache = nullptr);

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
                     const QImage& overlay = {},
                     const std::vector<GalleryOverlayPart>& parts = {});
    void setOverlay(const QImage& image,
                    const std::vector<GalleryOverlayPart>& parts = {}, bool redraw = true);
    void drainGpuFrames();

private:
#if defined(MOTIONCAM_HAS_DIRECTLOG_PLACEBO) && QT_CONFIG(vulkan)
    std::unique_ptr<QVulkanInstance> mInstance;
#endif
    GalleryVulkanSurface* mSurface = nullptr;
    QWidget* mContainer = nullptr;
};
