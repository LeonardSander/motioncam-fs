#include "ClipPlayerDialog.h"
#include <QCloseEvent>
#include <QApplication>
#include <QCoreApplication>
#include <QDir>
#include <QEvent>
#include <QFile>
#include <QFileInfo>
#include <QFutureWatcher>
#include <QBuffer>
#include <QGraphicsOpacityEffect>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QMouseEvent>
#include <QPainter>
#include <QPropertyAnimation>
#include <QPushButton>
#include <QResizeEvent>
#include <QSlider>
#include <QScrollArea>
#include <QScrollBar>
#include <QCheckBox>
#include <QStackedLayout>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QStyle>
#include <QToolTip>
#include <QVBoxLayout>
#include <QWheelEvent>
#include <QtConcurrent>
#include <algorithm>
#include <array>
#include <spdlog/spdlog.h>
#if QT_VERSION >= QT_VERSION_CHECK(6,0,0)
#include <QAudioFormat>
#include <QAudioSink>
#include <QMediaDevices>
#else
#include <QAudioDeviceInfo>
#include <QAudioFormat>
#include <QAudioOutput>
#endif

namespace {
QIcon fullscreenIcon(bool restore) {
    QPixmap pixmap(24,24);pixmap.fill(Qt::transparent);
    QPainter painter(&pixmap);painter.setRenderHint(QPainter::Antialiasing);
    painter.setPen(QPen(Qt::white,2.0,Qt::SolidLine,Qt::RoundCap,Qt::RoundJoin));
    const QPointF center(12,12);
    const std::array<QPointF,4> outer{{{4,4},{20,4},{4,20},{20,20}}};
    for(const QPointF& corner:outer){
        const QPointF diagonal=(corner-center)/std::sqrt(128.0);
        const QPointF start=restore?corner:center+diagonal*3.0;
        const QPointF end=restore?center+diagonal*3.0:corner;
        painter.drawLine(start,end);
        const QPointF direction=end-start;
        const double length=std::hypot(direction.x(),direction.y());
        const QPointF unit=direction/length,normal(-unit.y(),unit.x());
        painter.drawLine(end,end-unit*4.0+normal*2.6);
        painter.drawLine(end,end-unit*4.0-normal*2.6);
    }
    return QIcon(pixmap);
}

QIcon thumbnailChevronIcon(bool up) {
    QPixmap pixmap(30,30);pixmap.fill(Qt::transparent);
    QPainter painter(&pixmap);painter.setRenderHint(QPainter::Antialiasing);
    painter.setPen(QPen(Qt::white,3.0,Qt::SolidLine,Qt::RoundCap,Qt::RoundJoin));
    const qreal upper=up?9.0:20.0,lower=up?20.0:9.0;
    painter.drawPolyline(QPolygonF({QPointF(5.0,lower),QPointF(15.0,upper),QPointF(25.0,lower)}));
    return QIcon(pixmap);
}

class DuplicateSlider final : public QSlider {
public:
    explicit DuplicateSlider(QWidget* parent) : QSlider(Qt::Horizontal,parent) {}
    void setDuplicates(std::shared_ptr<const std::vector<bool>> duplicates) {
        mDuplicates=std::move(duplicates);update();
    }
protected:
    void paintEvent(QPaintEvent* event) override {
        QSlider::paintEvent(event);
        if(!mDuplicates||mDuplicates->empty()||maximum()<=0)return;
        QPainter painter(this);painter.setPen(Qt::NoPen);painter.setBrush(QColor(225,45,45));
        const int count=std::min<int>(maximum()+1,mDuplicates->size());
        for(int frame=0;frame<count;++frame){
            if(!(*mDuplicates)[frame])continue;
            const int left=qRound(double(frame)*width()/(maximum()+1));
            const int right=qRound(double(frame+1)*width()/(maximum()+1));
            painter.drawRect(left,height()-4,std::max(1,right-left),4);
        }
    }
private:
    std::shared_ptr<const std::vector<bool>> mDuplicates;
};

class FrameThumbnailLabel final : public QLabel {
public:
    using QLabel::QLabel;
    std::function<void()> clicked;
protected:
    void mousePressEvent(QMouseEvent* event) override {
        if(event->button()==Qt::LeftButton&&clicked){clicked();event->accept();return;}
        QLabel::mousePressEvent(event);
    }
};
}

ClipPlayerDialog::ClipPlayerDialog(QVector<Clip> clips, int initialMountId, QWidget* parent)
    : QDialog(parent), mClips(std::move(clips)) {
    mThumbnailDiskCache=std::make_unique<QTemporaryDir>();
    setAttribute(Qt::WA_DeleteOnClose); setWindowTitle(tr("Mounted clip gallery")); resize(1100,720);
    setStyleSheet("QToolTip{background-color:#1976d2;color:white;border:1px solid #64a9e8;padding:4px 6px;}");
    setMouseTracking(true);
    auto* layout=new QVBoxLayout(this);layout->setContentsMargins(0,0,0,0);layout->setSpacing(0);
    auto* stage=new QWidget(this);stage->setContentsMargins(0,0,0,0);stage->setStyleSheet("border:0;");
    auto* stack=new QStackedLayout(stage);stack->setStackingMode(QStackedLayout::StackAll);stack->setContentsMargins(0,0,0,0);stack->setSpacing(0);
    mVideo=new QLabel(tr("Preparing clip…"),this); mVideo->setAlignment(Qt::AlignCenter);
    // A retained surface must remain pixel-size stable while the window is
    // resized. QLabel will center and clip/pad this pixmap until the decoder
    // supplies a surface rendered for the new viewport.
    mVideo->setScaledContents(false);
    mVideo->setMinimumSize(640,360);mVideo->setSizePolicy(QSizePolicy::Ignored,QSizePolicy::Ignored);
    mVideo->setFrameShape(QFrame::NoFrame);mVideo->setLineWidth(0);mVideo->setContentsMargins(0,0,0,0);
    mVideo->setStyleSheet("background:#606060;color:#e0e0e0;border:none;margin:0;padding:0;");stack->addWidget(mVideo);
    mOverlay=new QWidget(stage);mOverlay->setStyleSheet("background:transparent;");
    auto* overlayLayout=new QVBoxLayout(mOverlay);overlayLayout->setContentsMargins(10,10,10,0);
    mTitle=new QLabel(mOverlay);mTitle->setStyleSheet("color:white;background:rgba(0,0,0,120);padding:5px 9px;border-radius:4px;");
    overlayLayout->addWidget(mTitle,0,Qt::AlignLeft);overlayLayout->addStretch(1);
    auto* controls=new QHBoxLayout;controls->setSpacing(8);auto* previous=new QPushButton(mOverlay);
    mPlayPause=new QPushButton(mOverlay); auto* next=new QPushButton(mOverlay);
    mAudioButton=new QPushButton(mOverlay);mAudioButton->setCheckable(true);
    mFullscreenButton=new QPushButton(mOverlay);mThumbnailToggle=new QPushButton(mOverlay);
    for(auto* button:{previous,mPlayPause,next,mAudioButton,mFullscreenButton,mThumbnailToggle}){button->setFixedSize(38,38);button->setFlat(true);button->setMouseTracking(true);button->setStyleSheet("QPushButton{color:white;background:rgba(0,0,0,145);border:0;border-radius:19px} QPushButton:hover{background:rgba(70,70,70,210)}");}
    previous->setIcon(style()->standardIcon(QStyle::SP_MediaSkipBackward));previous->setToolTip(tr("Previous clip"));
    next->setIcon(style()->standardIcon(QStyle::SP_MediaSkipForward));next->setToolTip(tr("Next clip"));
    mPlayPause->setToolTip(tr("Play / pause"));mAudioButton->setToolTip(tr("Mute / unmute audio"));mFullscreenButton->setToolTip(tr("Toggle fullscreen"));
    mFullscreenButton->setIcon(fullscreenIcon(false));
    mThumbnailToggle->setToolTip(tr("Show frame thumbnails"));
    mThumbnailToggle->setIcon(thumbnailChevronIcon(true));mThumbnailToggle->setIconSize(QSize(22,22));
    controls->addStretch();controls->addWidget(previous);controls->addWidget(mPlayPause);controls->addWidget(next);controls->addWidget(mThumbnailToggle);controls->addWidget(mAudioButton);controls->addWidget(mFullscreenButton);controls->addStretch();overlayLayout->addLayout(controls);
    mThumbnailScroll=new QScrollArea(mOverlay);mThumbnailScroll->setWidgetResizable(true);
    mThumbnailScroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    mThumbnailScroll->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    mThumbnailScroll->setFixedHeight(170);mThumbnailScroll->setFrameShape(QFrame::NoFrame);
    mThumbnailScroll->setStyleSheet(
        "QScrollArea{background:rgba(0,0,0,80);border:0;}"
        "QWidget#galleryThumbnailContent{background:transparent;}"
        "QScrollBar:horizontal{background:rgba(0,0,0,25);height:10px;margin:0;border:0;}"
        "QScrollBar::handle:horizontal{background:rgba(255,255,255,40);min-width:30px;border:0;border-radius:5px;}"
        "QScrollBar::add-line:horizontal,QScrollBar::sub-line:horizontal{width:0;border:0;background:transparent;}"
        "QScrollBar::add-page:horizontal,QScrollBar::sub-page:horizontal{background:transparent;}");
    mThumbnailContent=new QWidget(mThumbnailScroll);mThumbnailContent->setObjectName(QStringLiteral("galleryThumbnailContent"));
    mThumbnailScroll->setWidget(mThumbnailContent);mThumbnailScroll->hide();overlayLayout->addWidget(mThumbnailScroll);overlayLayout->addSpacing(8);
    connect(mThumbnailScroll->horizontalScrollBar(),&QScrollBar::valueChanged,
        this,[this]{updateVisibleThumbnailWidgets();});
    mPosition=new DuplicateSlider(mOverlay); mPosition->setRange(0,0);
    mPosition->setStyleSheet("QToolTip{background-color:#1976d2;color:white;border:1px solid #64a9e8;padding:4px 6px;}");
    mPosition->setMouseTracking(true);
    auto* seekLayout=new QVBoxLayout;seekLayout->setContentsMargins(10,10,10,10);seekLayout->addWidget(mPosition);
    overlayLayout->addLayout(seekLayout);stack->addWidget(mOverlay);layout->addWidget(stage,1);
    mOverlayOpacity=new QGraphicsOpacityEffect(mOverlay);mOverlay->setGraphicsEffect(mOverlayOpacity);mOverlayOpacity->setOpacity(1.0);
    mOverlayAnimation=new QPropertyAnimation(mOverlayOpacity,"opacity",this);mOverlayAnimation->setDuration(260);
    mOverlayTimer.setSingleShot(true);mOverlayTimer.setInterval(4000);connect(&mOverlayTimer,&QTimer::timeout,this,[this]{setOverlayVisible(false);});
    mSurfaceUpdateTimer.setSingleShot(true);mSurfaceUpdateTimer.setInterval(300);
    connect(&mSurfaceUpdateTimer,&QTimer::timeout,this,[this]{
        // A later resize may arrive while a prior surface is still starting.
        // The retained image, rather than first-frame state, is the authority
        // for whether it is safe and necessary to restart at the final size.
        if(mIndex<0||mLastPresentedImage.isNull())return;
        // While zoom is animating, build the crop for its already-known final
        // target in parallel. Keep presenting the intermediate transform until
        // that backing surface arrives.
        const bool prefetchingZoom=mZoomAnimationTimer.isActive();
        const double animatedZoom=mZoomPercent;
        if(prefetchingZoom)mZoomPercent=mZoomAnimationTarget;
        mViewportRefreshPending=true;
        seekToFrame(mPosition->value());
        if(prefetchingZoom){mZoomPercent=animatedZoom;updateDisplayedImage();}
    });
    mZoomAnimationTimer.setInterval(16);
    mZoomAnimationTimer.setTimerType(Qt::PreciseTimer);
    connect(&mZoomAnimationTimer,&QTimer::timeout,this,&ClipPlayerDialog::advanceZoomAnimation);
    // Collapse wheel bursts and window-manager resize storms into one preview
    // generation instead of repeatedly tearing down an active producer.
    const std::array<QWidget*,4> trackedWidgets{this,stage,mVideo,mOverlay};
    for(auto* widget:trackedWidgets)widget->setMouseTracking(true);
    qApp->installEventFilter(this);mOverlay->raise();revealOverlay();
    connect(&mDecoder,&QProcess::readyReadStandardOutput,this,&ClipPlayerDialog::consumeOutput);
    connect(&mDecoder,qOverload<int,QProcess::ExitStatus>(&QProcess::finished),this,&ClipPlayerDialog::decoderFinished);
    connect(&mDecoder, &QProcess::errorOccurred, this, [this](QProcess::ProcessError error) {
        // Killing a decoder with buffered input can queue WriteError after
        // stopDecoder() returns and the same QProcess has already been
        // restarted. Process exits (including genuine broken pipes) are
        // diagnosed by decoderFinished(), which also has FFmpeg's stderr.
        if (mClosing || mStoppingDecoder || error == QProcess::Crashed ||
            error == QProcess::WriteError) return;
        mPlaybackFailed = true;
        mFrameTimer.stop();
        const QString decoderError=QString::fromUtf8(mDecoder.readAllStandardError()).trimmed();
        const QString detail=decoderError.isEmpty()?mDecoder.errorString():decoderError;
        mTitle->setText(tr("FFmpeg could not be started: %1").arg(detail));
        if(mLastPresentedImage.isNull())mVideo->setText(tr("Playback unavailable"));
        spdlog::warn("Gallery FFmpeg could not be started: {}", detail.toStdString());
    });
    connect(&mFrameTimer,&QTimer::timeout,this,&ClipPlayerDialog::showNextFrame);
    connect(mPlayPause,&QPushButton::clicked,this,[this]{
        if(mPaused&&mThumbnailScroll&&mThumbnailScroll->isVisible()&&mPosition&&
           mPosition->value()>=mPosition->maximum()&&
           mDecoder.state()==QProcess::NotRunning&&mFrames.empty()&&mBytes.isEmpty()){
            openClip(mIndex,0.0);return;
        }
        mPaused=!mPaused;updateButtonIcons();
        if(mPaused){mFrameTimer.stop();if(mAudioEnabled&&mAudioSink){mAudioClockBaseMs=audioPositionMs();mAudioClock.invalidate();mAudioSink->suspend();}
            if(mThumbnailScroll&&mThumbnailScroll->isVisible())
                emit thumbnailBackfillRequested(currentMountId(),mPositionSeconds);
        }
        else {updateFrameTimerInterval();mFrameTimer.start();if(mThumbnailScroll&&mThumbnailScroll->isVisible())
                emit currentClipChanged(currentMountId(),mPositionSeconds);
            if(mAudioEnabled&&mAudioSink){
            if(mAudioStartPending&&mFirstFrameReady){startAudioAt(mPositionSeconds);mAudioStartPending=false;}
            else if(!mAudioStartPending){mAudioClock.start();mAudioSink->resume();}
        }}
    });
    connect(mAudioButton,&QPushButton::toggled,this,&ClipPlayerDialog::setAudioEnabled);
    connect(previous,&QPushButton::clicked,this,[this]{ if(!mClips.isEmpty())openClip((mIndex-1+mClips.size())%mClips.size()); });
    connect(next,&QPushButton::clicked,this,&ClipPlayerDialog::advance);
    connect(mThumbnailToggle,&QPushButton::clicked,this,[this]{
        const bool show=!mThumbnailScroll->isVisible();mThumbnailScroll->setVisible(show);
        mThumbnailCollectionEnabled->store(show);
        mThumbnailToggle->setIcon(thumbnailChevronIcon(!show));
        mThumbnailToggle->setToolTip(show?tr("Hide frame thumbnails"):tr("Show frame thumbnails"));
        const double seconds=mIndex>=0?mPosition->value()/std::max(1.0,mClips[mIndex].fps):0.0;
        if(show){
            rebuildThumbnailStrip();revealOverlay();
            // Active playback already produces thumbnails for every presented
            // frame. Only use the separate backfill renderer while idle.
            if(mPaused)emit thumbnailBackfillRequested(currentMountId(),seconds);
        }else{
            emit currentClipChanged(currentMountId(),seconds);
            revealOverlay();
        }
    });
    connect(mFullscreenButton,&QPushButton::clicked,this,[this]{
        if(isFullScreen())showNormal();else showFullScreen();
        updateButtonIcons();revealOverlay();
    });
    mIndex=0; for(int i=0;i<mClips.size();++i)if(mClips[i].mountId==initialMountId){mIndex=i;break;}
    if(!mClips.isEmpty())openClip(mIndex);
}

