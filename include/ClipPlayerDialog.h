#pragma once
#include "GalleryVulkanVideo.h"
#include <QDialog>
#include <QImage>
#include <QPointF>
#include <QProcess>
#include <QPointer>
#include <QTimer>
#include <QThreadPool>
#include <QVector>
#include <QHash>
#include <QSet>
#include <QList>
#include <QElapsedTimer>
#include <QtGlobal>
#include <memory>
#include <atomic>
#include <deque>
#include <optional>
#include <vector>
#include <array>
#include <functional>
class QLabel; class QPushButton; class QSlider; class QListView; class QCheckBox;
class QEvent;
class QBuffer;
class QTemporaryDir;
class GalleryGpuVideo;
namespace motioncam { struct VulkanGalleryGpuBuffer; }
class QGraphicsOpacityEffect; class QPropertyAnimation; class QWidget;
#if QT_VERSION >= QT_VERSION_CHECK(6,0,0)
class QAudioSink;
#else
class QAudioOutput;
#endif
class ClipPlayerDialog final : public QDialog {
    Q_OBJECT
public:
    enum class FramePushResult { Accepted, Retry, Stopped };
    struct Clip { int mountId=-1; QString title; QString sourceFile; double fps=24.0; double durationSeconds=0.0; int sourceFrames=0; int width=0; int height=0; int nativeWidth=0; int nativeHeight=0; int orientation=-1; bool isSequence=true; bool directPresentation=false; bool autoAdvance=false; bool sourceAudioChecked=false; std::shared_ptr<const std::vector<uint8_t>> audioWav; std::shared_ptr<const std::vector<bool>> duplicateFrames; std::shared_ptr<const std::vector<int>> sourceFrameToOutput; std::shared_ptr<const std::vector<bool>> sourceFrameDuplicated; std::shared_ptr<const std::vector<std::string>> stillFrameNames; QSet<int> selectedSourceFrames; };
    explicit ClipPlayerDialog(QVector<Clip> clips, int initialMountId, QWidget* parent=nullptr);
    ~ClipPlayerDialog() override;
#ifdef Q_OS_ANDROID
    void setNavigationReturnSnapshot(std::function<QPixmap()> returnSnapshot);
#endif
    void closeImmediately();
    int currentMountId() const;
    bool selectMount(int mountId);
    void seekToSeconds(double seconds);
    double currentDurationSeconds() const;
    double currentFrameRate() const;
    void setAutomaticAdvanceEnabled(bool enabled);
    void setPlaybackPaused(bool paused);
    bool isPlaybackPaused() const { return mPaused; }
    bool directSequencePresentation() const { return mDirectSequencePresentation; }
    void requestThumbnailBackfill();
    QPair<int,int> visibleThumbnailOutputRange() const;
    void setThumbnailStripVisible(bool visible);
    void setFpsIndicatorEnabled(bool enabled);
    void setHistogramEnabled(bool enabled);
    void setPerformanceUncapped(bool enabled);
    void setPerformanceAudioEnabled(bool enabled);
    void setHistogramExpanded(bool expanded);
    void setPerformancePixelOverlayEnabled(bool enabled);
    void setClippingEnabled(bool enabled);
    void setPerformanceOverlayPinned(bool pinned);
    bool histogramEnabled() const { return mHistogramEnabled; }
    std::shared_ptr<std::atomic<int>> playbackTarget() const { return mPlaybackTarget; }
    std::shared_ptr<std::atomic<int>> incomingFrame() const { return mIncomingFrame; }
    std::shared_ptr<std::atomic_bool> thumbnailCollectionEnabled() const { return mThumbnailCollectionEnabled; }
    std::shared_ptr<std::atomic_bool> thumbnailPrecacheEnabled() const { return mThumbnailPrecacheEnabled; }
    bool nativeVulkanPresenter() const;
    void runPerformanceGestureTest(std::function<void()> finished);
    void reloadCurrentClip();
    void invalidateThumbnails(int mountId);
    void updateClipInfo(int mountId, double fps, double durationSeconds, int sourceFrames,
                        int width, int height,
                        std::shared_ptr<const std::vector<bool>> duplicateFrames,
                        std::shared_ptr<const std::vector<int>> sourceFrameToOutput,
                        std::shared_ptr<const std::vector<bool>> sourceFrameDuplicated);
    FramePushResult pushRgb48Frame(const QByteArray& frame, int width, int height,
        std::shared_ptr<const std::vector<uint16_t>> rawSamples = {},
        int rawWidth = 0, int rawHeight = 0, int rawChannels = 0,
        float rawBlack = 0.0f, float rawWhite = 65535.0f,
        int rawCfaSize = 2, std::array<uint8_t, 4> rawCfaPhase = {0, 1, 1, 2},
        std::array<float, 4> rawBlackLevels = {}, int rawOrientation = 0,
        std::shared_ptr<const std::vector<uint8_t>> frameBacking = {},
        bool framePreOriented = false,
        std::shared_ptr<const uint8_t> framePixels = {},
        std::shared_ptr<motioncam::VulkanGalleryGpuBuffer> gpuBuffer = {},
        int sourceFrameIndex = -1);
    void presentRgb48Frame(const QByteArray& frame, int width, int height,
        std::shared_ptr<const std::vector<uint16_t>> rawSamples = {},
        int rawWidth = 0, int rawHeight = 0, int rawChannels = 0,
        float rawBlack = 0.0f, float rawWhite = 65535.0f,
        int rawCfaSize = 2, std::array<uint8_t, 4> rawCfaPhase = {0, 1, 1, 2},
        std::array<float, 4> rawBlackLevels = {}, int rawOrientation = 0,
        std::shared_ptr<const std::vector<uint8_t>> frameBacking = {},
        bool framePreOriented = false,
        std::shared_ptr<const uint8_t> framePixels = {},
        std::shared_ptr<motioncam::VulkanGalleryGpuBuffer> gpuBuffer = {});
    void finishRgb48Frames();
    void failRgb48Frames(const QString& error);
    void setOutputFrameThumbnail(int outputFrame, const QByteArray& frame, int width, int height);
    void setOutputFrameThumbnailImage(int outputFrame, const QImage& image,
                                      bool cacheWhenHidden = false);
    static QSize thumbnailMaximumSize();
    std::shared_ptr<std::atomic<QSize>> thumbnailRenderBounds() const {return mThumbnailRenderBounds;}
    static QImage makeRgb48Thumbnail(const uint8_t* pixels, qsizetype bytes,
                                    int width, int height, int orientation, qreal pixelRatio = 1.0);
    void setSourceFrameThumbnail(int sourceFrame, const QByteArray& frame, int width, int height);
    void presentDroppedSourceFrame(int sourceFrame, const QByteArray& frame, int width, int height);
    void clearFrameSelections();
    void clearFrameSelections(const QSet<int>& mountIds);
    void selectAllFrameSelections();
signals:
    void histogramEnabledChanged(bool enabled);
    void clippingEnabledChanged(bool enabled);
    void currentClipChanged(int mountId, double startSeconds);
    void firstFramePresented(int mountId);
    void framePresented(int mountId, int frame);
    void playbackClosed();
    void sourceFrameSelectionChanged(int mountId, int sourceFrame, bool selected);
    void sourceFrameThumbnailRequested(int mountId, int sourceFrame);
    void thumbnailBackfillRequested(int mountId, double startSeconds);
protected:
    void closeEvent(QCloseEvent*) override;
    bool eventFilter(QObject*, QEvent*) override;
    void keyPressEvent(QKeyEvent*) override;
    void resizeEvent(QResizeEvent*) override;
private:
    bool hasReusableSourceFrame() const;
    struct RawFrame {
        std::shared_ptr<const std::vector<uint16_t>> samples;
        int width=0,height=0,channels=0;
        float black=0.0f, white=65535.0f;
        std::array<float, 4> blackLevels{};
        int cfaSize=2;
        std::array<uint8_t, 4> cfaPhase{0, 1, 1, 2};
        int orientation=0;
    };
    struct SubmittedFrame { int sourceFrame=0; RawFrame raw; };
    struct QueuedFrame {
        QImage image;
        int sourceFrame=0;
        RawFrame raw;
        std::shared_ptr<motioncam::VulkanGalleryGpuBuffer> gpuBuffer;
    };
    bool hasPresentedFrame() const {
        return !mLastPresentedImage.isNull() || bool(mLastPresentedGpuBuffer);
    }
    void openClip(int, double startSeconds=0.0); void startDecoder();
    void stopDecoder();
    void releaseGpuFrames(bool keepPresentedFrame=false);
    void captureNativeOverlay(bool submit = true);
    void suspendNativeBackingStore(bool suspend);
    void decoderFinished(int, QProcess::ExitStatus); void consumeOutput();
    void showNextFrame(); void advance(); QString ffmpegPath() const;
    int frameAtSliderPosition(int x) const;
    void updateSeekPosition(int x);
    void seekToFrame(int frame);
    void stepFrame(int direction);
    void completeFrameStep();
    void configureAudio();
    void beginSourceAudioLoad();
    static QByteArray sourceAudioWav(const Clip& clip, const QString& ffmpegExecutable,
                                     const std::shared_ptr<std::atomic_bool>& cancelled);
    void setAudioEnabled(bool enabled);
    enum class AudioMode { CappedMuted, RealTimeAudio, UncappedMuted };
    void cycleAudioMode();
    void setVolumePanelVisible(bool visible);
    void positionVolumePanel();
    void updateButtonIcons();
    void revealOverlay();
    void setOverlayVisible(bool visible);
#ifdef Q_OS_ANDROID
    void updateAndroidOverlayMask();
#endif
    void changeZoom(double wheelSteps);
    void advanceZoomAnimation();
    void setZoomAnimationTarget(double target);
    void clampPanToZoom();
    void adoptRenderedDimensions(int width, int height);
    QPointF effectivePanForZoom(const QPointF& pan, double zoomPercent) const;
    QPointF surfaceScaleForZoom(double zoomPercent) const;
    void updateTitle();
    void updateFpsIndicator();
    void updateHistogram();
    bool pointerOverThumbnailRow() const;
    void updateMouseSourcePosition(const QPointF& globalPosition);
    void updateDisplayedImage();
    double fitScale() const;
    void updateFrameTimerInterval();
    void startAudioAt(double seconds);
    qint64 audioPositionMs() const;
    qint64 audioPositionNs() const;
    QString framePositionText(int frame) const;
    static QString runtimeText(double seconds);
    void rebuildThumbnailStrip();
    void cacheThumbnail(int mountId, int sourceFrame, const QImage& image,
                        bool persistToDisk = true);
    QString thumbnailCachePath(int mountId, int sourceFrame) const;
    void refreshThumbnail(int sourceFrame);
    QImage thumbnailImageForSource(int sourceFrame);
    QSize thumbnailDisplayMaximumSize() const;
    int thumbnailStripHeight() const;
    QSize thumbnailSizeForSource(int sourceFrame) const;
    void setCurrentThumbnailFrame(int outputFrame);
    void setDroppedCursorVisible(bool visible);
    void setDuplicateCursorVisible(bool visible);
    void updateCursorStyle();
    void centerCurrentThumbnail();
    int sourceFrameForOutput(int outputFrame) const;
    QImage rgb48Image(const QByteArray& frame, int width, int height,
        std::shared_ptr<const std::vector<uint8_t>> backing = {},
        bool preOriented = false,
        std::shared_ptr<const uint8_t> pixels = {}) const;
    QImage rgb48Thumbnail(const QByteArray& frame, int width, int height) const;
    void setSourceFrameThumbnailImage(int sourceFrame, const QImage& image,
                                      bool cacheWhenHidden = false);
    QVector<Clip> mClips; int mIndex=-1; QLabel* mVideo=nullptr; QLabel* mPreparingLabel=nullptr; GalleryGpuVideo* mGpuVideo=nullptr; QLabel* mTitle=nullptr; QLabel* mFpsLabel=nullptr; QWidget* mHistogram=nullptr;
    QPushButton* mPlayPause=nullptr; QPushButton* mAudioButton=nullptr; QPushButton* mFullscreenButton=nullptr; QSlider* mPosition=nullptr; QProcess mDecoder; QTimer mFrameTimer;
    QPushButton* mThumbnailToggle=nullptr; QPushButton* mHistogramToggle=nullptr; QPushButton* mClippingToggle=nullptr; QListView* mThumbnailScroll=nullptr;
    QHash<int,QHash<int,QImage>> mThumbnailCache;
    QHash<int,QHash<int,QSize>> mStillThumbnailSizes;
    QList<QPair<int,int>> mThumbnailCacheOrder;
    qint64 mThumbnailCachedBytes=0;
    std::shared_ptr<std::atomic<QSize>> mThumbnailRenderBounds=
        std::make_shared<std::atomic<QSize>>(thumbnailMaximumSize()-QSize(2,2));
    std::unique_ptr<QTemporaryDir> mThumbnailDiskCache;
    std::shared_ptr<std::atomic<int>> mThumbnailWritesPending =
        std::make_shared<std::atomic<int>>(0);
    QThreadPool mThumbnailCacheWriter;
    int mCurrentThumbnailSource = -1;
    bool mDroppedCursorVisible = false;
    bool mDuplicateCursorVisible = false;
    bool mFrameStepInProgress = false;
    int mQueuedFrameStep = 0;
    QWidget* mOverlay=nullptr; QGraphicsOpacityEffect* mOverlayOpacity=nullptr;
    QPropertyAnimation* mOverlayAnimation=nullptr;
    QTimer mThumbnailBackfillTimer, mOverlayTimer, mNativeOverlayCaptureTimer, mSurfaceUpdateTimer, mZoomAnimationTimer;
#ifdef Q_OS_ANDROID
    QTimer mTouchPanUpdateTimer;
    std::function<QPixmap()> mNavigationReturnSnapshot;
    bool mNavigationClosing=false, mNavigationCloseReady=false;
#endif
    QImage mCapturedNativeOverlay;
    std::vector<GalleryOverlayPart> mCapturedNativeOverlayParts;
    std::unique_ptr<GalleryOverlayCache> mNativeOverlayCache;
    QPointer<QWidget> mNativeBackingStoreHost;
    QPointer<QWidget> mNativeMouseTarget;
    QPointer<QWidget> mNativeHoverTarget;
    QPointer<QWidget> mNativeTouchTarget;
    QPointF mNativeTouchPosition;
    QPointF mNativeTouchStart;
    bool mNativeThumbnailTouch=false;
    bool mNativeThumbnailDrag=false;
#ifdef Q_OS_ANDROID
    bool mThumbnailFollowPlayback=true;
#endif
    QElapsedTimer mZoomAnimationClock;
    QElapsedTimer mPresentationClock;
    QElapsedTimer mCadenceClock;
    int mCadenceStartFrame=-1;
    int mCadenceLastFrame=-1;
    std::deque<qint64> mPresentationIntervals;
    qint64 mLastPresentationNs=-1;
    QElapsedTimer mFpsIndicatorClock;
    bool mNativeOverlayCapturePending=false;
    static constexpr unsigned kCappedPlaybackBufferFrames=3;
    QBuffer* mAudioBuffer=nullptr;
    QTimer mAudioFadeTimer;
    QElapsedTimer mAudioFadeClock;
    bool mAudioFadeOut=false;
    qreal mAudioFadeStartVolume=0.0;
    QWidget* mVolumePanel=nullptr;
    QSlider* mVolumeSlider=nullptr;
    QPropertyAnimation* mVolumeAnimation=nullptr;
    QTimer mVolumeHideTimer;
    bool mVolumePanelWanted=false;
    QTimer mVuTimer;
    float mVuLevel=0.0f;
    int mVolumePercent=50;
#if QT_VERSION >= QT_VERSION_CHECK(6,0,0)
    QAudioSink* mAudioSink=nullptr;
#else
    QAudioOutput* mAudioSink=nullptr;
#endif
    QByteArray mAudioPcm; QElapsedTimer mAudioClock;
    qint64 mAudioClockBaseNs=0; int mAudioBytesPerSecond=0, mAudioBlockAlign=1;
    QByteArray mBytes; qsizetype mBytesOffset=0;
    std::deque<QueuedFrame> mFrames; std::deque<SubmittedFrame> mSubmittedFrames;
    RawFrame mPresentedRawFrame;
    QImage mLastPresentedImage;
    std::shared_ptr<motioncam::VulkanGalleryGpuBuffer> mLastPresentedGpuBuffer;
    int mWidth=0, mHeight=0, mFrameBytes=0, mInputFrameBytes=0;
    int mInputBitsPerChannel=16;
    int mNextInputFrame=0;
    double mPositionSeconds=0.0, mStartSeconds=0.0;
    bool mPaused=false, mClosing=false, mPlaybackFailed=false;
    bool mFpsIndicatorEnabled=true;
    bool mHistogramEnabled=false;
    bool mPerformanceUncapped=false;
    AudioMode mAudioMode=AudioMode::CappedMuted;
    bool mClippingEnabled=false;
    bool mHistogramExpanded=false;
    bool mPerformanceOverlayPinned=false;
    std::optional<uint16_t> mDetectedClipValue;
    const std::vector<uint16_t>* mIntensitySamples=nullptr;
    QPoint mIntensityPosition{-1,-1};
    QString mCachedIntensity;
    std::vector<std::array<uint32_t,3>> mClipValueCounts;
    std::vector<float> mHistogramCodePositions;
    std::array<float,4> mHistogramBinBlackLevels{};
    float mHistogramBinWhite=0.0f;
    double mHistogramBinDistribution=0.0;
    int mHistogramBinWidth=0;
    bool mHistogramResizePending=false;
    int mHistogramBinChannels=0;
    bool mDirectFramesFinished=false;
    bool mDirectSequencePresentation=false;
    bool mStoppingDecoder=false, mSeeking=false, mAudioEnabled=false;
    bool mFirstFrameReady=false, mAudioStartPending=false, mAudioLoading=false;
    int mAudioLoadGeneration=0;
    double mZoomPercent=0.0; // 0 is scale-to-fit; otherwise absolute source scale.
    double mRequestedZoomPercent=0.0;
    double mZoomAnimationTarget=0.0;
    bool mZoomAnimationStartupDelay=false;
    bool mViewportRefreshPending=false;
    bool mLastImageIsSource=false;
    QPointF mLastSurfaceScale{1.0,1.0};
    QPointF mLastSurfacePan;
    QPointF mLastSurfaceViewportPan;
    QPointF mDecoderSurfaceScale{1.0,1.0};
    QPointF mDecoderSurfacePan;
    QPointF mDecoderSurfaceViewportPan;
    bool mPanning=false;
    bool mWaitingForFirstFrame=false;
    QPoint mMouseSourcePosition{-1,-1};
    QPointF mPanSourcePixels, mLastPanGlobal;
#ifdef Q_OS_ANDROID
    bool mTouchActive=false;
    int mTouchPointCount=0;
    QPointF mLastTouchCenter;
    double mLastTouchDistance=0.0;
#endif
    std::shared_ptr<std::atomic_bool> mAudioLoadCancelled=
        std::make_shared<std::atomic_bool>(false);
    std::shared_ptr<std::atomic<int>> mPlaybackTarget=std::make_shared<std::atomic<int>>(0);
    std::shared_ptr<std::atomic<int>> mIncomingFrame=std::make_shared<std::atomic<int>>(0);
    std::shared_ptr<std::atomic_bool> mThumbnailCollectionEnabled=
        std::make_shared<std::atomic_bool>(false);
    std::shared_ptr<std::atomic_bool> mThumbnailPrecacheEnabled=
        std::make_shared<std::atomic_bool>(true);
};
