#pragma once
#include <QDialog>
#include <QImage>
#include <QPointF>
#include <QProcess>
#include <QTimer>
#include <QVector>
#include <QHash>
#include <QSet>
#include <QList>
#include <QElapsedTimer>
#include <QtGlobal>
#include <memory>
#include <atomic>
#include <deque>
#include <vector>
class QLabel; class QPushButton; class QSlider; class QScrollArea; class QCheckBox;
class QEvent;
class QBuffer;
class QTemporaryDir;
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
    struct Clip { int mountId=-1; QString title; QString sourceFile; double fps=24.0; double durationSeconds=0.0; int sourceFrames=0; int width=0; int height=0; int orientation=-1; bool isSequence=true; bool autoAdvance=false; bool sourceAudioChecked=false; std::shared_ptr<const std::vector<uint8_t>> audioWav; std::shared_ptr<const std::vector<bool>> duplicateFrames; std::shared_ptr<const std::vector<int>> sourceFrameToOutput; std::shared_ptr<const std::vector<bool>> sourceFrameDuplicated; QSet<int> selectedSourceFrames; };
    explicit ClipPlayerDialog(QVector<Clip> clips, int initialMountId, QWidget* parent=nullptr);
    ~ClipPlayerDialog() override;
    int currentMountId() const;
    bool selectMount(int mountId);
    void seekToSeconds(double seconds);
    double currentDurationSeconds() const;
    void setAutomaticAdvanceEnabled(bool enabled);
    void setPlaybackPaused(bool paused);
    bool isPlaybackPaused() const { return mPaused; }
    void requestThumbnailBackfill();
    void setThumbnailStripVisible(bool visible);
    std::shared_ptr<std::atomic<int>> playbackTarget() const { return mPlaybackTarget; }
    std::shared_ptr<std::atomic<int>> incomingFrame() const { return mIncomingFrame; }
    std::shared_ptr<std::atomic_bool> thumbnailCollectionEnabled() const { return mThumbnailCollectionEnabled; }
    void reloadCurrentClip();
    void updateClipInfo(int mountId, double fps, double durationSeconds, int sourceFrames,
                        int width, int height,
                        std::shared_ptr<const std::vector<bool>> duplicateFrames,
                        std::shared_ptr<const std::vector<int>> sourceFrameToOutput,
                        std::shared_ptr<const std::vector<bool>> sourceFrameDuplicated);
    FramePushResult pushRgb48Frame(const QByteArray& frame, int width, int height);
    void presentRgb48Frame(const QByteArray& frame, int width, int height);
    void finishRgb48Frames();
    void failRgb48Frames(const QString& error);
    void setOutputFrameThumbnail(int outputFrame, const QByteArray& frame, int width, int height);
    void setSourceFrameThumbnail(int sourceFrame, const QByteArray& frame, int width, int height);
    void presentDroppedSourceFrame(int sourceFrame, const QByteArray& frame, int width, int height);
    void clearFrameSelections();