ClipPlayerDialog::~ClipPlayerDialog(){mClosing=true;mAudioLoadCancelled->store(true);++mAudioLoadGeneration;stopDecoder();}
int ClipPlayerDialog::currentMountId()const{return mIndex>=0&&mIndex<mClips.size()?mClips[mIndex].mountId:-1;}
void ClipPlayerDialog::setPlaybackPaused(bool paused){if(mPaused!=paused&&mPlayPause)mPlayPause->click();}
void ClipPlayerDialog::requestThumbnailBackfill(){
    if(mThumbnailScroll&&mThumbnailScroll->isVisible())
        emit thumbnailBackfillRequested(currentMountId(),mPositionSeconds);
}
void ClipPlayerDialog::setThumbnailStripVisible(bool visible){
    if(mThumbnailScroll&&mThumbnailScroll->isVisible()!=visible&&mThumbnailToggle)
        mThumbnailToggle->click();
}
bool ClipPlayerDialog::selectMount(int mountId){
    for(int index=0;index<mClips.size();++index){
        if(mClips[index].mountId!=mountId)continue;
        if(index!=mIndex)openClip(index);
        return true;
    }
    return false;
}
void ClipPlayerDialog::seekToSeconds(double seconds){
    if(mIndex<0||mIndex>=mClips.size())return;
    seekToFrame(static_cast<int>(std::floor(
        std::max(0.0,seconds)*std::max(1.0,mClips[mIndex].fps))));
}
double ClipPlayerDialog::currentDurationSeconds()const{
    return mIndex>=0&&mIndex<mClips.size()?mClips[mIndex].durationSeconds:0.0;
}
void ClipPlayerDialog::setAutomaticAdvanceEnabled(bool enabled){
    for(auto& clip:mClips)clip.autoAdvance=enabled&&clip.isSequence&&clip.sourceFrames>1;
}
void ClipPlayerDialog::reloadCurrentClip(){
    if(mIndex<0)return;
    const bool wasPaused=mPaused;
    const double reloadSeconds=wasPaused&&mPosition
        ?mPosition->value()/std::max(1.0,mClips[mIndex].fps)
        :mPositionSeconds;
    openClip(mIndex,reloadSeconds);
    if(wasPaused){
        // Settings updates need one replacement frame, but must not turn a
        // paused gallery back into active playback.
        mPaused=true;updateButtonIcons();
        if(mClips[mIndex].sourceFrames>1&&!mFrameTimer.isActive())mFrameTimer.start();
    }
}
void ClipPlayerDialog::updateClipInfo(int mountId,double fps,double durationSeconds,
        int sourceFrames,int width,int height,
        std::shared_ptr<const std::vector<bool>> duplicateFrames,
        std::shared_ptr<const std::vector<int>> sourceFrameToOutput,
        std::shared_ptr<const std::vector<bool>> sourceFrameDuplicated){
    for(auto& clip:mClips)if(clip.mountId==mountId){
        clip.fps=std::max(1.0,fps);clip.durationSeconds=std::max(0.0,durationSeconds);
        clip.sourceFrames=std::max(1,sourceFrames);clip.width=width;clip.height=height;
        clip.duplicateFrames=std::move(duplicateFrames);
        clip.sourceFrameToOutput=std::move(sourceFrameToOutput);
        clip.sourceFrameDuplicated=std::move(sourceFrameDuplicated);break;
    }
}

void ClipPlayerDialog::stopDecoder(){
    mFrameTimer.stop();
    if(mDecoder.state()==QProcess::NotRunning)return;
    mStoppingDecoder=true;
    // QProcess::write() buffers complete raw frames in Qt. Killing FFmpeg
    // before that buffer is empty leaves waitForFinished() trying to flush to
    // a closed pipe, which raises SIGPIPE (and stops under a debugger) during
    // zoom/resize decoder restarts. Close stdin gracefully while the consumer
    // is still alive and drain its output so neither side blocks. Once Qt has
    // no pending input, terminating FFmpeg cannot produce a broken pipe.
    mDecoder.closeWriteChannel();
    while(mDecoder.state()!=QProcess::NotRunning&&mDecoder.bytesToWrite()>0){
        mDecoder.readAllStandardOutput();
        mDecoder.waitForBytesWritten(20);
    }
    mDecoder.readAllStandardOutput();
    if(mDecoder.state()!=QProcess::NotRunning)mDecoder.kill();
    if(!mDecoder.waitForFinished(2000)){
        spdlog::warn("Gallery FFmpeg did not terminate within two seconds");
    }
    mStoppingDecoder=false;
}

QString ClipPlayerDialog::ffmpegPath()const{
#ifdef _WIN32
    const QString bundled=QDir(QCoreApplication::applicationDirPath()).filePath("ffmpeg.exe");
#else
    const QString bundled=QDir(QCoreApplication::applicationDirPath()).filePath("ffmpeg");
#endif
    return QFileInfo(bundled).isExecutable()?bundled:QStandardPaths::findExecutable("ffmpeg");
}

void ClipPlayerDialog::openClip(int index,double startSeconds){
    if(index<0||index>=mClips.size())return;
    const bool changingClip=index!=mIndex;
    const bool preservePausedNavigation=changingClip&&mPaused&&mThumbnailScroll&&
        mThumbnailScroll->isVisible();
    if(changingClip){
        // A requested (unsnapped) wheel value belongs to the geometry that
        // produced it. Do not carry that hidden accumulator into another clip.
        mZoomAnimationTimer.stop();mZoomAnimationStartupDelay=false;
    }
    mSurfaceUpdateTimer.stop();
    const auto& requestedClip=mClips[index];
    const int requestedLastFrame=std::max(0,requestedClip.sourceFrames-1);
    const int requestedFrame=std::clamp(static_cast<int>(std::floor(
        std::max(0.0,startSeconds)*std::max(1.0,requestedClip.fps))),0,requestedLastFrame);
    const double requestedSeconds=requestedFrame/std::max(1.0,requestedClip.fps);
    // Invalidate the producer before closing the pipe it may currently be
    // writing. The connected handler increments the gallery generation
    // synchronously; newly queued callbacks cannot run until this returns.
    mPlaybackTarget->store(requestedFrame);mIncomingFrame->store(requestedFrame);
    emit currentClipChanged(requestedClip.mountId,requestedSeconds);
    stopDecoder();mBytes.clear();mBytesOffset=0;mFrames.clear();mSubmittedFrames.clear();
    mAudioLoadCancelled->store(true);
    mAudioLoadCancelled=std::make_shared<std::atomic_bool>(false);
    ++mAudioLoadGeneration;mAudioLoading=false;
    mFirstFrameReady=false;mAudioStartPending=mAudioEnabled;
    mIndex=index;
    if(changingClip)
        mRequestedZoomPercent=mZoomPercent>0.0?mZoomPercent:fitScale()*100.0;
    const auto& clip=mClips[index];
    const int lastFrame=std::max(0,clip.sourceFrames-1);
    const int startFrame=std::clamp(static_cast<int>(std::floor(
        std::max(0.0,startSeconds)*std::max(1.0,clip.fps))),0,lastFrame);
    mStartSeconds=startFrame/std::max(1.0,clip.fps);mPositionSeconds=mStartSeconds;
    mNextInputFrame=startFrame;
    mPlaybackTarget->store(startFrame);
    mIncomingFrame->store(startFrame);
    mPosition->setRange(0,lastFrame);mPosition->setValue(startFrame);
    static_cast<DuplicateSlider*>(mPosition)->setDuplicates(clip.duplicateFrames);
    mCurrentThumbnailSource=sourceFrameForOutput(startFrame);
    if(mThumbnailScroll->isVisible())rebuildThumbnailStrip();
    mPlaybackFailed=false;mPaused=preservePausedNavigation;
    mPlayPause->setEnabled(true);updateButtonIcons();
    const double shownZoom=mZoomPercent>0.0?mZoomPercent:fitScale()*100.0;
    mTitle->setText(QString("%1 — %2 / %3 — %4").arg(mClips[index].title)
        .arg(index+1).arg(mClips.size()).arg(mZoomPercent>0.0
            ?tr("Zoom %1%").arg(shownZoom,0,'f',1):tr("Scale to fit")));
    mWaitingForFirstFrame=!mLastPresentedImage.isNull();
    if(!mWaitingForFirstFrame)mVideo->setText(tr("Preparing playback…"));
    if(!mViewportRefreshPending)configureAudio();
    mViewportRefreshPending=false;
    if(mClips[index].sourceFrames>1)startDecoder();
    else {mFrameTimer.stop();mPlayPause->setEnabled(false);}
}

