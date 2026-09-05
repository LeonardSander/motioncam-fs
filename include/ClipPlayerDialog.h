#pragma once
#include <QDialog>
#include <QImage>
#include <QPointF>
#include <QProcess>
#include <QTimer>
#include <QVector>
#include <QElapsedTimer>
#include <QtGlobal>
#include <memory>
#include <atomic>
#include <vector>
class QLabel; class QPushButton; class QSlider;
class QEvent;
class QBuffer;
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
    struct Clip { int mountId=-1; QString title; QString sourceFile; double fps=24.0; double durationSeconds=0.0; int sourceFrames=0; int width=0; int height=0; int orientation=-1; bool isSequence=true; bool autoAdvance=false; bool sourceAudioChecked=false; std::shared_ptr<const std::vector<uint8_t>> audioWav; std::shared_ptr<const std::vector<bool>> duplicateFrames; };
    explicit ClipPlayerDialog(QVector<Clip> clips, int initialMountId, QWidget* parent=nullptr);
    ~ClipPlayerDialog() override;
    int currentMountId() const;
    bool selectMount(int mountId);
    void seekToSeconds(double seconds);
    double currentDurationSeconds() const;
    void setAutomaticAdvanceEnabled(bool enabled);
    std::shared_ptr<std::atomic<int>> playbackTarget() const { return mPlaybackTarget; }
    std::shared_ptr<std::atomic<int>> incomingFrame() const { return mIncomingFrame; }
    void reloadCurrentClip();
    void updateClipInfo(int mountId, double fps, double durationSeconds, int sourceFrames,
                        int width, int height,
                        std::shared_ptr<const std::vector<bool>> duplicateFrames);
    FramePushResult pushRgb48Frame(const QByteArray& frame, int width, int height);
    void presentRgb48Frame(const QByteArray& frame, int width, int height);
    void finishRgb48Frames();
    void failRgb48Frames(const QString& error);
signals:
    void currentClipChanged(int mountId, double startSeconds);
    void firstFramePresented(int mountId);
    void framePresented(int mountId, int frame);
    void playbackClosed();
protected:
    void closeEvent(QCloseEvent*) override;
    bool eventFilter(QObject*, QEvent*) override;
    void keyPressEvent(QKeyEvent*) override;
    void resizeEvent(QResizeEvent*) override;
private:
    void openClip(int, double startSeconds=0.0); void startDecoder();
    void stopDecoder();
    void decoderFinished(int, QProcess::ExitStatus); void consumeOutput();
    void showNextFrame(); void advance(); QString ffmpegPath() const;
    int frameAtSliderPosition(int x) const;
    void updateSeekPosition(int x);
    void seekToFrame(int frame);
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
    QImage rgb48Image(const QByteArray& frame, int width, int height) const;
    QVector<Clip> mClips; int mIndex=-1; QLabel* mVideo=nullptr; QLabel* mTitle=nullptr;
    QPushButton* mPlayPause=nullptr; QPushButton* mAudioButton=nullptr; QPushButton* mFullscreenButton=nullptr; QSlider* mPosition=nullptr; QProcess mDecoder; QTimer mFrameTimer;
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
    QByteArray mBytes; QVector<QImage> mFrames; QImage mLastPresentedImage;
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
};
