#ifdef __ANDROID__
#include "android/TouchScrolling.h"
#include "android/PageCrossfade.h"
#endif
#include "ClipPlayerDialog.h"
#include "GalleryVulkanVideo.h"
#include "VulkanGalleryPreview.h"
#include <QCloseEvent>
#include <QCursor>
#include <QApplication>
#include <QCoreApplication>
#include <QDir>
#if QT_VERSION >= QT_VERSION_CHECK(6,0,0)
#include <QEnterEvent>
#endif
#include <QEasingCurve>
#include <QEvent>
#include <QFile>
#include <QFileInfo>
#include <QFutureWatcher>
#include <QBuffer>
#include <QGraphicsOpacityEffect>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QLineF>
#include <QMouseEvent>
#if defined(MOTIONCAM_HAS_QUICKWIDGETS) && !defined(Q_OS_ANDROID)
#include <QQuickWidget>
#include <QQuickItem>
#include <QQuickWindow>
#include <QSGSimpleTextureNode>
#include <QSGRendererInterface>
#endif
#include <QPainter>
#include <QPainterPath>
#include <QCache>
#include <QPropertyAnimation>
#include <QPushButton>
#include <QResizeEvent>
#include <QRegion>
#include <QSaveFile>
#include <QSettings>
#include <QSlider>
#include <QListView>
#include <QAbstractListModel>
#include <QStyledItemDelegate>
#include <QStyleOptionButton>
#include <QScrollBar>
#include <QSignalBlocker>
#include <QStackedLayout>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QStyle>
#include <QToolTip>
#include <QTouchEvent>
#include <QVBoxLayout>
#include <QWheelEvent>
#include <QtConcurrent>
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <functional>
#include <thread>
#include <vector>
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
constexpr int nativeOverlayIntervalMs=16;
struct PreparedGalleryAudio { QByteArray pcm; int sampleRate=0; };
PreparedGalleryAudio prepareGalleryAudio(const std::shared_ptr<const std::vector<uint8_t>>& audio,
                                         const std::shared_ptr<std::atomic_bool>& cancelled){
    PreparedGalleryAudio result;
    if(!audio||audio->empty()||cancelled->load())return result;
    const auto* wav=reinterpret_cast<const uint8_t*>(audio->data());const size_t size=static_cast<size_t>(audio->size());
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
    if(!dataAt||!dataBytes||!channels||!sampleRate||!blockAlign||
       blockAlign<channels*(bits/8)||(!integerPcm&&!floatPcm)){
        return result;
    }
    const int sourceStride=bits/8;
    const qsizetype frames=static_cast<qsizetype>(dataBytes/blockAlign);
    result.pcm.resize(frames*2*static_cast<qsizetype>(sizeof(float)));
    auto* stereo=reinterpret_cast<float*>(result.pcm.data());
    auto sampleAt=[&](qsizetype frame,int channel){
        const size_t offset=dataAt+static_cast<size_t>(frame)*blockAlign+
            static_cast<size_t>(channel)*sourceStride;
        if(offset+sourceStride>size)return 0.0f;
        if(floatPcm){
            const uint32_t bitsValue=le32(offset);
            float value;std::memcpy(&value,&bitsValue,sizeof(value));return value;
        }
        int32_t value=0;
        if(bits==16){
            const uint16_t raw=le16(offset);
            value=(raw<0x8000u?int32_t(raw):int32_t(raw)-0x10000)*65536;
        }else if(bits==24){
            const uint32_t raw=uint32_t(wav[offset])|
                uint32_t(wav[offset+1])<<8|uint32_t(wav[offset+2])<<16;
            value=(raw<0x800000u?int32_t(raw):int32_t(raw)-0x1000000)*256;
        }else{
            const uint32_t raw=le32(offset);
            value=raw<0x80000000u?int32_t(raw):int32_t(int64_t(raw)-0x100000000LL);
        }
        // WAVE_FORMAT_EXTENSIBLE stores valid bits left-aligned in the container.
        return static_cast<float>(double(value)/2147483648.0);
    };
    if(floatPcm&&channels==2&&blockAlign==2*sizeof(float)){
        std::memcpy(stereo,wav+dataAt,static_cast<size_t>(result.pcm.size()));
    }else{
        for(qsizetype frame=0;frame<frames;++frame){
            if((frame&4095)==0&&cancelled->load())return {};
            stereo[frame*2]=sampleAt(frame,0);
            stereo[frame*2+1]=sampleAt(frame,channels>1?1:0);
        }
    }
    if(cancelled->load())return {};
    result.sampleRate=static_cast<int>(sampleRate);
    return result;
}
class VuVolumeSlider final : public QSlider {
public:
    explicit VuVolumeSlider(QWidget* parent):QSlider(Qt::Vertical,parent) { setRange(0,100); }
    void setLevel(float level) { mLevel=std::clamp(level,0.0f,1.0f); update(); }
protected:
    void paintEvent(QPaintEvent*) override {
        QPainter p(this);p.setRenderHint(QPainter::Antialiasing);
        const QRectF rail(width()/2.0-4,8,8,height()-16);
        p.setPen(Qt::NoPen);p.setBrush(QColor(8,12,9,240));p.drawRoundedRect(rail,4,4);
        QLinearGradient colors(rail.topLeft(),rail.bottomLeft());
        colors.setColorAt(0,QColor(225,42,40));
        colors.setColorAt(0.38,QColor(230,170,34));
        colors.setColorAt(1,QColor(48,210,75));
        p.save();
        QPainterPath clip;clip.addRoundedRect(rail,4,4);p.setClipPath(clip);
        const qreal top=rail.bottom()-mLevel*rail.height();
        p.fillRect(QRectF(rail.left(),top,rail.width(),rail.bottom()-top),colors);
        p.restore();
        const qreal y=rail.bottom()-value()/100.0*rail.height();
        p.setPen(Qt::NoPen);p.setBrush(QColor(255,255,255,128));
        p.drawRoundedRect(QRectF(width()/2.0-12,y-5,24,10),5,5);
    }
    void mousePressEvent(QMouseEvent* event) override { setFromY(event->pos().y()); event->accept(); }
    void mouseMoveEvent(QMouseEvent* event) override { if(event->buttons()&Qt::LeftButton)setFromY(event->pos().y());event->accept(); }
private:
    void setFromY(int y){setValue(qBound(0,qRound(100.0*(height()-7-y)/std::max(1,height()-14)),100));}
    float mLevel=0;
};
QPixmap galleryIconCanvas(int size=24) {
    const qreal ratio=qApp->devicePixelRatio();
    QPixmap image(QSize(size,size)*ratio);image.setDevicePixelRatio(ratio);
    image.fill(Qt::transparent);return image;
}

QIcon mediaIcon(QStyle::StandardPixmap kind) {
    QPixmap image=galleryIconCanvas();QPainter painter(&image);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setPen(QPen(Qt::white,2,Qt::SolidLine,Qt::RoundCap,Qt::RoundJoin));
    painter.setBrush(Qt::white);
    const auto triangle=[&](int x){painter.drawPolygon(QPolygonF{{qreal(x),5},{qreal(x+8),12},{qreal(x),19}});};
    if(kind==QStyle::SP_MediaSkipBackward||kind==QStyle::SP_MediaSkipForward||
       kind==QStyle::SP_MediaSeekForward){
        if(kind==QStyle::SP_MediaSkipBackward){painter.translate(24,0);painter.scale(-1,1);}
        triangle(3);triangle(13);
    }else if(kind==QStyle::SP_MediaPlay){painter.drawPolygon(QPolygonF{{8,4},{20,12},{8,20}});}
    else if(kind==QStyle::SP_MediaPause){painter.drawRect(6,5,3,14);painter.drawRect(15,5,3,14);}
    else {
        painter.drawPolygon(QPolygonF{{3,9},{7,9},{12,5},{12,19},{7,15},{3,15}});
        painter.setBrush(Qt::NoBrush);
        if(kind==QStyle::SP_MediaVolume){
            painter.drawArc(QRectF(8,6,10,12),-60*16,120*16);
            painter.drawArc(QRectF(7,2,16,20),-55*16,110*16);
        }else {painter.drawLine(16,8,22,16);painter.drawLine(22,8,16,16);}
    }
    return QIcon(image);
}

QIcon closeGalleryIcon() {
    QPixmap image=galleryIconCanvas();QPainter painter(&image);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setPen(QPen(QColor(245,90,90),2.5,Qt::SolidLine,Qt::RoundCap));
    painter.drawLine(6,6,18,18);painter.drawLine(18,6,6,18);
    return QIcon(image);
}

QIcon fullscreenIcon(bool restore) {
    QPixmap pixmap=galleryIconCanvas();
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
    QPixmap pixmap=galleryIconCanvas(30);
    QPainter painter(&pixmap);painter.setRenderHint(QPainter::Antialiasing);
    painter.setPen(QPen(Qt::white,3.0,Qt::SolidLine,Qt::RoundCap,Qt::RoundJoin));
    const qreal upper=up?9.0:20.0,lower=up?20.0:9.0;
    painter.drawPolyline(QPolygonF({QPointF(5.0,lower),QPointF(15.0,upper),QPointF(25.0,lower)}));
    return QIcon(pixmap);
}

class RawHistogram final : public QWidget {
public:
    using Bins=std::array<std::vector<float>,3>;
    explicit RawHistogram(QWidget* parent) : QWidget(parent) {
        setFixedSize(560,260);
    }
    std::function<void()> doubleClicked;
    std::function<void()> distributionChanged;
    std::function<void()> binsGeometryChanged;
    double distribution() const { return mDistribution; }
    int binCount() const { return std::clamp(width()-16,2,65535); }
    void scrollDistribution(double steps) {
        const double next=std::clamp(mDistribution+steps*0.5,-1.0,1.0);
        if(next==mDistribution)return;
        mDistribution=next;
        if(qEnvironmentVariableIsSet("MOTIONCAM_GALLERY_PERF_PROFILE"))
            spdlog::info("GALLERY_PERF event=histogram_distribution value={:.3f}",
                mDistribution);
        if(distributionChanged)distributionChanged();
    }
    void setBins(Bins bins, float white, const std::array<float,4>& blackLevels,
                 const std::array<uint8_t,4>& phaseColors, int phases) {
        mBins=std::move(bins);
        mWhite=white;
        mBlackLevels=blackLevels;
        mPhaseColors=phaseColors;
        mPhases=phases;
        if(!property("nativeCaptureOnly").toBool())update();
    }
    void setNativeCapture(bool enabled) { mNativeCapture=enabled; }
    static double position(double value,double white,double black,double distribution) {
        if(white<=black)return 0.0;
        const double range=white-black;
        const int below=std::max(1,static_cast<int>(std::ceil(std::log2(range))));
        const double above=white<65535.0 ?
            std::min(1.0,std::log2((65535.0-black)/range)) : 0.0;
        const double first=std::pow(3.0,distribution);
        const double shadowFocus=(1.0-distribution)*0.5;
        auto width=[&](int stop) {
            const double fraction=double(std::min(stop,below-1))/
                std::max(1,below-1);
            const double shaped=(1.0-shadowFocus)*fraction+
                shadowFocus*fraction*fraction*fraction;
            return first+(1.0-first)*shaped;
        };
        auto widthSum=[&](int count) {
            const double denominator=std::max(1,below-1);
            const double triangular=count*(count-1)*0.5;
            const double linear=triangular/denominator;
            const double cubic=triangular*triangular/
                (denominator*denominator*denominator);
            return count*first+(1.0-first)*
                ((1.0-shadowFocus)*linear+shadowFocus*cubic);
        };
        const double belowWidth=widthSum(below);
        const double darkWidth=black>0.0 ? width(below-1) : 0.0;
        const double total=belowWidth+darkWidth+above*first;
        if(value>=white) {
            const double ev=std::min(above,std::log2((value-black)/range));
            return (darkWidth+belowWidth+ev*first)/total;
        }
        if(value<=black) {
            if(black<=0.0)return 0.0;
            return std::clamp(value/black,0.0,1.0)*darkWidth/total;
        }
        const double ev=std::log2(range/(value-black));
        const int full=std::min(below,static_cast<int>(ev));
        double offset=widthSum(full);
        offset+=std::min(1.0,ev-full)*width(std::min(full,below-1));
        return std::clamp((darkWidth+belowWidth-offset)/total,0.0,1.0);
    }
protected:
    void resizeEvent(QResizeEvent* event) override {
        QWidget::resizeEvent(event);
        if(binsGeometryChanged)binsGeometryChanged();
    }
    void wheelEvent(QWheelEvent* event) override {
        const double steps=!event->pixelDelta().isNull() ?
            event->pixelDelta().y()/40.0 : event->angleDelta().y()/120.0;
        if(steps==0.0) { QWidget::wheelEvent(event); return; }
        scrollDistribution(steps);
        event->accept();
    }
    void mouseDoubleClickEvent(QMouseEvent* event) override {
#ifdef Q_OS_ANDROID
        event->accept();
        return;
#endif
        if(event->button()==Qt::LeftButton && doubleClicked) {
            doubleClicked();
            event->accept();
            return;
        }
        QWidget::mouseDoubleClickEvent(event);
    }
    void paintEvent(QPaintEvent*) override {
        if(property("nativeCaptureOnly").toBool()&&!mNativeCapture)return;
        const auto paintStarted=std::chrono::steady_clock::now();
        QPainter painter(this);
        painter.fillRect(rect(),QColor(0,0,0,165));
        const QRect graph(8,8,width()-16,height()-16);
        painter.setPen(QColor(125,125,125,150));
        const double black=blackLevel();
        const int maxStop=mWhite>black ?
            static_cast<int>(std::ceil(std::log2(double(mWhite)-black))) : 0;
        for(int stop=0;stop<=maxStop;++stop) {
            const int x=graph.left()+qRound((graph.width()-1)*
                position(black+std::ldexp(double(mWhite)-black,-stop),mWhite,black,mDistribution));
            painter.drawLine(x,graph.top(),x,graph.bottom());
        }
        if(mWhite>0.0f && mWhite<65535.0f) {
            const int above=static_cast<int>(std::floor(
                std::min(1.0,std::log2((65535.0-black)/(mWhite-black)))));
            for(int stop=1;stop<=above;++stop) {
                const int x=graph.left()+qRound((graph.width()-1)*
                    position(black+std::ldexp(double(mWhite)-black,stop),mWhite,black,mDistribution));
                painter.drawLine(x,graph.top(),x,graph.bottom());
            }
        }
        if(mBins[0].empty())return;
        Bins displayBins=mBins;
        const int binCount=static_cast<int>(displayBins[0].size());
        for(auto& channel:displayBins) {
            int previous=-1;
            for(int bin=0;bin<binCount;++bin) {
                if(channel[bin]<=0.0f)continue;
                if(previous>=0 && bin-previous<=13)
                    for(int gap=previous+1;gap<bin;++gap) {
                        const float t=float(gap-previous)/float(bin-previous);
                        const float smooth=t*t*(3.0f-2.0f*t);
                        channel[gap]=channel[previous]+smooth*
                            (channel[bin]-channel[previous]);
                    }
                previous=bin;
            }
        }
        float peak=1.0f;
        for(const auto& channel:displayBins)
            for(float count:channel)peak=std::max(peak,count);
        painter.save();
        painter.setClipRect(graph);
        const qreal baseline=graph.bottom()-1.5;
        const qreal amplitude=graph.height()-3.0;
        const float toe=std::min(24.0f,peak);
        const qreal toeHeight=std::pow(toe/peak,0.4)*amplitude;
        auto heightFor=[&](int channel,int bin) {
            const float count=displayBins[channel][bin];
            return count<toe ? count/toe*toeHeight :
                std::pow(count/peak,0.4)*amplitude;
        };
        // Draw each RGB combination once. Repeated translucent overpainting
        // makes the bottom of dense histograms look like a solid color strip.
        const std::array<QRgb,8> fillColors={
            qRgba(0,0,0,0),qRgba(240,65,65,42),
            qRgba(65,215,65,42),qRgba(230,195,65,42),
            qRgba(75,115,240,42),qRgba(175,75,205,42),
            qRgba(65,205,220,42),qRgba(175,175,175,42)};
        QImage fill(graph.size(),QImage::Format_ARGB32);
        fill.fill(Qt::transparent);
        std::vector<std::array<double,3>> tops(fill.width());
        std::vector<uint8_t> populated(fill.width(),0);
        for(int x=0;x<fill.width();++x) {
            const int bin=std::clamp(qRound(x*double(binCount-1)/
                std::max(1,fill.width()-1)),0,binCount-1);
            if(displayBins[0][bin]==0.0f && displayBins[1][bin]==0.0f &&
               displayBins[2][bin]==0.0f)continue;
            tops[x]={baseline-heightFor(0,bin),
                baseline-heightFor(1,bin),baseline-heightFor(2,bin)};
            populated[x]=(displayBins[0][bin]>0 ? 1 : 0) |
                         (displayBins[1][bin]>0 ? 2 : 0) |
                         (displayBins[2][bin]>0 ? 4 : 0);
        }
        const int lastY=std::min(fill.height(),
            static_cast<int>(std::ceil(baseline-graph.top())));
        for(int y=0;y<lastY;++y) {
            const qreal absoluteY=graph.top()+y;
            if(absoluteY>=baseline)break;
            auto* row=reinterpret_cast<QRgb*>(fill.scanLine(y));
            for(int x=0;x<fill.width();++x) {
                if(!populated[x])continue;
                const int mask=(populated[x]&1 && absoluteY>=tops[x][0] ? 1 : 0) |
                    (populated[x]&2 && absoluteY>=tops[x][1] ? 2 : 0) |
                    (populated[x]&4 && absoluteY>=tops[x][2] ? 4 : 0);
                if(mask)row[x]=fillColors[mask];
            }
        }
        painter.drawImage(graph.topLeft(),fill);
        const auto fillFinished=std::chrono::steady_clock::now();
        const std::array<QRgb,8> lineColors={
            qRgb(0,0,0),qRgb(255,75,75),qRgb(80,255,80),qRgb(230,195,65),
            qRgb(85,130,255),qRgb(175,75,205),qRgb(70,225,235),qRgb(255,255,255)};
        std::array<QImage,3> strokeMasks;
        for(int c=0;c<3;++c) {
            strokeMasks[c]=QImage(graph.size(),QImage::Format_ARGB32);
            strokeMasks[c].fill(Qt::transparent);
            QPainter maskPainter(&strokeMasks[c]);
            maskPainter.setRenderHint(QPainter::Antialiasing);
            maskPainter.translate(-graph.topLeft());
            maskPainter.setPen(QPen(Qt::white,1.3));
            std::vector<QLineF> lines;
            QPointF previous;
            bool inRun=false;
            for(int x=0;x<binCount;++x) {
                const qreal position=graph.left()+x*(graph.width()-1)/
                    double(binCount-1);
                if(displayBins[c][x]==0) {
                    if(inRun)lines.emplace_back(previous,QPointF(position,baseline));
                    inRun=false;
                    continue;
                }
                const qreal top=baseline-heightFor(c,x);
                if(!inRun)previous=QPointF(
                    graph.left()+std::max(0,x-1)*(graph.width()-1)/
                        double(binCount-1),baseline);
                const QPointF current(position,top);
                lines.emplace_back(previous,current);
                previous=current;
                inRun=true;
            }
            if(inRun)lines.emplace_back(previous,QPointF(graph.right(),baseline));
            if(!lines.empty())maskPainter.drawLines(lines.data(),static_cast<int>(lines.size()));
        }
        const auto masksFinished=std::chrono::steady_clock::now();
        QImage strokes(graph.size(),QImage::Format_ARGB32);
        strokes.fill(Qt::transparent);
        for(int y=0;y<strokes.height();++y) {
            auto* output=reinterpret_cast<QRgb*>(strokes.scanLine(y));
            const auto* red=reinterpret_cast<const QRgb*>(strokeMasks[0].constScanLine(y));
            const auto* green=reinterpret_cast<const QRgb*>(strokeMasks[1].constScanLine(y));
            const auto* blue=reinterpret_cast<const QRgb*>(strokeMasks[2].constScanLine(y));
            for(int x=0;x<strokes.width();++x) {
                const int r=qAlpha(red[x]),g=qAlpha(green[x]),b=qAlpha(blue[x]);
                const int mask=(r>0?1:0)|(g>0?2:0)|(b>0?4:0);
                if(mask) {
                    const QRgb color=lineColors[mask];
                    output[x]=qRgba(qRed(color),qGreen(color),qBlue(color),
                        std::max({r,g,b})*210/255);
                }
            }
        }
        painter.drawImage(graph.topLeft(),strokes);
        const auto strokesFinished=std::chrono::steady_clock::now();
        painter.restore();
        painter.setPen(QPen(QColor(150,105,45,95),1.5));
        std::array<int,4> blackMarkerXs{};
        std::array<int,4> blackMarkerMasks{};
        int blackMarkerCount=0;
        for(int phase=0;phase<mPhases;++phase) {
            if(mBlackLevels[phase]<=0.0f)continue;
            const int x=graph.left()+qRound((graph.width()-1)*
                position(mBlackLevels[phase],mWhite,black,mDistribution));
            int marker=0;
            while(marker<blackMarkerCount && blackMarkerXs[marker]!=x)++marker;
            if(marker==blackMarkerCount) {
                blackMarkerXs[marker]=x;
                ++blackMarkerCount;
                painter.drawLine(x,graph.top(),x,graph.bottom());
            }
            blackMarkerMasks[marker]|=1<<std::min<int>(2,mPhaseColors[phase]);
        }
        painter.setPen(QColor(125,125,125,150));
        for(int stop=0;stop<=maxStop;++stop) {
            const int x=graph.left()+qRound((graph.width()-1)*
                position(black+std::ldexp(double(mWhite)-black,-stop),
                    mWhite,black,mDistribution));
            for(int marker=0;marker<blackMarkerCount;++marker)
                if(std::abs(x-blackMarkerXs[marker])<=1) {
                    painter.drawLine(x,graph.top(),x,graph.bottom());
                    break;
                }
        }
        painter.setPen(Qt::NoPen);
        for(int marker=0;marker<blackMarkerCount;++marker) {
            painter.setBrush(QColor::fromRgb(lineColors[blackMarkerMasks[marker]]));
            painter.drawEllipse(QPointF(blackMarkerXs[marker],graph.bottom()+4.0),2.0,2.0);
        }
        painter.setPen(QColor(220,220,220));
        for(int stop=0;stop<=maxStop;stop+=4) {
            const int x=graph.left()+qRound((graph.width()-1)*
                position(black+std::ldexp(double(mWhite)-black,-stop),mWhite,black,mDistribution));
            const QString label=stop==0?QStringLiteral("0 EV"):
                QStringLiteral("-%1").arg(stop);
            const int labelWidth=painter.fontMetrics().horizontalAdvance(label);
            const int labelX=std::clamp(x-labelWidth/2,0,width()-labelWidth);
            const QRect labelRect(labelX,graph.top(),labelWidth,
                painter.fontMetrics().height());
            painter.drawText(labelRect,Qt::AlignCenter,label);
        }
        if(mWhite>0.0f && mWhite<65535.0f) {
            const int x=graph.left()+qRound((graph.width()-1)*
                position(std::min(65535.0,black+2.0*(double(mWhite)-black)),mWhite,black,mDistribution));
            const QString label=QStringLiteral("+1");
            const int labelWidth=painter.fontMetrics().horizontalAdvance(label);
            const int labelX=std::clamp(x-labelWidth/2,0,width()-labelWidth);
            painter.drawText(QRect(labelX,graph.top(),labelWidth,
                painter.fontMetrics().height()),Qt::AlignCenter,label);
        }
        if(qEnvironmentVariableIsSet("MOTIONCAM_GALLERY_PERF_HISTOGRAM_PAINT")) {
            const auto now=std::chrono::steady_clock::now();
            auto ms=[](auto end,auto start) {
                return std::chrono::duration<double,std::milli>(end-start).count();
            };
            spdlog::info("GALLERY_PERF event=histogram_paint width={} height={} fill_ms={:.3f} masks_ms={:.3f} combine_ms={:.3f} rest_ms={:.3f} total_ms={:.3f}",
                width(),height(),ms(fillFinished,paintStarted),
                ms(masksFinished,fillFinished),ms(strokesFinished,masksFinished),
                ms(now,strokesFinished),ms(now,paintStarted));
        }
    }
private:
    bool mNativeCapture=false;
    double blackLevel() const {
        double black=0.0;
        for(int phase=0;phase<mPhases;++phase)
            if(mBlackLevels[phase]>0.0f && (black==0.0 || mBlackLevels[phase]<black))
                black=mBlackLevels[phase];
        return black;
    }
    Bins mBins{};
    float mWhite=0.0f;
    std::array<float,4> mBlackLevels{};
    std::array<uint8_t,4> mPhaseColors{};
    int mPhases=0;
    double mDistribution=1.0;
};

