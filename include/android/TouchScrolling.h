#pragma once

#include <QAbstractScrollArea>
#include "WidgetPaintSignature.h"
#include <QGraphicsEffect>
#include <QPointer>
#include <cmath>
#include <QTimer>
#include <deque>
#include <QElapsedTimer>
#include <QDebug>
#include <QApplication>
#include <QWindow>
#include <QScrollArea>
#include <QPainter>
#include <QPaintEvent>
#include <algorithm>
#include <QScrollBar>
#include <QScrollEvent>
#include <QScroller>


namespace motioncam {

// Cache whole control groups as opaque rasters, avoiding repeated background
// fills as well as text painting. The shared signature keeps live state fresh.
class ScrollGroupRaster final : public QGraphicsEffect {
public:
    ScrollGroupRaster(QWidget* widget,qint64 maximumBytes)
        :QGraphicsEffect(widget),mWidget(widget),mMaximumBytes(maximumBytes){}
protected:
    void draw(QPainter* painter) override {
        const auto signature=widgetPaintSignature(*mWidget);
        const qreal ratio=painter->device()->devicePixelRatioF();
        const QSizeF size=sourceBoundingRect(Qt::LogicalCoordinates).size();
        const qint64 bytes=qint64(std::ceil(size.width()*ratio))*qint64(std::ceil(size.height()*ratio))*4;
        if(bytes>mMaximumBytes){mImage={};drawSource(painter);return;}
        if(mImage.isNull()||signature!=mSignature||ratio!=mImage.devicePixelRatio()){
            mImage={};
            const auto source=sourcePixmap(Qt::LogicalCoordinates,&mOffset,NoPad);
            if(source.isNull()){drawSource(painter);return;}
            QImage opaque(source.size(),QImage::Format_RGB32);
            if(opaque.isNull()){drawSource(painter);return;}
            opaque.setDevicePixelRatio(ratio);
            opaque.fill(mWidget->palette().color(QPalette::Window));
            {QPainter capture(&opaque);capture.drawPixmap(0,0,source);}
            mImage=QPixmap::fromImage(std::move(opaque));mSignature=signature;
        }
        if(mImage.isNull()){drawSource(painter);return;}
        painter->setCompositionMode(QPainter::CompositionMode_Source);
        painter->drawPixmap(mOffset,mImage);
    }
private:
    QWidget* mWidget;
    QPixmap mImage;
    QPoint mOffset;
    quint64 mSignature=0;
    qint64 mMaximumBytes;
};

// One gesture owns both axes, including the independently scrolling settings
// column under its starting point. Inner viewports must not grab the gesture.
class TouchScrolling final : public QObject {
public:
    explicit TouchScrolling(QAbstractScrollArea* area,
                            const QList<QAbstractScrollArea*>& columns = {},
                            bool grabGesture = true)
        : QObject(area), mArea(area), mColumns(columns) {
        area->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        area->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        auto* viewport = area->viewport();
        if(qobject_cast<QScrollArea*>(area)){
            mOpaqueViewports.append(viewport);
            for(auto* column:columns)mOpaqueViewports.append(column->viewport());
            for(auto* widget:mOpaqueViewports){
                widget->ensurePolished();
                widget->setAttribute(Qt::WA_OpaquePaintEvent);
            }
        }
        mScroller = QScroller::scroller(viewport);
        qApp->installEventFilter(this);
        auto properties = mScroller->scrollerProperties();
        properties.setScrollMetric(QScrollerProperties::FrameRate, QScrollerProperties::Fps60);
        properties.setScrollMetric(QScrollerProperties::AxisLockThreshold, 0.0);
        properties.setScrollMetric(QScrollerProperties::DragStartDistance, 0.001);
        properties.setScrollMetric(QScrollerProperties::HorizontalOvershootPolicy,
                                   QScrollerProperties::OvershootAlwaysOff);
        properties.setScrollMetric(QScrollerProperties::VerticalOvershootPolicy,
                                   QScrollerProperties::OvershootAlwaysOff);
        properties.setScrollMetric(QScrollerProperties::DecelerationFactor, 0.1);
        mScroller->setScrollerProperties(properties);
        if (grabGesture) QScroller::grabGesture(viewport, QScroller::TouchGesture);
        mFrames.setTimerType(Qt::PreciseTimer);
        mFrames.setInterval(16);
        connect(&mFrames,&QTimer::timeout,this,[this]{
            if(!mScroller){mFrames.stop();return;}
            if(mSamples.empty())return;
            // One input interval of history fills gaps in uneven touch delivery
            // without extrapolating beyond the finger or altering release physics.
            const qint64 time=mInputClock.elapsed()-(mScroller->state()==QScroller::Inactive?0:32);
            while(mSamples.size()>1&&mSamples[1].time<=time)mSamples.pop_front();
            QPointF position=mSamples.front().position;
            if(mSamples.size()>1){
                const auto& next=mSamples[1];
                const qreal fraction=std::clamp(qreal(time-mSamples.front().time)/
                    std::max<qint64>(1,next.time-mSamples.front().time),0.0,1.0);
                position+=(next.position-position)*fraction;
            }
            apply(position);
            if(mScroller->state()==QScroller::Inactive&&time>=mSamples.back().time){
                mFrames.stop();clearGroups();
            }
        });
        connect(mScroller, &QScroller::stateChanged, this, [this](QScroller::State state) {
            if(qEnvironmentVariableIsSet("MOTIONCAM_GALLERY_PERF_PROFILE")){
                if(mProfileClock.isValid()&&mProfileFrames)
                    qInfo().nospace()<<"TOUCH_SCROLL state="<<mProfileState
                        <<" duration_ms="<<mProfileClock.elapsed()<<" position_updates="<<mProfileFrames
                        <<" window_updates="<<mWindowUpdates<<" max_gap_ms="<<mMaxGap;
                mProfileClock.restart();mFrameClock.restart();
                mProfileState=state;mProfileFrames=mWindowUpdates=mMaxGap=0;
            }
            if(state==QScroller::Dragging&&qobject_cast<QScrollArea*>(mArea)){
                cacheGroups();mFrames.start();
            }
            if(state==QScroller::Pressed){
                const bool interrupted=mFrames.isActive();
                mFrames.stop();clearGroups();
                if(interrupted)mScroller->resendPrepareEvent();
            }
            if(state==QScroller::Inactive&&!mFrames.isActive())clearGroups();
        });
    }

protected:
    bool eventFilter(QObject* watched, QEvent* event) override {
        if(!mScroller)return false;
        // Android also emits mouse moves for a touch drag. Let the touch
        // gesture move the content without hover-driven style repaints.
        if(event->type()==QEvent::MouseMove&&mScroller->state()==QScroller::Dragging&&
           watched==mArea->window()->windowHandle())return true;
        if(event->type()==QEvent::UpdateRequest&&watched==mArea->window()&&mProfileClock.isValid())++mWindowUpdates;
        if(event->type()==QEvent::Paint){
            if(auto* widget=qobject_cast<QWidget*>(watched);mOpaqueViewports.contains(widget)){
                QPainter painter(widget);
                painter.setCompositionMode(QPainter::CompositionMode_Source);
                painter.fillRect(static_cast<QPaintEvent*>(event)->rect(),widget->palette().window());
            }
        }
        if(watched!=mArea->viewport())return false;
        if (event->type() == QEvent::ScrollPrepare) {
            auto* prepare = static_cast<QScrollPrepareEvent*>(event);
            for(auto* widget:mOpaqueViewports)widget->setAttribute(Qt::WA_OpaquePaintEvent);
            mVertical = mArea->verticalScrollBar();
            const QPoint global = mArea->viewport()->mapToGlobal(prepare->startPos().toPoint());
            for (auto* column : mColumns) {
                if (column->rect().contains(column->mapFromGlobal(global))) {
                    mVertical = column->verticalScrollBar();
                    break;
                }
            }
            auto* horizontal = mArea->horizontalScrollBar();
            prepare->setViewportSize(mArea->viewport()->size());
            prepare->setContentPosRange(QRectF(horizontal->minimum(), mVertical->minimum(),
                horizontal->maximum() - horizontal->minimum(),
                mVertical->maximum() - mVertical->minimum()));
            mTarget = QPointF(horizontal->value(), mVertical->value());
            mInputClock.start();mSamples.clear();mSamples.push_back({0,mTarget});
            prepare->setContentPos(mTarget);
            prepare->accept();
            return true;
        }
        if (event->type() == QEvent::Scroll) {
            mTarget = static_cast<QScrollEvent*>(event)->contentPos();
            if(mFrames.isActive()){
                const qint64 time=mInputClock.elapsed();
                if(!mSamples.empty()&&mSamples.back().time==time)mSamples.back().position=mTarget;
                else mSamples.push_back({time,mTarget});
                while(mSamples.size()>16)mSamples.pop_front();
            }else apply(mTarget);
            event->accept();
            return true;
        }
        if (event->type() == QEvent::Hide) {
            mScroller->stop();mFrames.stop();clearGroups();
        }
        return false;
    }

private:
    void clearGroups(){
        for(const auto& group:mCachedGroups)if(group)group->setGraphicsEffect(nullptr);
        mCachedGroups.clear();
    }
    void cacheGroups(){
        QList<QWidget*> groups;
        if(mColumns.isEmpty()){
            if(auto* scroll=qobject_cast<QScrollArea*>(mArea))groups.append(scroll->widget());
        }else{
            // Give the column being dragged first use of the bounded budget.
            for(auto* column:mColumns)if(column->verticalScrollBar()==mVertical)
                if(auto* scroll=qobject_cast<QScrollArea*>(column))groups.append(scroll->widget());
            for(auto* column:mColumns)if(column->verticalScrollBar()!=mVertical)
                if(auto* scroll=qobject_cast<QScrollArea*>(column))groups.append(scroll->widget());
        }
        const QRect visible(mArea->viewport()->mapToGlobal(QPoint()),mArea->viewport()->size());
        qint64 bytes=0;
        for(int i=0;i<groups.size();++i){
            auto* group=groups[i];
            if(!group||group->graphicsEffect()||!group->isVisible()||
               !visible.intersects(QRect(group->mapToGlobal(QPoint()),group->size())))continue;
            const qreal ratio=group->devicePixelRatioF();
            const qint64 cost=qint64(std::ceil(group->width()*ratio))*qint64(std::ceil(group->height()*ratio))*4;
            if(cost>32*1024*1024){
                groups.append(group->findChildren<QWidget*>(QString(),Qt::FindDirectChildrenOnly));
                continue;
            }
            if(cost<=0||bytes+cost>32*1024*1024)continue;
            group->setGraphicsEffect(new ScrollGroupRaster(group,cost));
            mCachedGroups.append(group);bytes+=cost;
        }
    }
    void apply(const QPointF& position) {
        if(mArea->horizontalScrollBar()->value()==qRound(position.x())&&
           (!mVertical||mVertical->value()==qRound(position.y())))return;
        if(mProfileClock.isValid()){
            ++mProfileFrames;
            mMaxGap=std::max(mMaxGap,int(mFrameClock.restart()));
        }
        mArea->horizontalScrollBar()->setValue(qRound(position.x()));
        if (mVertical) mVertical->setValue(qRound(position.y()));
        // Touch/IME messages otherwise keep the low-priority backing-store
        // update behind another input batch. Flush this gesture's combined
        // position once, without processing more input recursively.
        if(mScroller->state()==QScroller::Dragging&&qobject_cast<QScrollArea*>(mArea))
            QCoreApplication::sendPostedEvents(mArea->window(),QEvent::UpdateRequest);
    }
    QAbstractScrollArea* mArea;
    QList<QAbstractScrollArea*> mColumns;
    QList<QWidget*> mOpaqueViewports;
    QList<QPointer<QWidget>> mCachedGroups;
    QPointer<QScrollBar> mVertical;
    QPointer<QScroller> mScroller;
    struct Sample {qint64 time;QPointF position;};
    QTimer mFrames;
    QElapsedTimer mInputClock;
    std::deque<Sample> mSamples;
    QElapsedTimer mProfileClock, mFrameClock;
    int mProfileState=QScroller::Inactive, mProfileFrames=0, mWindowUpdates=0, mMaxGap=0;
    QPointF mTarget;
};

} // namespace motioncam