int ClipPlayerDialog::sourceFrameForOutput(int outputFrame)const{
    if(mIndex<0||mIndex>=mClips.size())return -1;
    const auto& map=mClips[mIndex].sourceFrameToOutput;
    if(!map)return outputFrame;
    for(int source=0;source<static_cast<int>(map->size());++source)
        if((*map)[source]==outputFrame)return source;
    int nearest=-1,nearestOutput=-1;
    for(int source=0;source<static_cast<int>(map->size());++source)
        if((*map)[source]>=0&&(*map)[source]<outputFrame&&(*map)[source]>nearestOutput){
            nearest=source;nearestOutput=(*map)[source];
        }
    return nearest;
}

void ClipPlayerDialog::rebuildThumbnailStrip(){
    if(!mThumbnailContent||mIndex<0||mIndex>=mClips.size())return;
    delete mThumbnailContent->layout();
    const auto children=mThumbnailContent->findChildren<QWidget*>(QString(),Qt::FindDirectChildrenOnly);
    for(auto* child:children)delete child;
    mThumbnailLabels.clear();mThumbnailItems.clear();
    const auto& clip=mClips[mIndex];
    const int sourceCount=clip.sourceFrameToOutput
        ?static_cast<int>(clip.sourceFrameToOutput->size()):clip.sourceFrames;
    constexpr int itemStride=192;
    mThumbnailContent->setFixedSize(16+sourceCount*itemStride,156);
    updateVisibleThumbnailWidgets();
    QTimer::singleShot(0,this,[this]{centerCurrentThumbnail();});
}

void ClipPlayerDialog::updateVisibleThumbnailWidgets(){
    if(!mThumbnailContent||!mThumbnailScroll||!mThumbnailScroll->isVisible()||
       mIndex<0||mIndex>=mClips.size())return;
    constexpr int itemStride=192;
    const auto& clip=mClips[mIndex];
    const int sourceCount=clip.sourceFrameToOutput
        ?static_cast<int>(clip.sourceFrameToOutput->size()):clip.sourceFrames;
    const int scroll=mThumbnailScroll->horizontalScrollBar()->value();
    const int viewport=std::max(1,mThumbnailScroll->viewport()->width());
    const int first=std::max(0,scroll/itemStride-2);
    const int last=std::min(sourceCount-1,(scroll+viewport)/itemStride+2);
    const auto existing=mThumbnailItems.keys();
    for(int source:existing)if(source<first||source>last){
        delete mThumbnailItems.take(source)->parentWidget();
        mThumbnailLabels.remove(source);
    }
    for(int source=first;source<=last;++source){
        if(mThumbnailItems.contains(source))continue;
        const int output=clip.sourceFrameToOutput&&source<static_cast<int>(clip.sourceFrameToOutput->size())
            ?(*clip.sourceFrameToOutput)[source]:source;
        const bool dropped=output<0;
        const bool duplicated=clip.sourceFrameDuplicated&&source<static_cast<int>(clip.sourceFrameDuplicated->size())&&(*clip.sourceFrameDuplicated)[source];
        auto* item=new QWidget(mThumbnailContent);item->setGeometry(8+source*itemStride,7,184,142);
        auto* column=new QVBoxLayout(item);column->setContentsMargins(0,0,0,0);column->setSpacing(2);
        auto* frameBackground=new QWidget(item);frameBackground->setFixedSize(184,120);
        frameBackground->setProperty("frameStatusColor",dropped?QColor(235,190,35,60):
            duplicated?QColor(225,45,45,60):QColor(Qt::transparent));
        auto* frameLayout=new QHBoxLayout(frameBackground);frameLayout->setContentsMargins(4,4,4,4);
        auto* label=new FrameThumbnailLabel(frameBackground);label->setFixedSize(176,112);label->setAlignment(Qt::AlignCenter);
        label->setCursor(Qt::PointingHandCursor);label->setText(dropped?QString():tr("Loading…"));
        const QString color=dropped?QStringLiteral("transparent"):QStringLiteral("rgba(20,20,20,120)");
        label->setStyleSheet(QString("background:%1;border:0;").arg(color));
        label->clicked=[this,source,output,dropped]{
            if(dropped){
                if(!mPaused)mPlayPause->click();
                emit sourceFrameThumbnailRequested(currentMountId(),source);
            }
            else seekToFrame(output);
        };
        auto* check=new QCheckBox(QString::number(source+1),item);
        check->setStyleSheet("color:white;background:transparent;");
        check->setChecked(clip.selectedSourceFrames.contains(source));
        connect(check,&QCheckBox::toggled,this,[this,source](bool selected){
            if(mIndex<0)return;
            if(selected)mClips[mIndex].selectedSourceFrames.insert(source);
            else mClips[mIndex].selectedSourceFrames.remove(source);
            emit sourceFrameSelectionChanged(currentMountId(),source,selected);
        });
        frameLayout->addWidget(label);
        column->addWidget(frameBackground);column->addWidget(check,0,Qt::AlignLeft);
        item->show();mThumbnailLabels.insert(source,label);mThumbnailItems.insert(source,frameBackground);
        refreshThumbnailLabel(source);
    }
}

void ClipPlayerDialog::refreshThumbnailLabel(int sourceFrame){
    auto* label=mThumbnailLabels.value(sourceFrame,nullptr);if(!label)return;
    auto* item=mThumbnailItems.value(sourceFrame,nullptr);if(!item)return;
    const bool current=sourceFrame==mCurrentThumbnailSource;
    const QColor status=item->property("frameStatusColor").value<QColor>();
    const QColor background=current?QColor(25,118,210,90):status;
    item->setStyleSheet(QString("background:rgba(%1,%2,%3,%4);border:0;border-radius:4px;")
        .arg(background.red()).arg(background.green()).arg(background.blue()).arg(background.alpha())
        );
    QImage cached=mThumbnailCache.value(currentMountId()).value(sourceFrame);
    if(cached.isNull()&&mThumbnailDiskCache&&mThumbnailDiskCache->isValid()){
        cached.load(thumbnailCachePath(currentMountId(),sourceFrame));
        if(!cached.isNull())cacheThumbnail(currentMountId(),sourceFrame,cached,false);
    }
    if(cached.isNull()){label->setPixmap(QPixmap());return;}
    label->setPixmap(QPixmap::fromImage(cached));label->setText(QString());
}

void ClipPlayerDialog::setCurrentThumbnailFrame(int outputFrame){
    const int source=sourceFrameForOutput(outputFrame);if(source==mCurrentThumbnailSource)return;
    const int previous=mCurrentThumbnailSource;mCurrentThumbnailSource=source;
    if(previous>=0)refreshThumbnailLabel(previous);
    if(source>=0)refreshThumbnailLabel(source);
    centerCurrentThumbnail();
}

void ClipPlayerDialog::setDroppedCursorVisible(bool visible){
    if(mDroppedCursorVisible==visible)return;mDroppedCursorVisible=visible;updateCursorStyle();
}
void ClipPlayerDialog::setDuplicateCursorVisible(bool visible){
    if(mDuplicateCursorVisible==visible)return;mDuplicateCursorVisible=visible;updateCursorStyle();
}
void ClipPlayerDialog::updateCursorStyle(){
    const QString tooltip=QStringLiteral(
        "QToolTip{background-color:#1976d2;color:white;border:1px solid #64a9e8;padding:4px 6px;}");
    if(!mDroppedCursorVisible&&!mDuplicateCursorVisible){mPosition->setStyleSheet(tooltip);return;}
    const QString color=mDroppedCursorVisible?QStringLiteral("#e6bd24"):QStringLiteral("#d93636");
    const QString border=mDroppedCursorVisible?QStringLiteral("#ffe27a"):QStringLiteral("#ff8585");
    mPosition->setStyleSheet(tooltip+QString(
        " QSlider::handle:horizontal{background:%1;border:1px solid %2;"
        "width:16px;margin:-5px 0;border-radius:8px;}").arg(color,border));
}
void ClipPlayerDialog::centerCurrentThumbnail(){
    if(!mThumbnailScroll||!mThumbnailScroll->isVisible())return;
    auto* bar=mThumbnailScroll->horizontalScrollBar();
    const int center=8+mCurrentThumbnailSource*192+92;
    bar->setValue(std::clamp(center-mThumbnailScroll->viewport()->width()/2,
        bar->minimum(),bar->maximum()));
    updateVisibleThumbnailWidgets();
}

QString ClipPlayerDialog::thumbnailCachePath(int mountId,int sourceFrame)const{
    if(!mThumbnailDiskCache||!mThumbnailDiskCache->isValid())return {};
    return QDir(mThumbnailDiskCache->path()).filePath(
        QStringLiteral("%1-%2.jpg").arg(mountId).arg(sourceFrame));
}

void ClipPlayerDialog::cacheThumbnail(int mountId,int sourceFrame,const QImage& image,
                                      bool persistToDisk){
    constexpr int maximumCachedFrames=512;
    if(persistToDisk&&mThumbnailDiskCache&&mThumbnailDiskCache->isValid()){
        const QString path=thumbnailCachePath(mountId,sourceFrame);
        if(!QFileInfo::exists(path)&&!image.save(path,"JPG",80))
            spdlog::warn("Could not persist gallery thumbnail cache entry");
    }
    auto& cache=mThumbnailCache[mountId];const QPair<int,int> key{mountId,sourceFrame};
    mThumbnailCacheOrder.removeAll(key);mThumbnailCacheOrder.append(key);
    cache.insert(sourceFrame,image);
    while(mThumbnailCacheOrder.size()>maximumCachedFrames){
        const auto expired=mThumbnailCacheOrder.takeFirst();
        auto found=mThumbnailCache.find(expired.first);
        if(found==mThumbnailCache.end())continue;
        found->remove(expired.second);
        if(found->isEmpty())mThumbnailCache.erase(found);
    }
}

void ClipPlayerDialog::setSourceFrameThumbnail(int sourceFrame,const QByteArray& frame,int width,int height){
    if(!mThumbnailCollectionEnabled->load())return;
    const QImage thumbnail=rgb48Thumbnail(frame,width,height);if(thumbnail.isNull())return;
    cacheThumbnail(currentMountId(),sourceFrame,thumbnail);
    refreshThumbnailLabel(sourceFrame);
}

void ClipPlayerDialog::setOutputFrameThumbnail(int outputFrame,const QByteArray& frame,int width,int height){
    const int source=sourceFrameForOutput(outputFrame);if(source>=0)setSourceFrameThumbnail(source,frame,width,height);
}
void ClipPlayerDialog::presentDroppedSourceFrame(int sourceFrame,const QByteArray& frame,int width,int height){
    if(!mThumbnailCollectionEnabled->load()||mIndex<0)return;
    setSourceFrameThumbnail(sourceFrame,frame,width,height);
    const QImage image=rgb48Image(frame,width,height);if(image.isNull())return;
    mLastPresentedImage=image;mLastImageIsSource=true;mWaitingForFirstFrame=false;
    int cursor=0;const auto& map=mClips[mIndex].sourceFrameToOutput;
    if(map)for(int source=sourceFrame-1;source>=0;--source)
        if((*map)[source]>=0){cursor=(*map)[source];break;}
    mPosition->setValue(cursor);mPlaybackTarget->store(cursor);
    mPositionSeconds=cursor/std::max(1.0,mClips[mIndex].fps);
    const int previous=mCurrentThumbnailSource;mCurrentThumbnailSource=sourceFrame;
    if(previous>=0)refreshThumbnailLabel(previous);
    refreshThumbnailLabel(sourceFrame);
    setDuplicateCursorVisible(false);setDroppedCursorVisible(true);
    centerCurrentThumbnail();updateDisplayedImage();completeFrameStep();
}
void ClipPlayerDialog::clearFrameSelections(){
    for(auto& clip:mClips)clip.selectedSourceFrames.clear();
    if(mThumbnailScroll&&mThumbnailScroll->isVisible())rebuildThumbnailStrip();
}