QIcon histogramIcon() {
    QPixmap pixmap=galleryIconCanvas();
    QPainter painter(&pixmap);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setPen(QPen(QColor(220,220,220,190),1));
    painter.drawLine(QPointF(2,21),QPointF(22,21));
    const std::array<QColor,3> colors={
        QColor(255,95,95),QColor(100,235,105),QColor(105,155,255)};
    const std::array<std::array<QPointF,7>,3> traces={{
        {{{2,20},{4,18},{6,10},{8,5},{10,11},{13,18},{22,20}}},
        {{{2,20},{7,18},{10,13},{12,6},{14,10},{17,18},{22,20}}},
        {{{2,20},{11,19},{15,15},{18,7},{20,11},{21,17},{22,20}}}
    }};
    for(int channel=0;channel<3;++channel) {
        QPainterPath path(traces[channel][0]);
        for(int point=1;point<7;++point)
            path.lineTo(traces[channel][point]);
        painter.setPen(QPen(colors[channel],1.7,Qt::SolidLine,Qt::RoundCap,
            Qt::RoundJoin));
        painter.drawPath(path);
    }
    return QIcon(pixmap);
}

QIcon clippingIcon() {
    QPixmap pixmap=galleryIconCanvas();
    QPainter painter(&pixmap);
    painter.setPen(QPen(QColor(225,225,225),1.5));
    painter.drawRect(2,2,20,20);
    painter.fillRect(5,5,6,6,QColor(210,75,75));
    painter.fillRect(13,5,6,6,QColor(80,210,95));
    painter.fillRect(5,13,6,6,QColor(90,135,235));
    painter.fillRect(13,13,6,6,QColor(120,65,25));
    return QIcon(pixmap);
}

class DuplicateSlider final : public QSlider {
public:
    explicit DuplicateSlider(QWidget* parent) : QSlider(Qt::Horizontal,parent) {}
    void setDuplicates(std::shared_ptr<const std::vector<bool>> duplicates) {
        mDuplicates=std::move(duplicates);invalidateGalleryOverlay(*this);update();
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

struct FrameThumbnailData {
    QImage image;
    QColor background;
    bool selected=false, dropped=false;
};

class FrameThumbnailModel final : public QAbstractListModel {
public:
    using QAbstractListModel::QAbstractListModel;
    std::function<QSize(int)> size;
    std::function<FrameThumbnailData(int)> thumbnail;
    std::function<void(int)> activate;
    std::function<void(int,bool)> select;
    int rowCount(const QModelIndex& parent={}) const override {return parent.isValid()?0:mCount;}
    QVariant data(const QModelIndex& index,int role) const override {
        return index.isValid()&&role==Qt::DisplayRole?QVariant(QString::number(index.row())):QVariant();
    }
    void setCount(int count){beginResetModel();mCount=count;endResetModel();}
private:
    int mCount=0;
};

class FrameThumbnailDelegate final : public QStyledItemDelegate {
public:
    using QStyledItemDelegate::QStyledItemDelegate;
    int imageCacheKiB() const {return mImages.totalCost();}
    std::pair<unsigned,unsigned> takeRasterCounts() const {
        const auto counts=std::make_pair(mPainted,mReused);mPainted=mReused=0;return counts;
    }
    QSize sizeHint(const QStyleOptionViewItem&,const QModelIndex& index) const override {
        return static_cast<const FrameThumbnailModel*>(index.model())->size(index.row())+QSize(16,30);
    }
    void paint(QPainter* painter,const QStyleOptionViewItem& option,const QModelIndex& index) const override {
        const auto* model=static_cast<const FrameThumbnailModel*>(index.model());
        const auto data=model->thumbnail(index.row());
        const QSize size=model->size(index.row());
        paintBackground(painter,option,data,size);
        paintCell(painter,option,index,data,size);
    }
private:
    mutable unsigned mPainted=0,mReused=0;
    // Cache immutable pixels, not whole cells: blank placeholders, captions and
    // playback highlights should not allocate another native-resolution image.
    mutable QCache<quint64,QPixmap> mImages{16*1024}; // KiB
    template<class Factory> QPixmap cachedPixmap(quint64 key,Factory&& create) const {
        if(const auto* found=mImages.object(key)){++mReused;return *found;}
        ++mPainted;
        QPixmap pixmap=create();
        const qint64 bytes=qint64(pixmap.width())*pixmap.height()*pixmap.depth()/8;
        if(!pixmap.isNull()&&bytes<=qint64(mImages.maxCost())*1024)
            mImages.insert(key,new QPixmap(pixmap),std::max<qint64>(1,(bytes+1023)/1024));
        return pixmap;
    }
    static void paintBackground(QPainter* painter,const QStyleOptionViewItem& option,
            const FrameThumbnailData& data,const QSize& size){
        painter->save();painter->setPen(Qt::NoPen);painter->setBrush(data.background);
        painter->setRenderHint(QPainter::Antialiasing,true);
        painter->drawRoundedRect(QRect(option.rect.topLeft()+QPoint(8,0),size+QSize(8,8)),4,4);
        painter->restore();
    }
    void paintCell(QPainter* painter,const QStyleOptionViewItem& option,
            const QModelIndex& index,const FrameThumbnailData& data,const QSize& size) const {
        const QRect frame(option.rect.topLeft()+QPoint(8,0),size+QSize(8,8));
        const QRect pixels(frame.topLeft()+QPoint(4,4),size);
        painter->save();
        if(!data.dropped)painter->fillRect(pixels,QColor(20,20,20,120));
        if(!data.image.isNull()){
            const QSize scaled=data.image.size().scaled(size-QSize(2,2),Qt::KeepAspectRatio);
            const QRect target(pixels.center()-QPoint(scaled.width()/2,scaled.height()/2),scaled);
            const auto transform=painter->deviceTransform();
            const qreal ratio=std::hypot(transform.m11(),transform.m12());
            const QSize nativeSize=scaled*ratio;
            const quint64 key=qHashMulti(quint64(0),data.image.cacheKey(),nativeSize);
            const QPixmap pixmap=cachedPixmap(key,[&]{
                return QPixmap::fromImage(data.image).scaled(nativeSize,Qt::IgnoreAspectRatio,Qt::SmoothTransformation);
            });
            painter->setRenderHint(QPainter::SmoothPixmapTransform,true);
            painter->drawPixmap(target,pixmap);
        }
        QStyleOptionButton check;
        check.initFrom(option.widget);
        const QRect checkLine(frame.left(),frame.bottom()+3,frame.width(),20);
        check.rect=QRect(checkLine.left(),checkLine.center().y()-6,13,13);
        check.state=QStyle::State_Enabled|(data.selected?QStyle::State_On:QStyle::State_Off);
        const auto transform=painter->deviceTransform();
        const qreal ratio=std::hypot(transform.m11(),transform.m12());
        if(qFuzzyCompare(ratio,std::round(ratio))){
            const quint64 key=qHashMulti(quint64(1),data.selected,ratio,
                option.widget->palette().cacheKey(),quintptr(option.widget->style()));
            const QPixmap indicator=cachedPixmap(key,[&]{
                QPixmap image(check.rect.size()*ratio);image.setDevicePixelRatio(ratio);
                image.fill(Qt::transparent);QPainter capture(&image);
                auto local=check;local.rect.moveTopLeft(QPoint());
                option.widget->style()->drawPrimitive(QStyle::PE_IndicatorCheckBox,&local,&capture,option.widget);
                return image;
            });
            painter->drawPixmap(check.rect.topLeft(),indicator);
        }else{
            option.widget->style()->drawPrimitive(QStyle::PE_IndicatorCheckBox,&check,painter,option.widget);
        }
        painter->setPen(Qt::white);
        painter->drawText(checkLine.adjusted(18,0,0,0),Qt::AlignLeft|Qt::AlignVCenter,QString::number(index.row()));
        painter->restore();
    }
};

class FrameThumbnailView final : public QListView {
public:
    using QListView::QListView;
    void updateThumbnailLayout(){scheduleDelayedItemsLayout();}
protected:
    void mousePressEvent(QMouseEvent* event) override {
        mPressed=indexAt(event->pos());mPressPosition=event->pos();
        QListView::mousePressEvent(event);
    }
    void mouseReleaseEvent(QMouseEvent* event) override {
        const QModelIndex index=indexAt(event->pos());
        const bool clicked=event->button()==Qt::LeftButton&&index.isValid()&&index==mPressed&&
            (event->pos()-mPressPosition).manhattanLength()<QApplication::startDragDistance();
        mPressed=QModelIndex();
        QListView::mouseReleaseEvent(event);
        if(!clicked)return;
        auto* thumbnails=static_cast<FrameThumbnailModel*>(model());
        const QPoint local=event->pos()-visualRect(index).topLeft()-QPoint(8,0);
        const QSize size=thumbnails->size(index.row());
        if(local.x()<0||local.x()>=size.width()+8)return;
        if(local.y()>=size.height()+10)thumbnails->select(index.row(),!thumbnails->thumbnail(index.row()).selected);
        else if(local.y()>=0&&local.y()<size.height()+8)thumbnails->activate(index.row());
    }
    void keyPressEvent(QKeyEvent* event) override {
        if(currentIndex().isValid()){
            auto* thumbnails=static_cast<FrameThumbnailModel*>(model());
            const int source=currentIndex().row();
            if(event->key()==Qt::Key_Space){
                thumbnails->select(source,!thumbnails->thumbnail(source).selected);event->accept();return;
            }
            if(event->key()==Qt::Key_Return||event->key()==Qt::Key_Enter){
                thumbnails->activate(source);event->accept();return;
            }
        }
        QListView::keyPressEvent(event);
    }
private:
    QPersistentModelIndex mPressed;
    QPoint mPressPosition;
};

QSize frameThumbnailSize(int width,int height,int orientation,
        QSize bounds=ClipPlayerDialog::thumbnailMaximumSize()){
    if(width<=0||height<=0)return bounds;
    if(orientation==90||orientation==270)std::swap(width,height);
    const double scale=std::min(double(bounds.width())/width,double(bounds.height())/height);
    return {std::max(1,static_cast<int>(std::floor(width*scale))),
            std::max(1,static_cast<int>(std::floor(height*scale)))};
}
}

QSize ClipPlayerDialog::thumbnailMaximumSize(){return {176,112};}

QSize ClipPlayerDialog::thumbnailDisplayMaximumSize() const{
    auto bounds=thumbnailMaximumSize();
#ifdef Q_OS_ANDROID
    const bool landscape=width()>height();
    if(mHistogramEnabled&&landscape)bounds=QSize(112,56);
    const int minimumVisible=landscape?5:3;
    bounds.setWidth(std::min(bounds.width(),std::max(1,width()/minimumVisible-16)));
#endif
    return bounds;
}

int ClipPlayerDialog::thumbnailStripHeight() const{
#ifdef Q_OS_ANDROID
    const bool stills=mIndex>=0&&mIndex<mClips.size()&&!mClips[mIndex].isSequence;
    return (stills?thumbnailDisplayMaximumSize().height():thumbnailSizeForSource(0).height())+42;
#else
    return thumbnailMaximumSize().height()+58;
#endif
}

#if defined(MOTIONCAM_HAS_QUICKWIDGETS) && !defined(Q_OS_ANDROID)
class GalleryTextureItem final : public QQuickItem {
public:
    GalleryTextureItem() { setFlag(ItemHasContents); }

    void setFrame(const QImage& image, const QRectF& target, bool smooth) {
        mFrame = image;
        mTarget = target;
        mSmooth = smooth;
        update();
    }

protected:
    QSGNode* updatePaintNode(QSGNode* oldNode,
                            UpdatePaintNodeData*) override {
        const auto started = std::chrono::steady_clock::now();
        if (!mLoggedRenderApi && window()) {
            mLoggedRenderApi = true;
            spdlog::info("Gallery active scene graph API: {}",
                static_cast<int>(window()->rendererInterface()->graphicsApi()));
        }
        if(mFrame.isNull()||!window()){
            delete oldNode;
            mUploadedFrameKey=0;
            return nullptr;
        }
        if(oldNode&&mUploadedFrameKey==mFrame.cacheKey()){
            auto* node=static_cast<QSGSimpleTextureNode*>(oldNode);
            node->setRect(mTarget);
            node->setFiltering(mSmooth?QSGTexture::Linear:QSGTexture::Nearest);
            return node;
        }
        delete oldNode;
        auto* texture=window()->createTextureFromImage(mFrame);
        if(!texture){mUploadedFrameKey=0;return nullptr;}
        auto* node=new QSGSimpleTextureNode;
        node->setOwnsTexture(true);
        node->setTexture(texture);
        node->setRect(mTarget);
        node->setFiltering(mSmooth?QSGTexture::Linear:QSGTexture::Nearest);
        mUploadedFrameKey=mFrame.cacheKey();
        if (qEnvironmentVariableIsSet("MOTIONCAM_GALLERY_PERF_PROFILE") &&
            !mFrame.isNull())
            spdlog::info("GALLERY_PERF event=gpu_present source={}x{} viewport={}x{} cpu_ms={:.3f}",
                mFrame.width(), mFrame.height(), width(), height(),
                std::chrono::duration<double, std::milli>(
                    std::chrono::steady_clock::now() - started).count());
        return node;
    }

private:
    QImage mFrame;
    QRectF mTarget;
    bool mSmooth = true;
    qint64 mUploadedFrameKey=0;
    bool mLoggedRenderApi=false;
};

#endif

class GalleryGpuVideo final : public QWidget {
public:
    explicit GalleryGpuVideo(QWidget* parent) : QWidget(parent) {
        auto* layout = new QVBoxLayout(this);
        layout->setContentsMargins(0, 0, 0, 0);
#ifdef MOTIONCAM_HAS_DIRECTLOG_PLACEBO
        mNative = new GalleryVulkanVideo(this);
        if (mNative->usable()) {
            spdlog::info("Gallery native Vulkan presenter selected");
            layout->addWidget(mNative);
            return;
        }
        spdlog::warn("Gallery native Vulkan presenter unavailable");
        delete mNative;
        mNative = nullptr;
#endif
        createFallback();
    }

    void setFrame(const QImage& image, const QRectF& target, bool smooth) {
        if (mNative) { mNative->setFrame(image, target, smooth); return; }
#if defined(MOTIONCAM_HAS_QUICKWIDGETS) && !defined(Q_OS_ANDROID)
        if (mTextureItem) mTextureItem->setFrame(image, target, smooth);
#endif
    }
    void setGpuFrame(std::shared_ptr<motioncam::VulkanGalleryGpuBuffer> frame,
                     const QRectF& target, bool smooth, const QImage& overlay = {},
                     const std::vector<GalleryOverlayPart>& parts = {}) {
        if (mNative) mNative->setGpuFrame(std::move(frame), target, smooth, overlay, parts);
    }
    void setOverlay(const QImage& image,
                    const std::vector<GalleryOverlayPart>& parts = {}, bool redraw = true) {
        if (mNative) mNative->setOverlay(image, parts, redraw);
    }
    void drainGpuFrames() { if (mNative) mNative->drainGpuFrames(); }
    bool usable() const { return mNative ? mNative->usable() : mAvailable; }
    bool native() const { return mNative != nullptr; }
    qreal nativePixelRatio() const {
        return mNative ? mNative->pixelRatio() : devicePixelRatioF();
    }

