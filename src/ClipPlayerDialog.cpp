#include "ClipPlayerDialog.h"
#include <QCloseEvent>
#include <QCoreApplication>
#include <QDir>
#include <QEvent>
#include <QFile>
#include <QFileInfo>
#include <QFutureWatcher>
#include <QBuffer>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QMouseEvent>
#include <QPainter>
#include <QPushButton>
#include <QResizeEvent>
#include <QSlider>
#include <QStandardPaths>
#include <QStyle>
#include <QToolTip>
#include <QVBoxLayout>
#include <QtConcurrent>
#include <algorithm>
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

ClipPlayerDialog::ClipPlayerDialog(QVector<Clip> clips, int initialMountId, QWidget* parent)
    : QDialog(parent), mClips(std::move(clips)) {
    setAttribute(Qt::WA_DeleteOnClose); setWindowTitle(tr("Mounted clip gallery")); resize(1100,720);
    auto* layout=new QVBoxLayout(this); mTitle=new QLabel(this); layout->addWidget(mTitle);
    mVideo=new QLabel(tr("Preparing clip…"),this); mVideo->setAlignment(Qt::AlignCenter);
    mVideo->setMinimumSize(640,360); mVideo->setStyleSheet("background:#080808;color:#aaa;"); layout->addWidget(mVideo,1);
    auto* controls=new QHBoxLayout; auto* previous=new QPushButton(tr("Previous"),this);
    mPlayPause=new QPushButton(tr("Pause"),this); auto* next=new QPushButton(tr("Next"),this);
    mAudioButton=new QPushButton(tr("Audio: Muted"),this);mAudioButton->setCheckable(true);
    mPosition=new QSlider(Qt::Horizontal,this); mPosition->setRange(0,0);
    mPosition->setMouseTracking(true); mPosition->installEventFilter(this);
    controls->addWidget(previous); controls->addWidget(mPlayPause); controls->addWidget(next); controls->addWidget(mAudioButton); controls->addWidget(mPosition,1); layout->addLayout(controls);
    connect(&mDecoder,&QProcess::readyReadStandardOutput,this,&ClipPlayerDialog::consumeOutput);
    connect(&mDecoder,qOverload<int,QProcess::ExitStatus>(&QProcess::finished),this,&ClipPlayerDialog::decoderFinished);
    connect(&mDecoder, &QProcess::errorOccurred, this, [this](QProcess::ProcessError error) {
        if (mClosing || mStoppingDecoder || error == QProcess::Crashed) return;
        mPlaybackFailed = true;
        mFrameTimer.stop();
        const QString detail = mDecoder.errorString();
        mTitle->setText(tr("FFmpeg could not be started: %1").arg(detail));
        mVideo->setText(tr("Playback unavailable"));
        spdlog::warn("Gallery FFmpeg could not be started: {}", detail.toStdString());
    });
    connect(&mFrameTimer,&QTimer::timeout,this,&ClipPlayerDialog::showNextFrame);
    connect(mPlayPause,&QPushButton::clicked,this,[this]{
        mPaused=!mPaused;mPlayPause->setText(mPaused?tr("Play"):tr("Pause"));
        if(mPaused){mFrameTimer.stop();if(mAudioEnabled&&mAudioSink){mAudioClockBaseMs=audioPositionMs();mAudioClock.invalidate();mAudioSink->suspend();}}
        else {updateFrameTimerInterval();mFrameTimer.start();if(mAudioEnabled&&mAudioSink){
            if(mAudioStartPending&&mFirstFrameReady){startAudioAt(mPositionSeconds);mAudioStartPending=false;}
            else if(!mAudioStartPending){mAudioClock.start();mAudioSink->resume();}
        }}
    });
    connect(mAudioButton,&QPushButton::toggled,this,&ClipPlayerDialog::setAudioEnabled);
    connect(previous,&QPushButton::clicked,this,[this]{ if(!mClips.isEmpty())openClip((mIndex-1+mClips.size())%mClips.size()); });
    connect(next,&QPushButton::clicked,this,&ClipPlayerDialog::advance);
    mIndex=0; for(int i=0;i<mClips.size();++i)if(mClips[i].mountId==initialMountId){mIndex=i;break;}
    if(!mClips.isEmpty())openClip(mIndex);
}

ClipPlayerDialog::~ClipPlayerDialog(){mClosing=true;mAudioLoadCancelled->store(true);++mAudioLoadGeneration;stopDecoder();}
int ClipPlayerDialog::currentMountId()const{return mIndex>=0&&mIndex<mClips.size()?mClips[mIndex].mountId:-1;}
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
void ClipPlayerDialog::reloadCurrentClip(){if(mIndex>=0)openClip(mIndex,mPositionSeconds);}