void ClipPlayerDialog::startDecoder(){
    const QString exe=ffmpegPath();if(exe.isEmpty()){mPlaybackFailed=true;mFrameTimer.stop();if(mLastPresentedImage.isNull())mVideo->setText(tr("FFmpeg was not found"));return;}
    // openClip can run from the constructor, before Qt has performed its first
    // automatic layout pass. Activate both layouts so the decoder is sized to
    // the requested player window rather than QLabel's 640x360 minimum.
    if(layout())layout()->activate();
    if(mVideo->parentWidget()&&mVideo->parentWidget()->layout())
        mVideo->parentWidget()->layout()->activate();
    const QSize area=mVideo->size().expandedTo(QSize(640,360));mWidth=std::max(2,area.width()&~1);mHeight=std::max(2,area.height()&~1);mFrameBytes=mWidth*mHeight*4;
    mBytes.reserve(static_cast<qsizetype>(mFrameBytes)*3);
    const auto& clip=mClips[mIndex];
    const bool normalizeMixedFrames=!clip.isSequence;
    const int inputWidth=normalizeMixedFrames?mWidth:clip.width;
    const int inputHeight=normalizeMixedFrames?mHeight:clip.height;
    if(inputWidth<=0||inputHeight<=0){mPlaybackFailed=true;mFrameTimer.stop();if(mLastPresentedImage.isNull())mVideo->setText(tr("Invalid clip dimensions"));return;}
    mInputFrameBytes=inputWidth*inputHeight*(normalizeMixedFrames?4:6);
    QStringList a{"-hide_banner","-loglevel","error"};
    a<<"-f"<<"rawvideo"<<"-pixel_format"<<(normalizeMixedFrames?"rgba":"rgb48le")<<"-video_size"
     <<QString("%1x%2").arg(inputWidth).arg(inputHeight)<<"-framerate"
     <<QString::number(clip.fps,'g',9)<<"-i"<<"pipe:0";
    QString f;
    if(normalizeMixedFrames)
        f=QStringLiteral("format=rgba");
    else {
        const bool swap=clip.orientation==90||clip.orientation==270;
        const int displayInputWidth=swap?inputHeight:inputWidth;
        const int displayInputHeight=swap?inputWidth:inputHeight;
        // Scale-to-fit remains bounded by the player's advertised 3200%
        // maximum. Tiny sources are centered with padding instead of being
        // enlarged beyond that limit.
        const double scale=std::min({double(mWidth)/displayInputWidth,
                                     double(mHeight)/displayInputHeight,32.0});
        const int fitWidth=std::max(2,int(displayInputWidth*scale)&~1);
        const int fitHeight=std::max(2,int(displayInputHeight*scale)&~1);
        QString rotate;
        if(clip.orientation==90)rotate=QStringLiteral(",transpose=clock");
        else if(clip.orientation==180)rotate=QStringLiteral(",hflip,vflip");
        else if(clip.orientation==270)rotate=QStringLiteral(",transpose=cclock");
        const int preRotateWidth=swap?fitHeight:fitWidth;
        const int preRotateHeight=swap?fitWidth:fitHeight;
        if(mZoomPercent>0.0){
            // Crop before scaling. The decoder therefore never creates a
            // 32x full-resolution intermediate; memory tracks viewport size.
            const int targetPreWidth=swap?mHeight:mWidth;
            const int targetPreHeight=swap?mWidth:mHeight;
            const double zoom=mZoomPercent/100.0;
            const int cropWidth=std::max(1,std::min(inputWidth,
                qCeil(double(targetPreWidth)/zoom)));
            const int cropHeight=std::max(1,std::min(inputHeight,
                qCeil(double(targetPreHeight)/zoom)));
            const int scaledWidth=std::min(targetPreWidth,qRound(cropWidth*zoom));
            const int scaledHeight=std::min(targetPreHeight,qRound(cropHeight*zoom));
            const double maxPanX=std::max(0.0,(inputWidth-cropWidth)/2.0);
            const double maxPanY=std::max(0.0,(inputHeight-cropHeight)/2.0);
            mPanSourcePixels.setX(std::clamp(mPanSourcePixels.x(),-maxPanX,maxPanX));
            mPanSourcePixels.setY(std::clamp(mPanSourcePixels.y(),-maxPanY,maxPanY));
            const bool nearest=mZoomPercent>=400.0||
                std::abs(mZoomPercent/100.0-std::round(mZoomPercent/100.0))<0.0001;
            f=QString("crop=%1:%2:(iw-%1)/2+%3:(ih-%2)/2+%4,scale=%5:%6:flags=%7%8,pad=%9:%10:(ow-iw)/2:(oh-ih)/2:color=0x606060,format=rgba")
                .arg(cropWidth).arg(cropHeight).arg(mPanSourcePixels.x(),0,'f',3)
                .arg(mPanSourcePixels.y(),0,'f',3).arg(scaledWidth).arg(scaledHeight)
                .arg(nearest?QStringLiteral("neighbor"):QStringLiteral("bicubic"))
                .arg(rotate).arg(mWidth).arg(mHeight);
        }else{
            f=QString("scale=%1:%2%3,pad=%4:%5:(ow-iw)/2:(oh-ih)/2:color=0x606060,format=rgba")
                .arg(preRotateWidth).arg(preRotateHeight).arg(rotate).arg(mWidth).arg(mHeight);
        }
    }
    mDecoderSurfaceScale=surfaceScaleForZoom(mZoomPercent);
    mDecoderSurfacePan=mZoomPercent>0.0
        ?effectivePanForZoom(mPanSourcePixels,mZoomPercent):QPointF();
    a<<"-an"<<"-vf"<<f<<"-pix_fmt"<<"rgba"<<"-f"<<"rawvideo"<<"pipe:1";mDecoder.setProcessChannelMode(QProcess::SeparateChannels);mDecoder.start(exe,a,QIODevice::ReadWrite);
    updateFrameTimerInterval();mFrameTimer.start();
}

double ClipPlayerDialog::fitScale() const{
    if(mIndex<0||mIndex>=mClips.size()||mClips[mIndex].width<=0||mClips[mIndex].height<=0)return 1.0;
    const bool swap=mClips[mIndex].orientation==90||mClips[mIndex].orientation==270;
    const int width=swap?mClips[mIndex].height:mClips[mIndex].width;
    const int height=swap?mClips[mIndex].width:mClips[mIndex].height;
    return std::min({double(std::max(1,mVideo->width()))/width,
                     double(std::max(1,mVideo->height()))/height,32.0});
}
void ClipPlayerDialog::changeZoom(double wheelSteps){
    if(std::abs(wheelSteps)<0.0001||mIndex<0||mIndex>=mClips.size())return;
    const double minimum=fitScale()*100.0;
    if(mRequestedZoomPercent<=0.0)
        mRequestedZoomPercent=mZoomPercent>0.0?mZoomPercent:minimum;
    // Move through zoom space logarithmically. A wheel notch is an 8% scale
    // change, so it remains precise below 100% but covers the large range to
    // 3200% in a practical number of turns.
    mRequestedZoomPercent=std::clamp(mRequestedZoomPercent*
        std::exp(wheelSteps*std::log(1.08)),minimum,3200.0);
    double target=mRequestedZoomPercent;
    if(target<=minimum*1.05)target=0.0;
    else {
        const double integerScale=std::round(target/100.0)*100.0;
        if(integerScale>=minimum&&integerScale<=3200.0&&
           std::abs(target-integerScale)<=integerScale*0.05)target=integerScale;
    }
    setZoomAnimationTarget(target);
}

void ClipPlayerDialog::setZoomAnimationTarget(double target){
    const double minimum=fitScale()*100.0;
    const double current=mZoomPercent>0.0?mZoomPercent:minimum;
    const double effectiveTarget=target>0.0?target:minimum;
    if(std::abs(effectiveTarget-current)<0.01){
        mZoomPercent=target;mZoomAnimationTimer.stop();return;
    }
    mSurfaceUpdateTimer.stop();
    const bool wasAnimating=mZoomAnimationTimer.isActive();
    mZoomAnimationTarget=target;
    // Zooming out needs pixels outside the current efficient crop. Begin
    // preparing the wider target crop promptly and do not keep postponing it
    // for every event in the same wheel gesture. Zoom-in can safely continue
    // using the existing wider surface while input is coalesced.
    const bool zoomingOut=effectiveTarget<current;
    if(zoomingOut){
        if(!mSurfaceUpdateTimer.isActive()){
            mSurfaceUpdateTimer.setInterval(32);mSurfaceUpdateTimer.start();
        }
    }else{
        mSurfaceUpdateTimer.setInterval(48);mSurfaceUpdateTimer.start();
    }
    if(!wasAnimating){
        mZoomAnimationStartupDelay=true;
        mZoomAnimationClock.restart();
        mZoomAnimationTimer.start();
    }
}

void ClipPlayerDialog::advanceZoomAnimation(){
    constexpr double startupDelayMs=32.0; // Approximately two frames at 60 Hz.
    if(mZoomAnimationStartupDelay){
        if(mZoomAnimationClock.elapsed()<startupDelayMs)return;
        mZoomAnimationStartupDelay=false;
        mZoomAnimationClock.restart();
        return;
    }
    const double minimum=fitScale()*100.0;
    const double target=mZoomAnimationTarget>0.0?mZoomAnimationTarget:minimum;
    const double current=mZoomPercent>0.0?mZoomPercent:minimum;
    const double elapsed=std::clamp(double(mZoomAnimationClock.restart()),1.0,50.0);
    // A continuously retargetable low-pass in log space preserves velocity
    // across wheel events. It is monotonic for a fixed target and therefore
    // cannot overshoot or oscillate around snap points.
    const double alpha=1.0-std::exp(-elapsed/35.0);
    const double logDistance=std::log(std::max(0.0001,target)/std::max(0.0001,current));
    const bool finished=std::abs(logDistance)<0.003;
    const double value=finished?target:current*std::exp(logDistance*alpha);
    mZoomPercent=finished?mZoomAnimationTarget:value;
    if(mZoomPercent<=0.0)mPanSourcePixels=QPointF();
    else clampPanToZoom();
    updateDisplayedImage();
    const double shown=mZoomPercent>0.0?mZoomPercent:minimum;
    mTitle->setText(QString("%1 — %2 / %3 — %4").arg(mClips[mIndex].title)
        .arg(mIndex+1).arg(mClips.size()).arg(mZoomPercent>0.0
            ?tr("Zoom %1%").arg(shown,0,'f',1):tr("Scale to fit")));
    if(finished){
        mZoomAnimationTimer.stop();mZoomAnimationStartupDelay=false;
        const QPointF desiredScale=surfaceScaleForZoom(mZoomPercent);
        const QPointF desiredPan=mZoomPercent>0.0
            ?effectivePanForZoom(mPanSourcePixels,mZoomPercent):QPointF();
        const bool cropAlreadyPrepared=
            std::abs(mDecoderSurfaceScale.x()-desiredScale.x())<0.000001&&
            std::abs(mDecoderSurfaceScale.y()-desiredScale.y())<0.000001&&
            QLineF(mDecoderSurfacePan,desiredPan).length()<0.000001;
        if(!cropAlreadyPrepared){
            mSurfaceUpdateTimer.setInterval(16);mSurfaceUpdateTimer.start();
        }
    }
}

void ClipPlayerDialog::clampPanToZoom(){
    if(mZoomPercent<=0.0||mIndex<0||mIndex>=mClips.size()){
        mPanSourcePixels=QPointF();return;
    }
    const auto& clip=mClips[mIndex];
    if(clip.width<=0||clip.height<=0)return;
    const bool swap=clip.orientation==90||clip.orientation==270;
    const int viewportWidth=std::max(1,mWidth>0?mWidth:mVideo->width());
    const int viewportHeight=std::max(1,mHeight>0?mHeight:mVideo->height());
    const int targetPreWidth=swap?viewportHeight:viewportWidth;
    const int targetPreHeight=swap?viewportWidth:viewportHeight;
    const double zoom=mZoomPercent/100.0;
    const int cropWidth=std::max(1,std::min(clip.width,qCeil(targetPreWidth/zoom)));
    const int cropHeight=std::max(1,std::min(clip.height,qCeil(targetPreHeight/zoom)));
    const double maxPanX=std::max(0.0,(clip.width-cropWidth)/2.0);
    const double maxPanY=std::max(0.0,(clip.height-cropHeight)/2.0);
    mPanSourcePixels.setX(std::clamp(mPanSourcePixels.x(),-maxPanX,maxPanX));
    mPanSourcePixels.setY(std::clamp(mPanSourcePixels.y(),-maxPanY,maxPanY));
}

QPointF ClipPlayerDialog::effectivePanForZoom(const QPointF& pan,double zoomPercent)const{
    if(zoomPercent<=0.0||mIndex<0||mIndex>=mClips.size())return {};
    const auto& clip=mClips[mIndex];
    if(clip.width<=0||clip.height<=0)return pan;
    const bool swap=clip.orientation==90||clip.orientation==270;
    const int viewportWidth=std::max(1,mWidth>0?mWidth:mVideo->width());
    const int viewportHeight=std::max(1,mHeight>0?mHeight:mVideo->height());
    const int targetPreWidth=swap?viewportHeight:viewportWidth;
    const int targetPreHeight=swap?viewportWidth:viewportHeight;
    const double zoom=zoomPercent/100.0;
    const int cropWidth=std::max(1,std::min(clip.width,qCeil(targetPreWidth/zoom)));
    const int cropHeight=std::max(1,std::min(clip.height,qCeil(targetPreHeight/zoom)));
    const double centeredX=(clip.width-cropWidth)/2.0;
    const double centeredY=(clip.height-cropHeight)/2.0;
    // FFmpeg resolves crop origins to whole source pixels. Use that same
    // effective center in the retained-surface transform so the optimized
    // replacement cannot introduce a subpixel-phase position jump.
    return QPointF(qRound(centeredX+pan.x())-centeredX,
                   qRound(centeredY+pan.y())-centeredY);
}