    void disableNative() {
        auto* old = mNative;
        mNative = nullptr;
        delete old;
        createFallback();
    }

private:
    void createFallback() {
#if defined(MOTIONCAM_HAS_QUICKWIDGETS) && !defined(Q_OS_ANDROID)
        mQuick = new QQuickWidget(this);
        connect(mQuick, &QQuickWidget::sceneGraphError, this,
            [this](QQuickWindow::SceneGraphError, const QString& message) {
                mAvailable = false;
                spdlog::warn("Gallery GPU presentation unavailable: {}", message.toStdString());
            });
        mQuick->setResizeMode(QQuickWidget::SizeRootObjectToView);
        mQuick->setClearColor(QColor(96, 96, 96));
        mTextureItem = new GalleryTextureItem;
        mTextureItem->setSize(QSizeF(640, 360));
        mQuick->setContent(QUrl(), nullptr, mTextureItem);
        layout()->addWidget(mQuick);
        mAvailable = mQuick->status() != QQuickWidget::Error && mQuick->rootObject();
#endif
    }
    GalleryVulkanVideo* mNative = nullptr;
#if defined(MOTIONCAM_HAS_QUICKWIDGETS) && !defined(Q_OS_ANDROID)
    QQuickWidget* mQuick = nullptr;
    GalleryTextureItem* mTextureItem = nullptr;
#endif
    bool mAvailable = false;
};

bool ClipPlayerDialog::nativeVulkanPresenter() const {
    return mGpuVideo && mGpuVideo->native() && mGpuVideo->usable();
}

bool ClipPlayerDialog::hasReusableSourceFrame() const {
    return mLastImageIsSource &&
        (mDirectSequencePresentation || nativeVulkanPresenter() ||
         (mIndex >= 0 && mIndex < mClips.size() &&
          !mClips[mIndex].isSequence));
}

void ClipPlayerDialog::runPerformanceGestureTest(std::function<void()> finished) {
    if (!hasPresentedFrame()) { finished(); return; }
    setPlaybackPaused(true);
    mZoomAnimationTimer.stop();
    mSurfaceUpdateTimer.stop();
    const double minimum = fitScale() * 100.0;
    spdlog::info("GALLERY_PERF event=gesture_range start_percent={:.3f} end_percent=3200 zoom_steps=45 pan_steps=45",
        minimum);
    auto samples = std::make_shared<std::vector<double>>();
    auto ticks = std::make_shared<int>(0);
    auto timer = new QTimer(this);
    timer->setTimerType(Qt::PreciseTimer);
    timer->setInterval(16);
    connect(timer, &QTimer::timeout, this,
        [this, timer, ticks, samples, minimum, finished = std::move(finished)] {
            const int tick = (*ticks)++;
            if (tick == 90) {
                timer->stop();
                timer->deleteLater();
                std::sort(samples->begin(), samples->end());
                const auto percentile = [&](double fraction) {
                    return (*samples)[std::min(samples->size() - 1,
                        size_t(fraction * (samples->size() - 1)))];
                };
                spdlog::info("GALLERY_PERF event=gesture_sample frames={} update_p50_ms={:.3f} update_p95_ms={:.3f} update_max_ms={:.3f} native={}",
                    samples->size(), percentile(0.5), percentile(0.95),
                    samples->back(), nativeVulkanPresenter());
                finished();
                return;
            }
            if (tick < 45) {
                const double progress = double(tick + 1) / 45.0;
                mZoomPercent = minimum * std::exp(std::log(3200.0 / minimum) * progress);
                mPanSourcePixels = {};
            } else {
                mZoomPercent = 3200.0;
                mPanSourcePixels = QPointF((tick - 45) * 0.5, (tick - 45) * 0.25);
                clampPanToZoom();
            }
            const auto started = std::chrono::steady_clock::now();
            updateDisplayedImage();
            samples->push_back(std::chrono::duration<double, std::milli>(
                std::chrono::steady_clock::now() - started).count());
        });
    QTimer::singleShot(1000, this, [timer] { timer->start(); });
}

ClipPlayerDialog::ClipPlayerDialog(QVector<Clip> clips, int initialMountId, QWidget* parent)
    : QDialog(parent), mClips(std::move(clips)) {
    QSettings gallerySettings("com.motioncam","MotionCam FS");
    mAudioMode=static_cast<AudioMode>(qBound(0,gallerySettings.value("gallery/audioMode",0).toInt(),2));
    mPerformanceUncapped=mAudioMode==AudioMode::UncappedMuted;
    mVolumePercent=qBound(0,gallerySettings.value("gallery/volume",50).toInt(),100);
    mThumbnailDiskCache=std::make_unique<QTemporaryDir>();
    mThumbnailCacheWriter.setMaxThreadCount(1);
    setAttribute(Qt::WA_DeleteOnClose); setWindowTitle(tr("Gallery")); resize(1100,720);
#if defined(Q_OS_ANDROID) && QT_VERSION >= QT_VERSION_CHECK(6,9,0)
    setWindowFlag(Qt::ExpandedClientAreaHint);
    setAttribute(Qt::WA_ContentsMarginsRespectsSafeArea,false);
#endif
    QPalette galleryPalette=palette();
    galleryPalette.setColor(QPalette::Window,QColor(96,96,96));
    setPalette(galleryPalette);
    setAutoFillBackground(true);
    setStyleSheet("QToolTip{background-color:#1976d2;color:white;border:1px solid #64a9e8;padding:4px 6px;}");
    setMouseTracking(true);
    auto* layout=new QVBoxLayout(this);layout->setContentsMargins(0,0,0,0);layout->setSpacing(0);
    auto* stage=new QWidget(this);stage->setContentsMargins(0,0,0,0);stage->setStyleSheet("border:0;");
    QPalette stagePalette=stage->palette();
    stagePalette.setColor(QPalette::Window,QColor(96,96,96));
    stage->setPalette(stagePalette);
    stage->setAutoFillBackground(true);
    auto* stack=new QStackedLayout(stage);stack->setStackingMode(QStackedLayout::StackAll);stack->setContentsMargins(0,0,0,0);stack->setSpacing(0);
    mVideo=new QLabel(tr("Preparing playback…"),this); mVideo->setAlignment(Qt::AlignCenter);
    // A retained surface must remain pixel-size stable while the window is
    // resized. QLabel will center and clip/pad this pixmap until the decoder
    // supplies a surface rendered for the new viewport.
    mVideo->setScaledContents(false);
    mVideo->setMinimumSize(640,360);mVideo->setSizePolicy(QSizePolicy::Ignored,QSizePolicy::Ignored);
    mVideo->setFrameShape(QFrame::NoFrame);mVideo->setLineWidth(0);mVideo->setContentsMargins(0,0,0,0);
    mVideo->setStyleSheet("background:#606060;color:#e0e0e0;border:none;margin:0;padding:0;");stack->addWidget(mVideo);
    mOverlay=new QWidget(stage);mOverlay->setStyleSheet("background:transparent;");
    mPreparingLabel=new QLabel(mVideo->text(),stage);
    mPreparingLabel->setAttribute(Qt::WA_TransparentForMouseEvents);
    mPreparingLabel->setStyleSheet("background:transparent;color:#e0e0e0;border:0;padding:0;");
    mPreparingLabel->hide();
    auto* overlayLayout=new QVBoxLayout(mOverlay);overlayLayout->setContentsMargins(10,10,10,0);
#ifdef Q_OS_ANDROID
    overlayLayout->setContentsMargins(0,10,0,0);
    overlayLayout->setSpacing(10);
#if QT_VERSION >= QT_VERSION_CHECK(6,9,0)
    stage->setAttribute(Qt::WA_ContentsMarginsRespectsSafeArea,false);
    mOverlay->setAttribute(Qt::WA_ContentsMarginsRespectsSafeArea,false);
#endif
#endif
    const auto addInsetWidget=[&](QWidget* widget){
#ifdef Q_OS_ANDROID
        auto* row=new QHBoxLayout;
        row->setContentsMargins(10,0,10,0);
        row->addWidget(widget,0,Qt::AlignLeft);overlayLayout->addLayout(row);
#else
        overlayLayout->addWidget(widget,0,Qt::AlignLeft);
#endif
    };
    mTitle=new QLabel(mOverlay);mTitle->setTextFormat(Qt::RichText);mTitle->setStyleSheet("color:white;background:rgba(0,0,0,120);padding:5px 9px;border-radius:4px;");
    mFpsLabel=new QLabel(stage);
    mFpsLabel->setStyleSheet("color:white;background:rgba(0,0,0,120);padding:5px 9px;border-radius:4px;");
    mFpsLabel->setAttribute(Qt::WA_TransparentForMouseEvents);
    addInsetWidget(mTitle);
    mHistogram=new RawHistogram(mOverlay);
    static_cast<RawHistogram*>(mHistogram)->doubleClicked=[this]{
#ifndef Q_OS_ANDROID
        setHistogramExpanded(!mHistogramExpanded);
#endif
    };
    static_cast<RawHistogram*>(mHistogram)->distributionChanged=[this]{ updateHistogram(); };
    static_cast<RawHistogram*>(mHistogram)->binsGeometryChanged=[this]{
        if(mHistogramResizePending)return;
        mHistogramResizePending=true;
        QTimer::singleShot(0,this,[this]{
            mHistogramResizePending=false;
            if(mHistogramEnabled)updateHistogram();
        });
    };
    mHistogram->hide();
    addInsetWidget(mHistogram);
    overlayLayout->addStretch(1);
    auto* controls=new QHBoxLayout;controls->setSpacing(8);
#ifdef Q_OS_ANDROID
    controls->setContentsMargins(10,0,10,0);
#endif
    auto* previous=new QPushButton(mOverlay);
    mPlayPause=new QPushButton(mOverlay); auto* next=new QPushButton(mOverlay);
    mAudioButton=new QPushButton(mOverlay);
    mFullscreenButton=new QPushButton(mOverlay);mThumbnailToggle=new QPushButton(mOverlay);
    mHistogramToggle=new QPushButton(mOverlay);mHistogramToggle->setCheckable(true);
    mClippingToggle=new QPushButton(mOverlay);mClippingToggle->setCheckable(true);
    for(auto* button:{previous,mPlayPause,next,mAudioButton,mFullscreenButton,mThumbnailToggle,mHistogramToggle,mClippingToggle}){button->setFixedSize(38,38);button->setIconSize(QSize(21,21));button->setFlat(true);button->setMouseTracking(true);button->setStyleSheet("QPushButton{color:white;background:rgba(0,0,0,145);border:0;border-radius:19px} QPushButton:hover{background:rgba(45,45,45,185)} QPushButton:checked{background:rgba(70,95,125,220)}");}
    previous->setIcon(mediaIcon(QStyle::SP_MediaSkipBackward));previous->setToolTip(tr("Previous clip"));
    next->setIcon(mediaIcon(QStyle::SP_MediaSkipForward));next->setToolTip(tr("Next clip"));
    mPlayPause->setToolTip(tr("Play / pause"));mAudioButton->setToolTip(tr("Capped, muted — click for real-time audio"));mFullscreenButton->setToolTip(tr("Toggle fullscreen"));
    mFullscreenButton->setIcon(fullscreenIcon(false));
    mHistogramToggle->setIcon(histogramIcon());
    mHistogramToggle->setToolTip(tr("Show raw RGB histogram"));
    mClippingToggle->setIcon(clippingIcon());
    mClippingToggle->setToolTip(tr("Show raw clipping indication"));
    mThumbnailToggle->setToolTip(tr("Show frame thumbnails"));
    mThumbnailToggle->setIcon(thumbnailChevronIcon(true));mThumbnailToggle->setIconSize(QSize(22,22));
    mVolumePanel=new QWidget(mOverlay);mVolumePanel->setFixedWidth(38);
    mVolumePanel->setAttribute(Qt::WA_StyledBackground,true);
    mVolumePanel->setStyleSheet("background:rgba(0,0,0,145);border-radius:19px;");
    mVolumeSlider=new VuVolumeSlider(mVolumePanel);mVolumeSlider->setFixedWidth(28);
    mVolumeSlider->setValue(mVolumePercent);
    mVolumeSlider->setToolTip(tr("Gallery volume: %1%").arg(mVolumePercent));
    mVolumeSlider->hide();
    controls->addStretch();controls->addWidget(previous);controls->addWidget(mPlayPause);controls->addWidget(next);controls->addWidget(mThumbnailToggle);controls->addWidget(mAudioButton);controls->addWidget(mHistogramToggle);controls->addWidget(mClippingToggle);controls->addWidget(mFullscreenButton);controls->addStretch();overlayLayout->addLayout(controls);
    mVolumePanel->hide();
    mVolumeAnimation=new QPropertyAnimation(mVolumePanel,"geometry",this);
    mVolumeAnimation->setDuration(180);mVolumeAnimation->setEasingCurve(QEasingCurve::OutCubic);
    connect(mVolumeAnimation,&QPropertyAnimation::valueChanged,this,[this](const QVariant& value){
        const int available=value.toRect().height()-46;
        mVolumeSlider->setGeometry(5,8,28,std::max(0,available));
        mVolumeSlider->setVisible(available>12);
    });
    mVolumeHideTimer.setSingleShot(true);mVolumeHideTimer.setInterval(2200);
    connect(&mVolumeHideTimer,&QTimer::timeout,this,[this]{setVolumePanelVisible(false);});
    connect(mVolumeAnimation,&QPropertyAnimation::finished,this,[this]{
        if(mVolumePanel&&!mVolumePanelWanted){mVolumePanel->hide();updateButtonIcons();}
    });
    connect(mVolumeSlider,&QSlider::valueChanged,this,[this](int value){
        mVolumePercent=value;mVolumeSlider->setToolTip(tr("Gallery volume: %1%").arg(value));
        if(mAudioSink&&!mAudioFadeTimer.isActive())mAudioSink->setVolume(value/100.0);
        QSettings("com.motioncam","MotionCam FS").setValue("gallery/volume",value);
    });
    mVuTimer.setInterval(50);
    connect(&mVuTimer,&QTimer::timeout,this,[this]{
        float peak=0.0f;
        if(mAudioEnabled&&mAudioClock.isValid()&&mAudioBytesPerSecond>0){
            const qint64 begin=qBound<qint64>(0,audioPositionNs()*mAudioBytesPerSecond/1000000000LL,mAudioPcm.size());
            const qint64 end=std::min<qint64>(mAudioPcm.size(),begin+mAudioBytesPerSecond/20);
            const float* samples=reinterpret_cast<const float*>(mAudioPcm.constData());
            for(qint64 i=begin/4;i<end/4;++i)peak=std::max(peak,std::abs(samples[i]));
        }
        mVuLevel=std::max(peak,mVuLevel*0.72f);
        static_cast<VuVolumeSlider*>(mVolumeSlider)->setLevel(mVuLevel);
    });
    mAudioFadeTimer.setInterval(15);
    connect(&mAudioFadeTimer,&QTimer::timeout,this,[this]{
        if(!mAudioSink){mAudioFadeTimer.stop();return;}
        constexpr qreal fadeMs=120.0;
        const qreal fraction=std::clamp(mAudioFadeClock.elapsed()/fadeMs,0.0,1.0);
        const qreal level=mAudioFadeOut
            ?mAudioFadeStartVolume*(1.0-fraction)
            :mVolumePercent/100.0*fraction;
        mAudioSink->setVolume(level);
        if(fraction<1.0)return;
        mAudioFadeTimer.stop();
        if(mAudioFadeOut&&mPaused){
            mAudioSink->suspend();
            mAudioClockBaseNs=qRound64(mPositionSeconds*1.0e9);
            mAudioClock.invalidate();
        }
    });
    mThumbnailScroll=new FrameThumbnailView(mOverlay);
#if defined(Q_OS_ANDROID) && QT_VERSION >= QT_VERSION_CHECK(6,9,0)
    mThumbnailScroll->setAttribute(Qt::WA_ContentsMarginsRespectsSafeArea,false);
    mThumbnailScroll->viewport()->setAttribute(Qt::WA_ContentsMarginsRespectsSafeArea,false);
#endif
    mThumbnailScroll->setViewMode(QListView::IconMode);
    mThumbnailScroll->setFlow(QListView::LeftToRight);
    mThumbnailScroll->setWrapping(false);
    mThumbnailScroll->setMovement(QListView::Static);
    mThumbnailScroll->setResizeMode(QListView::Adjust);
    mThumbnailScroll->setSelectionMode(QAbstractItemView::NoSelection);
    mThumbnailScroll->setHorizontalScrollMode(QAbstractItemView::ScrollPerPixel);
    mThumbnailBackfillTimer.setSingleShot(true);mThumbnailBackfillTimer.setInterval(120);
    connect(&mThumbnailBackfillTimer,&QTimer::timeout,this,[this]{
        if(mPaused)requestThumbnailBackfill();
    });
    connect(mThumbnailScroll->horizontalScrollBar(),&QScrollBar::valueChanged,this,[this]{
        if(mPaused)mThumbnailBackfillTimer.start();
    });
#ifdef Q_OS_ANDROID
    new motioncam::TouchScrolling(mThumbnailScroll, {}, false);
#endif
    auto* thumbnails=new FrameThumbnailModel(mThumbnailScroll);
    thumbnails->size=[this](int source){return thumbnailSizeForSource(source);};
    thumbnails->thumbnail=[this](int source){
        FrameThumbnailData data;
        if(mIndex<0||mIndex>=mClips.size())return data;
        const auto& clip=mClips[mIndex];
        data.image=thumbnailImageForSource(source);
        data.dropped=clip.sourceFrameToOutput&&source<static_cast<int>(clip.sourceFrameToOutput->size())&&
            (*clip.sourceFrameToOutput)[source]<0;
        const bool duplicated=clip.sourceFrameDuplicated&&source<static_cast<int>(clip.sourceFrameDuplicated->size())&&
            (*clip.sourceFrameDuplicated)[source];
        data.background=source==mCurrentThumbnailSource?QColor(25,118,210,90):
            data.dropped?QColor(235,190,35,60):duplicated?QColor(225,45,45,60):QColor(Qt::transparent);
        data.selected=clip.selectedSourceFrames.contains(source);
        return data;
    };
    thumbnails->activate=[this](int source){
        if(mIndex<0||mIndex>=mClips.size())return;
        const auto& map=mClips[mIndex].sourceFrameToOutput;
        const int output=map&&source<static_cast<int>(map->size())?(*map)[source]:source;
        if(output<0){
            if(!mPaused)mPlayPause->click();
            emit sourceFrameThumbnailRequested(currentMountId(),source);
        }else QTimer::singleShot(0,this,[this,output]{seekToFrame(output);});
    };
    thumbnails->select=[this](int source,bool selected){
        if(mIndex<0||mIndex>=mClips.size())return;
        if(selected)mClips[mIndex].selectedSourceFrames.insert(source);
        else mClips[mIndex].selectedSourceFrames.remove(source);
        refreshThumbnail(source);
        emit sourceFrameSelectionChanged(currentMountId(),source,selected);
    };
    mThumbnailScroll->setModel(thumbnails);
    mThumbnailScroll->setItemDelegate(new FrameThumbnailDelegate(mThumbnailScroll));
    mThumbnailScroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    mThumbnailScroll->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    mThumbnailScroll->setFixedHeight(thumbnailStripHeight());
    mThumbnailScroll->setFrameShape(QFrame::NoFrame);
#ifdef Q_OS_ANDROID
    mThumbnailScroll->setContentsMargins(0,0,0,0);
    mThumbnailScroll->viewport()->setContentsMargins(0,0,0,0);
#endif
    mThumbnailScroll->setStyleSheet(
        "QListView{background:rgba(0,0,0,80);border:0;"
#ifdef Q_OS_ANDROID
        "padding:0;margin:0;"
#endif
        "}"
        "QScrollBar:horizontal{background:rgba(0,0,0,25);height:10px;margin:0;border:0;}"
        "QScrollBar::handle:horizontal{background:rgba(255,255,255,40);min-width:30px;border:0;border-radius:5px;}"
        "QScrollBar::add-line:horizontal,QScrollBar::sub-line:horizontal{width:0;border:0;background:transparent;}"
        "QScrollBar::add-page:horizontal,QScrollBar::sub-page:horizontal{background:transparent;}");
    mThumbnailScroll->hide();overlayLayout->addWidget(mThumbnailScroll);overlayLayout->addSpacing(8);
    connect(mThumbnailScroll->horizontalScrollBar(),&QScrollBar::rangeChanged,
        this,[this]{centerCurrentThumbnail();});
    mPosition=new DuplicateSlider(mOverlay); mPosition->setRange(0,0);
    mPosition->setStyleSheet("QToolTip{background-color:#1976d2;color:white;border:1px solid #64a9e8;padding:4px 6px;}");
    mPosition->setMouseTracking(true);
#ifdef Q_OS_ANDROID
    mPosition->setMinimumHeight(28);
#endif
    auto* seekLayout=new QVBoxLayout;seekLayout->setContentsMargins(10,10,10,10);seekLayout->addWidget(mPosition);
#ifdef Q_OS_ANDROID
    seekLayout->setContentsMargins(20,10,20,10);
#endif
    overlayLayout->addLayout(seekLayout);stack->addWidget(mOverlay);layout->addWidget(stage,1);
    mOverlayOpacity=new QGraphicsOpacityEffect(mOverlay);mOverlayOpacity->setOpacity(1.0);
    mOverlayAnimation=new QPropertyAnimation(mOverlayOpacity,"opacity",this);
    mOverlayAnimation->setDuration(260);
    QEasingCurve fadeCurve;
    fadeCurve.setCustomType([](qreal progress) -> qreal {
        constexpr qreal steepness=10.0;
        const qreal lower=1.0/(1.0+std::exp(steepness/2.0));
        const qreal value=1.0/(1.0+std::exp(-steepness*(progress-0.5)));
        return (value-lower)/(1.0-2.0*lower);
    });
    mOverlayAnimation->setEasingCurve(fadeCurve);
    connect(mOverlayAnimation,&QPropertyAnimation::finished,this,[this]{
        mNativeOverlayCaptureTimer.setInterval(nativeOverlayIntervalMs);
        if(nativeVulkanPresenter())captureNativeOverlay();
    });
    mOverlayTimer.setSingleShot(true);mOverlayTimer.setInterval(4000);connect(&mOverlayTimer,&QTimer::timeout,this,[this]{
        if(mPerformanceOverlayPinned||mHistogramExpanded||pointerOverThumbnailRow())return;
#ifndef Q_OS_ANDROID
        if((mAudioButton&&mAudioButton->underMouse())||
           (mVolumePanel&&mVolumePanel->underMouse()))return;
#endif
        setOverlayVisible(false);
    });
    mNativeOverlayCaptureTimer.setInterval(nativeOverlayIntervalMs);
    connect(&mNativeOverlayCaptureTimer,&QTimer::timeout,this,[this]{
        if(!mGpuVideo||!mGpuVideo->isVisible())return;
        // Publish the captured UI with the next video frame, including
        // uncapped playback, rather than submitting an independent UI frame.
        if(!mPaused&&mLastPresentationNs>=0&&mPresentationClock.isValid()&&
           mPresentationClock.nsecsElapsed()-mLastPresentationNs<100000000){
            mNativeOverlayCapturePending=true;
            return;
        }
        captureNativeOverlay();
    });
    mSurfaceUpdateTimer.setSingleShot(true);mSurfaceUpdateTimer.setInterval(300);
    connect(&mSurfaceUpdateTimer,&QTimer::timeout,this,[this]{
        // A later resize may arrive while a prior surface is still starting.
        // The retained image, rather than first-frame state, is the authority
        // for whether it is safe and necessary to restart at the final size.
        if(mIndex<0||!hasPresentedFrame())return;
        if(hasReusableSourceFrame()){
            updateDisplayedImage();return;
        }
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
#ifdef Q_OS_ANDROID
    mTouchPanUpdateTimer.setSingleShot(true);
    mTouchPanUpdateTimer.setInterval(16);
    mTouchPanUpdateTimer.setTimerType(Qt::PreciseTimer);
    connect(&mTouchPanUpdateTimer,&QTimer::timeout,this,
            &ClipPlayerDialog::updateDisplayedImage);
#endif
    // Collapse wheel bursts and window-manager resize storms into one preview
    // generation instead of repeatedly tearing down an active producer.
    const std::array<QWidget*,4> trackedWidgets{this,stage,mVideo,mOverlay};
    for(auto* widget:trackedWidgets){
        widget->setMouseTracking(true);
    }
#ifdef Q_OS_ANDROID
    // Let Qt synthesize mouse events for overlay controls. Accept raw touch
    // only on the picture, where multi-touch gestures need all touch points.
    mVideo->setAttribute(Qt::WA_AcceptTouchEvents);
#endif
#if defined(Q_OS_ANDROID) && QT_VERSION >= QT_VERSION_CHECK(6,9,0)
    for(auto* widget:mOverlay->findChildren<QWidget*>())
        widget->setAttribute(Qt::WA_ContentsMarginsRespectsSafeArea,false);
#endif
    qApp->installEventFilter(this);mOverlay->raise();mFpsLabel->raise();revealOverlay();
    updateFpsIndicator();
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
        if(!hasPresentedFrame())mVideo->setText(tr("Playback unavailable"));
        spdlog::warn("Gallery FFmpeg could not be started: {}", detail.toStdString());
    });
    connect(&mFrameTimer,&QTimer::timeout,this,[this]{
        showNextFrame();
        if(mFrameTimer.isActive()&&!mPerformanceUncapped)
            updateFrameTimerInterval();
    });
    connect(mPlayPause,&QPushButton::clicked,this,[this]{
        if(mPaused&&mThumbnailScroll&&mThumbnailScroll->isVisible()&&mPosition&&
           mPosition->value()>=mPosition->maximum()&&
           (mClips[mIndex].isSequence&&!mDirectSequencePresentation
                ?mDecoder.state()==QProcess::NotRunning:
            mDirectFramesFinished)&&mFrames.empty()&&mBytes.isEmpty()){
            mPaused=false;
            openClip(mIndex,0.0);return;
        }
        mPaused=!mPaused;updateButtonIcons();
        if(!mPaused){mPresentationIntervals.clear();mLastPresentationNs=-1;
            mPresentationClock.restart();mCadenceStartFrame=-1;mCadenceLastFrame=-1;
            mCadenceClock.invalidate();}
        updateFpsIndicator();
        if(mPaused){mFrameTimer.stop();if(mAudioEnabled&&mAudioSink){
                mAudioFadeOut=true;
                mAudioFadeStartVolume=mAudioSink->volume();
                mAudioFadeClock.restart();mAudioFadeTimer.start();
            }
            if(mThumbnailScroll&&mThumbnailScroll->isVisible())
                requestThumbnailBackfill();
        }
        else {updateFrameTimerInterval();mFrameTimer.start();if(mThumbnailScroll&&mThumbnailScroll->isVisible())
                emit currentClipChanged(currentMountId(),mPositionSeconds);
            if(mAudioEnabled&&mAudioSink){
            if(mFirstFrameReady){startAudioAt(mPositionSeconds);mAudioStartPending=false;}
        }}
    });
    connect(mAudioButton,&QPushButton::clicked,this,&ClipPlayerDialog::cycleAudioMode);
    connect(mHistogramToggle,&QPushButton::toggled,this,[this](bool enabled){
        setHistogramEnabled(enabled);
        emit histogramEnabledChanged(enabled);
    });
    connect(mClippingToggle,&QPushButton::toggled,this,[this](bool enabled){
        setClippingEnabled(enabled);
        emit clippingEnabledChanged(enabled);
    });
    connect(previous,&QPushButton::clicked,this,[this]{ if(!mClips.isEmpty())openClip((mIndex-1+mClips.size())%mClips.size()); });
    connect(next,&QPushButton::clicked,this,&ClipPlayerDialog::advance);
    connect(mThumbnailToggle,&QPushButton::clicked,this,[this]{
        const bool show=!mThumbnailScroll->isVisible();mThumbnailScroll->setVisible(show);
#ifdef Q_OS_ANDROID
        QScroller::scroller(mThumbnailScroll->viewport())->stop();
        if(show)mThumbnailFollowPlayback=true;
#endif
        if(!show){
            mThumbnailCacheWriter.clear();
            revealOverlay();
        }
        mThumbnailCollectionEnabled->store(show);
        mThumbnailPrecacheEnabled->store(show);
        mThumbnailToggle->setIcon(thumbnailChevronIcon(!show));
        mThumbnailToggle->setToolTip(show?tr("Hide frame thumbnails"):tr("Show frame thumbnails"));
        const double seconds=mIndex>=0?mPosition->value()/std::max(1.0,mClips[mIndex].fps):0.0;
        if(show){
            rebuildThumbnailStrip();revealOverlay();
            // Active playback already produces thumbnails for every presented
            // frame. Only use the separate backfill renderer while idle.
            if(mPaused)requestThumbnailBackfill();
        }else{
            emit currentClipChanged(currentMountId(),seconds);
            revealOverlay();
        }
    });
#ifdef __ANDROID__
    mFullscreenButton->setToolTip(tr("Close gallery"));
    connect(mFullscreenButton,&QPushButton::clicked,this,&QWidget::close);
#else
    connect(mFullscreenButton,&QPushButton::clicked,this,[this]{
        if(isFullScreen())showNormal();else showFullScreen();
        updateButtonIcons();revealOverlay();
    });
#endif
    mIndex=0; for(int i=0;i<mClips.size();++i)if(mClips[i].mountId==initialMountId){mIndex=i;break;}
    if(!mClips.isEmpty())openClip(mIndex);
}

ClipPlayerDialog::~ClipPlayerDialog(){mClosing=true;suspendNativeBackingStore(false);mAudioLoadCancelled->store(true);++mAudioLoadGeneration;stopDecoder();}
int ClipPlayerDialog::currentMountId()const{return mIndex>=0&&mIndex<mClips.size()?mClips[mIndex].mountId:-1;}
void ClipPlayerDialog::setPlaybackPaused(bool paused){if(mPaused!=paused&&mPlayPause)mPlayPause->click();}
QPair<int,int> ClipPlayerDialog::visibleThumbnailOutputRange() const{
    if(mIndex<0||!mThumbnailScroll)return {0,0};
    const auto& clip=mClips[mIndex];
    const auto* view=mThumbnailScroll;
    int first=-1,last=-1;
    for(int x=0;x<view->viewport()->width();++x){
        const auto index=view->indexAt(QPoint(x,4));
        if(!index.isValid())continue;
        const int source=index.row();
        const int output=clip.sourceFrameToOutput?clip.sourceFrameToOutput->at(source):source;
        if(output<0)continue;
        if(first<0)first=output;
        last=output;
    }
    const int current=std::max(0,mPosition->value());
    return first<0?QPair<int,int>{current,current}:QPair<int,int>{first,last};
}
void ClipPlayerDialog::requestThumbnailBackfill(){
    // Do not cancel a seek/settings replacement before its first frame has
    // reached the presenter. That frame will request the idle backfill.
    if(mPaused&&mFirstFrameReady&&mThumbnailScroll&&mThumbnailScroll->isVisible())
        emit thumbnailBackfillRequested(currentMountId(),mPosition->value()/std::max(1.0,currentFrameRate()));
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
    seekToFrame(static_cast<int>(std::round(
        std::max(0.0,seconds)*std::max(1.0,mClips[mIndex].fps))));
}
double ClipPlayerDialog::currentDurationSeconds()const{
    return mIndex>=0&&mIndex<mClips.size()?mClips[mIndex].durationSeconds:0.0;
}
double ClipPlayerDialog::currentFrameRate()const{
    return mIndex>=0&&mIndex<mClips.size()?mClips[mIndex].fps:0.0;
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
void ClipPlayerDialog::invalidateThumbnails(int mountId){
    mThumbnailCacheWriter.clear();
    mThumbnailCacheWriter.waitForDone();
    for(const auto& image:mThumbnailCache.value(mountId))mThumbnailCachedBytes-=image.sizeInBytes();
    mThumbnailCache.remove(mountId);
    mStillThumbnailSizes.remove(mountId);
    for(auto it=mThumbnailCacheOrder.begin();it!=mThumbnailCacheOrder.end();)
        if(it->first==mountId)it=mThumbnailCacheOrder.erase(it);else ++it;
    if(mThumbnailDiskCache&&mThumbnailDiskCache->isValid()){
        QDir cacheDir(mThumbnailDiskCache->path());
        const auto files=cacheDir.entryList(
            {QStringLiteral("%1-*.jpg").arg(mountId)},QDir::Files);
        for(const auto& file:files)cacheDir.remove(file);
    }
    if(currentMountId()==mountId)rebuildThumbnailStrip();
}
void ClipPlayerDialog::updateClipInfo(int mountId,double fps,double durationSeconds,
        int sourceFrames,int width,int height,
        std::shared_ptr<const std::vector<bool>> duplicateFrames,
        std::shared_ptr<const std::vector<int>> sourceFrameToOutput,
        std::shared_ptr<const std::vector<bool>> sourceFrameDuplicated){
    for(auto& clip:mClips)if(clip.mountId==mountId){
        clip.fps=std::max(1.0,fps);clip.durationSeconds=std::max(0.0,durationSeconds);
        clip.sourceFrames=std::max(1,sourceFrames);
        clip.nativeWidth=width;clip.nativeHeight=height;
        // width/height describe the active gallery stream. Keep them until
        // the first replacement frame reports its real dimensions; writing
        // FileInfo's native size here makes reduced previews render as a small
        // centered image and changes the zoom coordinate system mid-refresh.
        if(clip.width<=0||clip.height<=0){
            clip.width=clip.nativeWidth;clip.height=clip.nativeHeight;
        }
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

void ClipPlayerDialog::releaseGpuFrames(bool keepPresentedFrame){
    if(mGpuVideo&&!keepPresentedFrame)mGpuVideo->drainGpuFrames();
    mFrames.clear();
    if(!keepPresentedFrame)mLastPresentedGpuBuffer.reset();
}

void ClipPlayerDialog::suspendNativeBackingStore(bool suspend){
    if(suspend){
        if(!mNativeBackingStoreHost&&window()->updatesEnabled()){
            mNativeBackingStoreHost=window();
            mNativeBackingStoreHost->setUpdatesEnabled(false);
        }
    }else if(mNativeBackingStoreHost){
        auto* host=mNativeBackingStoreHost.data();mNativeBackingStoreHost=nullptr;
        host->setUpdatesEnabled(true);
    }
}

void ClipPlayerDialog::captureNativeOverlay(bool submit){
    if(!mGpuVideo||!mGpuVideo->native())return;
    mNativeOverlayCapturePending=false;
    const QSize logicalSize=mGpuVideo->size();
    if(logicalSize.isEmpty())return;
    const auto started=std::chrono::steady_clock::now();
    mOverlayOpacity->setEnabled(false);
    // Native presentation owns the pixels. These widgets remain input targets
    // and are painted only by the synchronous offscreen atlas capture.
    mOverlay->setUpdatesEnabled(false);
    mFpsLabel->setUpdatesEnabled(false);
    const bool preparing=!hasPresentedFrame();
    mPreparingLabel->setVisible(preparing);
    if(preparing){
        mPreparingLabel->setText(mVideo->text());mPreparingLabel->adjustSize();
        mPreparingLabel->move((logicalSize.width()-mPreparingLabel->width())/2,
            (logicalSize.height()-mPreparingLabel->height())/2);
    }
    const qreal opacity=mOverlayOpacity->opacity();
    if(!preparing&&opacity<=0.0&&!mFpsLabel->isVisible()&&mCapturedNativeOverlayParts.empty())return;
    std::vector<GalleryOverlayRoot> roots;
    QRegion auxiliary;
    if(mHistogram->isVisible())auxiliary+=mHistogram->geometry();
    if(mThumbnailScroll->isVisible())auxiliary+=mThumbnailScroll->geometry();
    roots={{mOverlay,opacity,auxiliary,true},
        {mHistogram,opacity},
        {mThumbnailScroll,opacity},{mFpsLabel,1.0},{mPreparingLabel,1.0}};
    static_cast<RawHistogram*>(mHistogram)->setNativeCapture(true);
    const auto prepared=std::chrono::steady_clock::now();
    if(!mNativeOverlayCache)mNativeOverlayCache=std::make_unique<GalleryOverlayCache>();
    auto capture=captureGalleryOverlay(*mGpuVideo,roots,mGpuVideo->nativePixelRatio(),mNativeOverlayCache.get());
    static_cast<RawHistogram*>(mHistogram)->setNativeCapture(false);
    const auto captured=std::chrono::steady_clock::now();
    const auto* thumbnailDelegate=static_cast<FrameThumbnailDelegate*>(mThumbnailScroll->itemDelegate());
    const auto rasters=thumbnailDelegate->takeRasterCounts();
    if(capture.parts.empty())mCapturedNativeOverlay={};
    else if(capture.image!=mCapturedNativeOverlay)mCapturedNativeOverlay=std::move(capture.image);
    mCapturedNativeOverlayParts=std::move(capture.parts);
    // Active video will pick up this atlas on its next frame. An independent
    // overlay present repeats an old video frame and competes with its cadence.
    const bool redraw=mPaused||mIndex<0||mClips[mIndex].sourceFrames<=1||
        !mPresentationClock.isValid()||mLastPresentationNs<0||
        mPresentationClock.nsecsElapsed()-mLastPresentationNs>=100000000;
    if(submit)mGpuVideo->setOverlay(mCapturedNativeOverlay,mCapturedNativeOverlayParts,redraw);
    if(qEnvironmentVariableIsSet("MOTIONCAM_GALLERY_PERF_PROFILE"))
        spdlog::info("GALLERY_PERF event=native_overlay_capture viewport={}x{} atlas={}x{} parts={} opacity={:.3f} painted={} reused={} thumbnail_rasters={} thumbnail_reused={} thumbnail_bytes={} thumbnail_pixmap_kib={} paint_ms={:.3f} prepare_ms={:.3f} capture_ms={:.3f} elapsed_ms={:.3f}",
            logicalSize.width(),logicalSize.height(),mCapturedNativeOverlay.width(),
            mCapturedNativeOverlay.height(),mCapturedNativeOverlayParts.size(),double(opacity),capture.paintedWidgets,capture.reusedWidgets,rasters.first,rasters.second,mThumbnailCachedBytes,
            thumbnailDelegate->imageCacheKiB(),
            double(capture.paintNanoseconds)/1000000.0,
            std::chrono::duration<double,std::milli>(prepared-started).count(),
            std::chrono::duration<double,std::milli>(captured-prepared).count(),
            std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-started).count());
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
#ifdef Q_OS_ANDROID
    if(changingClip){
        mThumbnailFollowPlayback=true;
        QScroller::scroller(mThumbnailScroll->viewport())->stop();
    }
#endif
    const bool preservePausedNavigation=mPaused&&(!changingClip||
        (mThumbnailScroll&&mThumbnailScroll->isVisible()));
    if(changingClip){
        // A requested (unsnapped) wheel value belongs to the geometry that
        // produced it. Do not carry that hidden accumulator into another clip.
        mZoomAnimationTimer.stop();mZoomAnimationStartupDelay=false;
    }
    mSurfaceUpdateTimer.stop();
    const auto& requestedClip=mClips[index];
    const int requestedLastFrame=std::max(0,requestedClip.sourceFrames-1);
    const int requestedFrame=std::clamp(static_cast<int>(std::round(
        std::max(0.0,startSeconds)*std::max(1.0,requestedClip.fps))),0,requestedLastFrame);
    const double requestedSeconds=requestedFrame/std::max(1.0,requestedClip.fps);
    // Direct presentation uses a GPU widget above mVideo. Before hiding it,
    // copy the frame being shown into the fallback label. Otherwise the label
    // can expose a much older clip (often the last still) during the handoff.
    if(changingClip&&mGpuVideo&&mGpuVideo->isVisible()&&
       !mGpuVideo->native()&&
       !mLastPresentedImage.isNull()){
        const bool directPresentation=mDirectSequencePresentation;
        mDirectSequencePresentation=false;
        updateDisplayedImage();
        mDirectSequencePresentation=directPresentation;
    }
    // Invalidate the producer before closing the pipe it may currently be
    // writing. The connected handler increments the gallery generation
    // synchronously; newly queued callbacks cannot run until this returns.
    mPlaybackTarget->store(requestedFrame);mIncomingFrame->store(requestedFrame);
    releaseGpuFrames(true);
    emit currentClipChanged(requestedClip.mountId,requestedSeconds);
    stopDecoder();mBytes.clear();mBytesOffset=0;mFrames.clear();mSubmittedFrames.clear();
    if(changingClip)mAudioFadeTimer.stop();
    mPresentedRawFrame={};
    updateHistogram();
    mAudioLoadCancelled->store(true);
    mAudioLoadCancelled=std::make_shared<std::atomic_bool>(false);
    ++mAudioLoadGeneration;mAudioLoading=false;
    mFirstFrameReady=false;mAudioStartPending=mAudioEnabled;
    if(!changingClip&&!mPaused&&mAudioEnabled&&mAudioSink){
        mAudioFadeTimer.stop();
        mAudioSink->stop();mAudioClock.invalidate();
    }
    mIndex=index;
    mInputBitsPerChannel=16;
    // Present sequence frames at source size so a larger viewport does not
    // increase the FFmpeg output pipe and copy cost.
    mDirectSequencePresentation=mClips[index].isSequence &&
        (mClips[index].directPresentation ||
         qEnvironmentVariableIsSet("MOTIONCAM_GALLERY_DIRECT_PRESENT")) &&
        !qEnvironmentVariableIsSet("MOTIONCAM_GALLERY_FFMPEG_PRESENT");
#ifdef MOTIONCAM_HAS_DIRECTLOG_PLACEBO
    const bool nativeStillPresentation = !mClips[index].isSequence;
#else
    constexpr bool nativeStillPresentation = false;
#endif
    if((mDirectSequencePresentation||nativeStillPresentation)&&!mGpuVideo){
#if defined(MOTIONCAM_HAS_QUICKWIDGETS) && !defined(Q_OS_ANDROID)
        static const bool vulkanSelected=[] {
#if !defined(Q_OS_MACOS) && !defined(Q_OS_ANDROID)
            QQuickWindow::setGraphicsApi(QSGRendererInterface::Vulkan);
#endif
            return true;
        }();
        (void)vulkanSelected;
#endif
        auto* stage=mVideo->parentWidget();
        mGpuVideo=new GalleryGpuVideo(stage);
        QPalette videoPalette=mGpuVideo->palette();
        videoPalette.setColor(QPalette::Window,QColor(96,96,96));
        mGpuVideo->setPalette(videoPalette);
        mGpuVideo->setAutoFillBackground(true);
        mGpuVideo->setSizePolicy(QSizePolicy::Ignored,QSizePolicy::Ignored);
        mGpuVideo->setMouseTracking(true);
#ifdef Q_OS_ANDROID
        mGpuVideo->setAttribute(Qt::WA_AcceptTouchEvents);
#endif
        static_cast<QStackedLayout*>(stage->layout())->insertWidget(1,mGpuVideo);
        if(mGpuVideo->native()){
            mOverlayOpacity->setEnabled(false);
            mNativeOverlayCaptureTimer.start();
        }
#if defined(MOTIONCAM_HAS_QUICKWIDGETS) && !defined(Q_OS_ANDROID)
        if(mGpuVideo->native()){
            // Keep the grey preparation label visible until a processed GPU
            // frame exists. Mapping an empty native surface exposes the
            // compositor's undefined blue contents during startup.
            mGpuVideo->hide();
            mOverlay->raise();
            mFpsLabel->raise();
        }
#endif
    }
    if(mGpuVideo&&!mGpuVideo->native())
        mGpuVideo->hide();
    if(qEnvironmentVariableIsSet("MOTIONCAM_GALLERY_PERF_PROFILE"))
        spdlog::info("GALLERY_PERF event=gallery_presenter mount={} mode={}",
            mClips[index].mountId,
            mDirectSequencePresentation?"direct":
            nativeStillPresentation&&mGpuVideo&&mGpuVideo->native()
                ?"native_still":"ffmpeg");
    if(changingClip)
        mRequestedZoomPercent=mZoomPercent>0.0?mZoomPercent:fitScale()*100.0;
    const auto& clip=mClips[index];
    const int lastFrame=std::max(0,clip.sourceFrames-1);
    const int startFrame=std::clamp(static_cast<int>(std::round(
        std::max(0.0,startSeconds)*std::max(1.0,clip.fps))),0,lastFrame);
    mStartSeconds=startFrame/std::max(1.0,clip.fps);mPositionSeconds=mStartSeconds;
    mNextInputFrame=startFrame;
    mPlaybackTarget->store(startFrame);
    mIncomingFrame->store(startFrame);
    mPosition->setRange(0,lastFrame);mPosition->setValue(startFrame);
    static_cast<DuplicateSlider*>(mPosition)->setDuplicates(clip.duplicateFrames);
    mCurrentThumbnailSource=sourceFrameForOutput(startFrame);
    if(mThumbnailScroll->isVisible())rebuildThumbnailStrip();
    mPlaybackFailed=false;mDirectFramesFinished=false;mPaused=preservePausedNavigation;
    mPresentationIntervals.clear();mLastPresentationNs=-1;mPresentationClock.restart();
    mCadenceStartFrame=-1;mCadenceLastFrame=-1;mCadenceClock.invalidate();
    updateFpsIndicator();
    mPlayPause->setEnabled(true);updateButtonIcons();
    mMouseSourcePosition=QPoint(-1,-1);updateTitle();
    mWaitingForFirstFrame=hasPresentedFrame();
    if(!mWaitingForFirstFrame)mVideo->setText(tr("Preparing playback…"));
    else if(!changingClip)updateDisplayedImage();
    // Refreshing the current video frame does not change its audio source.
    // In particular, keep a suspended sink untouched while the player is
    // paused; rebuilding it can replay a buffered tail during the refresh.
    if(!mViewportRefreshPending&&(changingClip||!mAudioSink))configureAudio();
    mViewportRefreshPending=false;
    if(mClips[index].sourceFrames>1){
        if(clip.isSequence)startDecoder();
        else {updateFrameTimerInterval();mFrameTimer.start();}
    }
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
    if(!mThumbnailScroll||mIndex<0||mIndex>=mClips.size())return;
    const auto& clip=mClips[mIndex];
    const int sourceCount=clip.sourceFrameToOutput
        ?static_cast<int>(clip.sourceFrameToOutput->size()):clip.sourceFrames;
    mThumbnailScroll->setFixedHeight(thumbnailStripHeight());
    mThumbnailScroll->setUniformItemSizes(clip.isSequence);
    mThumbnailRenderBounds->store((clip.isSequence?thumbnailSizeForSource(0):
        thumbnailDisplayMaximumSize())-QSize(2,2),std::memory_order_relaxed);
    static_cast<FrameThumbnailModel*>(mThumbnailScroll->model())->setCount(sourceCount);
    mThumbnailScroll->doItemsLayout();
    centerCurrentThumbnail();
    QTimer::singleShot(0,this,[this]{centerCurrentThumbnail();});
}

QSize ClipPlayerDialog::thumbnailSizeForSource(int sourceFrame) const{
    auto bounds=thumbnailDisplayMaximumSize();
    if(mIndex<0||mIndex>=mClips.size())return bounds;
    const auto& clip=mClips[mIndex];
    if(!clip.isSequence){
        const QSize cached=mStillThumbnailSizes.value(clip.mountId).value(sourceFrame);
        if(cached.isValid())return frameThumbnailSize(cached.width(),cached.height(),-1,
            bounds.boundedTo(QSize(std::max(1,int(cached.width()/devicePixelRatioF())),
                std::max(1,int(cached.height()/devicePixelRatioF())))));
        return bounds;
    }
    const qreal ratio=devicePixelRatioF();
    const bool swap=clip.orientation==90||clip.orientation==270;
    if(clip.width>0&&clip.height>0)
        bounds=bounds.boundedTo(QSize(std::max(1,int((swap?clip.height:clip.width)/ratio)),
            std::max(1,int((swap?clip.width:clip.height)/ratio))));
    return frameThumbnailSize(clip.width,clip.height,clip.orientation,bounds);
}

QImage ClipPlayerDialog::thumbnailImageForSource(int sourceFrame){
    QImage cached=mThumbnailCache.value(currentMountId()).value(sourceFrame);
    if(cached.isNull()&&mThumbnailDiskCache&&mThumbnailDiskCache->isValid()){
        cached.load(thumbnailCachePath(currentMountId(),sourceFrame));
        if(!cached.isNull())cacheThumbnail(currentMountId(),sourceFrame,cached,false);
    }
    return cached;
}

void ClipPlayerDialog::refreshThumbnail(int sourceFrame){
    if(!mThumbnailScroll||!mThumbnailScroll->isVisible())return;
    const auto index=mThumbnailScroll->model()->index(sourceFrame,0);
    if(index.isValid())mThumbnailScroll->viewport()->update(mThumbnailScroll->visualRect(index));
}

void ClipPlayerDialog::setCurrentThumbnailFrame(int outputFrame){
    const int source=sourceFrameForOutput(outputFrame);if(source==mCurrentThumbnailSource)return;
    const int previous=mCurrentThumbnailSource;mCurrentThumbnailSource=source;
    if(previous>=0)refreshThumbnail(previous);
    if(source>=0)refreshThumbnail(source);
    if(!mPaused)centerCurrentThumbnail();
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
    if(!mThumbnailScroll||!mThumbnailScroll->isVisible()||
       mCurrentThumbnailSource<0||mNativeThumbnailTouch||
       (mNativeMouseTarget&&mThumbnailScroll->isAncestorOf(mNativeMouseTarget)))return;
#ifdef Q_OS_ANDROID
    if(!mThumbnailFollowPlayback||
       QScroller::scroller(mThumbnailScroll->viewport())->state()!=QScroller::Inactive)return;
#endif
    auto* bar=mThumbnailScroll->horizontalScrollBar();
    if(mIndex<0||mIndex>=mClips.size())return;
    const auto index=mThumbnailScroll->model()->index(mCurrentThumbnailSource,0);
    if(!index.isValid())return;
    const QRect item=mThumbnailScroll->visualRect(index);
    const int viewport=std::max(1,mThumbnailScroll->viewport()->width());
    bar->setValue(std::clamp(bar->value()+item.left()+8+(thumbnailSizeForSource(mCurrentThumbnailSource).width()+8)/2-viewport/2,
        bar->minimum(),bar->maximum()));
}

QString ClipPlayerDialog::thumbnailCachePath(int mountId,int sourceFrame)const{
    if(!mThumbnailDiskCache||!mThumbnailDiskCache->isValid())return {};
    return QDir(mThumbnailDiskCache->path()).filePath(
        QStringLiteral("%1-%2.jpg").arg(mountId).arg(sourceFrame));
}

void ClipPlayerDialog::cacheThumbnail(int mountId,int sourceFrame,const QImage& image,
                                      bool persistToDisk){
    constexpr int maximumCachedFrames=4096;
    constexpr qint64 maximumCachedBytes=32LL*1024*1024;
    const bool variableSize=mIndex>=0&&mountId==currentMountId()&&!mClips[mIndex].isSequence;
    const QSize previousSize=variableSize?thumbnailSizeForSource(sourceFrame):QSize();
    // Geometry survives pixel-cache eviction so revisiting a still cannot
    // change its item width merely because its image is temporarily unloaded.
    if(variableSize)mStillThumbnailSizes[mountId].insert(sourceFrame,image.size());
    if(persistToDisk&&mThumbnailDiskCache&&mThumbnailDiskCache->isValid()){
        const QString path=thumbnailCachePath(mountId,sourceFrame);
        if(!QFileInfo::exists(path)){
            constexpr int maximumPendingWrites=16;
            const auto pending=mThumbnailWritesPending;
            if(pending->fetch_add(1,std::memory_order_relaxed)<maximumPendingWrites){
                // The ticket also releases a queued task discarded by clear().
                const auto ticket=std::shared_ptr<void>(nullptr,[pending](void*){
                    pending->fetch_sub(1,std::memory_order_relaxed);
                });
                mThumbnailCacheWriter.start([path,image,ticket]{
                    if(QFileInfo::exists(path))return;
                    QSaveFile file(path);
                    if(!file.open(QIODevice::WriteOnly)||
                       !image.save(&file,"JPG",80)||!file.commit())
                        spdlog::warn("Could not persist gallery thumbnail cache entry");
                });
            }else{
                pending->fetch_sub(1,std::memory_order_relaxed);
            }
        }
    }
    auto& cache=mThumbnailCache[mountId];const QPair<int,int> key{mountId,sourceFrame};
    mThumbnailCacheOrder.removeAll(key);mThumbnailCacheOrder.append(key);
    mThumbnailCachedBytes-=cache.value(sourceFrame).sizeInBytes();
    cache.insert(sourceFrame,image);
    mThumbnailCachedBytes+=image.sizeInBytes();
    while(mThumbnailCacheOrder.size()>maximumCachedFrames||mThumbnailCachedBytes>maximumCachedBytes){
        const auto expired=mThumbnailCacheOrder.takeFirst();
        auto found=mThumbnailCache.find(expired.first);
        if(found==mThumbnailCache.end())continue;
        mThumbnailCachedBytes-=found->value(expired.second).sizeInBytes();
        found->remove(expired.second);
        if(found->isEmpty())mThumbnailCache.erase(found);
    }
    if(variableSize&&mThumbnailScroll&&mThumbnailScroll->isVisible()&&
       previousSize!=thumbnailSizeForSource(sourceFrame))
        static_cast<FrameThumbnailView*>(mThumbnailScroll)->updateThumbnailLayout();
}

void ClipPlayerDialog::setSourceFrameThumbnail(int sourceFrame,const QByteArray& frame,int width,int height){
    if(!mThumbnailCollectionEnabled->load())return;
    setSourceFrameThumbnailImage(sourceFrame,rgb48Thumbnail(frame,width,height));
}

void ClipPlayerDialog::setSourceFrameThumbnailImage(int sourceFrame,const QImage& image,
        bool cacheWhenHidden){
    const bool visible=mThumbnailCollectionEnabled->load();
    if((!visible&&!cacheWhenHidden)||image.isNull())return;
    cacheThumbnail(currentMountId(),sourceFrame,image,visible);
    if(qEnvironmentVariableIsSet("MOTIONCAM_GALLERY_PERF_PROFILE") &&
       (sourceFrame<10 || sourceFrame%30==0))
        spdlog::info("GALLERY_PERF event=thumbnail_applied source={} current={} visible={}",
            sourceFrame,mCurrentThumbnailSource,
            mThumbnailScroll&&mThumbnailScroll->isVisible());
    if(visible)refreshThumbnail(sourceFrame);
}

void ClipPlayerDialog::setOutputFrameThumbnail(int outputFrame,const QByteArray& frame,int width,int height){
    const int source=sourceFrameForOutput(outputFrame);if(source>=0)setSourceFrameThumbnail(source,frame,width,height);
}
void ClipPlayerDialog::setOutputFrameThumbnailImage(int outputFrame,const QImage& image,
        bool cacheWhenHidden){
    const int source=sourceFrameForOutput(outputFrame);
    if(source>=0)setSourceFrameThumbnailImage(source,image,cacheWhenHidden);
}
void ClipPlayerDialog::presentDroppedSourceFrame(int sourceFrame,const QByteArray& frame,int width,int height){
    if(!mThumbnailCollectionEnabled->load()||mIndex<0)return;
    setSourceFrameThumbnail(sourceFrame,frame,width,height);
    const QImage image=rgb48Image(frame,width,height);if(image.isNull())return;
    mLastPresentedImage=image;mLastPresentedGpuBuffer.reset();
    mLastImageIsSource=true;mWaitingForFirstFrame=false;
    int cursor=0;const auto& map=mClips[mIndex].sourceFrameToOutput;
    if(map)for(int source=sourceFrame-1;source>=0;--source)
        if((*map)[source]>=0){cursor=(*map)[source];break;}
    mPosition->setValue(cursor);mPlaybackTarget->store(cursor);
    mPositionSeconds=cursor/std::max(1.0,mClips[mIndex].fps);
    const int previous=mCurrentThumbnailSource;mCurrentThumbnailSource=sourceFrame;
    if(previous>=0)refreshThumbnail(previous);
    refreshThumbnail(sourceFrame);
    setDuplicateCursorVisible(false);setDroppedCursorVisible(true);
    centerCurrentThumbnail();updateDisplayedImage();updateTitle();completeFrameStep();
}
void ClipPlayerDialog::clearFrameSelections(){
    for(auto& clip:mClips)clip.selectedSourceFrames.clear();
    if(mThumbnailScroll&&mThumbnailScroll->isVisible())rebuildThumbnailStrip();
}
void ClipPlayerDialog::clearFrameSelections(const QSet<int>& mountIds){
    for(auto& clip:mClips)
        if(mountIds.contains(clip.mountId))clip.selectedSourceFrames.clear();
    if(mThumbnailScroll&&mThumbnailScroll->isVisible()&&mountIds.contains(currentMountId()))
        rebuildThumbnailStrip();
}

void ClipPlayerDialog::selectAllFrameSelections(){
    for(auto& clip:mClips){
        clip.selectedSourceFrames.clear();
        const int sourceCount=clip.sourceFrameToOutput
            ?static_cast<int>(clip.sourceFrameToOutput->size()):clip.sourceFrames;
        for(int source=0;source<sourceCount;++source)
            clip.selectedSourceFrames.insert(source);
    }
    if(mThumbnailScroll&&mThumbnailScroll->isVisible())rebuildThumbnailStrip();
}

void ClipPlayerDialog::startDecoder(){
    if(mDirectSequencePresentation){
        updateFrameTimerInterval();mFrameTimer.start();return;
    }
    const QString exe=ffmpegPath();if(exe.isEmpty()){mPlaybackFailed=true;mFrameTimer.stop();if(!hasPresentedFrame())mVideo->setText(tr("FFmpeg was not found"));return;}
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
    if(inputWidth<=0||inputHeight<=0){mPlaybackFailed=true;mFrameTimer.stop();if(!hasPresentedFrame())mVideo->setText(tr("Invalid clip dimensions"));return;}
    mInputFrameBytes=inputWidth*inputHeight*(normalizeMixedFrames?4:
        mInputBitsPerChannel==8?3:6);
    QStringList a{"-hide_banner","-loglevel","error"};
    a<<"-f"<<"rawvideo"<<"-pixel_format"<<(normalizeMixedFrames?"rgba":
        mInputBitsPerChannel==8?"rgb24":"rgb48le")<<"-video_size"
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
    // Preserve the requested center independently of the integer source crop.
    // This is the coordinate used to position retained frames in the viewport.
    mDecoderSurfaceViewportPan=mZoomPercent>0.0?mPanSourcePixels:QPointF();
    a<<"-an"<<"-vf"<<f<<"-pix_fmt"<<"rgba"<<"-f"<<"rawvideo"<<"pipe:1";mDecoder.setProcessChannelMode(QProcess::SeparateChannels);mDecoder.start(exe,a,QIODevice::ReadWrite);
    updateFrameTimerInterval();mFrameTimer.start();
}

double ClipPlayerDialog::fitScale() const{
    if(mIndex<0||mIndex>=mClips.size()||mClips[mIndex].width<=0||mClips[mIndex].height<=0)return 1.0;
    const bool swap=mClips[mIndex].orientation==90||mClips[mIndex].orientation==270;
    const int width=swap?mClips[mIndex].height:mClips[mIndex].width;
    const int height=swap?mClips[mIndex].width:mClips[mIndex].height;
    const QWidget* viewport=mVideo;
    if(mGpuVideo&&mGpuVideo->native()&&mGpuVideo->usable())
        viewport=mGpuVideo;
    return std::min({double(std::max(1,viewport->width()))/width,
                     double(std::max(1,viewport->height()))/height,32.0});
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
    mMouseSourcePosition=QPoint(-1,-1);updateTitle();
    mSurfaceUpdateTimer.stop();
    const bool reusableSource=hasReusableSourceFrame();
    const bool wasAnimating=mZoomAnimationTimer.isActive();
    mZoomAnimationTarget=target;
    // Zooming out needs pixels outside the current efficient crop. Begin
    // preparing the wider target crop promptly and do not keep postponing it
    // for every event in the same wheel gesture. Zoom-in can safely continue
    // using the existing wider surface while input is coalesced.
    const bool zoomingOut=effectiveTarget<current;
    // A retained source frame is complete; crop refreshes would decode the
    // entire DNG after each wheel event.
    if(!reusableSource){
        if(zoomingOut){
            if(!mSurfaceUpdateTimer.isActive()){
                mSurfaceUpdateTimer.setInterval(32);mSurfaceUpdateTimer.start();
            }
        }else{
            mSurfaceUpdateTimer.setInterval(48);mSurfaceUpdateTimer.start();
        }
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
    updateTitle();
    if(finished){
        mZoomAnimationTimer.stop();mZoomAnimationStartupDelay=false;
        const QPointF desiredScale=surfaceScaleForZoom(mZoomPercent);
        const QPointF desiredPan=mZoomPercent>0.0
            ?effectivePanForZoom(mPanSourcePixels,mZoomPercent):QPointF();
        const bool cropAlreadyPrepared=
            std::abs(mDecoderSurfaceScale.x()-desiredScale.x())<0.000001&&
            std::abs(mDecoderSurfaceScale.y()-desiredScale.y())<0.000001&&
            QLineF(mDecoderSurfacePan,desiredPan).length()<0.000001;
        if(!cropAlreadyPrepared&&!hasReusableSourceFrame()){
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

void ClipPlayerDialog::adoptRenderedDimensions(int width,int height){
    if(mIndex<0||mIndex>=mClips.size()||width<=0||height<=0)return;
    auto& clip=mClips[mIndex];
    if(clip.width==width&&clip.height==height)return;
    const int oldWidth=std::max(1,clip.width);
    const int oldHeight=std::max(1,clip.height);
    const double sourceScaleX=double(width)/oldWidth;
    const double sourceScaleY=double(height)/oldHeight;
    const double zoomScale=std::sqrt((double(oldWidth)/width)*
                                     (double(oldHeight)/height));

    // Pan is stored in pixels of the active preview. Rebase it, along with
    // the retained FFmpeg surface description, so the same normalized source
    // point remains under the viewport when proxy/binning/HQ changes size.
    mPanSourcePixels=QPointF(mPanSourcePixels.x()*sourceScaleX,
                             mPanSourcePixels.y()*sourceScaleY);
    mLastSurfacePan=QPointF(mLastSurfacePan.x()*sourceScaleX,
                            mLastSurfacePan.y()*sourceScaleY);
    mLastSurfaceViewportPan=QPointF(mLastSurfaceViewportPan.x()*sourceScaleX,
                                    mLastSurfaceViewportPan.y()*sourceScaleY);
    mDecoderSurfacePan=QPointF(mDecoderSurfacePan.x()*sourceScaleX,
                               mDecoderSurfacePan.y()*sourceScaleY);
    mDecoderSurfaceViewportPan=QPointF(mDecoderSurfaceViewportPan.x()*sourceScaleX,
                                       mDecoderSurfaceViewportPan.y()*sourceScaleY);
    mLastSurfaceScale=QPointF(mLastSurfaceScale.x()/sourceScaleX,
                              mLastSurfaceScale.y()/sourceScaleY);
    mDecoderSurfaceScale=QPointF(mDecoderSurfaceScale.x()/sourceScaleX,
                                 mDecoderSurfaceScale.y()/sourceScaleY);
    auto rebaseZoom=[zoomScale](double& value){
        if(value>0.0)value=std::clamp(value*zoomScale,0.01,3200.0);
    };
    rebaseZoom(mZoomPercent);
    rebaseZoom(mRequestedZoomPercent);
    rebaseZoom(mZoomAnimationTarget);
    clip.width=width;clip.height=height;
    if(mThumbnailScroll&&mThumbnailScroll->isVisible())rebuildThumbnailStrip();
    clampPanToZoom();
    mIntensitySamples=nullptr;
    updateTitle();
}

void ClipPlayerDialog::updateTitle(){
    if(!mTitle||mIndex<0||mIndex>=mClips.size())return;
#ifdef Q_OS_ANDROID
    // Android embeds the gallery in the activity window. Qt tooltip popup
    // windows can crash there; keep scrub feedback in the captured overlay.
    if(mSeeking){
        mTitle->setText(framePositionText(mPosition->value()).toHtmlEscaped()
            .replace(QLatin1Char('\n'),QStringLiteral("<br>")));
        return;
    }
#endif
    const double shownZoom=mZoomPercent>0.0?mZoomPercent:fitScale()*100.0;
    QString view=mZoomPercent>0.0
        ?tr("Zoom %1%").arg(shownZoom,0,'f',1):tr("Scale to fit");
    if(mHistogramEnabled&&mDetectedClipValue)
        view+=tr(" — Clip %1").arg(*mDetectedClipValue);
    if(mHistogramEnabled&&mMouseSourcePosition.x()>=0&&
       mMouseSourcePosition.y()>=0){
        view+=tr(" — x %1, y %2").arg(mMouseSourcePosition.x())
            .arg(mMouseSourcePosition.y());
    }
    const auto& raw=mPresentedRawFrame;
    if(!mHistogramEnabled){
        mIntensitySamples=nullptr;
        mCachedIntensity.clear();
    }else if(mIntensitySamples!=raw.samples.get() ||
       mIntensityPosition!=mMouseSourcePosition) {
        mIntensitySamples=raw.samples.get();
        mIntensityPosition=mMouseSourcePosition;
        mCachedIntensity.clear();
        if(raw.samples&&raw.width>0&&raw.height>0&&
           (raw.channels==1||raw.channels==3)&&
           mMouseSourcePosition.x()>=0&&mMouseSourcePosition.y()>=0){
            const auto& clip=mClips[mIndex];
            const int nativeWidth=clip.nativeWidth>0?clip.nativeWidth:raw.width;
            const int nativeHeight=clip.nativeHeight>0?clip.nativeHeight:raw.height;
            const int orientedWidth=raw.orientation==90||raw.orientation==270
                ?raw.height:raw.width;
            const int orientedHeight=raw.orientation==90||raw.orientation==270
                ?raw.width:raw.height;
            const int orientedX=std::clamp(static_cast<int>(std::floor(
                (mMouseSourcePosition.x()+0.5)*orientedWidth/nativeWidth)),0,orientedWidth-1);
            const int orientedY=std::clamp(static_cast<int>(std::floor(
                (mMouseSourcePosition.y()+0.5)*orientedHeight/nativeHeight)),0,orientedHeight-1);
            int x=orientedX,y=orientedY;
            if(raw.orientation==90){x=orientedY;y=raw.height-1-orientedX;}
            else if(raw.orientation==180){x=raw.width-1-orientedX;y=raw.height-1-orientedY;}
            else if(raw.orientation==270){x=raw.width-1-orientedY;y=orientedX;}
            const size_t offset=(static_cast<size_t>(y)*raw.width+x)*raw.channels;
            if(offset+raw.channels<=raw.samples->size()){
                static const std::array<QString,3> colors={
                    QStringLiteral("#ff7777"),QStringLiteral("#8fff8f"),
                    QStringLiteral("#8faaff")};
                mCachedIntensity=tr(" — intensity ").toHtmlEscaped();
                const int phaseBlock=std::max(1,raw.cfaSize/2);
                const int phase=(((y/phaseBlock)&1)<<1)|((x/phaseBlock)&1);
                for(int channel=0;channel<raw.channels;++channel){
                    if(channel>0)mCachedIntensity+=QStringLiteral(", ");
                    const int color=raw.channels==3?channel:
                        std::min<int>(2,raw.cfaPhase[phase]);
                    mCachedIntensity+=QStringLiteral("<span style='color:%1'>%2</span>")
                        .arg(colors[color]).arg((*raw.samples)[offset+channel]);
                }
            }
        }
    }
    QString title=mClips[mIndex].title;
    const auto& names=mClips[mIndex].stillFrameNames;
    const int source=mCurrentThumbnailSource>=0?mCurrentThumbnailSource:
        sourceFrameForOutput(mPosition->value());
    if(names&&source>=0&&source<static_cast<int>(names->size()))
        title+=QStringLiteral(" — ")+QString::fromStdString((*names)[source]);
    mTitle->setText(QString("%1 — %2 / %3 — %4%5")
        .arg(title.toHtmlEscaped()).arg(mIndex+1).arg(mClips.size())
        .arg(view.toHtmlEscaped()).arg(mCachedIntensity));
}

void ClipPlayerDialog::updateFpsIndicator(){
    if(!mFpsLabel)return;
    mFpsLabel->setVisible(mFpsIndicatorEnabled&&!mPaused&&mIndex>=0&&mClips[mIndex].sourceFrames>1&&
        !mPresentationIntervals.empty());
    auto positionLabel=[this]{
        mFpsLabel->adjustSize();
        mFpsLabel->move(mFpsLabel->parentWidget()->width()-mFpsLabel->width()-10,10);
        mFpsLabel->raise();
    };
    if(mPresentationIntervals.empty()){mFpsIndicatorClock.invalidate();return;}
    mFpsLabel->move(mFpsLabel->parentWidget()->width()-mFpsLabel->width()-10,10);
    if(mFpsIndicatorClock.isValid()&&mFpsIndicatorClock.elapsed()<250)return;
    mFpsIndicatorClock.restart();
    auto sorted=std::vector<qint64>(mPresentationIntervals.begin(),mPresentationIntervals.end());
    std::sort(sorted.begin(),sorted.end());
    const size_t index=sorted.size()-std::max<size_t>(1,(sorted.size()+99)/100);
    // The slowest 1% of presentation intervals determine the low frame rate.
    const qint64 slow=sorted[index];
    const QString text=tr("%1 fps").arg(1.0e9/std::max<qint64>(1,slow),0,'f',1);
    if(mFpsLabel->text()!=text){
        mFpsLabel->setText(text);
        positionLabel();
    }
}

void ClipPlayerDialog::setHistogramEnabled(bool enabled){
    if(!enabled && mHistogramExpanded)setHistogramExpanded(false);
    mHistogramEnabled=enabled;
    if(!enabled)mPresentedRawFrame.samples.reset();
    const QSignalBlocker toggleSignals(mHistogramToggle);
    mHistogramToggle->setChecked(enabled);
    mHistogramToggle->setToolTip(enabled?tr("Hide raw RGB histogram"):
        tr("Show raw RGB histogram"));
    mHistogram->setVisible(enabled);
    if(mThumbnailScroll&&mThumbnailScroll->isVisible())rebuildThumbnailStrip();
    updateHistogram();
}

void ClipPlayerDialog::setPerformanceUncapped(bool enabled){
    mAudioMode=enabled?AudioMode::UncappedMuted:AudioMode::CappedMuted;
    mPerformanceUncapped=enabled;
    setAudioEnabled(false);
}

void ClipPlayerDialog::setPerformanceAudioEnabled(bool enabled){
    mAudioMode=enabled?AudioMode::RealTimeAudio:AudioMode::CappedMuted;
    mPerformanceUncapped=false;
    setAudioEnabled(enabled);
}

void ClipPlayerDialog::setClippingEnabled(bool enabled){
    if(mClippingEnabled!=enabled){
        // Every cached clip must use the new indication state, including disk
        // entries and clips revisited after toggling on a different clip.
        for(const auto& clip:mClips)invalidateThumbnails(clip.mountId);
    }
    mClippingEnabled=enabled;
    const QSignalBlocker toggleSignals(mClippingToggle);
    mClippingToggle->setChecked(enabled);
    mClippingToggle->setToolTip(enabled?tr("Hide raw clipping indication"):
        tr("Show raw clipping indication"));
}

void ClipPlayerDialog::setPerformanceOverlayPinned(bool pinned){
    mPerformanceOverlayPinned=pinned;
    if(!pinned){revealOverlay();return;}
    mOverlayTimer.stop();
    mOverlayAnimation->stop();
    mOverlayOpacity->setOpacity(1.0);
    mOverlay->setAttribute(Qt::WA_TransparentForMouseEvents,false);
}

void ClipPlayerDialog::setPerformancePixelOverlayEnabled(bool enabled){
    if(mIndex<0 || mIndex>=mClips.size())return;
    const auto& clip=mClips[mIndex];
    mMouseSourcePosition=enabled ? QPoint(
        std::max(0,clip.nativeWidth/2),std::max(0,clip.nativeHeight/2)) : QPoint(-1,-1);
    mIntensitySamples=nullptr;
    updateTitle();
}

void ClipPlayerDialog::setHistogramExpanded(bool expanded){
#ifdef Q_OS_ANDROID
    if(expanded)return;
#endif
    if(mHistogramExpanded==expanded)return;
    mHistogramExpanded=expanded;
    auto* layout=static_cast<QVBoxLayout*>(mOverlay->layout());
    if(expanded){
        mHistogram->setMinimumSize(0,0);
        mHistogram->setMaximumSize(QWIDGETSIZE_MAX,QWIDGETSIZE_MAX);
        mHistogram->setSizePolicy(QSizePolicy::Expanding,QSizePolicy::Expanding);
        layout->setAlignment(mHistogram,Qt::Alignment{});
        layout->setStretch(1,1);
        layout->setStretch(2,0);
        mOverlayTimer.stop();
        mOverlayAnimation->stop();
        mOverlayOpacity->setOpacity(1.0);
        mOverlay->setAttribute(Qt::WA_TransparentForMouseEvents,false);
    }else{
#ifdef Q_OS_ANDROID
        updateAndroidOverlayMask();
#else
        mHistogram->setFixedSize(560,260);
#endif
        mHistogram->setSizePolicy(QSizePolicy::Fixed,QSizePolicy::Fixed);
        layout->setAlignment(mHistogram,Qt::AlignLeft);
        layout->setStretch(1,0);
        layout->setStretch(2,1);
        revealOverlay();
    }
    layout->invalidate();
    layout->activate();
}

void ClipPlayerDialog::updateHistogram(){
    const auto histogramStarted=std::chrono::steady_clock::now();
    const auto& raw=mPresentedRawFrame;
    mIntensitySamples=nullptr;
    mDetectedClipValue.reset();
    if(!mHistogramEnabled){
        updateTitle();
        return;
    }
    const int binCount=static_cast<RawHistogram*>(mHistogram)->binCount();
    RawHistogram::Bins bins{};
    for(auto& channel:bins)channel.assign(binCount,0.0f);
    if(raw.samples && raw.width>0 && raw.height>0 &&
       (raw.channels==1||raw.channels==3) && raw.white>raw.black) {
        const size_t pixels=std::min(raw.samples->size()/raw.channels,
            static_cast<size_t>(raw.width)*raw.height);
        const size_t sampleTarget=std::max<size_t>(250000,
            static_cast<size_t>(binCount)*128);
        size_t stride=std::max<size_t>(1,pixels/sampleTarget);
        if(stride>1 && stride%2==0)++stride;
        if(mClipValueCounts.size()!=65536)mClipValueCounts.resize(65536);
        std::fill(mClipValueCounts.begin(),mClipValueCounts.end(),
                  std::array<uint32_t,3>{});
        std::array<size_t,3> sampleCounts{};
        const int phaseBlock=std::max(1,raw.cfaSize/2);
        const double distribution=static_cast<RawHistogram*>(mHistogram)->distribution();
        if(mHistogramBinChannels!=raw.channels || mHistogramBinWhite!=raw.white ||
           mHistogramBinWidth!=binCount ||
           mHistogramBinDistribution!=distribution ||
           mHistogramBinBlackLevels!=raw.blackLevels) {
            mHistogramBinChannels=raw.channels;
            mHistogramBinWhite=raw.white;
            mHistogramBinWidth=binCount;
            mHistogramBinDistribution=distribution;
            mHistogramBinBlackLevels=raw.blackLevels;
            const int phases=raw.channels==3?3:4;
            double histogramBlack=0.0;
            for(int phase=0;phase<phases;++phase)
                if(raw.blackLevels[phase]>0.0f &&
                   (histogramBlack==0.0 || raw.blackLevels[phase]<histogramBlack))
                    histogramBlack=raw.blackLevels[phase];
            mHistogramCodePositions.resize(65536);
            for(int value=0;value<65536;++value)
                mHistogramCodePositions[value]=static_cast<float>(
                    RawHistogram::position(value,raw.white,histogramBlack,distribution)*
                        (binCount-1));
        }
        for(size_t pixel=0;pixel<pixels;pixel+=stride) {
            for(int channel=0;channel<raw.channels;++channel) {
                const uint16_t value=(*raw.samples)[pixel*raw.channels+channel];
                const int phase=raw.channels==3?channel:
                    static_cast<int>(((((pixel/raw.width)/phaseBlock)&1u)<<1) |
                                     (((pixel%raw.width)/phaseBlock)&1u));
                const int color=raw.channels==3?channel:
                    std::min<int>(2,raw.cfaPhase[phase]);
                ++sampleCounts[color];
                ++mClipValueCounts[value][color];
            }
        }
        // Sample the raw-code histogram at the displayed x coordinates. A code
        // can cover several screen pixels near black; interpolating in code
        // space preserves its shape as the stop widths change.
        for(int color=0;color<3;++color) {
            std::vector<float> codeCounts(65536);
            int previous=-1;
            for(int value=0;value<65536;++value) {
                const float count=static_cast<float>(mClipValueCounts[value][color]);
                if(count<=0.0f)continue;
                if(previous>=0 && value>previous+1 && value-previous<=8)
                    for(int missing=previous+1;missing<value;++missing)
                        codeCounts[missing]=codeCounts[previous]+
                            (count-codeCounts[previous])*
                                float(missing-previous)/float(value-previous);
                codeCounts[value]=count;
                previous=value;
            }
            std::vector<double> cumulative(65537);
            for(int value=0;value<65536;++value)
                cumulative[value+1]=cumulative[value]+codeCounts[value];
            const auto begin=mHistogramCodePositions.begin();
            const auto end=mHistogramCodePositions.end();
            for(int bin=0;bin<binCount;++bin) {
                const auto first=std::lower_bound(begin,end,float(bin)-0.5f);
                const auto last=std::lower_bound(first,end,float(bin)+0.5f);
                if(last>first) {
                    bins[color][bin]=static_cast<float>(
                        cumulative[last-begin]-cumulative[first-begin]);
                    continue;
                }
                const auto upper=std::lower_bound(begin,end,float(bin));
                if(upper==begin || upper==end)continue;
                const int right=static_cast<int>(upper-begin);
                const int left=right-1;
                const float span=mHistogramCodePositions[right]-
                    mHistogramCodePositions[left];
                if(span<=0.0f)continue;
                const float t=(bin-mHistogramCodePositions[left])/span;
                const float y0=codeCounts[left],y1=codeCounts[right];
                const float delta=y1-y0;
                auto slope=[](float a,float b) {
                    return a*b>0.0f ? 2.0f*a*b/(a+b) : 0.0f;
                };
                const float m0=left>0 ? slope(y0-codeCounts[left-1],delta) : delta;
                const float m1=right<65535 ?
                    slope(delta,codeCounts[right+1]-y1) : delta;
                const float t2=t*t,t3=t2*t;
                bins[color][bin]=std::max(0.0f,
                    (2*t3-3*t2+1)*y0+(t3-2*t2+t)*m0+
                    (-2*t3+3*t2)*y1+(t3-t2)*m1);
            }
        }
        for(int color=0;color<3;++color) {
            if(sampleCounts[color]==0)continue;
            const size_t brightestCount=std::max<size_t>(1,
                (sampleCounts[color]+99)/100);
            size_t included=0,modeCount=0;
            int modeValue=-1;
            for(int value=65535;value>=0 && included<brightestCount;--value) {
                const size_t count=std::min<size_t>(
                    mClipValueCounts[value][color],brightestCount-included);
                included+=count;
                if(count>modeCount) {
                    modeCount=count;
                    modeValue=value;
                }
            }
            // A repeated clipping code dominates the bright tail even when
            // a few brighter hot pixels prevent it from being the raw maximum.
            if(modeCount*10>=brightestCount && modeValue>raw.black &&
               modeValue!=qRound(raw.white) &&
               (!mDetectedClipValue || modeValue>*mDetectedClipValue))
                mDetectedClipValue=static_cast<uint16_t>(modeValue);
        }
    }
    if(mHistogram)
        mHistogram->setProperty("nativeCaptureOnly",
            nativeVulkanPresenter()&&mGpuVideo->isVisible());
    if(mHistogram)
        static_cast<RawHistogram*>(mHistogram)->setBins(std::move(bins),raw.white,
            raw.blackLevels,raw.channels==3 ? std::array<uint8_t,4>{0,1,2,0} :
                raw.cfaPhase,raw.samples && raw.white>raw.black && raw.white>0.0f
                ? (raw.channels==3?3:4) : 0);
    updateTitle();
    if(qEnvironmentVariableIsSet("MOTIONCAM_GALLERY_PERF_PROFILE"))
        spdlog::info("GALLERY_PERF event=histogram_frame width={} height={} channels={} bins={} latency_ms={:.3f}",
            raw.width,raw.height,raw.channels,binCount,
            std::chrono::duration<double,std::milli>(
                std::chrono::steady_clock::now()-histogramStarted).count());
}

void ClipPlayerDialog::setFpsIndicatorEnabled(bool enabled){
    mFpsIndicatorEnabled=enabled;
    updateFpsIndicator();
}

bool ClipPlayerDialog::pointerOverThumbnailRow()const{
#ifdef Q_OS_ANDROID
    // Android's synthesized cursor can remain over the last tapped control.
    return false;
#else
    return mThumbnailScroll&&mThumbnailScroll->isVisible()&&
        mThumbnailScroll->rect().contains(mThumbnailScroll->mapFromGlobal(QCursor::pos()));
#endif
}

void ClipPlayerDialog::updateMouseSourcePosition(const QPointF& globalPosition){
    QPoint next(-1,-1);
    if(mVideo&&mIndex>=0&&mIndex<mClips.size()){
        const auto& clip=mClips[mIndex];
        const int width=clip.width,height=clip.height;
        if(width>0&&height>0){
            const QPointF position=mVideo->mapFromGlobal(globalPosition.toPoint());
            const bool swap=clip.orientation==90||clip.orientation==270;
            const int displayWidth=swap?height:width;
            const int displayHeight=swap?width:height;
            int x=-1,y=-1;
            if(mZoomPercent>0.0&&!mLastImageIsSource){
                // Mirror the decoder's integer crop, scale and pad geometry.
                // At nearest-neighbour zoom, using the ideal full-image
                // transform can be one displayed pixel out because FFmpeg
                // first rounds the crop origin to a source-pixel boundary.
                const double zoom=mZoomPercent/100.0;
                const int targetPreWidth=swap?mHeight:mWidth;
                const int targetPreHeight=swap?mWidth:mHeight;
                const int cropWidth=std::max(1,std::min(width,qCeil(targetPreWidth/zoom)));
                const int cropHeight=std::max(1,std::min(height,qCeil(targetPreHeight/zoom)));
                const int cropX=qRound((width-cropWidth)/2.0+mPanSourcePixels.x());
                const int cropY=qRound((height-cropHeight)/2.0+mPanSourcePixels.y());
                const int scaledWidth=std::min(targetPreWidth,qRound(cropWidth*zoom));
                const int scaledHeight=std::min(targetPreHeight,qRound(cropHeight*zoom));
                const int outputWidth=swap?scaledHeight:scaledWidth;
                const int outputHeight=swap?scaledWidth:scaledHeight;
                const int outputX=(mVideo->width()-outputWidth)/2;
                const int outputY=(mVideo->height()-outputHeight)/2;
                const int px=static_cast<int>(std::floor(position.x()))-outputX;
                const int py=static_cast<int>(std::floor(position.y()))-outputY;
                if(px>=0&&px<outputWidth&&py>=0&&py<outputHeight){
                    const int orientedCropWidth=swap?cropHeight:cropWidth;
                    const int orientedCropHeight=swap?cropWidth:cropHeight;
                    const int dx=std::min(orientedCropWidth-1,
                        px*orientedCropWidth/outputWidth);
                    const int dy=std::min(orientedCropHeight-1,
                        py*orientedCropHeight/outputHeight);
                    x=dx;y=dy;
                    if(clip.orientation==90){x=dy;y=cropHeight-1-dx;}
                    else if(clip.orientation==180){x=cropWidth-1-dx;y=cropHeight-1-dy;}
                    else if(clip.orientation==270){x=cropWidth-1-dy;y=dx;}
                    x+=cropX;y+=cropY;
                }
            }else{
                int outputWidth=0,outputHeight=0,outputX=0,outputY=0;
                if(mZoomPercent>0.0){
                    const double zoom=mZoomPercent/100.0;
                    QPointF displayPan=mPanSourcePixels;
                    if(clip.orientation==90)
                        displayPan=QPointF(-displayPan.y(),displayPan.x());
                    else if(clip.orientation==180)displayPan=-displayPan;
                    else if(clip.orientation==270)
                        displayPan=QPointF(displayPan.y(),-displayPan.x());
                    outputWidth=std::max(1,qRound(displayWidth*zoom));
                    outputHeight=std::max(1,qRound(displayHeight*zoom));
                    outputX=(mVideo->width()-outputWidth)/2-
                        qRound(displayPan.x()*zoom);
                    outputY=(mVideo->height()-outputHeight)/2-
                        qRound(displayPan.y()*zoom);
                }else{
                    const QPointF scale=surfaceScaleForZoom(mZoomPercent);
                    outputWidth=std::max(1,qRound(displayWidth*scale.x()));
                    outputHeight=std::max(1,qRound(displayHeight*scale.y()));
                    outputX=(mVideo->width()-outputWidth)/2;
                    outputY=(mVideo->height()-outputHeight)/2;
                }
                const int px=static_cast<int>(std::floor(position.x()))-outputX;
                const int py=static_cast<int>(std::floor(position.y()))-outputY;
                if(px>=0&&px<outputWidth&&py>=0&&py<outputHeight){
                    const int dx=std::min(displayWidth-1,px*displayWidth/outputWidth);
                    const int dy=std::min(displayHeight-1,py*displayHeight/outputHeight);
                    x=dx;y=dy;
                    if(clip.orientation==90){x=dy;y=height-1-dx;}
                    else if(clip.orientation==180){x=width-1-dx;y=height-1-dy;}
                    else if(clip.orientation==270){x=width-1-dy;y=dx;}
                }
            }
            if(x>=0&&y>=0){
                const int nativeWidth=clip.nativeWidth>0?clip.nativeWidth:width;
                const int nativeHeight=clip.nativeHeight>0?clip.nativeHeight:height;
                x=std::clamp(static_cast<int>(std::floor((x+0.5)*nativeWidth/width)),0,nativeWidth-1);
                y=std::clamp(static_cast<int>(std::floor((y+0.5)*nativeHeight/height)),0,nativeHeight-1);
                const auto& raw=mPresentedRawFrame;
                if(mHistogramEnabled&&raw.samples&&raw.channels==1&&raw.orientation==0&&
                   raw.width==static_cast<uint32_t>(nativeWidth)&&
                   raw.height==static_cast<uint32_t>(nativeHeight)&&
                   clip.width>0&&clip.height>0&&
                   raw.width%static_cast<uint32_t>(clip.width)==0&&
                   raw.height%static_cast<uint32_t>(clip.height)==0){
                    const int phaseBlock=std::max(1,raw.cfaSize/2);
                    const int scaleX=static_cast<int>(raw.width/clip.width);
                    const int scaleY=static_cast<int>(raw.height/clip.height);
                    // Even-stride proxy decimation keeps Bayer parity by
                    // selecting alternating phases inside each source block.
                    // Sampling every block center would select one color only.
                    if(scaleX>phaseBlock&&scaleX%phaseBlock==0&&
                       (scaleX/phaseBlock)%2==0){
                        const int proxyX=x/scaleX;
                        x=std::min(nativeWidth-1,proxyX*scaleX+
                            (proxyX&1)*phaseBlock);
                    }
                    if(scaleY>phaseBlock&&scaleY%phaseBlock==0&&
                       (scaleY/phaseBlock)%2==0){
                        const int proxyY=y/scaleY;
                        y=std::min(nativeHeight-1,proxyY*scaleY+
                            (proxyY&1)*phaseBlock);
                    }
                }
                next=QPoint(x,y);
            }
        }
    }
    if(next==mMouseSourcePosition)return;
    mMouseSourcePosition=next;updateTitle();
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
    mPlayPause->setIcon(mediaIcon(mPaused?QStyle::SP_MediaPlay:QStyle::SP_MediaPause));
    const bool pillVisible=mVolumePanel&&mVolumePanel->isVisible();
    const int audioStyle=pillVisible?2:mAudioMode==AudioMode::RealTimeAudio?1:0;
    if(mAudioButton->property("galleryAudioStyle").toInt()!=audioStyle){
        mAudioButton->setProperty("galleryAudioStyle",audioStyle);
        mAudioButton->setStyleSheet(audioStyle==2
            ?"QPushButton{color:white;background:transparent;border:0;border-radius:19px} QPushButton:hover{background:transparent;}"
            :audioStyle==1
            ?"QPushButton{color:white;background:rgba(0,0,0,145);border:0;border-radius:19px} QPushButton:hover{background:rgba(0,0,0,145);}"
            :"QPushButton{color:white;background:rgba(0,0,0,145);border:0;border-radius:19px} QPushButton:hover{background:rgba(45,45,45,185);}");
    }
    mAudioButton->setIcon(mediaIcon(
        mAudioMode==AudioMode::UncappedMuted?QStyle::SP_MediaSeekForward:
        mAudioMode==AudioMode::RealTimeAudio?QStyle::SP_MediaVolume:
        QStyle::SP_MediaVolumeMuted));
    mAudioButton->setToolTip(mAudioMode==AudioMode::UncappedMuted
        ?tr("Uncapped, muted — click for capped playback"):
        mAudioMode==AudioMode::RealTimeAudio
        ?tr("Real-time audio — click for uncapped, muted playback"):
        tr("Capped, muted — click for real-time audio"));
    if(mFullscreenButton){
#ifdef Q_OS_ANDROID
        mFullscreenButton->setIcon(closeGalleryIcon());
#else
        mFullscreenButton->setIcon(fullscreenIcon(isFullScreen()));
#endif
    }
    updateFpsIndicator();
}
void ClipPlayerDialog::setOverlayVisible(bool visible){
    if(mPerformanceOverlayPinned){
        mOverlayTimer.stop();
        mOverlayAnimation->stop();
        mOverlayOpacity->setOpacity(1.0);
        return;
    }
    if(!visible&&mHistogramExpanded)return;
    mOverlay->setAttribute(Qt::WA_TransparentForMouseEvents,!visible);
#ifdef Q_OS_ANDROID
    updateAndroidOverlayMask();
#endif
    mOverlayAnimation->stop();mOverlayAnimation->setStartValue(mOverlayOpacity->opacity());
    mOverlayAnimation->setEndValue(visible?1.0:0.0);mOverlayAnimation->start();
    if(nativeVulkanPresenter()){
        mNativeOverlayCaptureTimer.setInterval(nativeOverlayIntervalMs);
        captureNativeOverlay();
    }
    if(!visible)QToolTip::hideText();
}
#ifdef Q_OS_ANDROID
void ClipPlayerDialog::updateAndroidOverlayMask(){
    if(!mOverlay)return;
    if(!mHistogramExpanded)mHistogram->setFixedSize(
        std::min(560,std::max(1,mOverlay->width()-20)),height()>width()?190:84);
    QRegion controls;
    for(auto* child:mOverlay->findChildren<QWidget*>(QString(),Qt::FindDirectChildrenOnly)){
        if(child->isHidden()||child->testAttribute(Qt::WA_TransparentForMouseEvents))
            continue;
        controls+=child->geometry().adjusted(-4,-4,4,4);
    }
    mOverlay->setMask(controls);
}
#endif
void ClipPlayerDialog::revealOverlay(){
    if(mPerformanceOverlayPinned)return;
    const bool alreadyFadingIn=mOverlayAnimation->state()==QAbstractAnimation::Running&&
        mOverlayAnimation->endValue().toDouble()>0.5;
    if(!alreadyFadingIn&&(mOverlayOpacity->opacity()<0.999||
                          mOverlayAnimation->state()==QAbstractAnimation::Running))
        setOverlayVisible(true);
    if(mHistogramExpanded||pointerOverThumbnailRow())mOverlayTimer.stop();
    else mOverlayTimer.start();
}
void ClipPlayerDialog::updateDisplayedImage(){
    // The old surface remains the interactive fallback while a decoder
    // refresh is pending. Suppressing it here made all wheel/drag input appear
    // ignored until the replacement frame arrived, especially during play.
    if(mLastPresentedImage.isNull()&&!mLastPresentedGpuBuffer)return;
#ifdef Q_OS_ANDROID
    const bool fastTouchPreview=mTouchActive;
#else
    constexpr bool fastTouchPreview=false;
#endif
#ifndef Q_OS_ANDROID
    if (mGpuVideo && mGpuVideo->native() && !mGpuVideo->usable()) {
        spdlog::warn("Gallery native Vulkan presenter failed; retrying through fallback presentation");
        mGpuVideo->disableNative();
        mNativeOverlayCaptureTimer.stop();
        mLastPresentedGpuBuffer.reset();
        // Restart delivery as host frames. Existing Qt Quick/Metal and label
        // presenters remain available if native Vulkan fails after startup.
        QTimer::singleShot(0, this, [this] { reloadCurrentClip(); });
    }
#endif
    auto presentPixmap=[this](const QPixmap& pixmap){
        mVideo->setPixmap(pixmap);
    };
    // During a clip change the old GPU image remains the visible surface.
    // Geometry and presenter selection already describe the incoming clip;
    // applying either to the retained frame would expose the fallback label.
    if(mWaitingForFirstFrame&&mGpuVideo&&mGpuVideo->native()&&
       hasPresentedFrame())return;
    if(mGpuVideo&&mGpuVideo->native()&&mGpuVideo->isVisible()&&
       !mDirectSequencePresentation&&!mLastPresentedImage.isNull()&&
       !mLastImageIsSource){
        // FFmpeg already rendered this frame at viewport size. Keep the
        // native surface mapped when crossing from a direct clip to this
        // playback path; unmapping exposes the driver's blue clear frame.
        mGpuVideo->setFrame(mLastPresentedImage,QRectF(mGpuVideo->rect()),
                            !fastTouchPreview);
        return;
    }
    if((mDirectSequencePresentation || mLastPresentedGpuBuffer ||
        (mGpuVideo&&mGpuVideo->native()&&mGpuVideo->isVisible()&&
         mLastImageIsSource))&&mGpuVideo&&mGpuVideo->usable()){
        const int frameWidth=mLastPresentedGpuBuffer
            ?int((mLastPresentedGpuBuffer->orientation==90||
                  mLastPresentedGpuBuffer->orientation==270)
                  ?mLastPresentedGpuBuffer->height:mLastPresentedGpuBuffer->width)
            :mLastPresentedImage.width();
        const int frameHeight=mLastPresentedGpuBuffer
            ?int((mLastPresentedGpuBuffer->orientation==90||
                  mLastPresentedGpuBuffer->orientation==270)
                  ?mLastPresentedGpuBuffer->width:mLastPresentedGpuBuffer->height)
            :mLastPresentedImage.height();
        QRectF target;
        bool smooth=true;
        if(mZoomPercent<=0.0){
            const double scale=fitScale();
            const QSizeF size(frameWidth*scale,frameHeight*scale);
            target=QRectF(QPointF((mGpuVideo->width()-size.width())/2.0,
                                  (mGpuVideo->height()-size.height())/2.0),size);
        }else{
            const double zoom=mZoomPercent/100.0;
            QPointF displayPan=mPanSourcePixels;
            const int orientation=mIndex>=0?mClips[mIndex].orientation:-1;
            if(orientation==90)
                displayPan=QPointF(-mPanSourcePixels.y(),mPanSourcePixels.x());
            else if(orientation==180)displayPan=-mPanSourcePixels;
            else if(orientation==270)
                displayPan=QPointF(mPanSourcePixels.y(),-mPanSourcePixels.x());
            const int scaledWidth=std::max(1,qRound(frameWidth*zoom));
            const int scaledHeight=std::max(1,qRound(frameHeight*zoom));
            target=QRectF((mGpuVideo->width()-scaledWidth)/2-
                              qRound(displayPan.x()*zoom),
                          (mGpuVideo->height()-scaledHeight)/2-
                              qRound(displayPan.y()*zoom),
                          scaledWidth,scaledHeight);
            smooth=!fastTouchPreview && mZoomPercent<400.0 &&
                std::abs(zoom-std::round(zoom))>=0.0001;
        }
        if(fastTouchPreview)smooth=false;
        // The native swapchain owns these pixels. Keep Qt's window backing
        // store from submitting competing frames as controls and scrollbars change.
#ifdef Q_OS_ANDROID
        if(mGpuVideo->native())suspendNativeBackingStore(true);
#endif
        if(mLastPresentedGpuBuffer)
            mGpuVideo->setGpuFrame(mLastPresentedGpuBuffer,target,smooth,
                                  mCapturedNativeOverlay, mCapturedNativeOverlayParts);
        else mGpuVideo->setFrame(mLastPresentedImage,target,smooth);
        if(!mGpuVideo->isVisible()){
            if(mGpuVideo->native()){
                captureNativeOverlay();
            }
            mGpuVideo->show();
            mGpuVideo->raise();
            mOverlay->raise();
            mFpsLabel->raise();
        }
        return;
    }
    if(mGpuVideo&&mGpuVideo->isVisible())mGpuVideo->hide();
    suspendNativeBackingStore(false);
    if(mOverlay->graphicsEffect()!=mOverlayOpacity)mOverlay->setGraphicsEffect(mOverlayOpacity);
    mOverlayOpacity->setEnabled(true);
    mOverlay->setUpdatesEnabled(true);
    mFpsLabel->setUpdatesEnabled(true);
    if(mLastImageIsSource){
        if(mZoomPercent<=0.0){
            const double scale=fitScale();
            const QSize target(std::max(1,qRound(mLastPresentedImage.width()*scale)),
                               std::max(1,qRound(mLastPresentedImage.height()*scale)));
            presentPixmap(QPixmap::fromImage(mLastPresentedImage).scaled(target,
                Qt::KeepAspectRatio,fastTouchPreview
                    ?Qt::FastTransformation:Qt::SmoothTransformation));return;
        }
    }else{
        // Transform the latest displayed playback frame while a replacement
        // surface is pending. This avoids jumping back to the first source
        // preview frame during wheel and drag interaction.
        const QPointF desiredScale=surfaceScaleForZoom(mZoomPercent);
        const QPointF ratio(desiredScale.x()/std::max(0.0001,mLastSurfaceScale.x()),
                            desiredScale.y()/std::max(0.0001,mLastSurfaceScale.y()));
        QPointF oldDisplayPan=mLastSurfaceViewportPan;
        QPointF newDisplayPan=mZoomPercent>0.0?mPanSourcePixels:QPointF();
        const int orientation=mIndex>=0?mClips[mIndex].orientation:-1;
        auto orientPan=[orientation](const QPointF& pan){
            if(orientation==90)return QPointF(-pan.y(),pan.x());
            if(orientation==180)return -pan;
            if(orientation==270)return QPointF(pan.y(),-pan.x());
            return pan;
        };
        oldDisplayPan=orientPan(oldDisplayPan);newDisplayPan=orientPan(newDisplayPan);
        QImage canvas(mVideo->size(),QImage::Format_RGB888);canvas.fill(QColor(96,96,96));
        // Describe the retained frame in viewport pixels instead of deriving a
        // fractional crop in the frame's original coordinates. Both its size
        // and placement are integral, so QPainter performs only the intended
        // frame scaling and cannot add a changing subpixel translation phase.
        const int frameWidth=std::max(1,qRound(mLastPresentedImage.width()*ratio.x()));
        const int frameHeight=std::max(1,qRound(mLastPresentedImage.height()*ratio.y()));
        const QPointF panDelta=newDisplayPan-oldDisplayPan;
        const int frameX=(canvas.width()-frameWidth)/2-
            qRound(panDelta.x()*desiredScale.x());
        const int frameY=(canvas.height()-frameHeight)/2-
            qRound(panDelta.y()*desiredScale.y());
        const QRect frameRect(frameX,frameY,frameWidth,frameHeight);
        QPainter painter(&canvas);painter.setRenderHint(QPainter::SmoothPixmapTransform,
            !fastTouchPreview&&mZoomPercent<400.0);
        painter.drawImage(frameRect,mLastPresentedImage);
        painter.end();presentPixmap(QPixmap::fromImage(canvas));return;
    }
    if(mZoomPercent<=0.0){
        presentPixmap(QPixmap::fromImage(mLastPresentedImage).scaled(mVideo->size(),
            Qt::KeepAspectRatio,fastTouchPreview
                ?Qt::FastTransformation:Qt::SmoothTransformation));return;
    }
    const double zoom=mZoomPercent/100.0;
    QPointF displayPan=mPanSourcePixels;
    const int orientation=mIndex>=0?mClips[mIndex].orientation:-1;
    if(orientation==90)displayPan=QPointF(-mPanSourcePixels.y(),mPanSourcePixels.x());
    else if(orientation==180)displayPan=-mPanSourcePixels;
    else if(orientation==270)displayPan=QPointF(mPanSourcePixels.y(),-mPanSourcePixels.x());
    const bool nearest=mZoomPercent>=400.0||
        std::abs(mZoomPercent/100.0-std::round(mZoomPercent/100.0))<0.0001;
    QImage canvas(mVideo->size(),QImage::Format_RGB888);canvas.fill(QColor(96,96,96));
    // Stills/source previews use the same viewport-space model as retained
    // decoder surfaces. Do not quantize a moving crop in source coordinates;
    // scale the complete frame to an integer viewport extent and move that
    // extent by an integer number of viewport pixels.
    const int frameWidth=std::max(1,qRound(mLastPresentedImage.width()*zoom));
    const int frameHeight=std::max(1,qRound(mLastPresentedImage.height()*zoom));
    const int frameX=(canvas.width()-frameWidth)/2-qRound(displayPan.x()*zoom);
    const int frameY=(canvas.height()-frameHeight)/2-qRound(displayPan.y()*zoom);
    const QRect frameRect(frameX,frameY,frameWidth,frameHeight);
    QPainter painter(&canvas);
    painter.setRenderHint(QPainter::SmoothPixmapTransform,!fastTouchPreview&&!nearest);
    painter.drawImage(frameRect,mLastPresentedImage);painter.end();
    presentPixmap(QPixmap::fromImage(canvas));
}

void ClipPlayerDialog::updateFrameTimerInterval(){
    if(mPerformanceUncapped){
        mFrameTimer.setInterval(1);
        return;
    }
    mFrameTimer.setTimerType(Qt::PreciseTimer);
    const double fps=std::clamp(mClips[mIndex].fps,1.0,240.0);
    qint64 remainingNs=0;
    if(mAudioEnabled&&!mAudioStartPending&&mAudioClock.isValid()){
        const qint64 now=audioPositionNs();
        const qint64 nextFrame=static_cast<qint64>(
            std::floor(static_cast<double>(now)*fps/1.0e9))+1;
        remainingNs=static_cast<qint64>(
            std::llround(static_cast<double>(nextFrame)*1.0e9/fps))-now;
    }else if(mCadenceStartFrame>=0&&mCadenceClock.isValid()){
        const int nextFrame=mFrames.empty()
            ?mCadenceLastFrame+1:mFrames.front().sourceFrame;
        remainingNs=static_cast<qint64>(std::llround(
            static_cast<double>(nextFrame-mCadenceStartFrame)*1.0e9/fps))-
            mCadenceClock.nsecsElapsed();
    }else{
        remainingNs=static_cast<qint64>(std::llround(1.0e9/fps));
    }
    // A millisecond timer may fire before the exact deadline. The frame
    // check below uses nanoseconds and re-arms for the remaining fraction.
    const int delay=mFrames.empty()?2:std::max<qint64>(1,
        remainingNs>0 ? remainingNs/1000000 : 1);
    mFrameTimer.setInterval(delay);
}
void ClipPlayerDialog::configureAudio(){
    mAudioFadeTimer.stop();
    if(mAudioSink){mAudioSink->stop();delete mAudioSink;mAudioSink=nullptr;}
    delete mAudioBuffer;mAudioBuffer=nullptr;mAudioPcm.clear();mAudioBytesPerSecond=0;mAudioBlockAlign=1;mAudioClockBaseNs=0;mAudioClock.invalidate();
    auto& clip=mClips[mIndex];
    const auto audio=clip.audioWav;
    const bool canLoadFromSource=clip.sourceFrames>1&&
        !clip.sourceAudioChecked&&!clip.sourceFile.isEmpty();
    const bool available=audio&&!audio->empty();
    mAudioEnabled=mAudioMode==AudioMode::RealTimeAudio&&
        clip.sourceFrames>1&&(available||canLoadFromSource);
    // Keep muted clips free of audio work. For active audio, sample conversion
    // runs in a worker so the video decoder can start without waiting for it.
    if(!mAudioEnabled){
        updateButtonIcons();
        return;
    }
    if(!available){
        mAudioButton->setToolTip(tr("Preparing audio…"));
        if(!mAudioLoading)beginSourceAudioLoad();
        return;
    }
    updateButtonIcons();
    if(mAudioLoadCancelled->load())
        mAudioLoadCancelled=std::make_shared<std::atomic_bool>(false);
    mAudioLoading=true;
    const int index=mIndex,generation=mAudioLoadGeneration;
    const auto cancelled=mAudioLoadCancelled;
    auto* watcher=new QFutureWatcher<PreparedGalleryAudio>(this);
    connect(watcher,&QFutureWatcher<PreparedGalleryAudio>::finished,this,
        [this,watcher,index,generation,cancelled]{
        PreparedGalleryAudio prepared=watcher->result();watcher->deleteLater();
        if(mClosing||generation!=mAudioLoadGeneration||index!=mIndex)return;
        mAudioLoading=false;
        if(cancelled->load()){
            if(mAudioMode==AudioMode::RealTimeAudio)configureAudio();
            return;
        }
        if(prepared.sampleRate<=0||prepared.pcm.isEmpty()){
            mAudioEnabled=false;updateButtonIcons();return;
        }
        mAudioPcm=std::move(prepared.pcm);
        QAudioFormat format;format.setSampleRate(prepared.sampleRate);format.setChannelCount(2);
#if QT_VERSION >= QT_VERSION_CHECK(6,0,0)
        format.setSampleFormat(QAudioFormat::Float);
        mAudioSink=new QAudioSink(QMediaDevices::defaultAudioOutput(),format,this);
#else
        format.setCodec("audio/pcm");format.setSampleSize(32);format.setByteOrder(QAudioFormat::LittleEndian);format.setSampleType(QAudioFormat::Float);
        mAudioSink=new QAudioOutput(QAudioDeviceInfo::defaultOutputDevice(),format,this);
#endif
        mAudioSink->setVolume(mVolumePercent/100.0);
        mAudioBlockAlign=2*sizeof(float);
        mAudioBytesPerSecond=prepared.sampleRate*mAudioBlockAlign;
        mAudioBuffer=new QBuffer(&mAudioPcm,this);mAudioBuffer->open(QIODevice::ReadOnly);
        if(mAudioEnabled&&mFirstFrameReady&&!mPaused){
            mCadenceStartFrame=-1;mCadenceLastFrame=-1;mCadenceClock.invalidate();
            startAudioAt(mPositionSeconds);mAudioStartPending=false;
        }else mAudioStartPending=mAudioEnabled;
    });
    watcher->setFuture(QtConcurrent::run([audio,cancelled]{
        return prepareGalleryAudio(audio,cancelled);
    }));
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
        for(const QString& extension:{QStringLiteral("flac"),QStringLiteral("opus")}){
            const QString path=directory.absoluteFilePath("audio."+extension);
            if(!QFileInfo::exists(path)||ffmpegExecutable.isEmpty())continue;
            QProcess process;
            process.start(ffmpegExecutable,{"-hide_banner","-loglevel","error","-i",path,
                "-ac","2","-c:a","pcm_f32le","-f","wav","pipe:1"},QIODevice::ReadOnly);
            if(!process.waitForStarted(5000))continue;
            QByteArray output;QElapsedTimer timeout;timeout.start();
            while(process.state()!=QProcess::NotRunning){
                process.waitForFinished(100);output+=process.readAllStandardOutput();
                if(cancelled->load()||timeout.elapsed()>60000){process.kill();process.waitForFinished();return {};}
            }
            output+=process.readAllStandardOutput();
            if(process.exitCode()==0)return output;
        }
        return {};
    }
    if(suffix!=QStringLiteral("mov")&&suffix!=QStringLiteral("mp4")&&suffix!=QStringLiteral("mkv"))return {};
    const QString exe=ffmpegExecutable;if(exe.isEmpty())return {};
    auto extract=[&](const QString& codec){
        QProcess process;process.setProcessChannelMode(QProcess::SeparateChannels);
        process.start(exe,{QStringLiteral("-hide_banner"),QStringLiteral("-loglevel"),QStringLiteral("error"),
            QStringLiteral("-i"),source.absoluteFilePath(),QStringLiteral("-map"),QStringLiteral("0:a:0?"),
            QStringLiteral("-vn"),QStringLiteral("-ac"),QStringLiteral("2"),QStringLiteral("-c:a"),codec,
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
    QByteArray output=extract(QStringLiteral("pcm_f32le"));
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
    if(!cancelled->load()&&!supportedPcm(output))output.clear();
    return output;
}
qint64 ClipPlayerDialog::audioPositionNs()const{return mAudioClockBaseNs+(mAudioClock.isValid()?mAudioClock.nsecsElapsed():0);}
qint64 ClipPlayerDialog::audioPositionMs()const{return audioPositionNs()/1000000;}
void ClipPlayerDialog::startAudioAt(double seconds){
    if(!mAudioSink||!mAudioBuffer||mAudioBytesPerSecond<=0)return;
    mAudioFadeTimer.stop();mAudioFadeOut=false;
    mAudioSink->stop();mAudioSink->setVolume(0.0);
    qint64 byte=qBound<qint64>(0,qRound64(seconds*mAudioBytesPerSecond),mAudioPcm.size());
    byte-=byte%std::max(1,mAudioBlockAlign);
    mAudioBuffer->seek(byte);
    mAudioClockBaseNs=qRound64(double(byte)*1.0e9/mAudioBytesPerSecond);
    mAudioClock.start();mAudioSink->start(mAudioBuffer);
    mAudioFadeClock.restart();mAudioFadeTimer.start();
}
void ClipPlayerDialog::setAudioEnabled(bool enabled){
    mCadenceStartFrame=-1;mCadenceLastFrame=-1;mCadenceClock.invalidate();
    const bool clipCanHaveAudio=mIndex>=0&&mIndex<mClips.size()&&
        mClips[mIndex].sourceFrames>1&&
        (mClips[mIndex].audioWav||
         (!mClips[mIndex].sourceAudioChecked&&
          !mClips[mIndex].sourceFile.isEmpty()));
    mAudioEnabled=enabled&&clipCanHaveAudio;
    updateButtonIcons();
    if(mIndex>=0)updateFrameTimerInterval();
    if(mAudioEnabled){
        if(mAudioLoading)return;
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
        mAudioFadeTimer.stop();
        if(mAudioSink){mAudioSink->stop();mAudioClock.invalidate();mAudioStartPending=false;}
    }
}

void ClipPlayerDialog::cycleAudioMode(){
    mAudioMode=mAudioMode==AudioMode::CappedMuted?AudioMode::RealTimeAudio:
        mAudioMode==AudioMode::RealTimeAudio?AudioMode::UncappedMuted:
        AudioMode::CappedMuted;
    mPerformanceUncapped=mAudioMode==AudioMode::UncappedMuted;
    QSettings("com.motioncam","MotionCam FS").setValue("gallery/audioMode",static_cast<int>(mAudioMode));
    setVolumePanelVisible(mAudioMode==AudioMode::RealTimeAudio&&mAudioButton->underMouse());
    setAudioEnabled(mAudioMode==AudioMode::RealTimeAudio);
}

void ClipPlayerDialog::positionVolumePanel(){
    if(!mVolumePanel||!mAudioButton||!mVolumePanel->isVisible())return;
    const QPoint button=mAudioButton->mapTo(mOverlay,QPoint(0,0));
    const int height=mVolumePanel->height();
    mVolumePanel->setGeometry(button.x(),button.y()+38-height,38,height);
}

void ClipPlayerDialog::setVolumePanelVisible(bool visible){
    if(!mVolumePanel||!mVolumeAnimation)return;
    visible=visible&&mAudioMode==AudioMode::RealTimeAudio;
    if(visible)mVolumeHideTimer.stop();
    if(visible==mVolumePanelWanted&&(!visible||mVolumePanel->isVisible()))return;
    mVolumePanelWanted=visible;
    if(visible)mVuTimer.start();
    else{
        mVuTimer.stop();
        mVuLevel=0.0f;
        static_cast<VuVolumeSlider*>(mVolumeSlider)->setLevel(0.0f);
    }
    if(!visible&&!mVolumePanel->isVisible())return;
    const QPoint button=mAudioButton->mapTo(mOverlay,QPoint(0,0));
    const int bottom=button.y()+38;
    const int height=mVolumePanel->isVisible()?mVolumePanel->height():38;
    const QRect start(button.x(),bottom-height,38,height);
    const int target=visible?150:38;
    const QRect end(button.x(),bottom-target,38,target);
    mVolumeAnimation->stop();
    mVolumePanel->setGeometry(start);
    if(visible){
        mVolumePanel->show();mVolumePanel->stackUnder(mAudioButton);
        updateButtonIcons();
    }
    mVolumeAnimation->setStartValue(start);
    mVolumeAnimation->setEndValue(end);
    mVolumeAnimation->start();
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
        SubmittedFrame submitted=std::move(mSubmittedFrames.front());mSubmittedFrames.pop_front();
        mFrames.push_back({view.copy(),submitted.sourceFrame,std::move(submitted.raw)});
        mBytesOffset+=mFrameBytes;
    }
    if(mBytesOffset==mBytes.size()){mBytes.clear();mBytesOffset=0;}
    else if(mBytesOffset>=qint64(mFrameBytes)*2){mBytes.remove(0,mBytesOffset);mBytesOffset=0;}
    if(mFrameStepInProgress&&!mFrames.empty())
        QTimer::singleShot(0,this,&ClipPlayerDialog::showNextFrame);
}
void ClipPlayerDialog::showNextFrame(){
    const auto showStarted=std::chrono::steady_clock::now();
    consumeOutput();
    if(!mPaused&&mAudioEnabled&&!mAudioStartPending&&mAudioClock.isValid()&&
       mIndex>=0&&mClips[mIndex].autoAdvance&&mThumbnailScroll&&
       !mThumbnailScroll->isVisible()&&mFrames.empty()&&mBytes.isEmpty()&&
       audioPositionNs()>=qint64(double(mClips[mIndex].sourceFrames)*1.0e9/
           std::max(1.0,mClips[mIndex].fps))&&
       (mDirectFramesFinished||mDecoder.state()==QProcess::NotRunning)){
        advance();return;
    }
    if(mAudioEnabled&&!mAudioStartPending&&mAudioClock.isValid()&&mIndex>=0){
        const double fps=std::max(1.0,mClips[mIndex].fps);
        const int audioFrame=std::clamp(static_cast<int>(std::floor(
            static_cast<double>(audioPositionNs())*fps/1.0e9)),
            0,mPosition->maximum());
        mPlaybackTarget->store(audioFrame);
        // A frame that has already finished rendering is always useful: show
        // it immediately when late, or wait until its boundary when early.
        // Dropping is restricted to the pre-materialization scheduler; if
        // rendering itself exceeds one frame period, discarding here would
        // reject every completed frame forever.
        if(mFrames.empty()||mFrames.front().sourceFrame>audioFrame)return;
    }
    // Keep the first processed preview visible while filling a small cushion.
    // With one queued frame, any decode longer than a frame period becomes a
    // visible stall even when average throughput comfortably exceeds playback.
    // Paused seeks, frame stepping, audio and short clips must not wait here.
    if(mDirectSequencePresentation&&!mPerformanceUncapped&&!mAudioEnabled&&
       !mPaused&&!mFrameStepInProgress&&mCadenceStartFrame<0&&
       !mDirectFramesFinished&&mFrames.size()<kCappedPlaybackBufferFrames)return;
    if(!mFrames.empty()&&!mPerformanceUncapped&&!mAudioClock.isValid()&&
       !mPaused&&!mFrameStepInProgress&&mCadenceStartFrame>=0&&
       mCadenceClock.isValid()){
        const double fps=std::max(1.0,mClips[mIndex].fps);
        const qint64 dueNs=static_cast<qint64>(std::llround(
            static_cast<double>(mFrames.front().sourceFrame-mCadenceStartFrame)*
            1.0e9/fps));
        if(mCadenceClock.nsecsElapsed()<dueNs)return;
    }
    if(!mFrames.empty()){
        const QSize viewport=mVideo->size().expandedTo(QSize(640,360));
        const int viewportWidth=std::max(2,viewport.width()&~1);
        const int viewportHeight=std::max(2,viewport.height()&~1);
        if(mClips[mIndex].isSequence&&!mDirectSequencePresentation&&
           (mWidth!=viewportWidth||mHeight!=viewportHeight)){
            // This frame belongs to a decoder created for an intermediate
            // resize geometry. Never promote it to the visible surface. The
            // pending restart will render the retained frame at final size.
            // Keep the input-ID and byte queues intact until openClip() stops
            // and resets the complete pipeline. Clearing only materialized
            // frames cannot relabel output still buffered inside FFmpeg.
            mFrames.clear();
            mWaitingForFirstFrame=hasPresentedFrame();
            if(!mSurfaceUpdateTimer.isActive()){
                mSurfaceUpdateTimer.setInterval(300);mSurfaceUpdateTimer.start();
            }
            return;
        }
        const QueuedFrame queued=std::move(mFrames.front());mFrames.pop_front();
        if(!mClips[mIndex].isSequence){
            auto& clip=mClips[mIndex];
            const int frameWidth=queued.gpuBuffer
                ?int(queued.gpuBuffer->width):queued.image.width();
            const int frameHeight=queued.gpuBuffer
                ?int(queued.gpuBuffer->height):queued.image.height();
            const bool dimensionsChanged=clip.width!=frameWidth||
                clip.height!=frameHeight;
            clip.width=frameWidth;clip.height=frameHeight;
            const bool rawSwap=queued.raw.orientation==90||queued.raw.orientation==270;
            clip.nativeWidth=queued.raw.width>0
                ?(rawSwap?queued.raw.height:queued.raw.width):clip.width;
            clip.nativeHeight=queued.raw.height>0
                ?(rawSwap?queued.raw.width:queued.raw.height):clip.height;
            if(dimensionsChanged){
                mPanSourcePixels=QPointF();
                const double minimum=fitScale()*100.0;
                if(mZoomPercent>0.0&&mZoomPercent<=minimum*1.05){
                    mZoomPercent=0.0;mZoomAnimationTarget=0.0;
                    mZoomAnimationTimer.stop();mZoomAnimationStartupDelay=false;
                }else if(mZoomAnimationTarget>0.0&&
                         mZoomAnimationTarget<=minimum*1.05){
                    mZoomAnimationTarget=0.0;
                }
                if(mZoomPercent>0.0)mZoomPercent=std::max(mZoomPercent,minimum);
                if(mZoomAnimationTarget>0.0)
                    mZoomAnimationTarget=std::max(mZoomAnimationTarget,minimum);
                mRequestedZoomPercent=mZoomPercent>0.0
                    ?std::max(mRequestedZoomPercent,minimum):minimum;
            }
        }
        mLastPresentedImage=queued.image;
        mLastPresentedGpuBuffer=queued.gpuBuffer;
        mPresentedRawFrame=queued.raw;
        updateHistogram();
        mWaitingForFirstFrame=false;
        mLastImageIsSource=!mClips[mIndex].isSequence||mDirectSequencePresentation;
        mLastSurfaceScale=mDecoderSurfaceScale;
        mLastSurfacePan=mDecoderSurfacePan;
        mLastSurfaceViewportPan=mDecoderSurfaceViewportPan;
        const double fps=std::max(1.0,mClips[mIndex].fps);
        const int presentedFrame=std::clamp(queued.sourceFrame,0,mPosition->maximum());
        setDroppedCursorVisible(false);
        const auto& duplicateMask=mClips[mIndex].duplicateFrames;
        setDuplicateCursorVisible(duplicateMask&&presentedFrame<static_cast<int>(duplicateMask->size())&&
            (*duplicateMask)[presentedFrame]);
        if(!mPosition->isSliderDown())mPosition->setValue(presentedFrame);
        setCurrentThumbnailFrame(presentedFrame);
        updateTitle();
        if(nativeVulkanPresenter()&&(mNativeOverlayCapturePending||
           mThumbnailScroll->isVisible()||!mNativeOverlayCaptureTimer.isActive())){
            // Commit the UI and video in one presenter update. The row then
            // follows the frame being shown instead of racing a later capture.
            captureNativeOverlay(false);
            if(!mNativeOverlayCaptureTimer.isActive())mNativeOverlayCaptureTimer.start();
        }
        const auto displayUiStarted=std::chrono::steady_clock::now();
        updateDisplayedImage();
        if(qEnvironmentVariableIsSet("MOTIONCAM_GALLERY_PERF_PROFILE")&&
           mDirectSequencePresentation)
            spdlog::info("GALLERY_PERF event=gallery_display_ui elapsed_ms={:.3f}",
                std::chrono::duration<double,std::milli>(
                    std::chrono::steady_clock::now()-displayUiStarted).count());
        if(!mFirstFrameReady){
            mFirstFrameReady=true;
            emit firstFramePresented(currentMountId());
            if(mPaused&&mThumbnailScroll&&mThumbnailScroll->isVisible())
                requestThumbnailBackfill();
            if(mAudioEnabled&&mAudioStartPending&&!mPaused){startAudioAt(mPositionSeconds);mAudioStartPending=false;}
        }
        if(!mPaused){
            if(!mPresentationClock.isValid())mPresentationClock.start();
            if(!mPerformanceUncapped&&!mAudioClock.isValid()&&mCadenceStartFrame<0){
                mCadenceStartFrame=presentedFrame;
                mCadenceClock.start();
            }
            mCadenceLastFrame=presentedFrame;
            const qint64 now=mPresentationClock.nsecsElapsed();
            if(mLastPresentationNs>=0&&now>mLastPresentationNs){
                mPresentationIntervals.push_back(now-mLastPresentationNs);
                if(mPresentationIntervals.size()>20)mPresentationIntervals.pop_front();
            }
            mLastPresentationNs=now;
            updateFpsIndicator();
        }
        emit framePresented(currentMountId(),presentedFrame);
        if(qEnvironmentVariableIsSet("MOTIONCAM_GALLERY_PERF_PROFILE")&&
           mDirectSequencePresentation)
            spdlog::info("GALLERY_PERF event=gallery_show_ui elapsed_ms={:.3f}",
                std::chrono::duration<double,std::milli>(
                    std::chrono::steady_clock::now()-showStarted).count());
        mPositionSeconds=(presentedFrame+1)/fps;
        completeFrameStep();
        // A paused surface refresh still needs to consume and present its
        // first frame. Stop only after that replacement has reached the UI.
        if(mPaused)mFrameTimer.stop();
    }else{
        if(!mPlaybackFailed&&
           (mClips[mIndex].isSequence&&!mDirectSequencePresentation
                ?mDecoder.state()==QProcess::NotRunning:
            mDirectFramesFinished)&&mBytes.isEmpty()){
            mFrameTimer.stop();if(mClips[mIndex].autoAdvance&&!mThumbnailScroll->isVisible())advance();else {mPaused=true;updateButtonIcons();updateFpsIndicator();}
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
#ifdef Q_OS_ANDROID
    updateTitle();
#else
    QToolTip::showText(mPosition->mapToGlobal(QPoint(x,0)),framePositionText(frame),mPosition);
#endif
}
void ClipPlayerDialog::seekToFrame(int frame){
#ifdef Q_OS_ANDROID
    mThumbnailFollowPlayback=true;
    QScroller::scroller(mThumbnailScroll->viewport())->stop();
#endif
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
    if(watched==mAudioButton){
        if(event->type()==QEvent::Enter&&mAudioMode==AudioMode::RealTimeAudio)
            setVolumePanelVisible(true);
        else if(event->type()==QEvent::Leave&&mVolumePanel->isVisible())
            mVolumeHideTimer.start();
        else if(event->type()==QEvent::Move||event->type()==QEvent::Resize)
            positionVolumePanel();
    }else if(watched==mVolumePanel||watched==mVolumeSlider){
        if(event->type()==QEvent::Enter)mVolumeHideTimer.stop();
        else if(event->type()==QEvent::Leave&&
                !mAudioButton->underMouse()&&!mVolumePanel->underMouse()&&
                !mVolumeSlider->underMouse())
            mVolumeHideTimer.start();
    }
    const auto* watchedWidget=qobject_cast<QWidget*>(watched);
    const bool nativeVideoSurface=watched&&
        watched->property("galleryVulkanSurface").toBool();
    if(mPanning&&(event->type()==QEvent::MouseButtonRelease||
                  event->type()==QEvent::MouseMove||
                  event->type()==QEvent::Enter)){
        const auto* mouse=event->type()==QEvent::Enter?nullptr:
            static_cast<QMouseEvent*>(event);
        if((mouse&&event->type()==QEvent::MouseButtonRelease&&
            mouse->button()==Qt::LeftButton)||
           (mouse&&event->type()==QEvent::MouseMove&&
            !(mouse->buttons()&Qt::LeftButton))||
           (!mouse&&!(QApplication::mouseButtons()&Qt::LeftButton))){
            mPanning=false;
            unsetCursor();
            if(!hasReusableSourceFrame()){
                mSurfaceUpdateTimer.setInterval(16);
                mSurfaceUpdateTimer.start();
            }
            if(nativeVideoSurface||watched==mVideo||watched==mGpuVideo){
                event->accept();
                return true;
            }
        }
    }
    // A release outside the gallery may never reach the slider. The first
    // subsequent mouse event then tells us whether its button is still down.
    if(mSeeking&&(event->type()==QEvent::MouseButtonRelease||
                  event->type()==QEvent::MouseMove||
                  event->type()==QEvent::Enter)){
        const auto* mouse=event->type()==QEvent::Enter?nullptr:
            static_cast<QMouseEvent*>(event);
        if((mouse&&event->type()==QEvent::MouseButtonRelease&&
            mouse->button()==Qt::LeftButton)||
           (mouse&&event->type()==QEvent::MouseMove&&
            !(mouse->buttons()&Qt::LeftButton))||
           (!mouse&&!(QApplication::mouseButtons()&Qt::LeftButton))){
            QPoint global=QCursor::pos();
            if(mouse){
#if QT_VERSION >= QT_VERSION_CHECK(6,0,0)
                global=mouse->globalPosition().toPoint();
#else
                global=mouse->globalPos();
#endif
            }
            updateSeekPosition(mPosition->mapFromGlobal(global).x());
            mSeeking=false;
            mPosition->setSliderDown(false);
            mNativeMouseTarget=nullptr;
            seekToFrame(mPosition->value());
            if(watched==mPosition){event->accept();return true;}
        }
    }
    if(nativeVideoSurface&&event->type()==QEvent::Leave&&mNativeHoverTarget){
        auto* old=mNativeHoverTarget.data();mNativeHoverTarget=nullptr;
        old->setAttribute(Qt::WA_UnderMouse,false);
        QEvent leave(QEvent::Leave);QCoreApplication::sendEvent(old,&leave);
    }
    if(nativeVideoSurface&&mOverlay&&mOverlay->isVisible()&&
       (event->type()==QEvent::MouseMove||
        event->type()==QEvent::MouseButtonPress||
        event->type()==QEvent::MouseButtonRelease||
        event->type()==QEvent::MouseButtonDblClick)){
        if(event->type()==QEvent::MouseMove||
           event->type()==QEvent::MouseButtonPress)
            revealOverlay();
        auto* mouse=static_cast<QMouseEvent*>(event);
#if QT_VERSION >= QT_VERSION_CHECK(6,0,0)
        const QPointF global=mouse->globalPosition();
#else
        const QPointF global=mouse->globalPos();
#endif
        QWidget* hit=mOverlay->childAt(mOverlay->mapFromGlobal(global.toPoint()));
        if(hit==mTitle)hit=nullptr;
        if((event->type()==QEvent::MouseMove||event->type()==QEvent::MouseButtonPress)&&
           hit!=mNativeHoverTarget){
            if(auto* old=mNativeHoverTarget.data()){
                old->setAttribute(Qt::WA_UnderMouse,false);
                QEvent leave(QEvent::Leave);QCoreApplication::sendEvent(old,&leave);
            }
            mNativeHoverTarget=hit;
            if(hit){
                hit->setAttribute(Qt::WA_UnderMouse,true);
#if QT_VERSION >= QT_VERSION_CHECK(6,0,0)
                QEnterEvent enter(hit->mapFromGlobal(global.toPoint()),
                    mOverlay->mapFromGlobal(global.toPoint()),global);
#else
                QEvent enter(QEvent::Enter);
#endif
                QCoreApplication::sendEvent(hit,&enter);
            }
        }
        QWidget* target=mNativeMouseTarget?mNativeMouseTarget.data():hit;
        if(target){
#if QT_VERSION >= QT_VERSION_CHECK(6,0,0)
            QMouseEvent forwarded(event->type(),target->mapFromGlobal(global),
                global,mouse->button(),mouse->buttons(),mouse->modifiers());
#else
            QMouseEvent forwarded(event->type(),target->mapFromGlobal(global.toPoint()),
                global.toPoint(),mouse->button(),mouse->buttons(),mouse->modifiers());
#endif
            if(event->type()==QEvent::MouseButtonPress)mNativeMouseTarget=target;
            QCoreApplication::sendEvent(target,&forwarded);
            if(event->type()==QEvent::MouseButtonRelease)mNativeMouseTarget=nullptr;
            event->accept();
            return true;
        }
    }
    const bool belongsToPlayer=nativeVideoSurface||
        (watchedWidget&&(watchedWidget->window()==this||
                         watchedWidget==this||isAncestorOf(watchedWidget)));
#ifdef Q_OS_ANDROID
    if(event->type()==QEvent::KeyPress && isVisible() &&
       (belongsToPlayer||watchedWidget==parentWidget())){
        auto* key=static_cast<QKeyEvent*>(event);
        if(key->key()==Qt::Key_Back||key->key()==Qt::Key_Escape){
            spdlog::info("Gallery navigation close: key={} thumbnail_touch={} thumbnail_drag={}",
                key->key(),mNativeThumbnailTouch,mNativeThumbnailDrag);
            close();event->accept();return true;
        }
    }
    const bool touchSurface=watched==mVideo||watched==mGpuVideo||nativeVideoSurface;
    if(belongsToPlayer&&touchSurface&&
       (event->type()==QEvent::TouchBegin||event->type()==QEvent::TouchUpdate||
        event->type()==QEvent::TouchEnd||event->type()==QEvent::TouchCancel)){
        auto* touch=static_cast<QTouchEvent*>(event);
        const auto& points=touch->points();
        if(touchSurface){
            if(event->type()==QEvent::TouchBegin&&points.size()==1){
                revealOverlay();
                const QPointF global=points[0].globalPosition();
                auto* hit=mOverlay->childAt(mOverlay->mapFromGlobal(global.toPoint()));
                const bool thumbnailHit=mThumbnailScroll&&
                    mThumbnailScroll->isVisible()&&
                    mThumbnailScroll->rect().contains(
                        mThumbnailScroll->mapFromGlobal(global.toPoint()));
                if(thumbnailHit){
                    mNativeTouchTarget=hit?hit:mThumbnailScroll;
                    mNativeTouchPosition=global;
                    mNativeTouchStart=global;
                    auto* scroller=QScroller::scroller(mThumbnailScroll->viewport());
                    const bool stoppingScroll=scroller->state()==QScroller::Scrolling;
                    scroller->handleInput(
                        QScroller::InputPress, mThumbnailScroll->viewport()->mapFromGlobal(global),
                        touch->timestamp());
                    mNativeThumbnailTouch=true;
                    mNativeThumbnailDrag=stoppingScroll;
                    event->accept();return true;
                }
                if(hit&&(qobject_cast<QAbstractButton*>(hit)||hit==mPosition)){
                    mNativeTouchTarget=hit;
                    mNativeTouchPosition=global;
                    QMouseEvent press(QEvent::MouseButtonPress,hit->mapFromGlobal(global),
                        global,Qt::LeftButton,Qt::LeftButton,Qt::NoModifier);
                    QCoreApplication::sendEvent(hit,&press);
                    event->accept();return true;
                }
            }
            if(mNativeTouchTarget){
                if(!points.isEmpty())mNativeTouchPosition=points[0].globalPosition();
                if(mNativeThumbnailTouch&&event->type()==QEvent::TouchUpdate){
                    const QPointF delta=mNativeTouchPosition-mNativeTouchStart;
                    if(!mNativeThumbnailDrag&&std::abs(delta.x())>12.0&&
                       std::abs(delta.x())>std::abs(delta.y())*1.2)
                        mNativeThumbnailDrag=true;
                    auto* scroller=QScroller::scroller(mThumbnailScroll->viewport());
                    scroller->handleInput(QScroller::InputMove,
                        mThumbnailScroll->viewport()->mapFromGlobal(mNativeTouchPosition),
                        touch->timestamp());
                    mNativeThumbnailDrag=mNativeThumbnailDrag||scroller->state()==QScroller::Dragging;
                    if(mNativeThumbnailDrag)mThumbnailFollowPlayback=false;
                    event->accept();return true;
                }
                if(event->type()==QEvent::TouchEnd||event->type()==QEvent::TouchCancel){
                    if(mNativeThumbnailTouch){
                        auto* scroller=QScroller::scroller(mThumbnailScroll->viewport());
                        scroller->handleInput(QScroller::InputRelease,
                            mThumbnailScroll->viewport()->mapFromGlobal(mNativeTouchPosition),
                            touch->timestamp());
                        if(event->type()==QEvent::TouchCancel)scroller->stop();
                    }
                    auto* target=mNativeTouchTarget.data();
                    mNativeTouchTarget=nullptr;
                    const bool cancelled=event->type()==QEvent::TouchCancel;
                    if(cancelled){
                        if(auto* button=qobject_cast<QAbstractButton*>(target))button->setDown(false);
                        if(auto* slider=qobject_cast<QAbstractSlider*>(target))slider->setSliderDown(false);
                    }
                    if(target&&mNativeThumbnailTouch&&!mNativeThumbnailDrag&&!cancelled){
                        QMouseEvent press(QEvent::MouseButtonPress,
                            target->mapFromGlobal(mNativeTouchPosition),
                            mNativeTouchPosition,Qt::LeftButton,Qt::LeftButton,
                            Qt::NoModifier);
                        QCoreApplication::sendEvent(target,&press);
                    }
                    if(target&&!mNativeThumbnailDrag&&!cancelled){
                        QMouseEvent release(QEvent::MouseButtonRelease,
                            target->mapFromGlobal(mNativeTouchPosition),mNativeTouchPosition,
                            Qt::LeftButton,Qt::NoButton,Qt::NoModifier);
                        QCoreApplication::sendEvent(target,&release);
                    }
                    mNativeThumbnailTouch=false;
                    mNativeThumbnailDrag=false;
                }
                event->accept();return true;
            }
        }
        if(event->type()==QEvent::TouchEnd||event->type()==QEvent::TouchCancel||
           points.isEmpty()){
            const bool wasTouching=mTouchActive;
            mTouchActive=false;
            mTouchPanUpdateTimer.stop();
            if(wasTouching)updateDisplayedImage();
            if(!hasReusableSourceFrame()&&mZoomPercent>0.0){
                mSurfaceUpdateTimer.setInterval(16);mSurfaceUpdateTimer.start();
            }
            event->accept();return true;
        }
        const QPointF center=points.size()>1
            ?(points[0].globalPosition()+points[1].globalPosition())/2.0
            :points[0].globalPosition();
        const double distance=points.size()>1
            ?QLineF(points[0].globalPosition(),points[1].globalPosition()).length():0.0;
        if(!mTouchActive||event->type()==QEvent::TouchBegin||
           mTouchPointCount!=std::min<int>(2,points.size())){
            mTouchActive=true;
            mTouchPointCount=std::min<int>(2,points.size());
            mLastTouchCenter=center;
            mLastTouchDistance=distance;
            revealOverlay();
            event->accept();return true;
        }
        if(points.size()>1&&distance>0.0&&mLastTouchDistance>0.0){
            changeZoom(std::log(distance/mLastTouchDistance)/std::log(1.08));
        }
        if(mZoomPercent>0.0&&mIndex>=0&&mIndex<mClips.size()&&
           QLineF(center,mLastTouchCenter).length()>0.5){
            const QPointF displayDelta=-(center-mLastTouchCenter)/(mZoomPercent/100.0);
            QPointF sourceDelta=displayDelta;
            const int orientation=mClips[mIndex].orientation;
            if(orientation==90)sourceDelta=QPointF(displayDelta.y(),-displayDelta.x());
            else if(orientation==180)sourceDelta=-displayDelta;
            else if(orientation==270)sourceDelta=QPointF(-displayDelta.y(),displayDelta.x());
            mPanSourcePixels+=sourceDelta;
            clampPanToZoom();
            if(!mZoomAnimationTimer.isActive()&&!mTouchPanUpdateTimer.isActive())
                mTouchPanUpdateTimer.start();
        }
        mLastTouchCenter=center;
        mLastTouchDistance=distance;
        revealOverlay();
        event->accept();return true;
    }
    if(watchedWidget&&watchedWidget->parentWidget()==mOverlay&&
       (event->type()==QEvent::Show||event->type()==QEvent::Hide||
        event->type()==QEvent::Move||event->type()==QEvent::Resize))
        QTimer::singleShot(0,this,[this]{updateAndroidOverlayMask();});
#endif
    if(mFpsLabel&&watched==mFpsLabel->parentWidget()&&event->type()==QEvent::Resize)
        updateFpsIndicator();
    if(belongsToPlayer&&event->type()==QEvent::Leave&&
       mThumbnailScroll&&(watchedWidget==mThumbnailScroll||
       mThumbnailScroll->isAncestorOf(watchedWidget))&&!pointerOverThumbnailRow())
        mOverlayTimer.start();
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
    if(belongsToPlayer&&event->type()==QEvent::MouseMove&&!mPanning&&
       !mZoomAnimationTimer.isActive()){
        const auto* mouse=static_cast<QMouseEvent*>(event);
#if QT_VERSION >= QT_VERSION_CHECK(6,0,0)
        updateMouseSourcePosition(mouse->globalPosition());
#else
        updateMouseSourcePosition(mouse->globalPos());
#endif
    }
    if(belongsToPlayer&&event->type()==QEvent::Wheel){
        revealOverlay();
        const auto* wheel=static_cast<QWheelEvent*>(event);
        if(mHistogramEnabled && mHistogram->isVisible() &&
           mHistogram->rect().contains(mHistogram->mapFromGlobal(QCursor::pos()))) {
            const double steps=!wheel->pixelDelta().isNull() ?
                wheel->pixelDelta().y()/40.0 : wheel->angleDelta().y()/120.0;
            static_cast<RawHistogram*>(mHistogram)->scrollDistribution(steps);
            event->accept();return true;
        }
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
    const bool panSurface=nativeVideoSurface||watched==mVideo||
        watched==mGpuVideo||watched==mOverlay;
    if(belongsToPlayer&&panSurface&&event->type()==QEvent::MouseButtonDblClick){
        auto* mouse=static_cast<QMouseEvent*>(event);
        if(mouse->button()==Qt::LeftButton){
            mPanning=false;unsetCursor();
#ifndef Q_OS_ANDROID
            mFullscreenButton->click();
#endif
            return true;
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
            mMouseSourcePosition=QPoint(-1,-1);updateTitle();
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
            if(!hasReusableSourceFrame()){
                mSurfaceUpdateTimer.setInterval(16);mSurfaceUpdateTimer.start();
            }
            return true;
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
#ifndef Q_OS_ANDROID
        else QToolTip::showText(mPosition->mapToGlobal(QPoint(x,0)),
            framePositionText(frameAtSliderPosition(x)),mPosition);
#endif
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
QImage ClipPlayerDialog::rgb48Image(const QByteArray& frame,int width,int height,
        std::shared_ptr<const std::vector<uint8_t>> backing,bool preOriented,
        std::shared_ptr<const uint8_t> pixelsOwner)const{
    if(width<=0||height<=0)return {};
    const qint64 pixels=qint64(width)*height;
    const int stride=frame.size()==pixels*3?3:frame.size()>=pixels*6?6:0;
    if(!stride)return {};
    const int orientation=mIndex>=0?mClips[mIndex].orientation:-1;
    const bool swap=orientation==90||orientation==270;
    if(pixelsOwner && stride==3 &&
       (preOriented || (orientation!=90&&orientation!=180&&orientation!=270))){
        auto* owner=new std::shared_ptr<const uint8_t>(std::move(pixelsOwner));
        const int imageWidth=preOriented&&swap?height:width;
        const int imageHeight=preOriented&&swap?width:height;
        QImage view(reinterpret_cast<const uchar*>((*owner).get()),
            imageWidth,imageHeight,imageWidth*3,QImage::Format_RGB888,
            [](void* context){delete static_cast<std::shared_ptr<const uint8_t>*>(context);},
            owner);
        if(!view.isNull()){
#ifdef Q_OS_ANDROID
            // Android's QWidget backing store expects Qt-owned, aligned scanlines
            // when it converts RGB888 during a high-rate gallery paint.
            return view.copy();
#else
            return view;
#endif
        }
        delete owner;
    }
    if(backing && stride==3 && backing->size()==size_t(pixels*3) &&
       (preOriented || (orientation!=90&&orientation!=180&&orientation!=270))){
        auto* owner=new std::shared_ptr<const std::vector<uint8_t>>(std::move(backing));
        const int imageWidth=preOriented&&swap?height:width;
        const int imageHeight=preOriented&&swap?width:height;
        QImage view(reinterpret_cast<const uchar*>((*owner)->data()),
            imageWidth,imageHeight,imageWidth*3,QImage::Format_RGB888,
            [](void* context){delete static_cast<std::shared_ptr<const std::vector<uint8_t>>*>(context);},
            owner);
        if(!view.isNull()){
#ifdef Q_OS_ANDROID
            return view.copy();
#else
            return view;
#endif
        }
        delete owner;
    }
    QImage image(swap?height:width,swap?width:height,QImage::Format_RGB888);
    if(image.isNull())return {};
    const auto* source=reinterpret_cast<const uchar*>(frame.constData());
    if(orientation!=90&&orientation!=180&&orientation!=270){
        for(int y=0;y<height;++y){
            auto* destination=image.scanLine(y);
            const auto* row=source+qsizetype(y)*width*stride;
            for(int x=0;x<width;++x){
                const auto* input=row+qsizetype(x)*stride;
                auto* output=destination+qsizetype(x)*3;
                output[0]=input[stride==3?0:1];
                output[1]=input[stride==3?1:3];
                output[2]=input[stride==3?2:5];
            }
        }
    }else{
        // Give each worker complete destination rows. For 90/270-degree
        // frames, splitting source rows makes all workers scatter writes into
        // the same output rows and turns a 24-bit image into severe cache-line
        // contention. Each output row has a single writer here.
        auto* outputBits=image.bits();
        const qsizetype outputStride=image.bytesPerLine();
        const int outputWidth=image.width(),outputHeight=image.height();
        auto convertRows=[&](int first,int last){
        for(int outputY=first;outputY<last;++outputY){
            auto* row=outputBits+qsizetype(outputY)*outputStride;
            for(int outputX=0;outputX<outputWidth;++outputX){
                int sourceX=outputX,sourceY=outputY;
                if(orientation==90){sourceX=outputY;sourceY=height-1-outputX;}
                else if(orientation==180){sourceX=width-1-outputX;sourceY=height-1-outputY;}
                else {sourceX=width-1-outputY;sourceY=outputX;}
                const auto* input=source+
                    (qsizetype(sourceY)*width+sourceX)*stride;
                auto* output=row+qsizetype(outputX)*3;
                output[0]=input[stride==3?0:1];
                output[1]=input[stride==3?1:3];
                output[2]=input[stride==3?2:5];
            }
        }
        };
        const unsigned workers=width*qsizetype(height)>=4*1024*1024
            ?std::min(4u,std::max(1u,std::thread::hardware_concurrency())):1u;
        std::vector<std::thread> threads;
        threads.reserve(workers-1);
        try{
            for(unsigned worker=1;worker<workers;++worker)
                threads.emplace_back(convertRows,int(qsizetype(outputHeight)*worker/workers),
                    int(qsizetype(outputHeight)*(worker+1)/workers));
        }catch(...){
            for(auto& thread:threads)thread.join();
            throw;
        }
        convertRows(0,int(outputHeight/workers));
        for(auto& thread:threads)thread.join();
    }
    return image;
}
QImage ClipPlayerDialog::rgb48Thumbnail(const QByteArray& frame,int width,int height)const{
    const int orientation=mIndex>=0?mClips[mIndex].orientation:-1;
    return makeRgb48Thumbnail(reinterpret_cast<const uint8_t*>(frame.constData()),
                              frame.size(),width,height,orientation,devicePixelRatioF());
}
QImage ClipPlayerDialog::makeRgb48Thumbnail(const uint8_t* source,qsizetype bytes,
                                             int width,int height,int orientation,qreal pixelRatio){
    if(!source||width<=0||height<=0)return {};
    const qint64 pixels=qint64(width)*height;
    const int stride=bytes==pixels*3?3:bytes>=pixels*6?6:0;
    if(!stride)return {};
    const bool swap=orientation==90||orientation==270;
    const int orientedWidth=swap?height:width,orientedHeight=swap?width:height;
    const QSize outputSize=frameThumbnailSize(width,height,orientation)*pixelRatio;
    const int outputWidth=outputSize.width(),outputHeight=outputSize.height();
    QImage image(outputWidth,outputHeight,QImage::Format_RGB888);
    for(int y=0;y<outputHeight;++y){
        auto* destination=image.scanLine(y);
        const int orientedY=std::min(orientedHeight-1,y*orientedHeight/outputHeight);
        for(int x=0;x<outputWidth;++x){
            const int orientedX=std::min(orientedWidth-1,x*orientedWidth/outputWidth);
            int sourceX=orientedX,sourceY=orientedY;
            if(orientation==90){sourceX=orientedY;sourceY=height-1-orientedX;}
            else if(orientation==180){sourceX=width-1-orientedX;sourceY=height-1-orientedY;}
            else if(orientation==270){sourceX=width-1-orientedY;sourceY=orientedX;}
            const qsizetype input=(qsizetype(sourceY)*width+sourceX)*stride;
            destination[x*3]=source[input+(stride==3?0:1)];
            destination[x*3+1]=source[input+(stride==3?1:3)];
            destination[x*3+2]=source[input+(stride==3?2:5)];
        }
    }
    return image;
}
ClipPlayerDialog::FramePushResult ClipPlayerDialog::pushRgb48Frame(
        const QByteArray& frame,int width,int height,
        std::shared_ptr<const std::vector<uint16_t>> rawSamples,
        int rawWidth,int rawHeight,int rawChannels,float rawBlack,float rawWhite,
        int rawCfaSize,std::array<uint8_t,4> rawCfaPhase,
        std::array<float,4> rawBlackLevels,int rawOrientation,
        std::shared_ptr<const std::vector<uint8_t>> frameBacking,
        bool framePreOriented,std::shared_ptr<const uint8_t> framePixels,
        std::shared_ptr<motioncam::VulkanGalleryGpuBuffer> gpuBuffer,
        int sourceFrameIndex){
    // Stills are presented directly from the processed RGB48 callback. Feeding
    // them through a timed FFmpeg graph would display the same image twice and
    // can apply the video surface's output aspect ratio to the second copy.
    if(mIndex>=0&&mClips[mIndex].sourceFrames<=1)return FramePushResult::Accepted;
    if(mClosing||mPlaybackFailed)return FramePushResult::Stopped;
    if(mClips[mIndex].isSequence&&!mDirectSequencePresentation&&
       mDecoder.state()!=QProcess::Running)
        return FramePushResult::Retry;
    const int incomingSourceFrame=sourceFrameIndex>=0
        ?sourceFrameIndex:mIncomingFrame->load();
    if(mAudioEnabled&&mAudioClock.isValid()&&incomingSourceFrame>mNextInputFrame){
        mNextInputFrame=incomingSourceFrame;
        mPositionSeconds=mNextInputFrame/std::max(1.0,mClips[mIndex].fps);
    }
    if(mClips[mIndex].isSequence&&incomingSourceFrame<mNextInputFrame)
        return FramePushResult::Accepted;
    if(mClips[mIndex].isSequence&&incomingSourceFrame>mNextInputFrame)
        mNextInputFrame=incomingSourceFrame;
    // Bound frames accepted by the complete transform pipeline, including
    // data FFmpeg has already consumed from QProcess but not returned yet.
    // bytesToWrite() alone cannot observe that internal backlog.
    // Capped playback retains a bounded cushion to absorb decode jitter.
    // Uncapped playback keeps its shallow queue and maximum throughput.
    const size_t maximumPending=mDirectSequencePresentation
        ? (mPerformanceUncapped ? 2u : kCappedPlaybackBufferFrames) : (rawSamples ? 2u : 6u);
    if(mSubmittedFrames.size()+mFrames.size()>=maximumPending)
        return FramePushResult::Retry;
    if(!mClips[mIndex].isSequence||mDirectSequencePresentation){
        const auto convertStarted=std::chrono::steady_clock::now();
        const QImage image=gpuBuffer ? QImage{} : rgb48Image(frame,width,height,
            std::move(frameBacking),framePreOriented,std::move(framePixels));
        if(qEnvironmentVariableIsSet("MOTIONCAM_GALLERY_PERF_PROFILE") &&
           mDirectSequencePresentation)
            spdlog::info("GALLERY_PERF event=direct_present_convert source={}x{} ms={:.3f}",
                width,height,std::chrono::duration<double,std::milli>(
                    std::chrono::steady_clock::now()-convertStarted).count());
        if(image.isNull()&&!gpuBuffer)return FramePushResult::Stopped;
        const int sourceFrame=mClips[mIndex].isSequence
            ?incomingSourceFrame:std::max(mNextInputFrame,incomingSourceFrame);
        mFrames.push_back({image,sourceFrame,
            {std::move(rawSamples),rawWidth,rawHeight,rawChannels,rawBlack,rawWhite,
                rawBlackLevels,rawCfaSize,rawCfaPhase,rawOrientation},
            std::move(gpuBuffer)});
        mNextInputFrame=sourceFrame+1;
        return FramePushResult::Accepted;
    }
    if(mDecoder.bytesToWrite()>qint64(mInputFrameBytes)*2)return FramePushResult::Retry;
    if(mClips[mIndex].isSequence){
        if(width!=mClips[mIndex].width||height!=mClips[mIndex].height||
           frame.size()!=qint64(width)*height*(mInputBitsPerChannel==8?3:6))
            return FramePushResult::Stopped;
        if(mDecoder.write(frame)!=frame.size())return FramePushResult::Retry;
        mSubmittedFrames.push_back({mNextInputFrame,
            {std::move(rawSamples),rawWidth,rawHeight,rawChannels,rawBlack,rawWhite,
                rawBlackLevels,rawCfaSize,rawCfaPhase,rawOrientation}});
        ++mNextInputFrame;return FramePushResult::Accepted;
    }
    return FramePushResult::Stopped;
}
void ClipPlayerDialog::presentRgb48Frame(const QByteArray& frame,int width,int height,
        std::shared_ptr<const std::vector<uint16_t>> rawSamples,
        int rawWidth,int rawHeight,int rawChannels,float rawBlack,float rawWhite,
        int rawCfaSize,std::array<uint8_t,4> rawCfaPhase,
        std::array<float,4> rawBlackLevels,int rawOrientation,
        std::shared_ptr<const std::vector<uint8_t>> frameBacking,
        bool framePreOriented,std::shared_ptr<const uint8_t> framePixels,
        std::shared_ptr<motioncam::VulkanGalleryGpuBuffer> gpuBuffer){
    const int inputBits=gpuBuffer||frame.size()==qint64(width)*height*3?8:16;
    const bool formatChanged=mInputBitsPerChannel!=inputBits;
    if(formatChanged)mInputBitsPerChannel=inputBits;
    const bool dimensionsChanged=mIndex>=0&&
        (mClips[mIndex].width!=width||mClips[mIndex].height!=height);
    if(mClips[mIndex].isSequence&&
       (formatChanged||(dimensionsChanged&&mClips[mIndex].sourceFrames>1))){
        // Draft and other preprocessing modes may change the RGB staging
        // dimensions relative to FileInfo. Configure rawvideo from the first
        // actual frame so the producer can never retry a permanent mismatch.
        stopDecoder();
        adoptRenderedDimensions(width,height);
        startDecoder();
    }else if(dimensionsChanged&&
             (mClips[mIndex].isSequence||mClips[mIndex].sourceFrames<=1))
        adoptRenderedDimensions(width,height);
    // Video must become visible only after FFmpeg has produced the final
    // viewport-sized surface. Showing this source preview during a seek or
    // resize releases the held frame too early and briefly scales it using
    // geometry that does not belong to the active decoder.
    if(mIndex>=0&&mClips[mIndex].sourceFrames>1)return;
    if(mIndex>=0&&!mClips[mIndex].isSequence&&rawWidth>0&&rawHeight>0){
        const bool swap=rawOrientation==90||rawOrientation==270;
        mClips[mIndex].nativeWidth=swap?rawHeight:rawWidth;
        mClips[mIndex].nativeHeight=swap?rawWidth:rawHeight;
    }
    const QImage image=gpuBuffer ? QImage{} : rgb48Image(frame,width,height,
        std::move(frameBacking),framePreOriented,std::move(framePixels));
    if(image.isNull()&&!gpuBuffer)return;
    mLastPresentedImage=image;mLastImageIsSource=true;
    mLastPresentedGpuBuffer=std::move(gpuBuffer);
    mPresentedRawFrame={std::move(rawSamples),rawWidth,rawHeight,rawChannels,
        rawBlack,rawWhite,rawBlackLevels,rawCfaSize,rawCfaPhase,rawOrientation};
    updateHistogram();
    mWaitingForFirstFrame=false;
    setDroppedCursorVisible(false);
    setDuplicateCursorVisible(false);
    updateDisplayedImage();
    updateTitle();
    setCurrentThumbnailFrame(0);
    mFirstFrameReady=true;
    emit firstFramePresented(currentMountId());
    completeFrameStep();
    if(mAudioEnabled&&mAudioStartPending&&!mPaused){
        startAudioAt(mPositionSeconds);mAudioStartPending=false;
    }
    spdlog::info("Gallery presented processed RGB48 frame {}x{}",width,height);
}
void ClipPlayerDialog::finishRgb48Frames(){
    if(mIndex<0||mClips[mIndex].sourceFrames<=1)return;
    if(!mClips[mIndex].isSequence||mDirectSequencePresentation){
        mDirectFramesFinished=true;return;
    }
    if(mDecoder.state()==QProcess::Running)mDecoder.closeWriteChannel();
}
void ClipPlayerDialog::failRgb48Frames(const QString& error){mPlaybackFailed=true;mFrameTimer.stop();mTitle->setText(tr("Frame rendering failed: %1").arg(error));mDecoder.kill();completeFrameStep();}
void ClipPlayerDialog::advance(){if(!mClips.isEmpty())openClip((mIndex+1)%mClips.size());}
#ifdef Q_OS_ANDROID
void ClipPlayerDialog::setNavigationReturnSnapshot(std::function<QPixmap()> returnSnapshot){
    mNavigationReturnSnapshot=std::move(returnSnapshot);
}
#endif
void ClipPlayerDialog::closeImmediately(){
#ifdef Q_OS_ANDROID
    mNavigationCloseReady=true;
#endif
    close();
}
void ClipPlayerDialog::closeEvent(QCloseEvent* e){
#ifdef Q_OS_ANDROID
    if(mNavigationReturnSnapshot&&!mNavigationCloseReady){
        e->ignore();
        if(!mNavigationClosing){
            mNavigationClosing=true;
            const bool fading=motioncam::crossfadePage(mNavigationReturnSnapshot(),0.0f,1.0f,true);
            if(!fading){mNavigationCloseReady=true;QTimer::singleShot(0,this,&QWidget::close);}
            else{
                auto* completion=new QTimer(this);
                completion->setInterval(16);
                QElapsedTimer started;started.start();
                connect(completion,&QTimer::timeout,this,[this,completion,started]{
                    if(!mClosing&&!motioncam::pageCrossfadeFinished()&&started.elapsed()<2000)return;
                    completion->stop();completion->deleteLater();
                    if(!mClosing){mNavigationCloseReady=true;close();}
                });
                completion->start();
            }
        }
        return;
    }
#endif
    spdlog::info("Gallery closed: mount={} frame={} paused={} failed={}",
        currentMountId(),mCurrentThumbnailSource,mPaused,mPlaybackFailed);
    mClosing=true;
    releaseGpuFrames();
    emit playbackClosed();
    stopDecoder();
    QDialog::closeEvent(e);
    suspendNativeBackingStore(false);
#ifdef Q_OS_ANDROID
    QTimer::singleShot(0,qApp,[]{motioncam::finishPageCrossfade();});
#endif
}
void ClipPlayerDialog::resizeEvent(QResizeEvent* e){
    QDialog::resizeEvent(e);
#ifdef Q_OS_ANDROID
    QTimer::singleShot(0,this,[this]{updateAndroidOverlayMask();});
#endif
    positionVolumePanel();
    if(mThumbnailScroll){
#ifdef Q_OS_ANDROID
        if(mThumbnailScroll->isVisible())
#else
        if(mThumbnailScroll->isVisible()&&mThumbnailScroll->height()!=thumbnailStripHeight())
#endif
            rebuildThumbnailStrip();
        else mThumbnailScroll->viewport()->update();
    }
    // A clip switch keeps the previous frame visible until the replacement
    // arrives. Do not redraw that retained image using the new clip's size,
    // orientation, or presenter while its first frame is still pending.
    if(mWaitingForFirstFrame&&!mFirstFrameReady)return;
    // Keep the wheel accumulator aligned with the new fit geometry.
    if(mZoomPercent<=0.0&&mIndex>=0)
        mRequestedZoomPercent=fitScale()*100.0;
    if(hasReusableSourceFrame()){
        if(hasPresentedFrame())updateDisplayedImage();
        return;
    }
    // Never redraw the old frame into the new widget geometry here. Doing so
    // stretches an old-resolution surface during live resize/fullscreen
    // transitions. Also stop presentation immediately: otherwise a frame
    // already buffered by the old decoder can arrive after this event and be
    // composed into the new geometry. Keep the existing pixmap centered at
    // its original pixel size until openClip() starts the replacement surface.
    if(isVisible()&&mIndex>=0&&hasPresentedFrame()){
        mWaitingForFirstFrame=hasPresentedFrame();
        // An established decoder must stop presenting old-size buffered
        // frames immediately. A decoder whose first frame is still pending
        // remains ticking so the size guard in showNextFrame() can reject it.
        if(mFirstFrameReady)mFrameTimer.stop();
        mSurfaceUpdateTimer.setInterval(300);mSurfaceUpdateTimer.start();
    }
}
void ClipPlayerDialog::keyPressEvent(QKeyEvent* e){
#ifdef __ANDROID__
    if(e->key()==Qt::Key_Escape||e->key()==Qt::Key_Back){close();e->accept();return;}
#else
    if(e->key()==Qt::Key_Escape&&isFullScreen()){showNormal();updateButtonIcons();e->accept();return;}
    if(e->key()==Qt::Key_F){mFullscreenButton->click();e->accept();return;}
#endif
    if(e->key()==Qt::Key_Space){mPlayPause->click();e->accept();return;}
    if((e->key()==Qt::Key_Right||e->key()==Qt::Key_Left)&&mIndex>=0){
        stepFrame(e->key()==Qt::Key_Right?1:-1);
        e->accept();return;
    }
    QDialog::keyPressEvent(e);
}