void ClipPlayerDialog::stopDecoder(){
    mFrameTimer.stop();
    if(mDecoder.state()==QProcess::NotRunning)return;
    mStoppingDecoder=true;
    mDecoder.closeWriteChannel();
    mDecoder.kill();
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
    if(index<0||index>=mClips.size())return; stopDecoder();mBytes.clear();mFrames.clear();
    mAudioLoadCancelled->store(true);
    mAudioLoadCancelled=std::make_shared<std::atomic_bool>(false);
    ++mAudioLoadGeneration;mAudioLoading=false;
    mFirstFrameReady=false;mAudioStartPending=mAudioEnabled;
    mIndex=index;
    const auto& clip=mClips[index];
    const int lastFrame=std::max(0,clip.sourceFrames-1);
    const int startFrame=std::clamp(static_cast<int>(std::floor(
        std::max(0.0,startSeconds)*std::max(1.0,clip.fps))),0,lastFrame);
    mStartSeconds=startFrame/std::max(1.0,clip.fps);mPositionSeconds=mStartSeconds;
    mNextInputFrame=startFrame;
    mPlaybackTarget->store(startFrame);
    mIncomingFrame->store(startFrame);
    mPosition->setRange(0,lastFrame);mPosition->setValue(startFrame);
    mTriedSoftware=false;mPlaybackFailed=false;mPaused=false;mPlayPause->setEnabled(true);mPlayPause->setText(tr("Pause"));
    mTitle->setText(QString("%1 — %2 / %3").arg(mClips[index].title).arg(index+1).arg(mClips.size()));mVideo->setText(tr("Preparing Vulkan playback…"));
    configureAudio();
    emit currentClipChanged(mClips[index].mountId,mStartSeconds);
    if(mClips[index].sourceFrames>1)startDecoder(true);
    else {mFrameTimer.stop();mPlayPause->setText(tr("Still"));mPlayPause->setEnabled(false);}
}