QPointF ClipPlayerDialog::surfaceScaleForZoom(double zoomPercent)const{
    if(mIndex<0||mIndex>=mClips.size())return {1.0,1.0};
    const auto& clip=mClips[mIndex];
    if(clip.width<=0||clip.height<=0)return {1.0,1.0};
    const bool swap=clip.orientation==90||clip.orientation==270;
    const int viewportWidth=std::max(2,mWidth>0?mWidth:(mVideo->width()&~1));
    const int viewportHeight=std::max(2,mHeight>0?mHeight:(mVideo->height()&~1));
    if(zoomPercent<=0.0){
        const int displayWidth=swap?clip.height:clip.width;
        const int displayHeight=swap?clip.width:clip.height;
        const double scale=std::min({double(viewportWidth)/displayWidth,
                                     double(viewportHeight)/displayHeight,32.0});
        const int outputWidth=std::max(2,int(displayWidth*scale)&~1);
        const int outputHeight=std::max(2,int(displayHeight*scale)&~1);
        return {double(outputWidth)/displayWidth,double(outputHeight)/displayHeight};
    }
    const int targetPreWidth=swap?viewportHeight:viewportWidth;
    const int targetPreHeight=swap?viewportWidth:viewportHeight;
    const double zoom=zoomPercent/100.0;
    const int cropWidth=std::max(1,std::min(clip.width,qCeil(targetPreWidth/zoom)));
    const int cropHeight=std::max(1,std::min(clip.height,qCeil(targetPreHeight/zoom)));
    const int scaledWidth=std::min(targetPreWidth,qRound(cropWidth*zoom));
    const int scaledHeight=std::min(targetPreHeight,qRound(cropHeight*zoom));
    return swap?QPointF(double(scaledHeight)/cropHeight,double(scaledWidth)/cropWidth)
               :QPointF(double(scaledWidth)/cropWidth,double(scaledHeight)/cropHeight);
}

void ClipPlayerDialog::updateButtonIcons(){
    mPlayPause->setIcon(style()->standardIcon(mPaused?QStyle::SP_MediaPlay:QStyle::SP_MediaPause));
    mAudioButton->setIcon(style()->standardIcon(mAudioEnabled?QStyle::SP_MediaVolume:QStyle::SP_MediaVolumeMuted));
    if(mFullscreenButton)mFullscreenButton->setIcon(fullscreenIcon(isFullScreen()));
}
void ClipPlayerDialog::setOverlayVisible(bool visible){
    mOverlay->setAttribute(Qt::WA_TransparentForMouseEvents,!visible);
    mOverlayAnimation->stop();mOverlayAnimation->setStartValue(mOverlayOpacity->opacity());
    mOverlayAnimation->setEndValue(visible?1.0:0.0);mOverlayAnimation->start();
    if(!visible)QToolTip::hideText();
}
void ClipPlayerDialog::revealOverlay(){
    const bool alreadyFadingIn=mOverlayAnimation->state()==QAbstractAnimation::Running&&
        mOverlayAnimation->endValue().toDouble()>0.5;
    if(!alreadyFadingIn&&(mOverlayOpacity->opacity()<0.999||
                          mOverlayAnimation->state()==QAbstractAnimation::Running))
        setOverlayVisible(true);
    mOverlayTimer.start();
}
void ClipPlayerDialog::updateDisplayedImage(){
    // The old surface remains the interactive fallback while a decoder
    // refresh is pending. Suppressing it here made all wheel/drag input appear
    // ignored until the replacement frame arrived, especially during play.
    if(mLastPresentedImage.isNull())return;
    if(mLastImageIsSource){
        if(mZoomPercent<=0.0){
            const double scale=fitScale();
            const QSize target(std::max(1,qRound(mLastPresentedImage.width()*scale)),
                               std::max(1,qRound(mLastPresentedImage.height()*scale)));
            mVideo->setPixmap(QPixmap::fromImage(mLastPresentedImage).scaled(target,
                Qt::KeepAspectRatio,Qt::SmoothTransformation));return;
        }
    }else{
        // Transform the latest displayed playback frame while a replacement
        // surface is pending. This avoids jumping back to the first source
        // preview frame during wheel and drag interaction.
        const QPointF desiredScale=surfaceScaleForZoom(mZoomPercent);
        const QPointF ratio(desiredScale.x()/std::max(0.0001,mLastSurfaceScale.x()),
                            desiredScale.y()/std::max(0.0001,mLastSurfaceScale.y()));
        QPointF oldDisplayPan=mLastSurfacePan;
        QPointF newDisplayPan=mZoomPercent>0.0
            ?effectivePanForZoom(mPanSourcePixels,mZoomPercent):QPointF();
        const int orientation=mIndex>=0?mClips[mIndex].orientation:-1;
        auto orientPan=[orientation](const QPointF& pan){
            if(orientation==90)return QPointF(-pan.y(),pan.x());
            if(orientation==180)return -pan;
            if(orientation==270)return QPointF(pan.y(),-pan.x());
            return pan;
        };
        oldDisplayPan=orientPan(oldDisplayPan);newDisplayPan=orientPan(newDisplayPan);
        const QSizeF sourceSize(mLastPresentedImage.width()/ratio.x(),
                                mLastPresentedImage.height()/ratio.y());
        const QPointF center(mLastPresentedImage.width()/2.0+
                (newDisplayPan.x()-oldDisplayPan.x())*mLastSurfaceScale.x(),
            mLastPresentedImage.height()/2.0+
                (newDisplayPan.y()-oldDisplayPan.y())*mLastSurfaceScale.y());
        const QRectF source(center.x()-sourceSize.width()/2.0,
            center.y()-sourceSize.height()/2.0,sourceSize.width(),sourceSize.height());
        QImage canvas(mVideo->size(),QImage::Format_RGB888);canvas.fill(QColor(96,96,96));
        QPainter painter(&canvas);painter.setRenderHint(QPainter::SmoothPixmapTransform,
            mZoomPercent<400.0);painter.drawImage(QRectF(canvas.rect()),mLastPresentedImage,source);
        painter.end();mVideo->setPixmap(QPixmap::fromImage(canvas));return;
    }
    if(mZoomPercent<=0.0){
        mVideo->setPixmap(QPixmap::fromImage(mLastPresentedImage).scaled(mVideo->size(),
            Qt::KeepAspectRatio,Qt::SmoothTransformation));return;
    }
    const double zoom=mZoomPercent/100.0;
    QPointF displayPan=mPanSourcePixels;
    const int orientation=mIndex>=0?mClips[mIndex].orientation:-1;
    if(orientation==90)displayPan=QPointF(-mPanSourcePixels.y(),mPanSourcePixels.x());
    else if(orientation==180)displayPan=-mPanSourcePixels;
    else if(orientation==270)displayPan=QPointF(mPanSourcePixels.y(),-mPanSourcePixels.x());
    const int cropWidth=std::max(1,std::min(mLastPresentedImage.width(),qCeil(mVideo->width()/zoom)));
    const int cropHeight=std::max(1,std::min(mLastPresentedImage.height(),qCeil(mVideo->height()/zoom)));
    const int cropX=std::clamp(qRound((mLastPresentedImage.width()-cropWidth)/2.0+displayPan.x()),0,mLastPresentedImage.width()-cropWidth);
    const int cropY=std::clamp(qRound((mLastPresentedImage.height()-cropHeight)/2.0+displayPan.y()),0,mLastPresentedImage.height()-cropHeight);
    const bool nearest=mZoomPercent>=400.0||
        std::abs(mZoomPercent/100.0-std::round(mZoomPercent/100.0))<0.0001;
    QImage canvas(mVideo->size(),QImage::Format_RGB888);canvas.fill(QColor(96,96,96));
    const QSizeF scaledSize(cropWidth*zoom,cropHeight*zoom);
    const QRectF target((canvas.width()-scaledSize.width())/2.0,
        (canvas.height()-scaledSize.height())/2.0,scaledSize.width(),scaledSize.height());
    QPainter painter(&canvas);painter.setRenderHint(QPainter::SmoothPixmapTransform,!nearest);
    painter.drawImage(target,mLastPresentedImage,QRectF(cropX,cropY,cropWidth,cropHeight));painter.end();
    mVideo->setPixmap(QPixmap::fromImage(canvas));
}

