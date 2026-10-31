#pragma once

#include <QCoreApplication>
#include <QEvent>
#include <QVariant>
#include <QImage>
#include <QPixmap>
#include <QWidget>
#include <QTimer>
#include <QElapsedTimer>
#include <QPointer>
#include <functional>
#include <vector>
#ifdef Q_OS_ANDROID
#include <QJniEnvironment>
#include <QJniObject>
#include <limits>
#endif

namespace motioncam {

// Render suspended/hidden Qt pages without scheduling a backing-store repaint.
inline QPixmap grabPageSnapshot(QWidget* page){
    if(!page)return {};
    auto widgets=page->findChildren<QWidget*>();widgets.prepend(page);
    QList<QWidget*> suspended;
    for(auto* widget:widgets)if(widget->testAttribute(Qt::WA_UpdatesDisabled)){
        suspended.append(widget);widget->setAttribute(Qt::WA_UpdatesDisabled,false);
    }
    const auto image=page->grab();
    for(auto* widget:suspended)widget->setAttribute(Qt::WA_UpdatesDisabled,true);
    return image;
}

// Upload once; Android's render thread animates the full-resolution snapshot.
inline bool crossfadePage(const QPixmap& snapshot,float from=1.0f,float to=0.0f,bool hold=false){
#ifdef Q_OS_ANDROID
    const auto image=snapshot.toImage().convertToFormat(QImage::Format_ARGB32);
    if(image.isNull()||qint64(image.width())*image.height()>std::numeric_limits<jsize>::max())return false;
    const QJniObject activity=QNativeInterface::QAndroidApplication::context();
    if(!activity.isValid())return false;
    QJniEnvironment env;
    const jsize count=image.width()*image.height();
    jintArray pixels=env->NewIntArray(count);
    if(!pixels){if(env->ExceptionCheck())env->ExceptionClear();return false;}
    env->SetIntArrayRegion(pixels,0,count,reinterpret_cast<const jint*>(image.constBits()));
    if(env->ExceptionCheck()){env->ExceptionClear();env->DeleteLocalRef(pixels);return false;}
    const bool started=activity.callMethod<jboolean>("crossfadePage","([IIIFFZ)Z",
        pixels,jint(image.width()),jint(image.height()),jfloat(from),jfloat(to),jboolean(hold));
    env->DeleteLocalRef(pixels);
    return started;
#else
    Q_UNUSED(snapshot);Q_UNUSED(from);Q_UNUSED(to);Q_UNUSED(hold);return false;
#endif
}

inline bool pageCrossfadeFinished(){
#ifdef Q_OS_ANDROID
    const QJniObject activity=QNativeInterface::QAndroidApplication::context();
    return !activity.isValid()||activity.callMethod<jboolean>("pageCrossfadeFinished");
#else
    return true;
#endif
}

inline void finishPageCrossfade(){
#ifdef Q_OS_ANDROID
    const QJniObject activity=QNativeInterface::QAndroidApplication::context();
    if(activity.isValid())activity.callMethod<void>("finishPageCrossfade");
#endif
}

// Freeze the old backing store until the compositor snapshot has appeared.
// Navigation can then create native surfaces without exposing their first clear.
class PageFadeStage final : public QObject {
public:
    explicit PageFadeStage(QWidget* window):QObject(window),mWindow(window){
        setObjectName(QStringLiteral("pageFadeStage"));
        mTimer.setInterval(8);
        connect(&mTimer,&QTimer::timeout,this,[this]{
            bool ready=true;
#ifdef Q_OS_ANDROID
            const QJniObject activity=QNativeInterface::QAndroidApplication::context();
            ready=!activity.isValid()||activity.callMethod<jboolean>("pageCrossfadeReady");
#endif
            if(ready||mClock.elapsed()>200)finish(true);
        });
    }
    void begin(const QPixmap& snapshot,std::function<void()> action){
        if(action)mActions.push_back(std::move(action));
        if(mPending)return;
        ++mGeneration;
        mPending=true;mUpdatesEnabled=mWindow->updatesEnabled();
        mWindow->setUpdatesEnabled(false);
        if(!crossfadePage(snapshot,1.0f,1.0f,true)){finish(false);return;}
        mClock.start();mTimer.start();
    }
private:
    void finish(bool prepared){
        mTimer.stop();mPending=false;
        const auto generation=mGeneration;
        mWindow->setUpdatesEnabled(mUpdatesEnabled);
        auto actions=std::move(mActions);mActions.clear();
        QPointer<PageFadeStage> alive(this);
        for(auto& action:actions){
            action();
            if(!alive||mGeneration!=generation)return;
        }
        QCoreApplication::sendPostedEvents(mWindow,QEvent::UpdateRequest);
#ifdef Q_OS_ANDROID
        const QJniObject activity=QNativeInterface::QAndroidApplication::context();
        if(prepared&&activity.isValid())activity.callMethod<void>("revealPageCrossfade");
#else
        Q_UNUSED(prepared);
#endif
    }
    QWidget* mWindow;
    QTimer mTimer;
    QElapsedTimer mClock;
    std::vector<std::function<void()>> mActions;
    bool mPending=false,mUpdatesEnabled=true;
    quint64 mGeneration=0;
};

inline void stagePageCrossfade(QWidget* host,const QPixmap& snapshot,std::function<void()> action={}){
    auto* window=host->window();
    auto* stage=static_cast<PageFadeStage*>(window->findChild<QObject*>(
        QStringLiteral("pageFadeStage"),Qt::FindDirectChildrenOnly));
    if(!stage)stage=new PageFadeStage(window);
    stage->begin(snapshot,std::move(action));
}

class PageCrossfade final : public QObject {
public:
    explicit PageCrossfade(QWidget* page):QObject(page),mPage(page){page->installEventFilter(this);}
    static void show(QWidget* page){
        if(!page->isVisible())stagePageCrossfade(page->parentWidget(),grabPageSnapshot(page->parentWidget()));
        page->show();page->raise();
    }
protected:
    bool eventFilter(QObject*,QEvent* event) override {
        if(event->type()==QEvent::Hide&&!mPage->property("suppressNavigationFade").toBool()){
            auto* host=mPage->parentWidget();
            if(host&&host->isVisible())stagePageCrossfade(host,grabPageSnapshot(mPage));
        }
        return false;
    }
private:
    QWidget* mPage;
};

} // namespace motioncam