void ClipPlayerDialog::startDecoder(bool vulkan){
    const QString exe=ffmpegPath();if(exe.isEmpty()){mPlaybackFailed=true;mFrameTimer.stop();mVideo->setText(tr("FFmpeg was not found"));return;}
    const QSize area=mVideo->size().expandedTo(QSize(640,360));mWidth=std::max(2,area.width()&~1);mHeight=std::max(2,area.height()&~1);mFrameBytes=mWidth*mHeight*4;
    const auto& clip=mClips[mIndex];
    const bool normalizeMixedFrames=!clip.isSequence;
    const int inputWidth=normalizeMixedFrames?mWidth:clip.width;
    const int inputHeight=normalizeMixedFrames?mHeight:clip.height;
    if(inputWidth<=0||inputHeight<=0){mPlaybackFailed=true;mFrameTimer.stop();mVideo->setText(tr("Invalid clip dimensions"));return;}
    mInputFrameBytes=inputWidth*inputHeight*(normalizeMixedFrames?4:6);
    QStringList a{"-hide_banner","-loglevel","error"};if(vulkan)a<<"-init_hw_device"<<"vulkan=gallery"<<"-filter_hw_device"<<"gallery";
    a<<"-f"<<"rawvideo"<<"-pixel_format"<<(normalizeMixedFrames?"rgba":"rgb48le")<<"-video_size"
     <<QString("%1x%2").arg(inputWidth).arg(inputHeight)<<"-framerate"
     <<QString::number(clip.fps,'g',9)<<"-i"<<"pipe:0";
    QString f;
    if(normalizeMixedFrames)
        f=vulkan?QString("format=rgba,hwupload,scale_vulkan=w=%1:h=%2,hwdownload,format=rgba").arg(mWidth).arg(mHeight)
                :QStringLiteral("format=rgba");
    else {
        const double scale=std::min(double(mWidth)/inputWidth,double(mHeight)/inputHeight);
        const int fitWidth=std::max(2,int(inputWidth*scale)&~1);
        const int fitHeight=std::max(2,int(inputHeight*scale)&~1);
        f=vulkan
            ?QString("format=rgba,hwupload,scale_vulkan=w=%1:h=%2,hwdownload,format=rgba,pad=%3:%4:(ow-iw)/2:(oh-ih)/2:black")
                .arg(fitWidth).arg(fitHeight).arg(mWidth).arg(mHeight)
            :QString("scale=%1:%2:force_original_aspect_ratio=decrease,pad=%1:%2:(ow-iw)/2:(oh-ih)/2:black,format=rgba")
                .arg(mWidth).arg(mHeight);
    }
    a<<"-an"<<"-vf"<<f<<"-pix_fmt"<<"rgba"<<"-f"<<"rawvideo"<<"pipe:1";mDecoder.setProcessChannelMode(QProcess::SeparateChannels);mDecoder.start(exe,a,QIODevice::ReadWrite);
    updateFrameTimerInterval();mFrameTimer.start();
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
        mAudioButton->setText(tr("Audio: Muted"));
        return;
    }
    if(!available){
        if(!canLoadFromSource){mAudioButton->setChecked(false);mAudioButton->setText(tr("Audio: Muted"));}
        else {
            mAudioButton->setText(mAudioLoading?tr("Audio: Preparing…"):
                (mAudioEnabled?tr("Audio: Preparing…"):tr("Audio: Muted")));
            if(mAudioEnabled&&!mAudioLoading)beginSourceAudioLoad();
        }
        return;
    }
    mAudioButton->setText(mAudioEnabled?tr("Audio: On"):tr("Audio: Muted"));
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
    mAudioLoading=true;mAudioButton->setText(tr("Audio: Preparing…"));
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
    mAudioButton->setText(mAudioEnabled?tr("Audio: On"):tr("Audio: Muted"));
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
    // Bound decoded video to six frames. Leaving excess bytes in QProcess
    // applies back-pressure to FFmpeg instead of buffering an entire clip.
    const qint64 room=std::max<qint64>(0,qint64(mFrameBytes)*(6-mFrames.size())-mBytes.size());
    if(room>0)mBytes+=mDecoder.read(room);
    while(mFrameBytes>0&&mBytes.size()>=mFrameBytes&&mFrames.size()<6){QImage v(reinterpret_cast<const uchar*>(mBytes.constData()),mWidth,mHeight,mWidth*4,QImage::Format_RGBA8888);mFrames.push_back(v.copy());mBytes.remove(0,mFrameBytes);}
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
        if(mFrames.isEmpty()||qRound(mPositionSeconds*fps)>audioFrame)return;
    }
    if(!mFrames.isEmpty()){
        mLastPresentedImage=mFrames.takeFirst();
        mVideo->setPixmap(QPixmap::fromImage(mLastPresentedImage).scaled(mVideo->size(),Qt::KeepAspectRatio,Qt::SmoothTransformation));
        if(!mFirstFrameReady){
            mFirstFrameReady=true;
            if(mAudioEnabled&&mAudioStartPending&&!mPaused){startAudioAt(mPositionSeconds);mAudioStartPending=false;}
        }
        const double fps=std::max(1.0,mClips[mIndex].fps);
        const int presentedFrame=std::clamp(qRound(mPositionSeconds*fps),0,mPosition->maximum());
        if(!mPosition->isSliderDown())mPosition->setValue(presentedFrame);
        emit framePresented(currentMountId(),presentedFrame);
        mPositionSeconds+=1.0/fps;
    }else{
        if(!mPlaybackFailed&&mDecoder.state()==QProcess::NotRunning&&mBytes.isEmpty()){
            mFrameTimer.stop();if(mClips[mIndex].autoAdvance)advance();else mPlayPause->setText(tr("Play"));
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
    const bool wasPaused=mPaused;
    openClip(mIndex,std::clamp(frame,0,mPosition->maximum())/
        std::max(1.0,mClips[mIndex].fps));
    if(wasPaused){mPaused=true;mPlayPause->setText(tr("Play"));mFrameTimer.stop();}
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
    if(!mTriedSoftware&&mFrames.isEmpty()&&(status!=QProcess::NormalExit||code!=0)){
        mTriedSoftware=true;mBytes.clear();
        mTitle->setText(QStringLiteral("%1 — %2 / %3 — %4")
            .arg(mClips[mIndex].title).arg(mIndex+1).arg(mClips.size())
            .arg(tr("Vulkan unavailable; using software processing")));
        startDecoder(false);return;
    }
    if(mTriedSoftware&&mFrames.isEmpty()&&(status!=QProcess::NormalExit||code!=0)){
        mPlaybackFailed=true;mFrameTimer.stop();
        QString detail=decoderError;
        if(detail.size()>1200)detail=detail.right(1200);
        mTitle->setText(detail.isEmpty()?tr("FFmpeg could not process the RGB frame stream")
                                        :tr("FFmpeg playback failed: %1").arg(detail));
    }
}
QImage ClipPlayerDialog::rgb48Image(const QByteArray& frame,int width,int height)const{
    if(width<=0||height<=0||frame.size()<qint64(width)*height*6)return {};
    QImage image(width,height,QImage::Format_RGB888);const auto* source=reinterpret_cast<const uchar*>(frame.constData());
    for(int y=0;y<height;++y){uchar* destination=image.scanLine(y);for(int x=0;x<width;++x){const qsizetype input=(qsizetype(y)*width+x)*6,output=qsizetype(x)*3;destination[output]=source[input+1];destination[output+1]=source[input+3];destination[output+2]=source[input+5];}}
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
    if(mDecoder.bytesToWrite()>qint64(mInputFrameBytes)*2)return FramePushResult::Retry;
    if(mClips[mIndex].isSequence){
        if(width!=mClips[mIndex].width||height!=mClips[mIndex].height||
           frame.size()!=qint64(width)*height*6)return FramePushResult::Stopped;
        if(mDecoder.write(frame)!=frame.size())return FramePushResult::Retry;
        ++mNextInputFrame;return FramePushResult::Accepted;
    }
    const QImage source=rgb48Image(frame,width,height);if(source.isNull())return FramePushResult::Stopped;
    QImage canvas(mWidth,mHeight,QImage::Format_RGBA8888);canvas.fill(Qt::black);
    const QImage scaled=source.scaled(canvas.size(),Qt::KeepAspectRatio,Qt::SmoothTransformation);
    QPainter painter(&canvas);painter.drawImage((mWidth-scaled.width())/2,(mHeight-scaled.height())/2,scaled);painter.end();
    const QByteArray bytes(reinterpret_cast<const char*>(canvas.constBits()),canvas.sizeInBytes());
    if(mDecoder.write(bytes)!=bytes.size())return FramePushResult::Retry;
    ++mNextInputFrame;return FramePushResult::Accepted;
}
void ClipPlayerDialog::presentRgb48Frame(const QByteArray& frame,int width,int height){
    const QImage image=rgb48Image(frame,width,height);if(image.isNull())return;
    if(mIndex>=0&&mClips[mIndex].sourceFrames>1&&mClips[mIndex].isSequence&&
       (mClips[mIndex].width!=width||mClips[mIndex].height!=height)){
        // Draft and other preprocessing modes may change the RGB staging
        // dimensions relative to FileInfo. Configure rawvideo from the first
        // actual frame so the producer can never retry a permanent mismatch.
        stopDecoder();
        mClips[mIndex].width=width;
        mClips[mIndex].height=height;
        startDecoder(!mTriedSoftware);
    }
    mLastPresentedImage=image;
    mVideo->setPixmap(QPixmap::fromImage(mLastPresentedImage).scaled(
        mVideo->size(),Qt::KeepAspectRatio,Qt::SmoothTransformation));
    mFirstFrameReady=true;
    emit firstFramePresented(currentMountId());
    if(mAudioEnabled&&mAudioStartPending&&!mPaused){
        startAudioAt(mPositionSeconds);mAudioStartPending=false;
    }
    spdlog::info("Gallery presented processed RGB48 frame {}x{}",width,height);
}
void ClipPlayerDialog::finishRgb48Frames(){if(mIndex>=0&&mClips[mIndex].sourceFrames<=1)return;if(mDecoder.state()==QProcess::Running)mDecoder.closeWriteChannel();}
void ClipPlayerDialog::failRgb48Frames(const QString& error){mPlaybackFailed=true;mFrameTimer.stop();mTitle->setText(tr("Frame rendering failed: %1").arg(error));mDecoder.kill();}
void ClipPlayerDialog::advance(){if(!mClips.isEmpty())openClip((mIndex+1)%mClips.size());}
void ClipPlayerDialog::closeEvent(QCloseEvent* e){
    mClosing=true;
    stopDecoder();
    emit playbackClosed();
    QDialog::closeEvent(e);
}
void ClipPlayerDialog::resizeEvent(QResizeEvent* e){
    QDialog::resizeEvent(e);
    if(!mLastPresentedImage.isNull())mVideo->setPixmap(QPixmap::fromImage(mLastPresentedImage).scaled(
        mVideo->size(),Qt::KeepAspectRatio,Qt::SmoothTransformation));
}
void ClipPlayerDialog::keyPressEvent(QKeyEvent* e){if(e->key()==Qt::Key_Space){mPlayPause->click();e->accept();return;}if(e->key()==Qt::Key_Right){advance();e->accept();return;}if(e->key()==Qt::Key_Left&&!mClips.isEmpty()){openClip((mIndex-1+mClips.size())%mClips.size());e->accept();return;}QDialog::keyPressEvent(e);}