void ClipPlayerDialog::updateFrameTimerInterval(){
    // Audio changes the authority used to select a frame, not the cadence at
    // which the UI needs to render. Polling at 5 ms made the UI read the video
    // pipe and enter presentation logic up to 200 times per second. A normal
    // frame-period tick is sufficient to detect lateness and skip frames.
    mFrameTimer.setInterval(std::max(1,qRound(
        1000.0/std::clamp(mClips[mIndex].fps,1.0,240.0))));
}
void ClipPlayerDialog::configureAudio(){
    if(mAudioSink){mAudioSink->stop();delete mAudioSink;mAudioSink=nullptr;}
    delete mAudioBuffer;mAudioBuffer=nullptr;mAudioPcm.clear();mAudioBytesPerSecond=0;mAudioBlockAlign=1;mAudioClockBaseMs=0;mAudioClock.invalidate();
    auto& clip=mClips[mIndex];
    const auto audio=clip.audioWav;
    const QByteArray sourceAudio=audio&&!audio->empty()
        ? QByteArray(reinterpret_cast<const char*>(audio->data()),static_cast<qsizetype>(audio->size()))
        : QByteArray();
    const bool canLoadFromSource=!clip.sourceAudioChecked&&!clip.sourceFile.isEmpty();
    const bool available=!sourceAudio.isEmpty();
    mAudioButton->setEnabled(available||canLoadFromSource);
    // Opening the gallery must not synchronously initialize PipeWire/CoreAudio
    // or copy and parse a potentially large WAV while audio is muted. Keep the
    // capability visible and defer all device/buffer work until the user opts in.
    if(!mAudioEnabled){
        updateButtonIcons();mAudioButton->setToolTip(tr("Unmute audio"));
        return;
    }
    if(!available){
        if(!canLoadFromSource){mAudioButton->setChecked(false);updateButtonIcons();}
        else {
            mAudioButton->setToolTip(tr("Preparing audio…"));
            if(mAudioEnabled&&!mAudioLoading)beginSourceAudioLoad();
        }
        return;
    }
    updateButtonIcons();mAudioButton->setToolTip(mAudioEnabled?tr("Mute audio"):tr("Unmute audio"));
    const auto* wav=reinterpret_cast<const uint8_t*>(sourceAudio.constData());const size_t size=static_cast<size_t>(sourceAudio.size());
    auto le16=[&](size_t at){return at+2<=size?uint16_t(wav[at]|uint16_t(wav[at+1])<<8):uint16_t(0);};
    auto le32=[&](size_t at){return at+4<=size?uint32_t(wav[at]|uint32_t(wav[at+1])<<8|uint32_t(wav[at+2])<<16|uint32_t(wav[at+3])<<24):uint32_t(0);};
    uint16_t encoding=0,channels=0,bits=0,validBits=0,blockAlign=0;uint32_t sampleRate=0;size_t dataAt=0,dataBytes=0;
    if(size>=12&&(std::memcmp(wav,"RIFF",4)==0||std::memcmp(wav,"RF64",4)==0)&&std::memcmp(wav+8,"WAVE",4)==0){
        for(size_t at=12;at+8<=size;){const uint32_t bytes=le32(at+4);const size_t payload=at+8;
            if(std::memcmp(wav+at,"data",4)==0){dataAt=payload;dataBytes=bytes==0xffffffffu?size-payload:std::min<size_t>(bytes,size-payload);break;}
            if(payload+bytes>size)break;
            if(std::memcmp(wav+at,"fmt ",4)==0&&bytes>=16){
                encoding=le16(payload);channels=le16(payload+2);sampleRate=le32(payload+4);blockAlign=le16(payload+12);bits=le16(payload+14);validBits=bits;
                // WAVE_FORMAT_EXTENSIBLE stores PCM/float in the first DWORD
                // of its subtype GUID and may carry 24 valid bits in a 32-bit container.
                if(encoding==0xfffe&&bytes>=40&&le16(payload+16)>=22){
                    validBits=le16(payload+18);const uint32_t subtype=le32(payload+24);
                    if(subtype==1||subtype==3)encoding=static_cast<uint16_t>(subtype);
                }
            }
            at=payload+bytes+(bytes&1u);}
    }
    const bool integerPcm=encoding==1&&(
        (bits==16&&validBits==16)||(bits==24&&validBits==24)||
        (bits==32&&(validBits==24||validBits==32)));
    const bool floatPcm=encoding==3&&bits==32;
    if(!dataAt||!dataBytes||!channels||!sampleRate||!blockAlign||(!integerPcm&&!floatPcm)){mAudioButton->setEnabled(false);mAudioButton->setChecked(false);return;}
    mAudioPcm=QByteArray(reinterpret_cast<const char*>(wav+dataAt),static_cast<qsizetype>(dataBytes));
    QAudioFormat format;format.setSampleRate(static_cast<int>(sampleRate));format.setChannelCount(channels);
#if QT_VERSION >= QT_VERSION_CHECK(6,0,0)
    if(bits==24){
        // Qt 6 has no packed Int24 sample format. Losslessly sign-extend each
        // sample into its Int32 container; no resampling or level conversion.
        QByteArray expanded;expanded.resize(static_cast<qsizetype>(dataBytes/3*4));
        const auto* input=reinterpret_cast<const uchar*>(mAudioPcm.constData());
        auto* output=reinterpret_cast<uchar*>(expanded.data());
        for(qsizetype sample=0;sample<mAudioPcm.size()/3;++sample){
            output[sample*4]=0;output[sample*4+1]=input[sample*3];
            output[sample*4+2]=input[sample*3+1];output[sample*4+3]=input[sample*3+2];
        }
        mAudioPcm=std::move(expanded);mAudioBlockAlign=channels*4;
        format.setSampleFormat(QAudioFormat::Int32);
    }else{
        mAudioBlockAlign=blockAlign;
        format.setSampleFormat(floatPcm?QAudioFormat::Float:
            (bits==32?QAudioFormat::Int32:QAudioFormat::Int16));
    }
    mAudioSink=new QAudioSink(QMediaDevices::defaultAudioOutput(),format,this);
#else
    mAudioBlockAlign=blockAlign;format.setCodec("audio/pcm");format.setSampleSize(bits);format.setByteOrder(QAudioFormat::LittleEndian);format.setSampleType(floatPcm?QAudioFormat::Float:QAudioFormat::SignedInt);mAudioSink=new QAudioOutput(QAudioDeviceInfo::defaultOutputDevice(),format,this);
#endif
    mAudioBytesPerSecond=static_cast<int>(sampleRate)*mAudioBlockAlign;
    mAudioBuffer=new QBuffer(&mAudioPcm,this);mAudioBuffer->open(QIODevice::ReadOnly);
    if(mAudioEnabled)mAudioStartPending=true;
}
void ClipPlayerDialog::beginSourceAudioLoad(){
    if(mIndex<0||mIndex>=mClips.size()||mAudioLoading)return;
    if(mAudioLoadCancelled->load())
        mAudioLoadCancelled=std::make_shared<std::atomic_bool>(false);
    mAudioLoading=true;mAudioButton->setToolTip(tr("Preparing audio…"));
    const int index=mIndex,generation=mAudioLoadGeneration;
    const Clip clip=mClips[index];const QString executable=ffmpegPath();
    const auto cancelled=mAudioLoadCancelled;
    auto* watcher=new QFutureWatcher<QByteArray>(this);
    connect(watcher,&QFutureWatcher<QByteArray>::finished,this,[this,watcher,index,generation,cancelled]{
        const QByteArray loaded=watcher->result();watcher->deleteLater();
        if(mClosing||generation!=mAudioLoadGeneration||index!=mIndex)return;
        mAudioLoading=false;
        if(cancelled->load()){
            if(mAudioEnabled){
                mAudioLoadCancelled=std::make_shared<std::atomic_bool>(false);
                beginSourceAudioLoad();
            }
            return;
        }
        auto& clip=mClips[index];clip.sourceAudioChecked=true;
        if(!loaded.isEmpty())clip.audioWav=std::make_shared<const std::vector<uint8_t>>(
            reinterpret_cast<const uint8_t*>(loaded.constData()),
            reinterpret_cast<const uint8_t*>(loaded.constData())+loaded.size());
        configureAudio();
        if(mAudioEnabled&&mAudioSink&&mFirstFrameReady&&!mPaused){
            startAudioAt(mPositionSeconds);mAudioStartPending=false;
        }
    });
    watcher->setFuture(QtConcurrent::run([clip,executable,cancelled]{return sourceAudioWav(clip,executable,cancelled);}));
}
QByteArray ClipPlayerDialog::sourceAudioWav(const Clip& clip,const QString& ffmpegExecutable,
                                            const std::shared_ptr<std::atomic_bool>& cancelled){
    const QFileInfo source(clip.sourceFile);
    const QString suffix=source.suffix().toLower();
    if(suffix==QStringLiteral("dng")||source.isDir()){
        const QDir directory(source.isDir()?source.absoluteFilePath():source.absolutePath());
        const QString sourceBase=source.completeBaseName();
        const auto files=directory.entryInfoList(QDir::Files|QDir::Readable,QDir::Name);
        for(const QFileInfo& candidate:files){
            if(candidate.suffix().compare(QStringLiteral("wav"),Qt::CaseInsensitive)!=0)continue;
            if(candidate.completeBaseName().compare(QStringLiteral("audio"),Qt::CaseInsensitive)!=0&&
               candidate.completeBaseName().compare(sourceBase,Qt::CaseInsensitive)!=0)continue;
            QFile file(candidate.absoluteFilePath());
            if(file.open(QIODevice::ReadOnly)){
                QByteArray result;
                while(!file.atEnd()&&!cancelled->load())result+=file.read(1024*1024);
                return cancelled->load()?QByteArray():result;
            }
        }
        return {};
    }
    if(suffix!=QStringLiteral("mov")&&suffix!=QStringLiteral("mp4")&&suffix!=QStringLiteral("mkv"))return {};
    const QString exe=ffmpegExecutable;if(exe.isEmpty())return {};
    auto extract=[&](const QString& codec){
        QProcess process;process.setProcessChannelMode(QProcess::SeparateChannels);
        process.start(exe,{QStringLiteral("-hide_banner"),QStringLiteral("-loglevel"),QStringLiteral("error"),
            QStringLiteral("-i"),source.absoluteFilePath(),QStringLiteral("-map"),QStringLiteral("0:a:0?"),
            QStringLiteral("-vn"),QStringLiteral("-c:a"),codec,
            QStringLiteral("-f"),QStringLiteral("wav"),QStringLiteral("pipe:1")},QIODevice::ReadOnly);
        QElapsedTimer timeout;timeout.start();
        while(process.state()==QProcess::Starting&&!cancelled->load()&&timeout.elapsed()<5000)
            process.waitForStarted(100);
        if(process.state()==QProcess::Starting||cancelled->load()){
            process.kill();process.waitForFinished();return QByteArray();
        }
        if(process.state()==QProcess::NotRunning)return QByteArray();
        QByteArray output;
        while(process.state()!=QProcess::NotRunning){
            process.waitForFinished(100);output+=process.readAllStandardOutput();process.readAllStandardError();
            if(cancelled->load()||timeout.elapsed()>=60000){process.kill();process.waitForFinished();return QByteArray();}
        }
        output+=process.readAllStandardOutput();
        return process.exitStatus()==QProcess::NormalExit&&process.exitCode()==0?output:QByteArray();
    };
    // Preserve embedded PCM (including Int24/Float32) without re-encoding.
    // Compressed tracks cannot be sent to QAudioSink and fall back to PCM16 decode.
    QByteArray output=extract(QStringLiteral("copy"));
    auto supportedPcm=[](const QByteArray& bytes){
        if(bytes.size()<36)return false;
        const auto* p=reinterpret_cast<const uchar*>(bytes.constData());
        auto u16=[&](int at){return at+2<=bytes.size()?uint16_t(p[at]|uint16_t(p[at+1])<<8):uint16_t(0);};
        auto u32=[&](int at){return at+4<=bytes.size()?uint32_t(p[at]|uint32_t(p[at+1])<<8|uint32_t(p[at+2])<<16|uint32_t(p[at+3])<<24):uint32_t(0);};
        for(int at=12;at+8<=bytes.size();){const uint32_t count=u32(at+4);const int payload=at+8;
            if(std::memcmp(p+at,"fmt ",4)==0&&count>=16&&payload+16<=bytes.size()){
                uint16_t format=u16(payload),bits=u16(payload+14),valid=bits;
                if(format==0xfffe&&count>=40&&payload+40<=bytes.size()){valid=u16(payload+18);format=static_cast<uint16_t>(u32(payload+24));}
                return (format==1&&((bits==16&&valid==16)||(bits==24&&valid==24)||(bits==32&&(valid==24||valid==32))))||(format==3&&bits==32);
            }
            if(count==0xffffffffu||payload>bytes.size()||count>uint32_t(bytes.size()-payload))break;
            at=payload+static_cast<int>(count+(count&1u));
        }
        return false;
    };
    if(!cancelled->load()&&!supportedPcm(output))output=extract(QStringLiteral("pcm_s16le"));
    return output;
}
qint64 ClipPlayerDialog::audioPositionMs()const{return mAudioClockBaseMs+(mAudioClock.isValid()?mAudioClock.elapsed():0);}
void ClipPlayerDialog::startAudioAt(double seconds){if(!mAudioSink||!mAudioBuffer||mAudioBytesPerSecond<=0)return;mAudioSink->stop();qint64 byte=qBound<qint64>(0,qRound64(seconds*mAudioBytesPerSecond),mAudioPcm.size());byte-=byte%std::max(1,mAudioBlockAlign);mAudioBuffer->seek(byte);mAudioClockBaseMs=qRound64(double(byte)*1000.0/mAudioBytesPerSecond);mAudioClock.start();mAudioSink->start(mAudioBuffer);}
void ClipPlayerDialog::setAudioEnabled(bool enabled){
    mAudioEnabled=enabled&&mAudioButton->isEnabled();
    updateButtonIcons();mAudioButton->setToolTip(mAudioEnabled?tr("Mute audio"):tr("Unmute audio"));
    if(mIndex>=0)updateFrameTimerInterval();
    if(mAudioEnabled){
        if(!mAudioSink){
            if(mIndex>=0&&mIndex<mClips.size()&&mClips[mIndex].audioWav)
                configureAudio();
            else
                beginSourceAudioLoad();
            if(!mAudioSink)return;
        }
        if(!mPaused&&mFirstFrameReady){startAudioAt(mPositionSeconds);mAudioStartPending=false;}
        else mAudioStartPending=true;
    }else{
        if(mAudioLoading)mAudioLoadCancelled->store(true);
        if(mAudioSink){mAudioSink->stop();mAudioClock.invalidate();mAudioStartPending=false;}
    }
}