signals:
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
    struct QueuedFrame { QImage image; int sourceFrame=0; };
    void openClip(int, double startSeconds=0.0); void startDecoder();
    void stopDecoder();
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
    void updateButtonIcons();
    void revealOverlay();
    void setOverlayVisible(bool visible);
    void changeZoom(double wheelSteps);
    void advanceZoomAnimation();
    void setZoomAnimationTarget(double target);
    void clampPanToZoom();
    QPointF effectivePanForZoom(const QPointF& pan, double zoomPercent) const;
    QPointF surfaceScaleForZoom(double zoomPercent) const;
    void updateDisplayedImage();
    double fitScale() const;
    void updateFrameTimerInterval();
    void startAudioAt(double seconds);
    qint64 audioPositionMs() const;
    QString framePositionText(int frame) const;
    static QString runtimeText(double seconds);
    void rebuildThumbnailStrip();
    void updateVisibleThumbnailWidgets();
    void cacheThumbnail(int mountId, int sourceFrame, const QImage& image,
                        bool persistToDisk = true);
    QString thumbnailCachePath(int mountId, int sourceFrame) const;
    void refreshThumbnailLabel(int sourceFrame);
    void setCurrentThumbnailFrame(int outputFrame);
    void setDroppedCursorVisible(bool visible);
    void setDuplicateCursorVisible(bool visible);
    void updateCursorStyle();
    void centerCurrentThumbnail();
    int sourceFrameForOutput(int outputFrame) const;
    QImage rgb48Image(const QByteArray& frame, int width, int height) const;
    QImage rgb48Thumbnail(const QByteArray& frame, int width, int height) const;
    QVector<Clip> mClips; int mIndex=-1; QLabel* mVideo=nullptr; QLabel* mTitle=nullptr;
    QPushButton* mPlayPause=nullptr; QPushButton* mAudioButton=nullptr; QPushButton* mFullscreenButton=nullptr; QSlider* mPosition=nullptr; QProcess mDecoder; QTimer mFrameTimer;
    QPushButton* mThumbnailToggle=nullptr; QScrollArea* mThumbnailScroll=nullptr;
    QWidget* mThumbnailContent=nullptr; QHash<int,QLabel*> mThumbnailLabels;
    QHash<int,QWidget*> mThumbnailItems;
    QHash<int,QHash<int,QImage>> mThumbnailCache;
    QList<QPair<int,int>> mThumbnailCacheOrder;
    std::unique_ptr<QTemporaryDir> mThumbnailDiskCache;
    int mCurrentThumbnailSource = -1;
    bool mDroppedCursorVisible = false;
    bool mDuplicateCursorVisible = false;
    bool mFrameStepInProgress = false;
    int mQueuedFrameStep = 0;
    QWidget* mOverlay=nullptr; QGraphicsOpacityEffect* mOverlayOpacity=nullptr;
    QPropertyAnimation* mOverlayAnimation=nullptr;
    QTimer mOverlayTimer, mSurfaceUpdateTimer, mZoomAnimationTimer;
    QElapsedTimer mZoomAnimationClock;
    QBuffer* mAudioBuffer=nullptr;
#if QT_VERSION >= QT_VERSION_CHECK(6,0,0)
    QAudioSink* mAudioSink=nullptr;
#else
    QAudioOutput* mAudioSink=nullptr;
#endif
    QByteArray mAudioPcm; QElapsedTimer mAudioClock;
    qint64 mAudioClockBaseMs=0; int mAudioBytesPerSecond=0, mAudioBlockAlign=1;
    QByteArray mBytes; qsizetype mBytesOffset=0;
    std::deque<QueuedFrame> mFrames; std::deque<int> mSubmittedFrames;
    QImage mLastPresentedImage;
    int mWidth=0, mHeight=0, mFrameBytes=0, mInputFrameBytes=0;
    int mNextInputFrame=0;
    double mPositionSeconds=0.0, mStartSeconds=0.0;
    bool mPaused=false, mClosing=false, mPlaybackFailed=false;
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
    QPointF mDecoderSurfaceScale{1.0,1.0};
    QPointF mDecoderSurfacePan;
    bool mPanning=false;
    bool mWaitingForFirstFrame=false;
    QPointF mPanSourcePixels, mLastPanGlobal;
    std::shared_ptr<std::atomic_bool> mAudioLoadCancelled=
        std::make_shared<std::atomic_bool>(false);
    std::shared_ptr<std::atomic<int>> mPlaybackTarget=std::make_shared<std::atomic<int>>(0);
    std::shared_ptr<std::atomic<int>> mIncomingFrame=std::make_shared<std::atomic<int>>(0);
    std::shared_ptr<std::atomic_bool> mThumbnailCollectionEnabled=
        std::make_shared<std::atomic_bool>(false);
};