void ClipPlayerDialog::consumeOutput(){
    // Bound decoded video to three frames. Leaving excess bytes in QProcess
    // applies back-pressure to FFmpeg instead of buffering an entire clip.
    constexpr int queueFrames=3;
    const qsizetype available=mBytes.size()-mBytesOffset;
    const qint64 room=std::max<qint64>(0,qint64(mFrameBytes)*
        (queueFrames-static_cast<int>(mFrames.size()))-available);
    if(room>0)mBytes+=mDecoder.read(room);
    while(mFrameBytes>0&&mBytes.size()-mBytesOffset>=mFrameBytes&&
          static_cast<int>(mFrames.size())<queueFrames&&!mSubmittedFrames.empty()){
        QImage view(reinterpret_cast<const uchar*>(mBytes.constData()+mBytesOffset),
                    mWidth,mHeight,mWidth*4,QImage::Format_RGBA8888);
        const int sourceFrame=mSubmittedFrames.front();mSubmittedFrames.pop_front();
        mFrames.push_back({view.copy(),sourceFrame});
        mBytesOffset+=mFrameBytes;
    }
    if(mBytesOffset==mBytes.size()){mBytes.clear();mBytesOffset=0;}
    else if(mBytesOffset>=qint64(mFrameBytes)*2){mBytes.remove(0,mBytesOffset);mBytesOffset=0;}
    if(mFrameStepInProgress&&!mFrames.empty())
        QTimer::singleShot(0,this,&ClipPlayerDialog::showNextFrame);
}
void ClipPlayerDialog::showNextFrame(){
    consumeOutput();
    if(mAudioEnabled&&!mAudioStartPending&&mAudioClock.isValid()&&mIndex>=0){
        const double fps=std::max(1.0,mClips[mIndex].fps);
        const int audioFrame=std::clamp(static_cast<int>(std::floor(
            audioPositionMs()*fps/1000.0)),0,mPosition->maximum());
        mPlaybackTarget->store(audioFrame);
        // A frame that has already finished rendering is always useful: show
        // it immediately when late, or wait until its boundary when early.
        // Dropping is restricted to the pre-materialization scheduler; if
        // rendering itself exceeds one frame period, discarding here would
        // reject every completed frame forever.
        if(mFrames.empty()||qRound(mPositionSeconds*fps)>audioFrame)return;
    }
    if(!mFrames.empty()){
        const QSize viewport=mVideo->size().expandedTo(QSize(640,360));
        const int viewportWidth=std::max(2,viewport.width()&~1);
        const int viewportHeight=std::max(2,viewport.height()&~1);
        if(mWidth!=viewportWidth||mHeight!=viewportHeight){
            // This frame belongs to a decoder created for an intermediate
            // resize geometry. Never promote it to the visible surface. The
            // pending restart will render the retained frame at final size.
            // Keep the input-ID and byte queues intact until openClip() stops
            // and resets the complete pipeline. Clearing only materialized
            // frames cannot relabel output still buffered inside FFmpeg.
            mFrames.clear();
            mWaitingForFirstFrame=!mLastPresentedImage.isNull();
            if(!mSurfaceUpdateTimer.isActive()){
                mSurfaceUpdateTimer.setInterval(300);mSurfaceUpdateTimer.start();
            }
            return;
        }
        const QueuedFrame queued=std::move(mFrames.front());mFrames.pop_front();
        mLastPresentedImage=queued.image;
        mWaitingForFirstFrame=false;
        mLastImageIsSource=false;
        mLastSurfaceScale=mDecoderSurfaceScale;
        mLastSurfacePan=mDecoderSurfacePan;
        updateDisplayedImage();
        if(!mFirstFrameReady){
            mFirstFrameReady=true;
            emit firstFramePresented(currentMountId());
            if(mPaused&&mThumbnailScroll&&mThumbnailScroll->isVisible())
                emit thumbnailBackfillRequested(currentMountId(),mPositionSeconds);
            if(mAudioEnabled&&mAudioStartPending&&!mPaused){startAudioAt(mPositionSeconds);mAudioStartPending=false;}
        }
        const double fps=std::max(1.0,mClips[mIndex].fps);
        const int presentedFrame=std::clamp(queued.sourceFrame,0,mPosition->maximum());
        setDroppedCursorVisible(false);
        const auto& duplicateMask=mClips[mIndex].duplicateFrames;
        setDuplicateCursorVisible(duplicateMask&&presentedFrame<static_cast<int>(duplicateMask->size())&&
            (*duplicateMask)[presentedFrame]);
        if(!mPosition->isSliderDown())mPosition->setValue(presentedFrame);
        setCurrentThumbnailFrame(presentedFrame);
        emit framePresented(currentMountId(),presentedFrame);
        mPositionSeconds=(presentedFrame+1)/fps;
        completeFrameStep();
        // A paused surface refresh still needs to consume and present its
        // first frame. Stop only after that replacement has reached the UI.
        if(mPaused)mFrameTimer.stop();
    }else{
        if(!mPlaybackFailed&&mDecoder.state()==QProcess::NotRunning&&mBytes.isEmpty()){
            mFrameTimer.stop();if(mClips[mIndex].autoAdvance&&!mThumbnailScroll->isVisible())advance();else {mPaused=true;updateButtonIcons();}
        }
    }
}

int ClipPlayerDialog::frameAtSliderPosition(int x)const{
    if(!mPosition||mPosition->maximum()<=0)return 0;
    const int handle=mPosition->style()->pixelMetric(QStyle::PM_SliderLength,nullptr,mPosition);
    const int span=std::max(1,mPosition->width()-handle);
    return QStyle::sliderValueFromPosition(0,mPosition->maximum(),
        std::clamp(x-handle/2,0,span),span,mPosition->invertedAppearance());
}
void ClipPlayerDialog::updateSeekPosition(int x){
    const int frame=frameAtSliderPosition(x);mPosition->setValue(frame);
    QToolTip::showText(mPosition->mapToGlobal(QPoint(x,0)),framePositionText(frame),mPosition);
}
void ClipPlayerDialog::seekToFrame(int frame){
    if(mIndex<0||mIndex>=mClips.size())return;
    setDroppedCursorVisible(false);
    setDuplicateCursorVisible(false);
    const bool wasPaused=mPaused;
    openClip(mIndex,std::clamp(frame,0,mPosition->maximum())/
        std::max(1.0,mClips[mIndex].fps));
    if(wasPaused){
        mPaused=true;updateButtonIcons();
        // Keep the timer alive until showNextFrame() presents one refreshed
        // crop. Previously it was stopped here, leaving paused zoom/pan
        // updates queued invisibly until Play was pressed.
        if(mClips[mIndex].sourceFrames>1&&!mFrameTimer.isActive())mFrameTimer.start();
    }
}
void ClipPlayerDialog::stepFrame(int direction){
    if(mIndex<0||!mPosition||direction==0)return;
    direction=direction>0?1:-1;
    if(mFrameStepInProgress){mQueuedFrameStep=direction;return;}
    mFrameStepInProgress=true;
    if(!mPaused)mPlayPause->click();
    const auto sourceCount=[this](int index){
        const auto& map=mClips[index].sourceFrameToOutput;
        return map?static_cast<int>(map->size()):mClips[index].sourceFrames;
    };
    int source=mCurrentThumbnailSource;
    if(source<0)source=sourceFrameForOutput(mPosition->value());
    int targetSource=source+direction;
    if(direction>0&&targetSource>=sourceCount(mIndex)&&!mClips.isEmpty()){
        const int next=(mIndex+1)%mClips.size();const auto& nextMap=mClips[next].sourceFrameToOutput;
        int output=0;
        if(nextMap)for(int candidate=0;candidate<static_cast<int>(nextMap->size());++candidate)
            if((*nextMap)[candidate]>=0){output=(*nextMap)[candidate];break;}
        openClip(next,output/std::max(1.0,mClips[next].fps));
        mPaused=true;updateButtonIcons();
        if(mClips[mIndex].sourceFrames>1&&!mFrameTimer.isActive())mFrameTimer.start();
        if(nextMap&&!nextMap->empty()&&(*nextMap)[0]<0)
            emit sourceFrameThumbnailRequested(currentMountId(),0);
        return;
    }
    if(direction<0&&targetSource<0&&!mClips.isEmpty()){
        const int previous=(mIndex-1+mClips.size())%mClips.size();
        targetSource=std::max(0,sourceCount(previous)-1);
        const auto& previousMap=mClips[previous].sourceFrameToOutput;
        int output=targetSource;
        if(previousMap){
            output=0;for(int candidate=targetSource;candidate>=0;--candidate)
                if((*previousMap)[candidate]>=0){output=(*previousMap)[candidate];break;}
        }
        openClip(previous,output/std::max(1.0,mClips[previous].fps));
        mPaused=true;updateButtonIcons();
        if(mClips[mIndex].sourceFrames>1&&!mFrameTimer.isActive())mFrameTimer.start();
        if(previousMap&&(*previousMap)[targetSource]<0)
            emit sourceFrameThumbnailRequested(currentMountId(),targetSource);
        return;
    }
    const auto& map=mClips[mIndex].sourceFrameToOutput;
    if(map&&(*map)[targetSource]<0){
        emit sourceFrameThumbnailRequested(currentMountId(),targetSource);return;
    }
    seekToFrame(map?(*map)[targetSource]:targetSource);
}
void ClipPlayerDialog::completeFrameStep(){
    if(!mFrameStepInProgress)return;
    mFrameStepInProgress=false;
    const int queued=mQueuedFrameStep;mQueuedFrameStep=0;
    if(queued)QTimer::singleShot(0,this,[this,queued]{stepFrame(queued);});
}
QString ClipPlayerDialog::runtimeText(double seconds){
    const qint64 milliseconds=std::max<qint64>(0,qRound64(seconds*1000.0));
    const qint64 hours=milliseconds/3600000;
    const qint64 minutes=(milliseconds/60000)%60;
    const qint64 secs=(milliseconds/1000)%60;
    const qint64 millis=milliseconds%1000;
    return QStringLiteral("%1:%2:%3.%4").arg(hours,2,10,QChar('0'))
        .arg(minutes,2,10,QChar('0')).arg(secs,2,10,QChar('0'))
        .arg(millis,3,10,QChar('0'));
}
QString ClipPlayerDialog::framePositionText(int frame)const{
    if(mIndex<0||mIndex>=mClips.size())return {};
    const auto& clip=mClips[mIndex];
    return tr("Frame %1 / %2\n%3 / %4")
        .arg(frame+1).arg(std::max(1,clip.sourceFrames))
        .arg(runtimeText(frame/std::max(1.0,clip.fps)))
        .arg(runtimeText(std::max(0.0,clip.durationSeconds)));
}
bool ClipPlayerDialog::eventFilter(QObject* watched,QEvent* event){
    const auto* watchedWidget=qobject_cast<QWidget*>(watched);
    const bool belongsToPlayer=watchedWidget&&watchedWidget->window()==this;
    if(belongsToPlayer&&event->type()==QEvent::KeyPress){
        auto* key=static_cast<QKeyEvent*>(event);
        if(key->key()==Qt::Key_Left||key->key()==Qt::Key_Right){
            stepFrame(key->key()==Qt::Key_Right?1:-1);event->accept();return true;
        }
    }
    if(belongsToPlayer&&(event->type()==QEvent::MouseMove||
                         event->type()==QEvent::MouseButtonPress||
                         event->type()==QEvent::Enter))
        revealOverlay();
    if(belongsToPlayer&&event->type()==QEvent::Wheel){
        revealOverlay();
        const auto* wheel=static_cast<QWheelEvent*>(event);
        const bool overThumbnails=mThumbnailScroll&&mThumbnailScroll->isVisible()&&
            watchedWidget&&(watchedWidget==mThumbnailScroll||mThumbnailScroll->isAncestorOf(watchedWidget));
        if(overThumbnails){
            const QPoint pixels=wheel->pixelDelta();const QPoint angle=wheel->angleDelta();
            const int delta=!pixels.isNull()?(pixels.x()!=0?pixels.x():pixels.y()):
                (angle.x()!=0?angle.x():angle.y())/2;
            auto* bar=mThumbnailScroll->horizontalScrollBar();
            bar->setValue(bar->value()-delta);event->accept();return true;
        }
        const double steps=!wheel->pixelDelta().isNull()
            ? wheel->pixelDelta().y()/40.0 : wheel->angleDelta().y()/120.0;
        changeZoom(steps);event->accept();return true;
    }
    const bool panSurface=watched==mVideo||watched==mOverlay;
    if(belongsToPlayer&&panSurface&&event->type()==QEvent::MouseButtonDblClick){
        auto* mouse=static_cast<QMouseEvent*>(event);
        if(mouse->button()==Qt::LeftButton){
            mPanning=false;unsetCursor();mFullscreenButton->click();return true;
        }
    }
    // Once a pan starts, keep tracking it across overlay children and outside
    // the image widget. The application-level filter still receives those
    // events, and accepting the release prevents a stuck drag state.
    if(belongsToPlayer&&(panSurface||mPanning)&&mZoomPercent>0.0){
        auto* mouse=event->type()==QEvent::MouseButtonPress||event->type()==QEvent::MouseMove||
            event->type()==QEvent::MouseButtonRelease?static_cast<QMouseEvent*>(event):nullptr;
        if(mouse&&event->type()==QEvent::MouseButtonPress&&mouse->button()==Qt::LeftButton){
            mSurfaceUpdateTimer.stop();mPanning=true;
#if QT_VERSION >= QT_VERSION_CHECK(6,0,0)
            mLastPanGlobal=mouse->globalPosition();
#else
            mLastPanGlobal=mouse->globalPos();
#endif
            setCursor(Qt::ClosedHandCursor);return true;
        }
        if(mouse&&event->type()==QEvent::MouseMove&&mPanning){
#if QT_VERSION >= QT_VERSION_CHECK(6,0,0)
            const QPointF global=mouse->globalPosition();
#else
            const QPointF global=mouse->globalPos();
#endif
            const QPointF displayDelta=-(global-mLastPanGlobal)/(mZoomPercent/100.0);
            QPointF sourceDelta=displayDelta;const int orientation=mClips[mIndex].orientation;
            if(orientation==90)sourceDelta=QPointF(displayDelta.y(),-displayDelta.x());
            else if(orientation==180)sourceDelta=-displayDelta;
            else if(orientation==270)sourceDelta=QPointF(-displayDelta.y(),displayDelta.x());
            mPanSourcePixels+=sourceDelta;clampPanToZoom();
            mLastPanGlobal=global;updateDisplayedImage();return true;
        }
        if(mouse&&event->type()==QEvent::MouseButtonRelease&&mPanning&&mouse->button()==Qt::LeftButton){
            mPanning=false;unsetCursor();
            mSurfaceUpdateTimer.setInterval(16);mSurfaceUpdateTimer.start();return true;
        }
    }
    if(watched!=mPosition)return QDialog::eventFilter(watched,event);
    if(event->type()==QEvent::MouseButtonPress){
        auto* mouse=static_cast<QMouseEvent*>(event);
        if(mouse->button()==Qt::LeftButton){mSeeking=true;mPosition->setSliderDown(true);
#if QT_VERSION >= QT_VERSION_CHECK(6,0,0)
            updateSeekPosition(mouse->position().toPoint().x());
#else
            updateSeekPosition(mouse->pos().x());
#endif
            return true;}
    }else if(event->type()==QEvent::MouseMove){
        auto* mouse=static_cast<QMouseEvent*>(event);
#if QT_VERSION >= QT_VERSION_CHECK(6,0,0)
        const int x=mouse->position().toPoint().x();
#else
        const int x=mouse->pos().x();
#endif
        if(mSeeking)updateSeekPosition(x);
        else QToolTip::showText(mPosition->mapToGlobal(QPoint(x,0)),
            framePositionText(frameAtSliderPosition(x)),mPosition);
        return mSeeking;
    }else if(event->type()==QEvent::MouseButtonRelease){
        auto* mouse=static_cast<QMouseEvent*>(event);
        if(mSeeking&&mouse->button()==Qt::LeftButton){
#if QT_VERSION >= QT_VERSION_CHECK(6,0,0)
            updateSeekPosition(mouse->position().toPoint().x());
#else
            updateSeekPosition(mouse->pos().x());
#endif
            mSeeking=false;mPosition->setSliderDown(false);seekToFrame(mPosition->value());return true;}
    }else if(event->type()==QEvent::Leave){QToolTip::hideText();}
    return QDialog::eventFilter(watched,event);
}
void ClipPlayerDialog::decoderFinished(int code,QProcess::ExitStatus status){
    if(mClosing||mStoppingDecoder)return;consumeOutput();
    const QString decoderError=QString::fromUtf8(mDecoder.readAllStandardError()).trimmed();
    if(!decoderError.isEmpty())spdlog::warn("Gallery FFmpeg: {}",decoderError.toStdString());
    if(mFrames.empty()&&(status!=QProcess::NormalExit||code!=0)){
        mPlaybackFailed=true;mFrameTimer.stop();
        QString detail=decoderError;
        if(detail.size()>1200)detail=detail.right(1200);
        mTitle->setText(detail.isEmpty()?tr("FFmpeg could not process the RGB frame stream")
                                        :tr("FFmpeg playback failed: %1").arg(detail));
    }
}
QImage ClipPlayerDialog::rgb48Image(const QByteArray& frame,int width,int height)const{
    if(width<=0||height<=0||frame.size()<qint64(width)*height*6)return {};
    const int orientation=mIndex>=0?mClips[mIndex].orientation:-1;
    const bool swap=orientation==90||orientation==270;
    QImage image(swap?height:width,swap?width:height,QImage::Format_RGB888);
    const auto* source=reinterpret_cast<const uchar*>(frame.constData());
    for(int y=0;y<height;++y)for(int x=0;x<width;++x){
        int outputX=x,outputY=y;
        if(orientation==90){outputX=height-1-y;outputY=x;}
        else if(orientation==180){outputX=width-1-x;outputY=height-1-y;}
        else if(orientation==270){outputX=y;outputY=width-1-x;}
        const qsizetype input=(qsizetype(y)*width+x)*6;
        uchar* destination=image.scanLine(outputY)+qsizetype(outputX)*3;
        destination[0]=source[input+1];destination[1]=source[input+3];destination[2]=source[input+5];
    }
    return image;
}
QImage ClipPlayerDialog::rgb48Thumbnail(const QByteArray& frame,int width,int height)const{
    if(width<=0||height<=0||frame.size()<qint64(width)*height*6)return {};
    const int orientation=mIndex>=0?mClips[mIndex].orientation:-1;
    const bool swap=orientation==90||orientation==270;
    const int orientedWidth=swap?height:width,orientedHeight=swap?width:height;
    const double scale=std::min(176.0/orientedWidth,112.0/orientedHeight);
    const int outputWidth=std::max(1,static_cast<int>(std::floor(orientedWidth*scale)));
    const int outputHeight=std::max(1,static_cast<int>(std::floor(orientedHeight*scale)));
    QImage image(outputWidth,outputHeight,QImage::Format_RGB888);
    const auto* source=reinterpret_cast<const uchar*>(frame.constData());
    for(int y=0;y<outputHeight;++y){
        auto* destination=image.scanLine(y);
        const int orientedY=std::min(orientedHeight-1,y*orientedHeight/outputHeight);
        for(int x=0;x<outputWidth;++x){
            const int orientedX=std::min(orientedWidth-1,x*orientedWidth/outputWidth);
            int sourceX=orientedX,sourceY=orientedY;
            if(orientation==90){sourceX=orientedY;sourceY=height-1-orientedX;}
            else if(orientation==180){sourceX=width-1-orientedX;sourceY=height-1-orientedY;}
            else if(orientation==270){sourceX=width-1-orientedY;sourceY=orientedX;}
            const qsizetype input=(qsizetype(sourceY)*width+sourceX)*6;
            destination[x*3]=source[input+1];
            destination[x*3+1]=source[input+3];
            destination[x*3+2]=source[input+5];
        }
    }
    return image;
}
ClipPlayerDialog::FramePushResult ClipPlayerDialog::pushRgb48Frame(
        const QByteArray& frame,int width,int height){
    // Stills are presented directly from the processed RGB48 callback. Feeding
    // them through a timed FFmpeg graph would display the same image twice and
    // can apply the video surface's output aspect ratio to the second copy.
    if(mIndex>=0&&mClips[mIndex].sourceFrames<=1)return FramePushResult::Accepted;
    if(mClosing||mPlaybackFailed)return FramePushResult::Stopped;
    if(mDecoder.state()!=QProcess::Running)return FramePushResult::Retry;
    if(mAudioEnabled&&mIncomingFrame->load()>mNextInputFrame){
        mNextInputFrame=mIncomingFrame->load();
        mPositionSeconds=mNextInputFrame/std::max(1.0,mClips[mIndex].fps);
    }
    // Bound frames accepted by the complete transform pipeline, including
    // data FFmpeg has already consumed from QProcess but not returned yet.
    // bytesToWrite() alone cannot observe that internal backlog.
    if(mSubmittedFrames.size()>=6)return FramePushResult::Retry;
    if(mDecoder.bytesToWrite()>qint64(mInputFrameBytes)*2)return FramePushResult::Retry;
    if(mClips[mIndex].isSequence){
        if(width!=mClips[mIndex].width||height!=mClips[mIndex].height||
           frame.size()!=qint64(width)*height*6)return FramePushResult::Stopped;
        if(mDecoder.write(frame)!=frame.size())return FramePushResult::Retry;
        mSubmittedFrames.push_back(mNextInputFrame);
        ++mNextInputFrame;return FramePushResult::Accepted;
    }
    const QImage source=rgb48Image(frame,width,height);if(source.isNull())return FramePushResult::Stopped;
    QImage canvas(mWidth,mHeight,QImage::Format_RGBA8888);canvas.fill(QColor(96,96,96));
    const QImage scaled=source.scaled(canvas.size(),Qt::KeepAspectRatio,Qt::SmoothTransformation);
    QPainter painter(&canvas);painter.drawImage((mWidth-scaled.width())/2,(mHeight-scaled.height())/2,scaled);painter.end();
    const QByteArray bytes(reinterpret_cast<const char*>(canvas.constBits()),canvas.sizeInBytes());
    if(mDecoder.write(bytes)!=bytes.size())return FramePushResult::Retry;
    mSubmittedFrames.push_back(mNextInputFrame);
    ++mNextInputFrame;return FramePushResult::Accepted;
}
void ClipPlayerDialog::presentRgb48Frame(const QByteArray& frame,int width,int height){
    if(mIndex>=0&&mClips[mIndex].sourceFrames>1&&mClips[mIndex].isSequence&&
       (mClips[mIndex].width!=width||mClips[mIndex].height!=height)){
        // Draft and other preprocessing modes may change the RGB staging
        // dimensions relative to FileInfo. Configure rawvideo from the first
        // actual frame so the producer can never retry a permanent mismatch.
        stopDecoder();
        mClips[mIndex].width=width;
        mClips[mIndex].height=height;
        startDecoder();
    }
    // Video must become visible only after FFmpeg has produced the final
    // viewport-sized surface. Showing this source preview during a seek or
    // resize releases the held frame too early and briefly scales it using
    // geometry that does not belong to the active decoder.
    if(mIndex>=0&&mClips[mIndex].sourceFrames>1)return;
    const QImage image=rgb48Image(frame,width,height);if(image.isNull())return;
    mLastPresentedImage=image;mLastImageIsSource=true;
    mWaitingForFirstFrame=false;
    setDroppedCursorVisible(false);
    setDuplicateCursorVisible(false);
    updateDisplayedImage();
    setCurrentThumbnailFrame(0);
    mFirstFrameReady=true;
    emit firstFramePresented(currentMountId());
    completeFrameStep();
    if(mAudioEnabled&&mAudioStartPending&&!mPaused){
        startAudioAt(mPositionSeconds);mAudioStartPending=false;
    }
    spdlog::info("Gallery presented processed RGB48 frame {}x{}",width,height);
}
void ClipPlayerDialog::finishRgb48Frames(){if(mIndex>=0&&mClips[mIndex].sourceFrames<=1)return;if(mDecoder.state()==QProcess::Running)mDecoder.closeWriteChannel();}
void ClipPlayerDialog::failRgb48Frames(const QString& error){mPlaybackFailed=true;mFrameTimer.stop();mTitle->setText(tr("Frame rendering failed: %1").arg(error));mDecoder.kill();completeFrameStep();}
void ClipPlayerDialog::advance(){if(!mClips.isEmpty())openClip((mIndex+1)%mClips.size());}
void ClipPlayerDialog::closeEvent(QCloseEvent* e){
    mClosing=true;
    emit playbackClosed();
    stopDecoder();
    QDialog::closeEvent(e);
}
void ClipPlayerDialog::resizeEvent(QResizeEvent* e){
    QDialog::resizeEvent(e);
    updateVisibleThumbnailWidgets();
    // Scale-to-fit is viewport-relative. Rebase the hidden wheel
    // accumulator immediately so the next gesture starts at the new fit,
    // rather than jumping from the previous window geometry.
    if(mZoomPercent<=0.0&&mIndex>=0)
        mRequestedZoomPercent=fitScale()*100.0;
    // Never redraw the old frame into the new widget geometry here. Doing so
    // stretches an old-resolution surface during live resize/fullscreen
    // transitions. Also stop presentation immediately: otherwise a frame
    // already buffered by the old decoder can arrive after this event and be
    // composed into the new geometry. Keep the existing pixmap centered at
    // its original pixel size until openClip() starts the replacement surface.
    if(isVisible()&&mIndex>=0&&!mLastPresentedImage.isNull()){
        mWaitingForFirstFrame=!mLastPresentedImage.isNull();
        // An established decoder must stop presenting old-size buffered
        // frames immediately. A decoder whose first frame is still pending
        // remains ticking so the size guard in showNextFrame() can reject it.
        if(mFirstFrameReady)mFrameTimer.stop();
        mSurfaceUpdateTimer.setInterval(300);mSurfaceUpdateTimer.start();
    }
}
void ClipPlayerDialog::keyPressEvent(QKeyEvent* e){
    if(e->key()==Qt::Key_Escape&&isFullScreen()){showNormal();updateButtonIcons();e->accept();return;}
    if(e->key()==Qt::Key_F){mFullscreenButton->click();e->accept();return;}
    if(e->key()==Qt::Key_Space){mPlayPause->click();e->accept();return;}
    if((e->key()==Qt::Key_Right||e->key()==Qt::Key_Left)&&mIndex>=0){
        stepFrame(e->key()==Qt::Key_Right?1:-1);
        e->accept();return;
    }
    QDialog::keyPressEvent(e);
}
