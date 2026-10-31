#include "mainwindow.h"
#include "FrameTimingDialog.h"
#include "ClipPlayerDialog.h"
#include "ui_mainwindow.h"
#include "settingsdialog.h"
#include "CalibrationData.h"
#include "CameraFrameMetadata.h"
#include "CameraMetadata.h"
#include "DNGDecoder.h"
#include "GalleryColor.h"
#include "VulkanGalleryPreview.h"
#include "RawLogTransform.h"
#include "Utils.h"
#include "CpuWorkerBudget.h"
#include "VirtualFileSystemImpl.h"
#include "ArchiveImport.h"
#include "AudioWriter.h"

#include <motioncam/Decoder.hpp>

// Prevent Windows.h macros from interfering with std::max/min
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#endif

#include <QDragEnterEvent>
#include <QDebug>
#include <QDragMoveEvent>
#include <QDropEvent>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QCloseEvent>

using namespace motioncam;
#include <QMimeData>
#include <QPushButton>
#include <QAction>
#include <QMenuBar>
#include <QFileInfo>
#include <QGuiApplication>
#include <QProcess>
#include <QUrl>
#include <QPointer>
#include <QMessageBox>
#include <QMenu>
#include <QFileDialog>
#include <QFileSystemModel>
#include <QDialogButtonBox>
#include <QTreeView>
#include <QLineEdit>
#include <QInputDialog>
#include <QSettings>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QDir>
#include <QDirIterator>
#include <QSignalBlocker>
#include <QLabel>
#include <QPainter>
#include <QFrame>
#include <QProgressDialog>
#include <QProgressBar>
#ifdef __ANDROID__
#include <QJniObject>
#endif
#include <QScrollBar>
#include <QScroller>
#ifdef __ANDROID__
#include "android/TouchScrolling.h"
#include "android/PageCrossfade.h"
#endif
#include <QScrollArea>
#include <QListWidget>
#include <QSplitter>
#include <QToolButton>
#include <QStyleOptionButton>
#include <QStyle>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QThread>
#include <QUuid>
#include <QTemporaryDir>
#include <QStandardPaths>
#include <QEventLoop>
#include <QElapsedTimer>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QSaveFile>
#include <QCryptographicHash>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cmath>
#include <cstring>
#include <deque>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <QTimer>
#include <QtConcurrent>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <spdlog/spdlog.h>

#include <boost/filesystem.hpp>

extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/error.h>
}

#ifdef _WIN32
#include "win/FuseFileSystemImpl_Win.h"
#elif __APPLE__
#include "macos/FuseFileSystemImpl_MacOS.h"
#include <sys/mount.h>
#elif defined(__ANDROID__)
#include "android/ProcessingFileSystem.h"
#include "android/ExportDirectory.h"
#include <fcntl.h>
#include <unistd.h>
#elif defined(__linux__)
#include "linux/FuseFileSystemImpl_Linux.h"
#include <cerrno>
#include <sys/stat.h>
#endif

namespace {
#ifdef __ANDROID__
    class AndroidPopup final : public QWidget {
    public:
        AndroidPopup(QWidget* parent,const QString& title):QWidget(parent) {
            setObjectName(QStringLiteral("androidPopupOverlay"));
            setGeometry(parent->rect());parent->installEventFilter(this);
            setFocusPolicy(Qt::StrongFocus);
            setStyleSheet(
                "QWidget#androidPopupOverlay{background:rgba(0,0,0,180);}"
                "QFrame#androidPopupPanel{background:#19232d;border:1px solid #455364;border-radius:8px;}"
                "QFrame#androidPopupPanel QLabel{background:transparent;border:0;padding:0;color:#dfe1e2;}"
                "QFrame#androidPopupPanel QPushButton{background:#263445;border:1px solid #455364;"
                "border-radius:4px;padding:8px 12px;min-height:28px;min-width:0;color:#dfe1e2;}"
                "QFrame#androidPopupPanel QPushButton:pressed{background:#346792;}");
            auto* layout=new QVBoxLayout(this);layout->setContentsMargins(16,16,16,16);
            layout->addStretch();
            panel=new QFrame(this);panel->setObjectName(QStringLiteral("androidPopupPanel"));
            content=new QVBoxLayout(panel);content->setContentsMargins(20,20,20,20);content->setSpacing(14);
            auto* heading=new QLabel(title,panel);heading->setWordWrap(true);
            QFont font=heading->font();font.setBold(true);heading->setFont(font);content->addWidget(heading);
            layout->addWidget(panel,0,Qt::AlignHCenter);layout->addStretch();resizePanel();
        }
        void open(){resizePanel();show();raise();setFocus();}
        QFrame* panel;
        QVBoxLayout* content;
    protected:
        bool eventFilter(QObject* watched,QEvent* event) override {
            if(watched==parentWidget()&&event->type()==QEvent::Resize){
                if(property("dismissOnResize").toBool()){hide();deleteLater();}
                else {setGeometry(parentWidget()->rect());resizePanel();}
            }
            return QWidget::eventFilter(watched,event);
        }
        void mouseReleaseEvent(QMouseEvent* event) override {
            if(!panel->geometry().contains(event->pos()))deleteLater();
        }
        void keyPressEvent(QKeyEvent* event) override {
            if(event->key()==Qt::Key_Back||event->key()==Qt::Key_Escape){deleteLater();event->accept();}
            else QWidget::keyPressEvent(event);
        }
    private:
        void resizePanel(){
            panel->setFixedWidth(std::max(1,std::min(520,width()-32)));
        }
    };

    void showAndroidActionMenu(QWidget* parent,QMenu* menu,int first=0) {
        auto* popup=new AndroidPopup(parent,menu->title().remove('&'));
        popup->setProperty("dismissOnResize",true);
        popup->content->setSpacing(6);
        QVector<QPointer<QAction>> actions;
        for(auto* action:menu->actions())
            if(!action->isSeparator()&&action->isVisible())actions.push_back(action);
        const int pageSize=std::max(1,(parent->height()-180)/36);
        const int last=std::min(first+pageSize,int(actions.size()));
        const auto button=[&](const QString& text){
            auto* result=new QPushButton(text,popup->panel);
            result->setFixedHeight(30);
            result->setStyleSheet("QPushButton{padding:2px 10px;min-height:0;text-align:left;}");
            popup->content->addWidget(result);return result;
        };
        for(int row=first;row<last;++row){
            const auto action=actions[row];
            auto* choice=button(action->text().remove('&'));
            choice->setEnabled(action->isEnabled());
            if(action->menu())choice->setIcon(QIcon(QStringLiteral(":/qss_icons/dark/rc/arrow_right.png")));
            QObject::connect(choice,&QPushButton::clicked,popup,[popup,parent,action]{
                popup->hide();popup->deleteLater();
                if(!action)return;
                if(action->menu())showAndroidActionMenu(parent,action->menu());
                else QTimer::singleShot(0,action,[action]{if(action)action->trigger();});
            });
        }
        if(first>0||last<actions.size()){
            auto* pages=new QHBoxLayout();
            for(bool next:{false,true}){
                auto* control=new QPushButton(next?QObject::tr("Next page"):QObject::tr("Previous page"),popup->panel);
                control->setFixedHeight(30);
                control->setStyleSheet("QPushButton{padding:2px;min-height:0;}");
                control->setEnabled(next?last<actions.size():first>0);pages->addWidget(control);
                QObject::connect(control,&QPushButton::clicked,popup,[popup,parent,menu,first,last,pageSize,next]{
                    popup->hide();popup->deleteLater();
                    showAndroidActionMenu(parent,menu,next?last:std::max(0,first-pageSize));
                });
            }
            popup->content->addLayout(pages);
        }
        popup->open();
    }

    void showAndroidProgress(QProgressDialog& progress, QWidget* parent) {
        progress.setWindowFlags(Qt::Widget);
        progress.setWindowModality(Qt::NonModal);
        const int width=std::clamp(parent->width()-32,320,480);
        progress.setMaximumWidth(width);
        if(auto* label=progress.findChild<QLabel*>()){
            label->setWordWrap(true);
            label->setMaximumWidth(width-32);
            label->setMaximumHeight(72);
        }
        const QSize size=progress.sizeHint().expandedTo(QSize(width,112));
        progress.setGeometry(std::max(0,(parent->width()-width)/2),
                             std::max(0,(parent->height()-size.height())/2),
                             width,std::min(size.height(),parent->height()-16));
        progress.show();
        progress.raise();
    }

    class AndroidFinalizeWakeLock {
    public:
        AndroidFinalizeWakeLock() {
            const auto activity=QNativeInterface::QAndroidApplication::context();
            if(activity.isValid())activity.callMethod<void>("beginFinalizeWork");
        }
        ~AndroidFinalizeWakeLock() {
            const auto activity=QNativeInterface::QAndroidApplication::context();
            if(activity.isValid())activity.callMethod<void>("endFinalizeWork");
        }
        AndroidFinalizeWakeLock(const AndroidFinalizeWakeLock&)=delete;
        AndroidFinalizeWakeLock& operator=(const AndroidFinalizeWakeLock&)=delete;
    };

    QString androidFinalizeEntryName(const std::string& name) {
        QString display=QFileInfo(QString::fromStdString(name)).fileName();
        if(display.size()>52)
            display=display.left(24)+QChar(0x2026)+display.right(24);
        return display;
    }



#endif
    void copyFileAtomically(const QString& source, const QString& destination);

    QString clipInfoFirstLine(const FileInfo& info) {
        const int minutes = static_cast<int>(info.runtimeSeconds) / 60;
        const int seconds = static_cast<int>(info.runtimeSeconds) % 60;
        const QString runtime = QString("%1:%2").arg(minutes).arg(seconds, 2, 10, QChar('0'));
        const QString levels = QString::fromStdString(info.levelsInfo);
        int bitDepthStart = levels.lastIndexOf('b');
        while (bitDepthStart > 0 && levels.at(bitDepthStart - 1).isDigit())
            --bitDepthStart;
        const QString sourceType = QString::fromStdString(info.dataType);
        const int formatStart = sourceType.lastIndexOf(' ') + 1;
        const QString outputFormat = QString::fromStdString(info.outputPixelFormat);
        const QString inputColor = outputFormat.isEmpty() ? "white" : "#888888";
        QString result = QString(
            "<span style='color: #888888;'>Runtime: </span><span style='color: white;'>%1</span>"
            "<span style='color: #888888;'> | Resolution: </span><span style='color: white;'>%2x%3</span>"
            "<span style='color: #888888;'> | Type: %4</span><span style='color: %5;'>%6</span>")
            .arg(runtime)
            .arg(info.width)
            .arg(info.height)
            .arg(sourceType.left(formatStart).toHtmlEscaped())
            .arg(inputColor)
            .arg(sourceType.mid(formatStart).toHtmlEscaped());
        if (!outputFormat.isEmpty())
            result += QString("<span style='color: #888888;'> -> </span>"
                              "<span style='color: white;'>%1</span>")
                .arg(outputFormat.toHtmlEscaped());
        result += QString("<span style='color: #888888;'> %1</span>")
            .arg(levels.left(bitDepthStart).toHtmlEscaped());
        if (bitDepthStart >= 0)
            result += QString("<span style='color: white;'>%1</span>")
                .arg(levels.mid(bitDepthStart).toHtmlEscaped());
        return result;
    }

    void writeFinalizedCfaBadPixelOptions(const QString& temporaryDirectory,
                                          const QString& finalDirectory,
                                          const QString& sourceSidecar,
                                          bool perFrame = false) {
        if (!QFileInfo::exists(sourceSidecar)) return;
        QDirIterator frames(temporaryDirectory, {"*.dng", "*.DNG"}, QDir::Files,
                            QDirIterator::Subdirectories);
        if (!frames.hasNext()) return;
        do {
            const QString framePath = frames.next();
            QFile frame(framePath);
            if (!frame.open(QIODevice::ReadOnly))
                throw std::runtime_error("Could not inspect finalized DNG pixel layout");
            const QByteArray data = frame.readAll();
            const std::vector<uint8_t> dng(data.begin(), data.end());
            const auto bytes = motioncam::vfs::finalizedCfaBadPixelOptions(sourceSidecar.toStdString(), dng);
            if (!bytes.empty()) {
                const QString destination = perFrame
                    ? QFileInfo(framePath).absolutePath() + "/" +
                          QFileInfo(framePath).completeBaseName() + ".json"
                    : QDir(temporaryDirectory).filePath(
                          QFileInfo(finalDirectory).fileName() + ".json");
                QSaveFile file(destination);
                if (!file.open(QIODevice::WriteOnly) ||
                    file.write(bytes.data(), static_cast<qint64>(bytes.size())) !=
                        static_cast<qint64>(bytes.size()) || !file.commit())
                    throw std::runtime_error("Could not write finalized bad-pixel options JSON");
            }
            if (!perFrame) break;
        } while (frames.hasNext());
    }

    bool projectionDependencyAvailable() {
#ifdef _WIN32
        HMODULE library = LoadLibraryW(L"projectedfslib.dll");
        if (!library) return false;
        FreeLibrary(library);
        return true;
#elif defined(__APPLE__)
        const QStringList candidates{"/Library/Filesystems/macfuse.fs",
            "/Library/Frameworks/macfuse.framework", "/Library/Extensions/macfuse.kext",
            "/Library/Frameworks/fuse_t.framework"};
        for (const auto& path : candidates)
            if (QFileInfo::exists(path)) return true;
        return false;
#elif defined(__ANDROID__)
        return false;
#else
        return QFileInfo::exists(QStringLiteral("/dev/fuse")) &&
            !QStandardPaths::findExecutable(QStringLiteral("fusermount3")).isEmpty();
#endif
    }

    bool mergeDirectoryContents(const QString& sourcePath,const QString& destinationPath,
                                QString& error) {
        if(!QDir().mkpath(destinationPath)){
            error=QObject::tr("Could not create selection folder");return false;
        }
        QDir source(sourcePath);
        QDirIterator iterator(sourcePath,QDir::Files|QDir::NoDotAndDotDot,
                              QDirIterator::Subdirectories);
        while(iterator.hasNext()){
            const QString sourceFile=iterator.next();
            const QString relative=source.relativeFilePath(sourceFile);
            const QString destinationFile=QDir(destinationPath).filePath(relative);
            if(!QDir().mkpath(QFileInfo(destinationFile).absolutePath())){
                error=QObject::tr("Could not create selection subfolder for %1").arg(relative);
                return false;
            }
            QFile input(sourceFile);QSaveFile destination(destinationFile);
            if(!input.open(QIODevice::ReadOnly)||!destination.open(QIODevice::WriteOnly)){
                error=QObject::tr("Could not save selected frame %1").arg(relative);
                return false;
            }
            while(!input.atEnd()){
                const QByteArray block=input.read(1024*1024);
                if(block.isEmpty()&&input.error()!=QFileDevice::NoError){
                    destination.cancelWriting();
                    error=QObject::tr("Could not read finalized frame %1").arg(relative);
                    return false;
                }
                if(destination.write(block)!=block.size()){
                    destination.cancelWriting();
                    error=QObject::tr("Could not save selected frame %1").arg(relative);
                    return false;
                }
            }
            if(!destination.commit()){
                error=QObject::tr("Could not overwrite selected frame %1").arg(relative);
                return false;
            }
            input.close();QFile::remove(sourceFile);
        }
        return true;
    }

    bool mergeDirectoryContentsFlat(const QString& sourcePath, const QString& destinationPath,
                                    QString& error) {
        if (!QDir().mkpath(destinationPath)) {
            error = QObject::tr("Could not create selected frames output directory");
            return false;
        }
        QList<QPair<QString, QString>> files;
        QDirIterator iterator(sourcePath, QDir::Files | QDir::NoDotAndDotDot,
                              QDirIterator::Subdirectories);
        while (iterator.hasNext()) {
            const QString sourceFile = iterator.next();
            const QString fileName = QFileInfo(sourceFile).fileName();
            const QString destinationFile = QDir(destinationPath).filePath(fileName);
            files.append({sourceFile, destinationFile});
        }
        for (const auto& file : files) {
            const QString& sourceFile = file.first;
            const QString& destinationFile = file.second;
            const QString fileName = QFileInfo(destinationFile).fileName();
            if (!QFile::rename(sourceFile, destinationFile)) {
                QFile input(sourceFile);
                QSaveFile destination(destinationFile);
                if (!input.open(QIODevice::ReadOnly) || !destination.open(QIODevice::WriteOnly)) {
                    error = QObject::tr("Could not save selected frame %1").arg(fileName);
                    return false;
                }
                while (!input.atEnd()) {
                    const QByteArray block = input.read(1024 * 1024);
                    if ((block.isEmpty() && input.error() != QFileDevice::NoError) ||
                        destination.write(block) != block.size()) {
                        destination.cancelWriting();
                        error = QObject::tr("Could not save selected frame %1").arg(fileName);
                        return false;
                    }
                }
                if (!destination.commit()) {
                    error = QObject::tr("Could not save selected frame %1").arg(fileName);
                    return false;
                }
                input.close();
                QFile::remove(sourceFile);
            }
        }
        return true;
    }

    bool inheritHeroFrameSidecars(const QString& sourcePath, const QString& destinationPath,
                                  bool flatten, QString& error) {
        struct HeroFrame {
            QString prefix;
            QString fileName;
            int frame = 0;
            QString pp3;
            QString xml;
        };
        auto frameParts = [](const QString& fileName, QString& prefix, int& frame) {
            const QFileInfo info(fileName);
            const QString stem = info.completeBaseName();
            if (stem.size() < 6) return false;
            const QString suffix = stem.right(6);
            for (const QChar ch : suffix)
                if (!ch.isDigit()) return false;
            bool ok = false;
            frame = suffix.toInt(&ok);
            if (!ok) return false;
            prefix = stem.left(stem.size() - 6);
            return true;
        };
        auto sequenceKey = [flatten](const QString& root, const QString& path,
                                     const QString& prefix) {
            if (flatten) return prefix;
            const QString relative = QDir(root).relativeFilePath(path);
            return QFileInfo(relative).path() + QChar(0x1f) + prefix;
        };

        QHash<QString, QPair<QString, QString>> sidecarsByDng;
        QStringList existingDngs;
        QDirIterator existing(destinationPath, QDir::Files | QDir::NoDotAndDotDot,
                              QDirIterator::Subdirectories);
        while (existing.hasNext()) {
            const QString path = existing.next();
            const QString extension = QFileInfo(path).suffix().toLower();
            if (extension == QStringLiteral("dng")) {
                existingDngs.append(path);
            } else if (extension == QStringLiteral("pp3") ||
                       extension == QStringLiteral("xml")) {
                const QString dngPath = path.left(path.size() - extension.size() - 1);
                if (QFileInfo(dngPath).suffix().compare(QStringLiteral("dng"),
                                                        Qt::CaseInsensitive) != 0)
                    continue;
                auto& sidecars = sidecarsByDng[dngPath.toCaseFolded()];
                if (extension == QStringLiteral("pp3")) sidecars.first = path;
                else sidecars.second = path;
            }
        }
        QHash<QString, QVector<HeroFrame>> heroesByPrefix;
        QVector<HeroFrame> allHeroes;
        for (const QString& path : existingDngs) {
            HeroFrame hero;
            if (!frameParts(QFileInfo(path).fileName(), hero.prefix, hero.frame)) continue;
            hero.fileName = QFileInfo(path).fileName();
            const auto sidecars = sidecarsByDng.value(path.toCaseFolded());
            hero.pp3 = sidecars.first;
            hero.xml = sidecars.second;
            if (!hero.pp3.isEmpty() || !hero.xml.isEmpty()) {
                heroesByPrefix[sequenceKey(destinationPath, path, hero.prefix)].append(hero);
                allHeroes.append(std::move(hero));
            }
        }
        std::sort(allHeroes.begin(), allHeroes.end(), [](const HeroFrame& left, const HeroFrame& right) {
            return QString::compare(left.fileName, right.fileName, Qt::CaseInsensitive) < 0;
        });

        size_t copiedSidecars = 0;
        QDirIterator generated(sourcePath, QDir::Files | QDir::NoDotAndDotDot,
                               QDirIterator::Subdirectories);
        while (generated.hasNext()) {
            const QString generatedPath = generated.next();
            if (QFileInfo(generatedPath).suffix().compare(QStringLiteral("dng"), Qt::CaseInsensitive) != 0)
                continue;
            QString prefix;
            int frame = 0;
            if (!frameParts(QFileInfo(generatedPath).fileName(), prefix, frame)) continue;
            auto heroes = heroesByPrefix.value(sequenceKey(sourcePath, generatedPath, prefix));
            std::sort(heroes.begin(), heroes.end(), [](const HeroFrame& left, const HeroFrame& right) {
                return left.frame < right.frame;
            });
            for (const auto& sidecar : {qMakePair(&HeroFrame::pp3, QStringLiteral("pp3")),
                                        qMakePair(&HeroFrame::xml, QStringLiteral("xml"))}) {
                const HeroFrame* selected = nullptr;
                const HeroFrame* firstAfter = nullptr;
                if (!heroes.isEmpty()) {
                    for (const auto& hero : heroes) {
                        if ((hero.*sidecar.first).isEmpty()) continue;
                        if (hero.frame <= frame) selected = &hero;
                        else if (!firstAfter) firstAfter = &hero;
                    }
                } else {
                    // This clip has no hero of its own. In filename order,
                    // inherit the final edit from the preceding clip, or the
                    // first available edit from the following clip.
                    for (const auto& hero : allHeroes) {
                        if ((hero.*sidecar.first).isEmpty()) continue;
                        const int order = QString::compare(
                            hero.prefix, prefix, Qt::CaseInsensitive);
                        if (order < 0) selected = &hero;
                        else if (order > 0 && !firstAfter) firstAfter = &hero;
                    }
                }
                if (!selected) selected = firstAfter;
                if (!selected) continue;
                const QString stagedSidecar = generatedPath + QLatin1Char('.') + sidecar.second;
                try {
                    copyFileAtomically(selected->*sidecar.first, stagedSidecar);
                    ++copiedSidecars;
                } catch (const std::exception& exception) {
                    error = QString::fromUtf8(exception.what());
                    return false;
                }
            }
        }
        spdlog::info("Selected-frame hero sidecars: output={} heroes={} copies={}",
                     destinationPath.toStdString(), allHeroes.size(), copiedSidecars);
        return true;
    }

    void removeAdjacentClipSeparator(QVBoxLayout* layout, int cardIndex) {
        if (!layout || cardIndex < 0) return;
        const int candidates[] = {cardIndex - 1, cardIndex + 1};
        for (const int index : candidates) {
            if (index < 0 || index >= layout->count()) continue;
            auto* frame = qobject_cast<QFrame*>(layout->itemAt(index)->widget());
            if (!frame || frame->frameShape() != QFrame::HLine) continue;
            layout->removeWidget(frame);frame->deleteLater();return;
        }
    }

    bool invertColorMatrix(const std::array<float, 9>& input,
                           std::array<float, 9>& inverse);
    bool colorMatrixToD50(const motioncam::DNGFrameMetadata& metadata,
                          std::array<float, 9>& cameraToXyzD50,
                          float* firstIlluminantWeight = nullptr);

    constexpr auto PACKAGE_NAME = "com.motioncam";
    constexpr auto APP_NAME = "MotionCam FS";

    void styleClipMetadata(QLabel* label,const QString& extra={}) {
#ifdef __ANDROID__
        constexpr int pointSize=10;
#else
        constexpr int pointSize=9;
#endif
        label->setStyleSheet(QStringLiteral("font-size:%1pt;%2").arg(pointSize).arg(extra));
#ifdef __ANDROID__
        label->ensurePolished();
#endif
    }

#ifdef __ANDROID__
    constexpr bool RIFE_SUPPORTED = false;
#else
    constexpr bool RIFE_SUPPORTED = true;
#endif
    constexpr auto RIFE_REVISION = "b0542ef99f380f0fe17a1b44151ac6935e8b82d4";
    constexpr auto RIFE_ARCHIVE_SHA256 =
        "35bcf9b169e69f8aee5dfb26005424579109b7d9421bb16e3b675a5142b485bd";

    void applyGalleryColorTransform(std::vector<uint8_t>& bytes,
                                    const motioncam::DNGFrameMetadata& metadata,
                                    bool ignoreForwardMat,
                                    bool gainMapOnlyDebug,
                                    const motioncam::VulkanGalleryBayerSource* bayer = nullptr,
                                    const motioncam::VulkanGalleryOutput* output = nullptr,
                                    bool* gpuApplied = nullptr,
                                    bool enableGpu = true,
                                    const motioncam::VulkanGalleryTextureSource* texture = nullptr,
                                    const std::function<bool(uint16_t*, size_t)>* textureDownload = nullptr) {
        if (gpuApplied) *gpuApplied = false;
        if (bytes.size() % 6 != 0) return;
        const size_t pixelCount = bayer
            ? static_cast<size_t>(bayer->width) * bayer->height
            : texture ? static_cast<size_t>(texture->width) * texture->height
            : bytes.size() / 6;
        const auto transformStarted = std::chrono::steady_clock::now();
        // Small card previews already have a CPU color path. Avoid compiling
        // full gallery shaders for them ahead of the first playback frame.
        if (!output && !bayer && !texture && pixelCount <= 256u * 256u &&
            !qEnvironmentVariableIsSet("MOTIONCAM_GALLERY_VALIDATE_GPU"))
            enableGpu = false;
        static const std::array<uint16_t, 65536> srgbTransfer = [] {
            std::array<uint16_t, 65536> result{};
            for (size_t index = 0; index < result.size(); ++index) {
                const float value = static_cast<float>(index) / 65535.0f;
                const float encoded = value <= 0.0031308f
                    ? value * 12.92f
                    : 1.055f * std::pow(value, 1.0f / 2.4f) - 0.055f;
                result[index] = static_cast<uint16_t>(
                    std::lround(std::clamp(encoded, 0.0f, 1.0f) * 65535.0f));
            }
            return result;
        }();
        const float exposure = std::exp2(static_cast<float>(metadata.baselineExposure));
        const std::array<float,3> gains{
            gainMapOnlyDebug ? exposure
                : exposure / std::max(0.001f, metadata.asShotNeutral[0]),
            gainMapOnlyDebug ? exposure
                : exposure / std::max(0.001f, metadata.asShotNeutral[1]),
            gainMapOnlyDebug ? exposure
                : exposure / std::max(0.001f, metadata.asShotNeutral[2])};
        // ForwardMatrix maps white-balanced camera RGB to XYZ D50. Convert
        // that to linear sRGB (D65); without it, retain thumbnail-compatible
        // white balance and exposure handling.
        const std::array<float,9> xyzToSrgbD50{
             3.1338561f,-1.6168667f,-0.4906146f,
            -0.9787684f, 1.9161415f, 0.0334540f,
             0.0719453f,-0.2289914f, 1.4052427f};
        auto transfer=[](float value){
            // The input is clamped to [0, 1], so rounding to the nearest LUT
            // entry needs no general-purpose floating-point rounding call.
            const size_t index = static_cast<size_t>(
                std::clamp(value, 0.0f, 1.0f) * 65535.0f + 0.5f);
            return srgbTransfer[index];
        };
        constexpr float highlightBlendStart = 0.90f;
        auto neutralHighlightWeight = [](uint16_t red, uint16_t green, uint16_t blue) {
            const float leastExposed = static_cast<float>(
                std::min({red, green, blue})) / 65535.0f;
            const float t = std::clamp(
                (leastExposed - highlightBlendStart) / (1.0f - highlightBlendStart),
                0.0f, 1.0f);
            return t * t * (3.0f - 2.0f * t);
        };
        auto neutralHighlightLevel = [](float red, float green, float blue) {
            return red + green + blue - std::min({red, green, blue}) -
                   std::max({red, green, blue});
        };
        const bool hasMatrix = ignoreForwardMat
            ? (metadata.hasColorMatrix2 || metadata.hasColorMatrix1)
            : (metadata.hasForwardMatrix2 || metadata.hasForwardMatrix1);
        std::array<float, 9> cameraToDisplay{};
        std::array<float, 9> cameraToXyz{};
        float firstIlluminantWeight = metadata.hasForwardMatrix2 ? 0.0f : 1.0f;
        if (hasMatrix) {
            std::array<float, 9> inverted{};
            std::array<float, 9> forwardTransform{};
            const std::array<float, 9>* matrix = nullptr;
            if (gainMapOnlyDebug) {
                // Display the map through the camera color matrix without
                // channel scaling from AsShotNeutral.
                auto transformMetadata = metadata;
                transformMetadata.asShotNeutral = {1.0f, 1.0f, 1.0f};
                if (!ignoreForwardMat &&
                    (metadata.hasColorMatrix1 || metadata.hasColorMatrix2)) {
                    std::array<float, 9> unused{};
                    colorMatrixToD50(metadata, unused, &firstIlluminantWeight);
                }
                if (ignoreForwardMat) {
                    if (colorMatrixToD50(transformMetadata, inverted,
                                         &firstIlluminantWeight))
                        matrix = &inverted;
                } else {
                    const auto first = metadata.hasForwardMatrix1
                        ? metadata.forwardMatrix1 : metadata.forwardMatrix2;
                    const auto second = metadata.hasForwardMatrix2
                        ? metadata.forwardMatrix2 : first;
                    const auto calibration = motioncam::gallery::interpolateMatrix(
                        motioncam::gallery::cameraCalibrationMatrix(metadata, true),
                        motioncam::gallery::cameraCalibrationMatrix(metadata, false),
                        firstIlluminantWeight);
                    std::array<float, 9> inverseCalibration{};
                    if (motioncam::gallery::invertMatrix(calibration, inverseCalibration)) {
                        const auto forward = motioncam::gallery::interpolateMatrix(
                            first, second, firstIlluminantWeight);
                        for (int row = 0; row < 3; ++row)
                            for (int column = 0; column < 3; ++column)
                                for (int inner = 0; inner < 3; ++inner)
                                    forwardTransform[row * 3 + column] +=
                                        forward[row * 3 + inner] *
                                        inverseCalibration[inner * 3 + column];
                        matrix = &forwardTransform;
                    }
                }
            } else if (ignoreForwardMat) {
                if (!colorMatrixToD50(metadata, inverted,
                                      &firstIlluminantWeight)) {
                    spdlog::warn("Preview ColorMatrix transform is invalid; using neutral display transform");
                    matrix = nullptr;
                } else matrix = &inverted;
            } else {
                const bool forwardDiffers = metadata.hasForwardMatrix1 &&
                    metadata.hasForwardMatrix2 &&
                    metadata.forwardMatrix1 != metadata.forwardMatrix2;
                const bool calibrationDiffers =
                    motioncam::gallery::cameraCalibrationMatrix(metadata, true) !=
                    motioncam::gallery::cameraCalibrationMatrix(metadata, false);
                const bool tableDiffers = metadata.profileTables &&
                    !metadata.profileTables->hueSat1.values.empty() &&
                    !metadata.profileTables->hueSat2.values.empty();
                if (forwardDiffers || calibrationDiffers || tableDiffers) {
                    std::array<float, 9> unusedColorTransform{};
                    colorMatrixToD50(metadata, unusedColorTransform,
                                     &firstIlluminantWeight);
                }
                if (motioncam::gallery::forwardCameraToXyz(
                        metadata, firstIlluminantWeight, exposure, forwardTransform))
                    matrix = &forwardTransform;
                else
                    spdlog::warn("Preview ForwardMatrix transform is invalid; using neutral display transform");
            }
            // Both matrices and channel gains are constant for the frame.
            // Fold them here rather than doing two matrix passes per pixel.
            if (matrix) for (int row = 0; row < 3; ++row) {
                for (int column = 0; column < 3; ++column) {
                    cameraToXyz[row * 3 + column] = (*matrix)[row * 3 + column] *
                        (ignoreForwardMat || gainMapOnlyDebug ? 1.0f
                            : 1.0f / std::max(exposure, 1e-8f));
                    for (int xyz = 0; xyz < 3; ++xyz)
                        cameraToDisplay[row * 3 + column] +=
                            xyzToSrgbD50[row * 3 + xyz] *
                            (*matrix)[xyz * 3 + column] *
                            (ignoreForwardMat || gainMapOnlyDebug ? exposure : 1.0f);
                }
            }
        }
        const bool useMatrix = std::any_of(cameraToDisplay.begin(), cameraToDisplay.end(),
                                           [](float value) { return value != 0.0f; });
        const bool useProfileTables = useMatrix && !gainMapOnlyDebug &&
            metadata.profileTables &&
            (!metadata.profileTables->hueSat1.values.empty() ||
             !metadata.profileTables->hueSat2.values.empty() ||
             !metadata.profileTables->look.values.empty());
        if (qEnvironmentVariableIsSet("MOTIONCAM_GALLERY_PERF_PROFILE"))
            spdlog::info("GALLERY_PERF event=color_transform pixels={} matrix={} tables={} hue_values={} hue_value_divisions={} hue_encoding={} look_values={} look_value_divisions={} look_encoding={}",
                pixelCount, useMatrix, useProfileTables,
                metadata.profileTables ? metadata.profileTables->hueSat1.values.size() : 0,
                metadata.profileTables ? metadata.profileTables->hueSat1.valueDivisions : 0,
                metadata.profileTables ? metadata.profileTables->hueSat1.encoding : 0,
                metadata.profileTables ? metadata.profileTables->look.values.size() : 0,
                metadata.profileTables ? metadata.profileTables->look.valueDivisions : 0,
                metadata.profileTables ? metadata.profileTables->look.encoding : 0);
        motioncam::DNGProfileTable blendedHueSat;
        const motioncam::DNGProfileTable* hueSatTable = nullptr;
        if (useProfileTables) {
            const auto& first = metadata.profileTables->hueSat1;
            const auto& second = metadata.profileTables->hueSat2;
            hueSatTable = !first.values.empty() ? &first :
                !second.values.empty() ? &second : nullptr;
            if (!first.values.empty() && !second.values.empty() &&
                first.hueDivisions == second.hueDivisions &&
                first.saturationDivisions == second.saturationDivisions &&
                first.valueDivisions == second.valueDivisions &&
                first.encoding == second.encoding &&
                first.values.size() == second.values.size()) {
                blendedHueSat = first;
                const float secondWeight = 1.0f - firstIlluminantWeight;
                for (size_t index = 0; index < blendedHueSat.values.size(); ++index)
                    blendedHueSat.values[index] =
                        first.values[index] * firstIlluminantWeight +
                        second.values[index] * secondWeight;
                hueSatTable = &blendedHueSat;
            }
        }
        const bool validateGpu = enableGpu && !output &&
            qEnvironmentVariableIsSet("MOTIONCAM_GALLERY_VALIDATE_GPU");
        std::vector<uint8_t> original;
        if (validateGpu) original = bytes;
        if (enableGpu && motioncam::colorGalleryRgbVulkan(
                bytes, gains, cameraToDisplay, cameraToXyz, exposure,
                useProfileTables ? hueSatTable : nullptr,
                useProfileTables && !metadata.profileTables->look.values.empty()
                    ? &metadata.profileTables->look : nullptr,
                useMatrix, gainMapOnlyDebug, bayer, output, texture)) {
            if (gpuApplied) *gpuApplied = true;
            if (validateGpu) {
                applyGalleryColorTransform(original, metadata, ignoreForwardMat,
                                           gainMapOnlyDebug, bayer, nullptr,
                                           nullptr, false, texture, textureDownload);
                uint32_t maximum = 0;
                uint64_t sum = 0, aboveTwo = 0, aboveEight = 0;
                for (size_t index = 0; index < bytes.size() / 2; ++index) {
                    uint16_t gpuCode, cpuCode;
                    std::memcpy(&gpuCode, bytes.data() + index * 2, 2);
                    std::memcpy(&cpuCode, original.data() + index * 2, 2);
                    const uint32_t error = static_cast<uint32_t>(
                        std::abs(int(gpuCode) - int(cpuCode)));
                    maximum = std::max(maximum, error);
                    sum += error;
                    aboveTwo += error > 2;
                    aboveEight += error > 8;
                }
                spdlog::info("GALLERY_PERF event=gallery_gpu_compare pixels={} tables={} max_error={} mean_error={:.4f} above_2={} above_8={}",
                    pixelCount, useProfileTables, maximum,
                    bytes.empty() ? 0.0 : double(sum) / (bytes.size() / 2),
                    aboveTwo, aboveEight);
            }
            if (qEnvironmentVariableIsSet("MOTIONCAM_GALLERY_PERF_PROFILE"))
                spdlog::info("GALLERY_PERF event=gallery_color_stage pixels={} tables={} gpu=true final={} elapsed_ms={:.3f}",
                    pixelCount, useProfileTables, output != nullptr,
                    std::chrono::duration<double, std::milli>(
                        std::chrono::steady_clock::now() - transformStarted).count());
            return;
        }
        if (output && output->requireZeroCopy)
            throw std::runtime_error("Zero-copy gallery requires resident GPU input and shared Vulkan image output");
        if (bayer) {
            std::vector<uint16_t> recovered;
            const uint16_t* samples = bayer->samples;
            if (!samples) {
                if (!bayer->download || !bayer->download(recovered))
                    throw std::runtime_error("Could not download gallery Bayer preview");
                samples = recovered.data();
            }
            if (bayer->gain) {
                const size_t count = static_cast<size_t>(bayer->width) * bayer->height;
                if (recovered.empty()) recovered.assign(samples, samples + count);
                if (!bayer->recoverCpu || !bayer->recoverCpu(recovered))
                    throw std::runtime_error("Could not recover gallery Bayer gain");
                samples = recovered.data();
            }
            if (!motioncam::utils::demosaicBilinearBayerToRgb16Bytes(
                    samples, bytes, bayer->width, bayer->height,
                    bayer->phase, bayer->black, bayer->white))
                throw std::runtime_error("Could not reconstruct gallery Bayer preview");
        }
        if (texture) {
            if (!textureDownload || !*textureDownload)
                throw std::runtime_error("No DirectLog gallery CPU recovery path");
            std::vector<uint16_t> samples(pixelCount * 3u);
            if (!(*textureDownload)(samples.data(), samples.size()))
                throw std::runtime_error("Could not recover DirectLog gallery preview");
            if (texture->gain) {
                const auto& map = *texture->gain;
                const auto linearization = map.encodedWhite
                    ? motioncam::makeLog60LinearizationTable(map.encodedWhite)
                    : std::vector<uint16_t>{};
                const auto* logLookup = map.encodedWhite
                    ? &motioncam::cachedLog60EncodeLookup(
                        0.0, 65535.0, static_cast<uint16_t>(map.encodedWhite))
                    : nullptr;
                bytes.resize(samples.size() * sizeof(uint16_t));
                auto sample = [&](uint32_t x, uint32_t y, size_t channel) {
                    return map.values[(static_cast<size_t>(y) * map.width + x) * 3u + channel];
                };
                for (uint32_t y = 0; y < texture->height; ++y) {
                    const motioncam::utils::LogTriangularDitherRow<false> fastDither(
                        y, map.ditherSeed);
                    const motioncam::utils::LogTriangularDitherRow<true> hqDither(
                        y, map.ditherSeed);
                    for (uint32_t x = 0; x < texture->width; ++x) {
                        std::array<float, 3> gains{1.0f, 1.0f, 1.0f};
                        if (x >= map.left && x < map.right &&
                            y >= map.top && y < map.bottom) {
                            const float gx = ((x + 0.5f) / texture->width -
                                map.originH) / map.spacingH;
                            const float gy = ((y + 0.5f) / texture->height -
                                map.originV) / map.spacingV;
                            const float bx = std::floor(gx), by = std::floor(gy);
                            const float tx = std::clamp(gx - bx, 0.0f, 1.0f);
                            const float ty = std::clamp(gy - by, 0.0f, 1.0f);
                            const uint32_t x0 = static_cast<uint32_t>(std::clamp(
                                bx, 0.0f, static_cast<float>(map.width - 1u)));
                            const uint32_t y0 = static_cast<uint32_t>(std::clamp(
                                by, 0.0f, static_cast<float>(map.height - 1u)));
                            const uint32_t x1 = std::min(x0 + 1u, map.width - 1u);
                            const uint32_t y1 = std::min(y0 + 1u, map.height - 1u);
                            for (size_t channel = 0; channel < 3; ++channel) {
                                const float upper = sample(x0, y0, channel) * (1.0f - tx) +
                                    sample(x1, y0, channel) * tx;
                                const float lower = sample(x0, y1, channel) * (1.0f - tx) +
                                    sample(x1, y1, channel) * tx;
                                gains[channel] = upper * (1.0f - ty) + lower * ty;
                            }
                        }
                        if (map.secondaryWidth &&
                            x >= map.secondaryLeft && x < map.secondaryRight &&
                            y >= map.secondaryTop && y < map.secondaryBottom) {
                            const float gx = ((x + 0.5f) / texture->width -
                                map.secondaryOriginH) / map.secondarySpacingH;
                            const float gy = ((y + 0.5f) / texture->height -
                                map.secondaryOriginV) / map.secondarySpacingV;
                            const float bx = std::floor(gx), by = std::floor(gy);
                            const float tx = std::clamp(gx - bx, 0.0f, 1.0f);
                            const float ty = std::clamp(gy - by, 0.0f, 1.0f);
                            const uint32_t x0 = static_cast<uint32_t>(std::clamp(
                                bx, 0.0f, static_cast<float>(map.secondaryWidth - 1u)));
                            const uint32_t y0 = static_cast<uint32_t>(std::clamp(
                                by, 0.0f, static_cast<float>(map.secondaryHeight - 1u)));
                            const uint32_t x1 = std::min(x0 + 1u, map.secondaryWidth - 1u);
                            const uint32_t y1 = std::min(y0 + 1u, map.secondaryHeight - 1u);
                            const auto sampleSecondary = [&](uint32_t sx, uint32_t sy) {
                                return map.secondaryValues[static_cast<size_t>(sy) *
                                    map.secondaryWidth + sx];
                            };
                            const float upper = sampleSecondary(x0, y0) * (1.0f - tx) +
                                sampleSecondary(x1, y0) * tx;
                            const float lower = sampleSecondary(x0, y1) * (1.0f - tx) +
                                sampleSecondary(x1, y1) * tx;
                            const float luminance = upper * (1.0f - ty) + lower * ty;
                            for (float& gain : gains) gain *= luminance;
                        }
                        const float dither = map.highQualityDither
                            ? hqDither(x) : fastDither(x);
                        const size_t first = (static_cast<size_t>(y) *
                            texture->width + x) * 3u;
                        for (size_t channel = 0; channel < 3; ++channel) {
                            const float linear = map.debugGainMap
                                ? std::clamp(1.0f / gains[channel], 0.0f, 1.0f)
                                : std::clamp(
                                    (samples[first + channel] - texture->black[channel]) /
                                    (texture->white[channel] - texture->black[channel]) *
                                    gains[channel], 0.0, 1.0);
                            const auto code = static_cast<uint16_t>(std::lround(
                                linear * 65535.0f));
                            const auto normalized = logLookup
                                ? linearization[motioncam::encodeDitheredLog60(
                                    *logLookup, code, dither,
                                    static_cast<uint16_t>(map.encodedWhite))]
                                : code;
                            std::memcpy(bytes.data() + (first + channel) * 2u,
                                &normalized, sizeof(normalized));
                        }
                    }
                }
            } else if (!motioncam::utils::normalizeRgb16Bytes(
                           samples.data(), samples.size(), bytes,
                           texture->black, texture->white)) {
                throw std::runtime_error("Could not normalize DirectLog gallery preview");
            }
        }
        auto* pixels = reinterpret_cast<uint16_t*>(bytes.data());
        std::array<std::array<uint16_t, 65536>, 3> channelTransfer;
        if (!useMatrix) {
            for (size_t value = 0; value < 65536; ++value) {
                const float normalized = static_cast<float>(value) / 65535.0f;
                for (int channel = 0; channel < 3; ++channel)
                    channelTransfer[channel][value] = transfer(normalized * gains[channel]);
            }
        }
        auto convertRange = [&](size_t begin, size_t end) {
            if (!useMatrix) {
                for (size_t pixel = begin; pixel < end; ++pixel) {
                    const size_t offset = pixel * 3;
                    const int clippedChannels = (pixels[offset] == 65535) +
                        (pixels[offset + 1] == 65535) + (pixels[offset + 2] == 65535);
                    const bool sourceClipped = !gainMapOnlyDebug && clippedChannels > 0;
                    const float neutralWeight = gainMapOnlyDebug ? 0.0f
                        : neutralHighlightWeight(
                            pixels[offset], pixels[offset + 1], pixels[offset + 2]);
                    if (!sourceClipped && neutralWeight == 0.0f) {
                        for (int channel = 0; channel < 3; ++channel)
                            pixels[offset + channel] =
                                channelTransfer[channel][pixels[offset + channel]];
                        continue;
                    }
                    float display[3];
                    for (int channel = 0; channel < 3; ++channel)
                        display[channel] = pixels[offset + channel] / 65535.0f * gains[channel];
                    if (sourceClipped)
                        display[1] = std::max(display[1], std::min(display[0], display[2]));
                    const float neutral = neutralHighlightLevel(
                        display[0], display[1], display[2]);
                    for (int channel = 0; channel < 3; ++channel)
                        pixels[offset + channel] = transfer(
                            display[channel] * (1.0f - neutralWeight) +
                            neutral * neutralWeight);
                }
                return;
            }
            constexpr float normalize = 1.0f / 65535.0f;
            for (size_t pixel = begin; pixel < end; ++pixel) {
                const size_t offset = pixel * 3;
                const int clippedChannels = (pixels[offset] == 65535) +
                    (pixels[offset + 1] == 65535) + (pixels[offset + 2] == 65535);
                const bool sourceClipped = !gainMapOnlyDebug && clippedChannels > 0;
                const float neutralWeight = gainMapOnlyDebug ? 0.0f
                    : neutralHighlightWeight(
                        pixels[offset], pixels[offset + 1], pixels[offset + 2]);
                const float camera[3]{pixels[offset] * normalize,
                                      pixels[offset + 1] * normalize,
                                      pixels[offset + 2] * normalize};
                float display[3];
                if (useProfileTables) {
                    const auto& tables = *metadata.profileTables;
                    std::array<float, 3> xyz{};
                    for (int row = 0; row < 3; ++row)
                        for (int column = 0; column < 3; ++column)
                            xyz[row] += cameraToXyz[row * 3 + column] * camera[column];
                    std::array<float, 3> proPhoto{
                         1.3459433f * xyz[0] - 0.2556075f * xyz[1] - 0.0511118f * xyz[2],
                        -0.5445989f * xyz[0] + 1.5081673f * xyz[1] + 0.0205351f * xyz[2],
                         1.2118128f * xyz[2]};
                    if (hueSatTable && !tables.look.values.empty() &&
                        hueSatTable->valueDivisions == 1 &&
                        hueSatTable->encoding == 0 && tables.look.encoding == 0 &&
                        exposure > 0.0f && std::isfinite(exposure)) {
                        motioncam::gallery::applyHueSatAndLook(
                            proPhoto, *hueSatTable, tables.look, exposure);
                    } else {
                        if (hueSatTable)
                            motioncam::gallery::applyProfileTable(proPhoto, *hueSatTable);
                        for (float& channel : proPhoto) channel *= exposure;
                        if (!tables.look.values.empty())
                            motioncam::gallery::applyProfileTable(proPhoto, tables.look);
                    }
                    xyz = {
                        0.7976749f * proPhoto[0] + 0.1351917f * proPhoto[1] +
                            0.0313534f * proPhoto[2],
                        0.2880402f * proPhoto[0] + 0.7118741f * proPhoto[1] +
                            0.0000857f * proPhoto[2],
                        0.82521f * proPhoto[2]};
                    for (int row = 0; row < 3; ++row)
                        display[row] = xyzToSrgbD50[row * 3] * xyz[0] +
                            xyzToSrgbD50[row * 3 + 1] * xyz[1] +
                            xyzToSrgbD50[row * 3 + 2] * xyz[2];
                } else for (int row = 0; row < 3; ++row) {
                    display[row] = cameraToDisplay[row * 3] * camera[0] +
                                   cameraToDisplay[row * 3 + 1] * camera[1] +
                                   cameraToDisplay[row * 3 + 2] * camera[2];
                }
                if (sourceClipped)
                    display[1] = std::max(display[1], std::min(display[0], display[2]));
                const float neutral = neutralHighlightLevel(
                    display[0], display[1], display[2]);
                for (int channel = 0; channel < 3; ++channel)
                    pixels[offset + channel] = transfer(
                        display[channel] * (1.0f - neutralWeight) +
                        neutral * neutralWeight);
            }
        };
        // Profile-table interpolation is CPU-heavy even at proxy resolution.
        // Use smaller chunks only for those frames; the ordinary matrix path
        // keeps its lower thread overhead at proxy sizes.
        const size_t minimumPixelsPerWorker = useProfileTables
            ? 64 * 1024 : 256 * 1024;
        const unsigned int availableWorkers = motioncam::utils::availableCpuWorkers();
        const unsigned int workerLimit = useProfileTables ? 24u : 12u;
        const unsigned int workers = static_cast<unsigned int>(std::min<size_t>(
            std::min(workerLimit, availableWorkers), std::max<size_t>(1,
                (pixelCount + minimumPixelsPerWorker - 1) / minimumPixelsPerWorker)));
        std::vector<std::thread> threads;
        threads.reserve(workers - 1);
        for (unsigned int worker = 1; worker < workers; ++worker) {
            const size_t begin = pixelCount * worker / workers;
            const size_t end = pixelCount * (worker + 1) / workers;
            threads.emplace_back(convertRange, begin, end);
        }
        convertRange(0, pixelCount / workers);
        for (auto& thread : threads) thread.join();
        if (qEnvironmentVariableIsSet("MOTIONCAM_GALLERY_PERF_PROFILE"))
            spdlog::info("GALLERY_PERF event=gallery_color_stage pixels={} tables={} workers={} elapsed_ms={:.3f}",
                pixelCount, useProfileTables, workers,
                std::chrono::duration<double, std::milli>(
                    std::chrono::steady_clock::now() - transformStarted).count());
    }

    void applyGalleryPreviewColorTransform(motioncam::PreviewFrame& preview,
                                           bool ignoreForwardMat,
                                           bool gainMapOnlyDebug,
                                           const motioncam::VulkanGalleryOutput* output = nullptr,
                                           bool* gpuApplied = nullptr,
                                           std::vector<uint8_t>* destination = nullptr) {
        motioncam::VulkanGalleryBayerSource bayer;
        motioncam::VulkanGalleryTextureSource texture;
        if (preview.hasGalleryCfa()) {
            bayer.samples = (preview.gallerySource.buffer || preview.gallerySource.texture)
                ? nullptr : preview.galleryCfaSamples();
            bayer.gpu = preview.gallerySource.gpu;
            bayer.buffer = preview.gallerySource.buffer;
            bayer.texture = preview.gallerySource.texture;
            bayer.download = preview.gallerySource.vectorDownloader();
            bayer.width = preview.width;
            bayer.height = preview.height;
            bayer.phase = preview.galleryCfaPhase;
            bayer.black = preview.galleryBlack;
            bayer.white = preview.galleryWhite;
            bayer.gain = preview.galleryCfaGain.get();
            bayer.recoverCpu = preview.galleryCfaRecoverCpu;
        }
        if (preview.gallerySource.channels == 3 && preview.gallerySource.hasStorage()) {
            texture.gpu = preview.gallerySource.gpu;
            texture.texture = preview.gallerySource.texture;
            texture.buffer = preview.gallerySource.buffer;
            texture.width = preview.width;
            texture.height = preview.height;
            texture.black = preview.galleryGpuBlack;
            texture.white = preview.galleryGpuWhite;
            texture.gain = preview.galleryGpuGain.get();
        }
        applyGalleryColorTransform(destination ? *destination : preview.rgb, preview.metadata,
                                   ignoreForwardMat, gainMapOnlyDebug,
                                   (bayer.samples || bayer.buffer || bayer.texture) ? &bayer : nullptr,
                                   output, gpuApplied, true,
                                   (texture.texture || texture.buffer) ? &texture : nullptr,
                                   (texture.texture || texture.buffer) ? &preview.gallerySource.download : nullptr);
    }

    motioncam::RenderSettings previewRenderSettings(motioncam::RenderSettings settings) {
        settings.options = static_cast<motioncam::FileRenderOptions>(
            settings.options & ~(
                motioncam::RENDER_OPT_CAMMODEL_OVERRIDE));
        settings.cameraModel.clear();
        if (settings.quadBayerOption == motioncam::QuadBayerMode::CorrectQBCFAMetadata ||
            settings.quadBayerOption == motioncam::QuadBayerMode::WrongCFAMetadata)
            settings.quadBayerOption = motioncam::QuadBayerMode::Demosaic;
        settings.cameraNativeStaging = true;
        settings.streamingPreview = true;
        return settings;
    }

    void applyGalleryClipping(std::vector<uint8_t>& rgb,
                              const std::vector<uint8_t>& clipping) {
        if (rgb.size() != clipping.size() * 6) return;
        for (size_t pixel = 0; pixel < clipping.size(); ++pixel) {
            const uint8_t indication = clipping[pixel];
            if (!indication) continue;
            const uint8_t bits = indication > 1 ? indication - 2 : 0;
            const std::array<uint16_t, 3> color = indication == 1
                ? std::array<uint16_t, 3>{32768, 16384, 0}
                : bits == 7 ? std::array<uint16_t, 3>{0, 0, 0}
                : std::array<uint16_t, 3>{
                    static_cast<uint16_t>((bits & 1) ? 65535 : 0),
                    static_cast<uint16_t>((bits & 2) ? 65535 : 0),
                    static_cast<uint16_t>((bits & 4) ? 65535 : 0)};
            for (size_t channel = 0; channel < 3; ++channel) {
                rgb[pixel * 6 + channel * 2] = static_cast<uint8_t>(color[channel]);
                rgb[pixel * 6 + channel * 2 + 1] = static_cast<uint8_t>(color[channel] >> 8);
            }
        }
    }

    bool gainMapOnlyDebug(const motioncam::RenderSettings& settings) {
        return settings.vignetteCorrection == motioncam::VignetteCorrectionMode::Bake &&
            (settings.options & motioncam::RENDER_OPT_DEBUG_SHADING_MAP);
    }

    int normalizedGalleryOrientation(int orientation) {
        return orientation == 0 || orientation == 90 || orientation == 180 || orientation == 270
            ? orientation : -1;
    }

    bool invertColorMatrix(const std::array<float, 9>& input,
                           std::array<float, 9>& inverse) {
        const float determinant = input[0] * (input[4] * input[8] - input[5] * input[7]) -
            input[1] * (input[3] * input[8] - input[5] * input[6]) +
            input[2] * (input[3] * input[7] - input[4] * input[6]);
        if (std::abs(determinant) < 1.0e-8f) return false;
        const float scale = 1.0f / determinant;
        inverse = {(input[4] * input[8] - input[5] * input[7]) * scale,
            (input[2] * input[7] - input[1] * input[8]) * scale,
            (input[1] * input[5] - input[2] * input[4]) * scale,
            (input[5] * input[6] - input[3] * input[8]) * scale,
            (input[0] * input[8] - input[2] * input[6]) * scale,
            (input[2] * input[3] - input[0] * input[5]) * scale,
            (input[3] * input[7] - input[4] * input[6]) * scale,
            (input[1] * input[6] - input[0] * input[7]) * scale,
            (input[0] * input[4] - input[1] * input[3]) * scale};
        return true;
    }

    std::array<float, 9> multiplyMatrix(const std::array<float, 9>& a,
                                        const std::array<float, 9>& b) {
        std::array<float, 9> result{};
        for (int row = 0; row < 3; ++row)
            for (int column = 0; column < 3; ++column)
                for (int inner = 0; inner < 3; ++inner)
                    result[row * 3 + column] +=
                        a[row * 3 + inner] * b[inner * 3 + column];
        return result;
    }

    std::array<float, 3> multiplyVector(const std::array<float, 9>& matrix,
                                        const std::array<float, 3>& value) {
        return {matrix[0] * value[0] + matrix[1] * value[1] + matrix[2] * value[2],
                matrix[3] * value[0] + matrix[4] * value[1] + matrix[5] * value[2],
                matrix[6] * value[0] + matrix[7] * value[1] + matrix[8] * value[2]};
    }

    double illuminantTemperature(uint16_t illuminant) {
        switch (illuminant) {
            case 17: return 2856.0; // Standard Light A
            case 18: return 4874.0; // Standard Light B
            case 19: return 6774.0; // Standard Light C
            case 20: return 5503.0; // D55
            case 21: return 6504.0; // D65
            case 22: return 7504.0; // D75
            case 23: return 5003.0; // D50
            case 24: return 3200.0; // ISO studio tungsten
            default: return 0.0;
        }
    }

    double correlatedColorTemperature(double x, double y) {
        const double denominator = y - 0.1858;
        if (std::abs(denominator) < 1.0e-9) return 5000.0;
        const double n = (x - 0.3320) / denominator;
        return std::clamp(449.0 * n * n * n + 3525.0 * n * n +
                          6823.3 * n + 5520.33, 2000.0, 25000.0);
    }

    bool colorMatrixToD50(const motioncam::DNGFrameMetadata& metadata,
                          std::array<float, 9>& cameraToXyzD50,
                          float* firstIlluminantWeight) {
        if (!metadata.hasColorMatrix1 && !metadata.hasColorMatrix2) return false;
        const auto calibrated1 = metadata.hasColorMatrix1
            ? motioncam::gallery::calibratedColorMatrix(metadata, true)
            : std::array<float, 9>{};
        const auto calibrated2 = metadata.hasColorMatrix2
            ? motioncam::gallery::calibratedColorMatrix(metadata, false)
            : calibrated1;
        const double temperature1 = illuminantTemperature(metadata.calibrationIlluminant1);
        const double temperature2 = illuminantTemperature(metadata.calibrationIlluminant2);
        double sceneTemperature = temperature1 > 0.0 ? temperature1 : 5000.0;
        std::array<float, 9> inverse{};
        std::array<float, 9> interpolated{};
        std::array<float, 3> sourceWhite{0.96422f, 1.0f, 0.82521f};
        double weight = metadata.hasColorMatrix1 ? 1.0 : 0.0;
        for (int iteration = 0; iteration < 4; ++iteration) {
            weight = metadata.hasColorMatrix1 ? 1.0 : 0.0;
            if (metadata.hasColorMatrix1 && metadata.hasColorMatrix2 &&
                temperature1 > 0.0 && temperature2 > 0.0 && temperature1 != temperature2) {
                weight = std::clamp((1.0 / sceneTemperature - 1.0 / temperature2) /
                                    (1.0 / temperature1 - 1.0 / temperature2), 0.0, 1.0);
            }
            for (size_t index = 0; index < interpolated.size(); ++index) {
                const float first = metadata.hasColorMatrix1
                    ? calibrated1[index] : calibrated2[index];
                const float second = metadata.hasColorMatrix2
                    ? calibrated2[index] : first;
                interpolated[index] = static_cast<float>(first * weight + second * (1.0 - weight));
            }
            if (!invertColorMatrix(interpolated, inverse)) return false;
            sourceWhite = multiplyVector(inverse, metadata.asShotNeutral);
            const double sum = sourceWhite[0] + sourceWhite[1] + sourceWhite[2];
            if (!(sum > 1.0e-8)) return false;
            sceneTemperature = correlatedColorTemperature(sourceWhite[0] / sum,
                                                           sourceWhite[1] / sum);
        }
        if (firstIlluminantWeight)
            *firstIlluminantWeight = static_cast<float>(weight);
        const float sourceScale = sourceWhite[1] != 0.0f ? 1.0f / sourceWhite[1] : 1.0f;
        for (float& value : sourceWhite) value *= sourceScale;
        constexpr std::array<float, 9> bradford{
             0.8951f, 0.2664f,-0.1614f,
            -0.7502f, 1.7135f, 0.0367f,
             0.0389f,-0.0685f, 1.0296f};
        constexpr std::array<float, 9> bradfordInverse{
             0.9869929f,-0.1470543f, 0.1599627f,
             0.4323053f, 0.5183603f, 0.0492912f,
            -0.0085287f, 0.0400428f, 0.9684867f};
        constexpr std::array<float, 3> d50{0.96422f, 1.0f, 0.82521f};
        const auto sourceCone = multiplyVector(bradford, sourceWhite);
        const auto targetCone = multiplyVector(bradford, d50);
        if (std::abs(sourceCone[0]) < 1.0e-8f || std::abs(sourceCone[1]) < 1.0e-8f ||
            std::abs(sourceCone[2]) < 1.0e-8f) return false;
        const std::array<float, 9> coneScale{
            targetCone[0] / sourceCone[0],0,0,
            0,targetCone[1] / sourceCone[1],0,
            0,0,targetCone[2] / sourceCone[2]};
        const auto adaptation = multiplyMatrix(
            bradfordInverse, multiplyMatrix(coneScale, bradford));
        cameraToXyzD50 = multiplyMatrix(adaptation, inverse);
        return true;
    }

    motioncam::RenderSettings thumbnailRenderSettings(motioncam::RenderSettings settings) {
        settings = previewRenderSettings(std::move(settings));
        settings.thumbnailPreview = true;
        // Card images are only 176x112. Run the same preprocessing pipeline on
        // a small proxy so full-resolution CFA demosaic and display conversion
        // do not dominate mount/settings updates. HQ proxy demosaic is useful
        // for exported frames but its extra detail is not visible at card size.
        settings.options = static_cast<motioncam::FileRenderOptions>(
            settings.options | motioncam::RENDER_OPT_DRAFT);
        settings.options = static_cast<motioncam::FileRenderOptions>(
            settings.options & ~motioncam::RENDER_OPT_HIGHER_CFA_HQ);
        settings.draftScale = std::max(16, settings.draftScale);
        // Marking thousands of PDAF coordinates on a tiny proxy can cover the
        // entire card. Thumbnails are navigation aids, not diagnostics.
        if (settings.badPixelTreatment == motioncam::BadPixelTreatment::MarkPixels)
            settings.badPixelTreatment = motioncam::BadPixelTreatment::Disabled;
        return settings;
    }

    QImage previewImage(const std::vector<uint8_t>& rgb, uint32_t width, uint32_t height,
                        int orientation) {
        const size_t pixels = static_cast<size_t>(width) * height;
        const size_t stride = rgb.size() == pixels * 3 ? 3 : 6;
        if (!width || !height || rgb.size() < pixels * stride)
            return {};
        orientation = normalizedGalleryOrientation(orientation);
        const uint32_t outputWidth = orientation == 90 || orientation == 270 ? height : width;
        const uint32_t outputHeight = orientation == 90 || orientation == 270 ? width : height;
        QImage image(static_cast<int>(outputWidth), static_cast<int>(outputHeight), QImage::Format_RGB888);
        for (uint32_t y = 0; y < height; ++y) {
            for (uint32_t x = 0; x < width; ++x) {
                const size_t input = (static_cast<size_t>(y) * width + x) * stride;
                uint32_t outputX = x, outputY = y;
                if (orientation == 90) { outputX = height - 1 - y; outputY = x; }
                else if (orientation == 180) { outputX = width - 1 - x; outputY = height - 1 - y; }
                else if (orientation == 270) { outputX = y; outputY = width - 1 - x; }
                auto* destination = image.scanLine(static_cast<int>(outputY)) + outputX * 3;
                destination[0] = rgb[input + (stride == 3 ? 0 : 1)];
                destination[1] = rgb[input + (stride == 3 ? 1 : 3)];
                destination[2] = rgb[input + (stride == 3 ? 2 : 5)];
            }
        }
        return image;
    }

    void packGalleryRgb24(std::vector<uint8_t>& rgb) {
        if (rgb.size() % 6) return;
        const size_t pixels = rgb.size() / 6;
        for (size_t pixel = 0; pixel < pixels; ++pixel) {
            rgb[pixel * 3] = rgb[pixel * 6 + 1];
            rgb[pixel * 3 + 1] = rgb[pixel * 6 + 3];
            rgb[pixel * 3 + 2] = rgb[pixel * 6 + 5];
        }
        rgb.resize(pixels * 3);
    }

    template<typename T>
    std::vector<T> rotateInterleaved(const std::vector<T>& input, uint32_t width,
                                     uint32_t height, uint32_t channels,
                                     int orientation) {
        orientation = normalizedGalleryOrientation(orientation);
        if (orientation < 0 || orientation == 0 || !width || !height || !channels)
            return input;
        if (input.size() < static_cast<size_t>(width) * height * channels)
            return input;
        const uint32_t outputWidth = orientation == 90 || orientation == 270 ? height : width;
        std::vector<T> output(input.size());
        for (uint32_t y = 0; y < height; ++y) {
            for (uint32_t x = 0; x < width; ++x) {
                uint32_t outputX = x, outputY = y;
                if (orientation == 90) { outputX = height - 1 - y; outputY = x; }
                else if (orientation == 180) { outputX = width - 1 - x; outputY = height - 1 - y; }
                else if (orientation == 270) { outputX = y; outputY = width - 1 - x; }
                const size_t source = (static_cast<size_t>(y) * width + x) * channels;
                const size_t destination =
                    (static_cast<size_t>(outputY) * outputWidth + outputX) * channels;
                std::copy_n(input.begin() + static_cast<std::ptrdiff_t>(source), channels,
                            output.begin() + static_cast<std::ptrdiff_t>(destination));
            }
        }
        return output;
    }

    bool isCameraNativeFormat(const QString& mode) {
        const QString trimmed = mode.trimmed();
        return trimmed.startsWith("HEVC ", Qt::CaseInsensitive) ||
               trimmed.startsWith("AV1 ", Qt::CaseInsensitive) ||
               trimmed.startsWith("ProRes ", Qt::CaseInsensitive) ||
               trimmed.startsWith("CineForm ", Qt::CaseInsensitive);
    }

    bool isSevenZipFinalizeFormat(const QString& mode) {
        return mode.trimmed().compare("7z LZMA2 Ultra", Qt::CaseInsensitive) == 0;
    }

    bool isMetadataOnlyFinalizeFormat(const QString& mode) {
        return mode.trimmed().compare("Audio, GCSV, JSON Only", Qt::CaseInsensitive) == 0;
    }

    QString finalizeAudioCodec(QString& mode) {
        mode=mode.trimmed();
        for(const QString& codec:{QStringLiteral("flac"),QStringLiteral("opus")}){
            const QString suffix=" "+codec;
            if(mode.endsWith(suffix,Qt::CaseInsensitive)){
                mode.chop(suffix.size());
                return codec;
            }
        }
        return {};
    }

    QString sevenZipExecutable() {
        for (const QString& name : {QStringLiteral("7zz"), QStringLiteral("7z"), QStringLiteral("7za")}) {
            const QString bundled = QDir(QCoreApplication::applicationDirPath()).absoluteFilePath(
#ifdef _WIN32
                name + ".exe");
#else
                name);
#endif
            if (QFileInfo(bundled).isExecutable()) return bundled;
            const QString executable = QStandardPaths::findExecutable(name);
            if (!executable.isEmpty()) return executable;
        }
        return {};
    }

    void copyFileAtomically(const QString& source, const QString& destination) {
        if (!QFileInfo::exists(source) ||
            QFileInfo(source).absoluteFilePath() == QFileInfo(destination).absoluteFilePath()) return;
        QFile input(source);
        QSaveFile output(destination);
        if (!input.open(QIODevice::ReadOnly) || !output.open(QIODevice::WriteOnly))
            throw std::runtime_error("Could not create output sidecar " + destination.toStdString());
        while (!input.atEnd()) {
            const QByteArray block = input.read(1024 * 1024);
            if ((block.isEmpty() && input.error() != QFileDevice::NoError) ||
                output.write(block) != block.size())
                throw std::runtime_error("Could not copy output sidecar " + destination.toStdString());
        }
        if (!output.commit())
            throw std::runtime_error("Could not finish output sidecar " + destination.toStdString());
    }

    struct RemoveFilesOnExit {
        QStringList paths;
        ~RemoveFilesOnExit() { for (const auto& path : paths) QFile::remove(path); }
    };

    bool parseJxlDctDistance(const QString& mode, float& distance) {
        QString value = mode.trimmed();
        if (!value.startsWith("JPEG XL DCT", Qt::CaseInsensitive))
            return false;
        value = value.mid(11).trimmed();
        value.replace(',', '.');
        bool valid = false;
        const float parsed = value.toFloat(&valid);
        if (!valid || !std::isfinite(parsed) || parsed < 0.0f)
            return false;
        distance = parsed;
        return true;
    }

    bool dngFinalizeCompressionDistance(const QString& mode, float& distance) {
        QString trimmed = mode.trimmed();
        finalizeAudioCodec(trimmed);
        if (trimmed.compare("JPEG 92 Lossless", Qt::CaseInsensitive) == 0) {
            distance = motioncam::DNG_COMPRESSION_JPEG_LOSSLESS;
            return true;
        }
        if (trimmed.compare("JPEG92 Lossless Strip", Qt::CaseInsensitive) == 0) {
            distance = motioncam::DNG_COMPRESSION_JPEG92_STRIP;
            return true;
        }
        if (trimmed.compare("JPEG DCT 12b", Qt::CaseInsensitive) == 0 ||
            trimmed.compare("CinemaDNG 12-bit Lossy", Qt::CaseInsensitive) == 0 ||
            trimmed.compare("JPEG DCT Lossy", Qt::CaseInsensitive) == 0) {
            distance = motioncam::DNG_COMPRESSION_JPEG_DCT;
            return true;
        }
        if (trimmed.compare("JPEG XL Lossless", Qt::CaseInsensitive) == 0) {
            distance = 0.0f;
            return true;
        }
        return parseJxlDctDistance(trimmed, distance);
    }

#ifdef __APPLE__
    QStringList listMotionCamFuseMounts() {
        struct statfs* mounts = nullptr;
        const int count = getmntinfo(&mounts, MNT_NOWAIT);
        QStringList result;
        for (int index = 0; index < count; ++index) {
            const QString source = QString::fromLocal8Bit(mounts[index].f_mntfromname);
            if (!source.startsWith("MotionCamFuse@") && !source.startsWith("MotionCam Fuse@"))
                continue;
            result.push_back(QString::fromLocal8Bit(mounts[index].f_mntonname));
        }
        return result;
    }

    void unmountMacFusePath(const QString& mountPoint) {
        const QByteArray path = mountPoint.toUtf8();
        if (::unmount(path.constData(), 0) != 0) ::unmount(path.constData(), MNT_FORCE);
    }

    bool waitForMacFuseUnmount(const QString& mountPoint, int timeoutMs) {
        const QString expected = QFileInfo(mountPoint).absoluteFilePath();
        QElapsedTimer timer;
        timer.start();
        while (timer.elapsed() < timeoutMs) {
            const auto mounts = listMotionCamFuseMounts();
            const bool stillMounted = std::any_of(mounts.cbegin(), mounts.cend(),
                [&expected](const QString& path) {
                    return QFileInfo(path).absoluteFilePath() == expected;
                });
            if (!stillMounted) return true;
            QApplication::processEvents(QEventLoop::ExcludeUserInputEvents, 20);
            QThread::msleep(20);
        }
        return false;
    }
#endif

#if defined(__linux__) && !defined(__ANDROID__)
    QByteArray decodeLinuxMountInfoPath(QByteArray path) {
        path.replace("\\040", " ");
        path.replace("\\011", "\t");
        path.replace("\\012", "\n");
        path.replace("\\134", "\\");
        return path;
    }

    bool isLinuxMountPoint(const QString& path) {
        QFile mountInfo(QStringLiteral("/proc/self/mountinfo"));
        if (!mountInfo.open(QIODevice::ReadOnly)) return false;
        const QByteArray expected = QFile::encodeName(QDir::cleanPath(path));
        while (!mountInfo.atEnd()) {
            const auto fields = mountInfo.readLine().trimmed().split(' ');
            if (fields.size() > 4 && decodeLinuxMountInfoPath(fields.at(4)) == expected)
                return true;
        }
        return false;
    }

    bool cleanupStaleLinuxFuseMount(const QString& mountPath, QString& errorMessage) {
        const QString cleanPath = QDir::cleanPath(mountPath);
        // Never stat a possibly stale FUSE path: if its userspace server is
        // alive but unresponsive, the lookup itself can block indefinitely.
        if (!isLinuxMountPoint(cleanPath)) return true;

        QProcess fusermount;
        fusermount.start("fusermount3", {"-u", "-z", cleanPath});
        if (!fusermount.waitForStarted() || !fusermount.waitForFinished(10000) ||
            fusermount.exitStatus() != QProcess::NormalExit || fusermount.exitCode() != 0) {
            if (fusermount.state() != QProcess::NotRunning) {
                fusermount.kill();
                fusermount.waitForFinished(1000);
            }
            const QString details =
                QString::fromLocal8Bit(fusermount.readAllStandardError()).trimmed();
            errorMessage = QString("A stale FUSE mount exists at %1 and could not be detached%2.")
                               .arg(cleanPath,
                                    details.isEmpty() ? QString() : QString(": %1").arg(details));
            return false;
        }
        if (isLinuxMountPoint(cleanPath)) {
            errorMessage = QString("FUSE mount at %1 is still present after detachment. "
                                   "Import stopped to avoid accessing an unresponsive mount.")
                               .arg(cleanPath);
            return false;
        }
        return true;
    }
#endif

    QString rifeRuntimeRoot() {
        return QDir(QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation))
            .absoluteFilePath("rife-" + QString(RIFE_REVISION));
    }

    std::string avError(int error) {
        char text[AV_ERROR_MAX_STRING_SIZE]{};
        av_strerror(error, text, sizeof(text));
        return text;
    }

    class NutVideoPipe {
    public:
        NutVideoPipe(QProcess& process, QByteArray& diagnostic, int width, int height, double fps)
            : mProcess(process), mDiagnostic(diagnostic),
              mExpectedFrameBytes(static_cast<size_t>(width) * height * 3 * sizeof(uint16_t)) {
            int error = avformat_alloc_output_context2(&mContext, nullptr, "nut", nullptr);
            if (error < 0 || !mContext) throw std::runtime_error("Could not create NUT muxer");
            AVStream* stream = avformat_new_stream(mContext, nullptr);
            if (!stream) throw std::runtime_error("Could not create NUT video stream");
            mStreamIndex = stream->index;
            mStream = stream;
            stream->time_base = {1, 1000000000};
            const AVRational frameRate = av_d2q(fps > 0.0 ? fps : 24.0, 1000000);
            stream->avg_frame_rate = frameRate;
            stream->r_frame_rate = frameRate;
            stream->codecpar->codec_type = AVMEDIA_TYPE_VIDEO;
            stream->codecpar->codec_id = AV_CODEC_ID_RAWVIDEO;
            stream->codecpar->format = AV_PIX_FMT_RGB48LE;
            stream->codecpar->codec_tag = avcodec_pix_fmt_to_codec_tag(AV_PIX_FMT_RGB48LE);
            stream->codecpar->bits_per_coded_sample = 48;
            stream->codecpar->width = width;
            stream->codecpar->height = height;
            if (!stream->codecpar->codec_tag)
                throw std::runtime_error("FFmpeg has no raw-video tag for RGB48LE");
            constexpr int bufferSize = 64 * 1024;
            auto* buffer = static_cast<unsigned char*>(av_malloc(bufferSize));
            if (!buffer) throw std::runtime_error("Could not allocate NUT output buffer");
            mIo = avio_alloc_context(buffer, bufferSize, 1, this, nullptr, &writePacket, nullptr);
            if (!mIo) {
                av_free(buffer);
                throw std::runtime_error("Could not create NUT output stream");
            }
            mContext->pb = mIo;
            mContext->flags |= AVFMT_FLAG_CUSTOM_IO;
            error = avformat_write_header(mContext, nullptr);
            if (error < 0) throw std::runtime_error("Could not write NUT header: " + avError(error));
        }

        ~NutVideoPipe() {
            // finish() is deliberately explicit. During cancellation FFmpeg is
            // already dead, so attempting to write a trailer from this
            // destructor would call writePacket() with a defunct QProcess while
            // the stack is unwinding.
            if (mIo) {
                av_freep(&mIo->buffer);
                avio_context_free(&mIo);
            }
            avformat_free_context(mContext);
        }

        void writeFrame(const std::vector<uint8_t>& rgb, int64_t pts, int64_t duration) {
            if (rgb.size() != mExpectedFrameBytes)
                throw std::runtime_error("Invalid RGB48LE frame size");
            AVPacket* packet = av_packet_alloc();
            if (!packet) throw std::runtime_error("Could not allocate NUT video packet");
            const int error = av_new_packet(packet, static_cast<int>(rgb.size()));
            if (error < 0) {
                av_packet_free(&packet);
                throw std::runtime_error("Could not allocate NUT frame: " + avError(error));
            }
            std::memcpy(packet->data, rgb.data(), rgb.size());
            packet->stream_index = mStreamIndex;
            packet->pts = packet->dts = av_rescale_q(pts, {1, 1000000000}, mStream->time_base);
            packet->duration = std::max<int64_t>(1,
                av_rescale_q(duration, {1, 1000000000}, mStream->time_base));
            packet->flags |= AV_PKT_FLAG_KEY;
            const int writeError = av_write_frame(mContext, packet);
            av_packet_free(&packet);
            if (writeError < 0) {
                mProcess.waitForFinished(500);
                mDiagnostic += mProcess.readAll();
                const QString detail = QString::fromUtf8(mDiagnostic).trimmed().right(4000);
                throw std::runtime_error(("Could not stream NUT frame: " + avError(writeError) +
                    (detail.isEmpty() ? "" : "\n\nFFmpeg:\n" + detail.toStdString())));
            }
        }

        void finish() {
            if (!mFinished) {
                const int error = av_write_trailer(mContext);
                if (error < 0) throw std::runtime_error("Could not finish NUT stream: " + avError(error));
                avio_flush(mIo);
                mFinished = true;
            }
        }

    private:
        static int writePacket(void* opaque,
#if LIBAVFORMAT_VERSION_MAJOR >= 61
                               const uint8_t* data,
#else
                               uint8_t* data,
#endif
                               int size) {
            auto& self = *static_cast<NutVideoPipe*>(opaque);
            self.mDiagnostic += self.mProcess.readAll();
            if (self.mDiagnostic.size() > 8000)
                self.mDiagnostic = self.mDiagnostic.right(4000);
            int offset = 0;
            while (offset < size) {
                if (self.mProcess.state() == QProcess::NotRunning) return AVERROR(EIO);
                const qint64 written = self.mProcess.write(
                    reinterpret_cast<const char*>(data) + offset, size - offset);
                if (written < 0) return AVERROR(EIO);
                offset += static_cast<int>(written);
                while (self.mProcess.bytesToWrite() > 2 * 1024 * 1024) {
                    if (!self.mProcess.waitForBytesWritten(100) &&
                        self.mProcess.state() == QProcess::NotRunning) return AVERROR(EIO);
                    QApplication::processEvents();
                    self.mDiagnostic += self.mProcess.readAll();
                    if (self.mDiagnostic.size() > 8000)
                        self.mDiagnostic = self.mDiagnostic.right(4000);
                }
            }
            return size;
        }

        QProcess& mProcess;
        QByteArray& mDiagnostic;
        AVFormatContext* mContext = nullptr;
        AVIOContext* mIo = nullptr;
        AVStream* mStream = nullptr;
        size_t mExpectedFrameBytes = 0;
        int mStreamIndex = 0;
        bool mFinished = false;
    };
}

motioncam::RenderSettings MainWindow::buildRenderSettings() const {
    motioncam::RenderSettings settings;
    settings.detectDuplicateDngs = mDetectDuplicateDngs;

    // Build options bitfield
    settings.options = motioncam::RENDER_OPT_NONE;

    if(ui->draftModeCheckBox->checkState() == Qt::CheckState::Checked)
        settings.options |= motioncam::RENDER_OPT_DRAFT;

    settings.vignetteCorrection = stringToVignetteCorrectionMode(
        ui->vignetteCorrectionComboBox->currentText().toStdString());
    if(settings.vignetteCorrection == motioncam::VignetteCorrectionMode::Bake)
        settings.options |= motioncam::RENDER_OPT_APPLY_VIGNETTE_CORRECTION;

    if(ui->vignetteOnlyColorCheckBox->checkState() == Qt::CheckState::Checked)
        settings.options |= motioncam::RENDER_OPT_VIGNETTE_ONLY_COLOR;

    if(ui->optimizeGainMapsCheckBox->checkState() == Qt::CheckState::Checked)
        settings.options |= motioncam::RENDER_OPT_OPTIMIZE_GAIN_MAPS;

    if(ui->normalizeGainMapsCheckBox->checkState() == Qt::CheckState::Checked)
        settings.options |= motioncam::RENDER_OPT_NORMALIZE_SHADING_MAP;

    if(ui->debugVignetteCheckBox->checkState() == Qt::CheckState::Checked)
        settings.options |= motioncam::RENDER_OPT_DEBUG_SHADING_MAP;

    if(ui->normalizeExposureCheckBox->checkState() == Qt::CheckState::Checked)
        settings.options |= motioncam::RENDER_OPT_NORMALIZE_EXPOSURE;

    if(ui->smoothExposureCheckBox->checkState() == Qt::CheckState::Checked)
        settings.options |= motioncam::RENDER_OPT_SMOOTH_EXPOSURE;

    if(ui->smoothWhiteBalanceCheckBox->checkState() == Qt::CheckState::Checked)
        settings.options |= motioncam::RENDER_OPT_SMOOTH_WHITE_BALANCE;

    if(ui->bakeIsoCheckBox->checkState() == Qt::CheckState::Checked)
        settings.options |= motioncam::RENDER_OPT_BAKE_ISO;

    if(ui->cfrConversionCheckBox->checkState() == Qt::CheckState::Checked)
        settings.options |= motioncam::RENDER_OPT_FRAMERATE_CONVERSION;

    if(ui->cropEnableCheckBox->checkState() == Qt::CheckState::Checked)
        settings.options |= motioncam::RENDER_OPT_CROPPING;

    if(ui->camModelOverrideCheckBox->checkState() == Qt::CheckState::Checked)
        settings.options |= motioncam::RENDER_OPT_CAMMODEL_OVERRIDE;

    if(ui->logTransformCheckBox->checkState() == Qt::CheckState::Checked)
        settings.options |= motioncam::RENDER_OPT_LOG_TRANSFORM;
    if(ui->logHqCheckBox->isChecked())
        settings.options |= motioncam::RENDER_OPT_LOG_HQ;

    if(ui->remosaicCheckBox->checkState() == Qt::CheckState::Checked)
        settings.options |= motioncam::RENDER_OPT_REMOSAIC_TO_BAYER;

    if(ui->higherCfaHqCheckBox->isChecked())
        settings.options |= motioncam::RENDER_OPT_HIGHER_CFA_HQ;

    if(ui->dngCompressionCheckBox->checkState() == Qt::CheckState::Checked)
        settings.options |= motioncam::RENDER_OPT_JPEG_COMPRESSION;

    // Copy all other settings from member variable
    settings.draftScale =
        ui->draftModeCheckBox->isChecked() ? mRenderSettings.draftScale : 1;
    settings.cfrTarget = mRenderSettings.cfrTarget;
    settings.cropTarget = mRenderSettings.cropTarget;
    settings.cameraModel = mRenderSettings.cameraModel;
    settings.levels = mRenderSettings.levels;
    settings.logTransform = mRenderSettings.logTransform;
    settings.exposureCompensation = mRenderSettings.exposureCompensation;
    settings.quadBayerOption = mRenderSettings.quadBayerOption;
    settings.badPixelTreatment = mRenderSettings.badPixelTreatment;
    settings.cfaPhase = mRenderSettings.cfaPhase;
    settings.jxlDistance = mRenderSettings.jxlDistance;
    settings.orientation = mRenderSettings.orientation;
    settings.ignoreForwardMat = mRenderSettings.ignoreForwardMat;

    return settings;
}

MainWindow::MainWindow(QWidget *parent)
    : QMainWindow(parent)
    , ui(new Ui::MainWindow)
    , mProcessingWatcher(nullptr)
    , mProcessingInProgress(false)
    , mOptionsUpdatePending(false)
#ifdef _WIN32
    , mTaskbarList(nullptr)
#endif
{
    ui->setupUi(this);
    ui->rifeInterpolationCheckBox->setVisible(RIFE_SUPPORTED);
    ui->rifeInterpolationCheckBox->setEnabled(RIFE_SUPPORTED);
    ui->rifeRemoveButton->setVisible(RIFE_SUPPORTED);
    ui->rifeRemoveButton->setEnabled(RIFE_SUPPORTED);
#ifdef __ANDROID__
    connect(qApp, &QGuiApplication::applicationStateChanged, this,
        [this](Qt::ApplicationState state) {
            if (state != Qt::ApplicationActive ||
                mPendingAndroidNoticeMessage.isEmpty()) return;
            const QString title = std::exchange(mPendingAndroidNoticeTitle, {});
            const QString message = std::exchange(mPendingAndroidNoticeMessage, {});
            QTimer::singleShot(0, this, [this, title, message] {
                showAndroidNotice(title, message);
            });
        });
    // Qt 6.9+ reserves the display cutout and rounded corners as a safe area
    // unless the window opts into the expanded client area. Keep the whole
    // QWidget layout in that area as requested for the Android main window.
#if QT_VERSION >= QT_VERSION_CHECK(6, 9, 0)
    setWindowFlag(Qt::ExpandedClientAreaHint);
    setAttribute(Qt::WA_ContentsMarginsRespectsSafeArea, false);
    ui->centralwidget->setAttribute(Qt::WA_ContentsMarginsRespectsSafeArea, false);
    for(auto* widget:ui->centralwidget->findChildren<QWidget*>())
        widget->setAttribute(Qt::WA_ContentsMarginsRespectsSafeArea, false);
#endif
    QMenu* androidSessionMenu = nullptr;
#endif
#ifdef __ANDROID__
    ui->horizontalLayout->setContentsMargins(0,0,0,0);
    ui->verticalLayout_3->setContentsMargins(0,0,0,0);
    ui->verticalLayout_3->setAlignment(Qt::AlignTop);
    ui->verticalLayout_3->setSpacing(2);
    ui->dragAndDropScrollArea->setFrameShape(QFrame::NoFrame);
    ui->dragAndDropScrollArea->setStyleSheet("QScrollArea{border:0;padding:0;margin:0;}");
    ui->verticalSpacer_top->changeSize(0, 0, QSizePolicy::Minimum, QSizePolicy::Fixed);
    ui->verticalSpacer_bottom->changeSize(0, 0, QSizePolicy::Minimum, QSizePolicy::Fixed);
    ui->verticalLayout_3->invalidate();
    ui->dragAndDropLabel->setText(tr("Import a clip to begin"));
    ui->settingsTitle->setText(tr("Settings"));
    ui->preprocessingTitle->setText(tr("Preprocessing"));
    for (auto* title : {ui->settingsTitle, ui->preprocessingTitle}) {
        QFont font = title->font();
        font.setPixelSize(14);
        font.setBold(true);
        title->setFont(font);
    }

    auto* settingsStrip = new QScrollArea(ui->centralwidget);
    settingsStrip->setWidgetResizable(true);
    settingsStrip->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Expanding);
    auto* stripContent = new QWidget(settingsStrip);
    auto* stripLayout = new QHBoxLayout(stripContent);
    stripLayout->setContentsMargins(0, 0, 0, 0);
    stripLayout->setSpacing(0);
    QList<QAbstractScrollArea*> settingsColumns;
    int stripWidth = 0;
    for (auto* column : {ui->globalSettings, ui->globalPreprocessingSettings}) {
        ui->horizontalLayout->removeWidget(column);
        auto* vertical = new QScrollArea(stripContent);
        const int columnWidth = std::max(300, column->sizeHint().width());
        vertical->setFixedWidth(columnWidth);
        stripWidth += columnWidth;
        vertical->setWidgetResizable(true);
        vertical->setWidget(column);
        stripLayout->addWidget(vertical);
        vertical->setFrameShape(QFrame::NoFrame);
        vertical->setStyleSheet("QScrollArea{border:0;padding:0;margin:0;}");
        vertical->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        vertical->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        settingsColumns.append(vertical);
    }
    stripContent->setFixedWidth(stripWidth + stripLayout->spacing());
    settingsStrip->setWidget(stripContent);
    settingsStrip->setFrameShape(QFrame::NoFrame);
    settingsStrip->setStyleSheet("QScrollArea{border:0;padding:0;margin:0;}");
    new motioncam::TouchScrolling(settingsStrip, settingsColumns);
    new motioncam::TouchScrolling(ui->dragAndDropScrollArea);
    // The clip cards include transparent children. Give their scrolling
    // parent an opaque fill so newly exposed pixels are initialized while
    // Android moves the content widget and while the splitter resizes it.
    ui->scrollAreaWidgetContents->setAutoFillBackground(true);
    ui->scrollAreaWidgetContents->setAttribute(Qt::WA_OpaquePaintEvent, false);
    ui->horizontalLayout->removeWidget(ui->dragAndDropScrollArea);
    auto* splitter = new QSplitter(Qt::Horizontal, ui->centralwidget);
    splitter->setObjectName(QStringLiteral("androidMainSplitter"));
    QSettings splitterSettings(PACKAGE_NAME, APP_NAME);
    mAndroidSplitterHorizontal=std::clamp(splitterSettings.value("android/mainSplitterHorizontal",0.5).toDouble(),1.0/6.0,5.0/6.0);
    mAndroidSplitterVertical=std::clamp(splitterSettings.value("android/mainSplitterVertical",0.5).toDouble(),1.0/6.0,5.0/6.0);
    auto* saveSplitter=new QTimer(splitter);
    saveSplitter->setSingleShot(true);
    saveSplitter->setInterval(250);
    connect(splitter,&QSplitter::splitterMoved,this,[this,splitter,saveSplitter]{
        const auto sizes=splitter->sizes();
        if(sizes.size()!=2||sizes[0]+sizes[1]<=0)return;
        auto& fraction=splitter->orientation()==Qt::Horizontal
            ?mAndroidSplitterHorizontal:mAndroidSplitterVertical;
        fraction=double(sizes[0])/(sizes[0]+sizes[1]);
        saveSplitter->start();
    });
    connect(saveSplitter,&QTimer::timeout,this,[this]{
        QSettings settings(PACKAGE_NAME, APP_NAME);
        settings.setValue("android/mainSplitterHorizontal",mAndroidSplitterHorizontal);
        settings.setValue("android/mainSplitterVertical",mAndroidSplitterVertical);
        settings.sync();
    });
    splitter->setHandleWidth(12);
    splitter->setChildrenCollapsible(false);
    splitter->setStyleSheet(
        "QSplitter::handle:horizontal{background:#25282c;border:0;}"
        "QSplitter::handle:vertical{background:#25282c;border:0;}");
    ui->dragAndDropScrollArea->setMinimumWidth(0);
    settingsStrip->setMinimumWidth(0);
    splitter->addWidget(ui->dragAndDropScrollArea);
    splitter->addWidget(settingsStrip);
    ui->horizontalLayout->addWidget(splitter);
    QTimer::singleShot(0, this, [this] {
        updateAndroidOrientationPolicy();
    });
    for (auto* column : {ui->globalSettings, ui->globalPreprocessingSettings}) {
        for (auto* box : column->findChildren<QCheckBox*>()) {
            box->setProperty("androidIndicatorOnly", true);
            box->installEventFilter(this);
        }
        for (auto* combo : column->findChildren<QComboBox*>()) {
            combo->setProperty("androidInlineChoices", true);
            combo->installEventFilter(this);
        }
    }
#endif
    ui->dngCompressionModeComboBox->addItems({
        "JPEG XL Lossless flac", "JPEG XL Lossless opus",
        "HEVC 420 slow 14 flac", "HEVC 420 slow 14 opus",
        "Audio, GCSV, JSON Only"});
    ui->dngCompressionModeComboBox->setToolTip(
        tr("Append flac or opus to a DNG or Camera Native format to encode audio. "
           "Without a suffix, existing audio is copied unchanged. "
           "Audio, GCSV, JSON Only exports audio, gyro data, and per-clip metadata without video."));
    motioncam::warmVulkanGalleryPreview(
        QDir(QStandardPaths::writableLocation(QStandardPaths::CacheLocation))
            .filePath(QStringLiteral("gallery-vulkan-shaders")).toStdString());

#ifdef _WIN32
    // Initialize COM for taskbar progress
    CoInitialize(nullptr);
    CoCreateInstance(CLSID_TaskbarList, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&mTaskbarList));
    if (mTaskbarList) {
        mTaskbarList->HrInit();
    }
#endif

    // Setup processing watcher
    mProcessingWatcher = new QFutureWatcher<void>(this);
    connect(mProcessingWatcher, &QFutureWatcher<void>::finished, this, &MainWindow::onProcessingFinished);

#ifdef _WIN32
    mFuseFilesystem = std::make_unique<motioncam::FuseFileSystemImpl_Win>();
#elif __APPLE__
    mFuseFilesystem = std::make_unique<motioncam::FuseFileSystemImpl_MacOs>();
#elif defined(__ANDROID__)
    mFuseFilesystem = std::make_unique<motioncam::ProcessingFileSystem>();
#elif __linux__
    mFuseFilesystem = std::make_unique<motioncam::FuseFileSystemImpl_Linux>();
#endif

    // Thumbnail and gallery previews can retain a decoder for every imported
    // clip. Release those GPU contexts when playback moves to another
    // application. In-flight renders retain their own shared renderer until
    // the current frame finishes.
    connect(qApp, &QGuiApplication::applicationStateChanged, this,
            [this](Qt::ApplicationState state) {
#ifdef __ANDROID__
                if (state == Qt::ApplicationActive && mPendingAndroidFinalizeCard) {
                    const QString selection=motioncam::finalizeTreeSelectionStatus();
                    if(selection!="choosing"){
                        auto card=mPendingAndroidFinalizeCard;
                        mPendingAndroidFinalizeCard=nullptr;
                        if(selection=="ready")
                            QTimer::singleShot(350,this,[this,card]{
                                if(card)finalizeFile(card);
                            });
                    }
                }
                if (mAndroidExternalJsonCard) {
                    if (state == Qt::ApplicationActive &&
                        mAndroidExternalEditorLeftApp) {
                        auto card = mAndroidExternalJsonCard;
                        mAndroidExternalJsonCard = nullptr;
                        mAndroidExternalEditorLeftApp = false;
                        QTimer::singleShot(200, this, [this, card] {
                            if (card) reloadCalibration(card);
                        });
                    } else if (state != Qt::ApplicationActive) {
                        mAndroidExternalEditorLeftApp = true;
                    }
                }
#endif
                if (state != Qt::ApplicationInactive) return;
                for (const auto& mounted : mMountedFiles)
                    mFuseFilesystem->releasePreviewResources(mounted.mountId);
            });

    // Enable drag and drop on the scroll area
    ui->dragAndDropScrollArea->setAcceptDrops(true);
    ui->dragAndDropScrollArea->installEventFilter(this);

    restoreSettings();
#ifdef __ANDROID__
    auto* importAction = new QAction(tr("Import clip"), this);
    auto* importButton = new QPushButton(tr("Import clip(s)"),
                                         ui->dragAndDropScrollArea->widget());
    auto* importRow = new QHBoxLayout();
    importRow->setContentsMargins(0, 0, 0, 0);
    importRow->setSpacing(4);
    importRow->addWidget(importButton, 1);
    importButton->setMinimumWidth(0);
    auto* importSequenceButton = new QPushButton(tr("Import DNG sequence(s)"),
                                                 ui->dragAndDropScrollArea->widget());
    importSequenceButton->setToolTip(tr("Import a finalized DNG sequence folder"));
    importSequenceButton->setMinimumWidth(0);
    importRow->addWidget(importSequenceButton,1);
    auto* sessionButton = new QPushButton(tr("Session menu"),ui->dragAndDropScrollArea->widget());
    sessionButton->setObjectName(QStringLiteral("sessionActionsButton"));
    sessionButton->setMinimumWidth(0);
    for(auto* button:{importButton,importSequenceButton,sessionButton}){
        button->setStyleSheet("QPushButton{padding:4px 2px;min-width:0;}");
        button->setSizePolicy(QSizePolicy::Ignored,QSizePolicy::Fixed);
    }
    const int importHeight=30;
    for(auto* button:{importButton,importSequenceButton,sessionButton})button->setFixedHeight(importHeight);
    androidSessionMenu = new QMenu(sessionButton);
    androidSessionMenu->setTitle(tr("Session actions"));
    connect(sessionButton,&QPushButton::clicked,this,[this,androidSessionMenu]{
        showAndroidActionMenu(this,androidSessionMenu);
    });
    importRow->addWidget(sessionButton,1);
    ui->verticalLayout_3->insertLayout(0,importRow);
    auto* sidecarButton = new QPushButton(tr("Scan source directories for clip sidecar files"),
                                         ui->dragAndDropScrollArea->widget());
    sidecarButton->setObjectName(QStringLiteral("scanSidecarDirectoryButton"));
    auto* utilityRow=new QHBoxLayout();
    utilityRow->setContentsMargins(0,0,0,0);utilityRow->setSpacing(4);
    utilityRow->addWidget(sidecarButton,1);
    ui->verticalLayout_3->insertLayout(1,utilityRow);
    auto* preferencesButton = new QPushButton(tr("Preferences"),
                                              ui->dragAndDropScrollArea->widget());
    utilityRow->addWidget(preferencesButton,1);
    sidecarButton->setFixedHeight(importHeight);
    preferencesButton->setFixedHeight(importHeight);
    sidecarButton->setStyleSheet("QPushButton{padding:4px 2px;min-width:0;}");
    const int utilityButtonWidth=sidecarButton->sizeHint().width();
    for(auto* button:{sidecarButton,preferencesButton}){
        button->setMinimumWidth(utilityButtonWidth);
        button->setSizePolicy(QSizePolicy::Expanding,QSizePolicy::Fixed);
    }
    connect(preferencesButton, &QPushButton::clicked,
            this, &MainWindow::onOpenPreferences);
    connect(importSequenceButton, &QPushButton::clicked, this, [this] {
        motioncam::requestDngSequenceFolderImport();
        auto* poll = new QTimer(this);
        connect(poll, &QTimer::timeout, this, [this, poll] {
            const QString status=motioncam::dngSequenceFolderImportStatus();
            if(status=="idle"||status=="choosing"||status=="copying")return;
            poll->stop();poll->deleteLater();
            if(status.startsWith("done:")){
                const QStringList paths=status.mid(5).split(QChar('\n'),Qt::SkipEmptyParts);
                for(const auto& path:paths)mountFile(path);
            }
            else if(status.startsWith("error:"))
                showAndroidNotice(tr("DNG folder import failed"),status.mid(6));
        });
        poll->start(250);
    });
    connect(sidecarButton, &QPushButton::clicked, this, [this, sidecarButton] {
        const QString importRoot = QDir(QStandardPaths::writableLocation(
            QStandardPaths::AppLocalDataLocation)).filePath("imports");
        if (mMountedFiles.isEmpty()) {
            QMessageBox::information(this, tr("Scan source directories for clip sidecar files"),
                tr("Import a clip before scanning its source directory."));
            return;
        }
        sidecarButton->setEnabled(false);
        motioncam::scanDirectoryForSidecars(importRoot,
            mPendingAndroidJsonCard
                ? mPendingAndroidJsonCard->property("filePath").toString()
                : QString());
        auto* poll = new QTimer(this);
        connect(poll, &QTimer::timeout, this, [this, poll, sidecarButton] {
            const QString status = motioncam::sidecarScanStatus();
            if (status == "choosing" || status == "scanning" || status == "idle") return;
            poll->stop();poll->deleteLater();sidecarButton->setEnabled(true);
            if (status.startsWith("done:")) {
                for (const auto& mounted : mMountedFiles) {
                    auto* card = fileWidgetForMount(mounted.mountId);
                    if (!card) continue;
                    const QString path = card->property("filePath").toString();
                    const QString location = motioncam::sourceDirectoryForImport(path);
                    if (location.isEmpty()) continue;
                    card->setProperty("sourceDirectory", location);
                    if (auto* label = card->findChild<QLabel*>("sourceDirectoryLabel"))
                        label->setText(tr("Source: %1").arg(location));
                    QSettings(PACKAGE_NAME, APP_NAME).setValue(
                        "android/sourceDirectory/" + QFileInfo(path).dir().dirName(),
                        location);
                }
                if (auto* card = fileWidgetForMount(mMountedFiles.front().mountId))
                    reloadCalibration(card);
                auto pending = mPendingAndroidJsonCard;
                mPendingAndroidJsonCard = nullptr;
                if (pending) {
                    const QString path = pending->property("filePath").toString();
                    if (!motioncam::sourceDirectoryForImport(path).isEmpty()) {
                        QTimer::singleShot(0, this, [this, pending] {
                            if (pending) createCalibrationJson(pending);
                        });
                    } else {
                        QMessageBox::information(this, tr("Clip not found"),
                            tr("The selected directory does not contain this clip."));
                    }
                } else {
                    QMessageBox::information(this, tr("Sidecar scan complete"),
                        tr("Copied %1 sidecar files beside imported clips.")
                            .arg(status.mid(5)));
                }
            } else if (status.startsWith("error:")) {
                mPendingAndroidJsonCard = nullptr;
                QMessageBox::critical(this, tr("Sidecar scan failed"), status.mid(6));
            } else {
                mPendingAndroidJsonCard = nullptr;
            }
        });
        poll->start(250);
    });
    connect(importButton, &QPushButton::clicked, importAction, &QAction::trigger);
    connect(importAction, &QAction::triggered, this, [this] {
        const QStringList sourcePaths = QFileDialog::getOpenFileNames(
            this, tr("Import clip"));
        if (sourcePaths.isEmpty()) return;
        for (const QString& sourcePath : sourcePaths) {
            QFile source(sourcePath);
            if (!source.open(QIODevice::ReadOnly)) {
                showAndroidNotice(tr("Import failed"), source.errorString());
                continue;
            }
            QString name = motioncam::documentDisplayName(sourcePath);
            if (name.isEmpty())
                name = QFileInfo(QUrl(sourcePath).path()).fileName();
            name = QFileInfo(name).fileName();
            const QStringList supportedSuffixes{
                QStringLiteral("mcraw"), QStringLiteral("unspektra"),
                QStringLiteral("dng"), QStringLiteral("mov"),
                QStringLiteral("mp4"), QStringLiteral("mkv")};
            if (!supportedSuffixes.contains(QFileInfo(name).suffix().toLower())) {
                showAndroidNotice(tr("Import failed"),
                    tr("The selected file has no supported clip extension. Its reported name is: %1")
                        .arg(name));
                continue;
            }
            const QString directory = QStandardPaths::writableLocation(
                QStandardPaths::AppLocalDataLocation) + QStringLiteral("/imports/") +
                QUuid::createUuid().toString(QUuid::WithoutBraces);
            if (!QDir().mkpath(directory)) {
                showAndroidNotice(tr("Import failed"),
                    tr("Could not create import directory"));
                continue;
            }
            const QString destination = QDir(directory).filePath(name);
            motioncam::registerImportedClip(destination, sourcePath);
            bool importedByLink = false;
            if (QUrl(sourcePath).scheme() == QStringLiteral("content")) {
                const int descriptor = ::fcntl(source.handle(), F_DUPFD_CLOEXEC, 0);
                if (descriptor >= 0)
                    importedByLink = motioncam::installSeekableSourceLink(
                        destination, descriptor);
            }
            if (!importedByLink) {
                QSaveFile output(destination);
                if (!output.open(QIODevice::WriteOnly)) {
                    showAndroidNotice(tr("Import failed"), output.errorString());
                    continue;
                }
                bool copied = true;
                QByteArray chunk(1024 * 1024, Qt::Uninitialized);
                for (;;) {
                    const qint64 count = source.read(chunk.data(), chunk.size());
                    if (count < 0) {
                        showAndroidNotice(tr("Import failed"), source.errorString());
                        copied = false;
                        break;
                    }
                    if (count == 0) break;
                    if (output.write(chunk.constData(), count) != count) {
                        showAndroidNotice(tr("Import failed"), output.errorString());
                        copied = false;
                        break;
                    }
                }
                if (!copied) {
                    output.cancelWriting();
                    continue;
                }
                if (!output.commit()) {
                    showAndroidNotice(tr("Import failed"), output.errorString());
                    continue;
                }
            }
            mountFile(destination);
            const QString location = motioncam::sourceDirectoryForUri(sourcePath);
            QSettings(PACKAGE_NAME, APP_NAME).setValue(
                "android/sourceDirectory/" + QFileInfo(destination).dir().dirName(),
                location);
            for (const auto& mounted : mMountedFiles) {
                auto* card = fileWidgetForMount(mounted.mountId);
                if (!card || card->property("filePath").toString() != destination) continue;
                card->setProperty("sourceDirectory", location);
                if (auto* label = card->findChild<QLabel*>("sourceDirectoryLabel"))
                    label->setText(tr("Source: %1").arg(location));
                break;
            }
        }
    });
#endif
    mFuseMountingAvailable = projectionDependencyAvailable();
    if (!mFuseMountingAvailable) mFuseMountingEnabled = false;
#ifdef __APPLE__
    if (mFuseMountingEnabled)
        QTimer::singleShot(0, this, &MainWindow::cleanupStaleMacFuseMounts);
#endif
    mGlobalRenderSettings = mRenderSettings;
#ifdef _WIN32
    mFuseFilesystem->setCachePolicy(mCachePolicy);
    mFuseFilesystem->setCacheQuotaBytes(mCacheQuotaBytes);
    mCacheCleanupTimer = new QTimer(this);
    connect(mCacheCleanupTimer, &QTimer::timeout, this, [this] {
        mFuseFilesystem->cleanupCacheExpired();
    });
    if (mCachePolicy == motioncam::CachePolicy::Quota && mCacheCleanupIntervalSeconds > 0)
        mCacheCleanupTimer->start(mCacheCleanupIntervalSeconds * 1000);
#endif

    // Connect to widgets
    connect(ui->draftModeCheckBox, &QCheckBox::checkStateChanged, this, [this](Qt::CheckState state) {
        const QSignalBlocker draftQualitySignals(ui->draftQuality);
        if(state == Qt::CheckState::Checked) {
            ui->draftQuality->setCurrentIndex(0);
            mRenderSettings.draftScale = 2;
        } else {
            ui->draftQuality->setCurrentIndex(-1);
            mRenderSettings.draftScale = 1;
        }
        onRenderSettingsChanged(state);
    });
    connect(ui->vignetteCorrectionComboBox, &QComboBox::currentTextChanged, this,
            [this](const QString&) { onRenderSettingsChanged(Qt::CheckState::Unchecked); });
    connect(ui->normalizeGainMapsCheckBox, &QCheckBox::checkStateChanged, this, &MainWindow::onRenderSettingsChanged);
    connect(ui->debugVignetteCheckBox, &QCheckBox::checkStateChanged, this, &MainWindow::onRenderSettingsChanged);
    connect(ui->vignetteOnlyColorCheckBox, &QCheckBox::checkStateChanged, this, &MainWindow::onRenderSettingsChanged);
    connect(ui->optimizeGainMapsCheckBox, &QCheckBox::checkStateChanged, this, &MainWindow::onRenderSettingsChanged);
    connect(ui->normalizeExposureCheckBox, &QCheckBox::checkStateChanged, this, &MainWindow::onRenderSettingsChanged);
    connect(ui->smoothExposureCheckBox, &QCheckBox::checkStateChanged, this, &MainWindow::onRenderSettingsChanged);
    connect(ui->smoothWhiteBalanceCheckBox, &QCheckBox::checkStateChanged, this, &MainWindow::onRenderSettingsChanged);
    connect(ui->bakeIsoCheckBox, &QCheckBox::checkStateChanged, this, &MainWindow::onRenderSettingsChanged);
    connect(ui->cfrConversionCheckBox, &QCheckBox::checkStateChanged, this, &MainWindow::onRenderSettingsChanged);
    connect(ui->rifeInterpolationCheckBox, &QCheckBox::checkStateChanged, this, [this](Qt::CheckState) { saveSettings(); });
    connect(ui->rifeRemoveButton, &QPushButton::clicked, this, [this] {
        const QString runtimeRoot = rifeRuntimeRoot();
        const QString archivePath = QDir(QStandardPaths::writableLocation(
            QStandardPaths::AppLocalDataLocation)).absoluteFilePath("rife-download.zip");
        if (!QFileInfo::exists(runtimeRoot) && !QFileInfo::exists(archivePath)) {
            QMessageBox::information(this, "Frame interpolation", "No downloaded RIFE runtime was found.");
            return;
        }
        if (QMessageBox::question(this, "Remove frame interpolation runtime",
            "Delete the downloaded RIFE repository, model, and private Python environment?\n\n"
            "They will be downloaded and installed again the next time interpolated finalization is used.",
            QMessageBox::Yes | QMessageBox::No, QMessageBox::No) != QMessageBox::Yes)
            return;
        const bool runtimeRemoved = !QFileInfo::exists(runtimeRoot) || QDir(runtimeRoot).removeRecursively();
        const bool archiveRemoved = !QFileInfo::exists(archivePath) || QFile::remove(archivePath);
        if (!runtimeRemoved || !archiveRemoved)
            QMessageBox::critical(this, "Frame interpolation", "Could not completely remove the RIFE runtime.");
        else
            QMessageBox::information(this, "Frame interpolation", "The RIFE runtime was removed.");
    });
    connect(ui->cropEnableCheckBox, &QCheckBox::checkStateChanged, this, &MainWindow::onRenderSettingsChanged);
    connect(ui->camModelOverrideCheckBox, &QCheckBox::checkStateChanged, this, &MainWindow::onRenderSettingsChanged);
    connect(ui->logTransformCheckBox, &QCheckBox::checkStateChanged, this, &MainWindow::onRenderSettingsChanged);
    connect(ui->logHqCheckBox, &QCheckBox::checkStateChanged, this, &MainWindow::onRenderSettingsChanged);
    connect(ui->remosaicCheckBox, &QCheckBox::checkStateChanged, this, &MainWindow::onRenderSettingsChanged);
    connect(ui->higherCfaHqCheckBox, &QCheckBox::checkStateChanged, this, &MainWindow::onRenderSettingsChanged);
    connect(ui->dngCompressionCheckBox, &QCheckBox::checkStateChanged, this, &MainWindow::onRenderSettingsChanged);
    connect(ui->dngCompressionModeComboBox, &QComboBox::currentTextChanged, this,
            [this](const QString& text) {
        float distance = 0.0f;
        if (dngFinalizeCompressionDistance(text, distance))
            mRenderSettings.jxlDistance = distance;
        onRenderSettingsChanged(Qt::CheckState::Unchecked);
    });
    connect(ui->draftQuality, &QComboBox::currentIndexChanged, this, &MainWindow::onDraftModeQualityChanged);
    connect(ui->cfrTarget, &QComboBox::currentTextChanged, this, [this](const QString& text) {
        onCFRTargetChanged(text.toStdString());
    });
    connect(ui->exposureCompensationLineEdit, &QLineEdit::textChanged, this, [this](const QString& text) {
        onExposureCompensationChanged(text.toStdString());
    });
    connect(ui->cropTargetComboBox, &QComboBox::currentTextChanged, this, [this](const QString& text) {
        onCropTargetChanged(text.toStdString());
    });
    connect(ui->camModelOverrideComboBox, &QComboBox::currentTextChanged, this, [this](const QString& text) {
        onCamModelOverrideChanged(text.toStdString());
    });
    connect(ui->levelsComboBox, &QComboBox::currentTextChanged, this, [this](const QString& text) {
        onLevelsChanged(text.toStdString());
    });
    connect(ui->logTransformComboBox, &QComboBox::currentTextChanged, this, [this](const QString& text) {
        onLogTransformChanged(text.toStdString());
    });
    connect(ui->quadBayerComboBox, &QComboBox::currentTextChanged, this, [this](const QString& text) {
        onQuadBayerChanged(text.toStdString());
    });
    connect(ui->badPixelTreatmentComboBox, &QComboBox::currentTextChanged, this, [this](const QString& text) {
        mRenderSettings.badPixelTreatment = stringToBadPixelTreatment(text.toStdString());
        scheduleOptionsUpdate();
    });
    connect(ui->cfaPhaseComboBox, &QComboBox::currentTextChanged, this, [this](const QString& text) {
        onCfaPhaseChanged(text.toStdString());
    });

    connect(ui->defaultBtn, &QPushButton::clicked, this, &MainWindow::onSetDefaultSettings);

    auto* selectedFrameButtons = new QHBoxLayout();
    mFinalizeSelectedFramesButton = new QPushButton(tr("Finalize Selected Frames"), this);
    mClearSelectedFramesButton = new QPushButton(tr("Clear Selected Frames"), this);
    mSelectAllFramesButton = new QPushButton(tr("Select All Frames"), this);
    auto* selectionEditButtons = new QVBoxLayout();
    selectionEditButtons->setContentsMargins(0, 0, 0, 0);
    selectionEditButtons->addWidget(mClearSelectedFramesButton);
    selectionEditButtons->addWidget(mSelectAllFramesButton);
    selectedFrameButtons->addWidget(mFinalizeSelectedFramesButton);
    selectedFrameButtons->addLayout(selectionEditButtons);
    ui->compressionSection->addLayout(selectedFrameButtons);
    connect(mFinalizeSelectedFramesButton,&QPushButton::clicked,this,&MainWindow::finalizeSelectedFrames);
    connect(mClearSelectedFramesButton,&QPushButton::clicked,this,&MainWindow::clearSelectedFrames);
    connect(mSelectAllFramesButton,&QPushButton::clicked,this,&MainWindow::selectAllFrames);

    ui->defaultSection->removeWidget(ui->defaultBtn);
    auto* applyButtons = new QHBoxLayout();
    mApplySelectedButton = new QPushButton(tr("Apply to Selected"), this);
    mApplyAllButton = new QPushButton(tr("Apply to All"), this);
    mSelectedFilesLabel = new QLabel(tr("0 selected — editing GLOBAL settings"), this);
    mSelectedFilesLabel->setStyleSheet("color:#7f93ad; font-weight:600;");
    mSelectedFilesLabel->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    ui->settingsLayout->removeWidget(ui->settingsTitle);
    auto* settingsHeader = new QHBoxLayout();
    settingsHeader->addWidget(ui->settingsTitle);
    settingsHeader->addStretch();
    settingsHeader->addWidget(mSelectedFilesLabel);
    ui->settingsLayout->insertLayout(0, settingsHeader);
    applyButtons->addWidget(mApplySelectedButton);
    applyButtons->addWidget(mApplyAllButton);
    applyButtons->addWidget(ui->defaultBtn);
    const int applyButtonHeight = qMax(ui->defaultBtn->minimumHeight(),
                                       ui->defaultBtn->sizeHint().height());
    mApplySelectedButton->setFixedHeight(applyButtonHeight);
    mApplyAllButton->setFixedHeight(applyButtonHeight);
    ui->defaultBtn->setFixedHeight(applyButtonHeight);
    ui->preprocessingLayout->addLayout(applyButtons);
    connect(mApplySelectedButton, &QPushButton::clicked, this, &MainWindow::onApplySelected);
    connect(mApplyAllButton, &QPushButton::clicked, this, &MainWindow::onApplyAll);
    mApplySelectedButtonBaseStyle = mApplySelectedButton->styleSheet();
    mApplyAllButtonBaseStyle = mApplyAllButton->styleSheet();
    mApplySelectedButton->setEnabled(false);
    mApplyAllButton->setEnabled(false);
    updateApplyButtonsVisibility();

    auto* fileMenu = menuBar()->addMenu(tr("File"));
    auto* newSession = fileMenu->addAction(tr("New Session"));
    auto* loadSession = fileMenu->addAction(tr("Load Session..."));
    auto* saveSession = fileMenu->addAction(tr("Save Session"));
    auto* saveSessionAs = fileMenu->addAction(tr("Save Session As..."));
    newSession->setShortcut(QKeySequence::New);
    loadSession->setShortcut(QKeySequence::Open);
    saveSession->setShortcut(QKeySequence::Save);
    saveSessionAs->setShortcut(QKeySequence::SaveAs);
    connect(newSession, &QAction::triggered, this, &MainWindow::onNewSession);
    connect(loadSession, &QAction::triggered, this, &MainWindow::onLoadSession);
    connect(saveSession, &QAction::triggered, this, &MainWindow::onSaveSession);
    connect(saveSessionAs, &QAction::triggered, this, &MainWindow::onSaveSessionAs);
    mRecentSessionsMenu = fileMenu->addMenu(tr("Recent Sessions"));
#ifdef __ANDROID__
    androidSessionMenu->addAction(newSession);
    androidSessionMenu->addAction(loadSession);
    androidSessionMenu->addAction(saveSession);
    androidSessionMenu->addAction(saveSessionAs);
    androidSessionMenu->addSeparator();
    androidSessionMenu->addMenu(mRecentSessionsMenu);
#endif
    fileMenu->addSeparator();
    auto* openClip = fileMenu->addAction(tr("Open Clip..."));
    connect(openClip, &QAction::triggered, this, [this] {
        QDialog dialog(this);
        dialog.setWindowTitle(tr("Open Clips"));
        dialog.resize(760, 520);

        auto* layout = new QVBoxLayout(&dialog);
        auto* navigation = new QHBoxLayout();
        auto* upButton = new QPushButton(tr("Up"), &dialog);
        auto* location = new QLineEdit(&dialog);
        navigation->addWidget(upButton);
        navigation->addWidget(location);
        layout->addLayout(navigation);

        auto* model = new QFileSystemModel(&dialog);
        model->setRootPath(QDir::rootPath());
        model->setFilter(QDir::AllEntries | QDir::NoDotAndDotDot);
        auto* browser = new QTreeView(&dialog);
        browser->setModel(model);
        browser->setSelectionMode(QAbstractItemView::ExtendedSelection);
        browser->setRootIsDecorated(false);
        browser->setSortingEnabled(true);
        browser->sortByColumn(0, Qt::AscendingOrder);
        layout->addWidget(browser);
        layout->addWidget(new QLabel(tr("Select clips or folders. MOV/MP4 files must contain NATIVE in the name."), &dialog));

        auto* buttons = new QDialogButtonBox(QDialogButtonBox::Open | QDialogButtonBox::Cancel, &dialog);
        auto* acceptButton = buttons->button(QDialogButtonBox::Open);
        acceptButton->setEnabled(false);
        layout->addWidget(buttons);

        const auto supported = [](const QString& path) {
            const QFileInfo info(path);
            if (info.isDir()) return true;
            const QString name = info.fileName();
            return name.endsWith(".mcraw", Qt::CaseInsensitive) ||
                   name.endsWith(".unspektra", Qt::CaseInsensitive) ||
                   name.endsWith(".7z", Qt::CaseInsensitive) ||
                   name.endsWith(".dng", Qt::CaseInsensitive) ||
                   (name.contains("NATIVE", Qt::CaseInsensitive) &&
                    (name.endsWith(".mov", Qt::CaseInsensitive) ||
                     name.endsWith(".mp4", Qt::CaseInsensitive) ||
                     name.endsWith(".mkv", Qt::CaseInsensitive)));
        };
        const auto selectedPaths = [browser, model] {
            QStringList paths;
            for (const auto& index : browser->selectionModel()->selectedRows(0))
                paths.append(model->filePath(index));
            return paths;
        };
        const auto navigate = [browser, model, location](const QString& path) {
            const QFileInfo info(path);
            if (!info.isDir()) return;
            const QString directory = info.absoluteFilePath();
            browser->selectionModel()->clearSelection();
            browser->setRootIndex(model->index(directory));
            location->setText(directory);
        };
        navigate(QDir::homePath());
        connect(upButton, &QPushButton::clicked, &dialog, [location, navigate] {
            navigate(QDir(location->text()).absoluteFilePath(".."));
        });
        connect(location, &QLineEdit::returnPressed, &dialog, [location, navigate] {
            navigate(location->text());
        });
        connect(browser, &QTreeView::doubleClicked, &dialog, [model, navigate](const QModelIndex& index) {
            if (model->isDir(index)) navigate(model->filePath(index));
        });
        connect(browser->selectionModel(), &QItemSelectionModel::selectionChanged, &dialog,
                [acceptButton, selectedPaths, supported] {
            const auto paths = selectedPaths();
            acceptButton->setEnabled(!paths.isEmpty() &&
                std::all_of(paths.cbegin(), paths.cend(), supported));
        });
        connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
        connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
        if (dialog.exec() == QDialog::Accepted)
            mountFiles(selectedPaths());
    });
#ifdef __APPLE__
    fileMenu->addSeparator();
    auto* forceUnmount = fileMenu->addAction(tr("Force Unmount All"));
    connect(forceUnmount, &QAction::triggered, this, &MainWindow::forceUnmountAllMacFuseMounts);
#endif
    QSettings appSettings(PACKAGE_NAME, APP_NAME);
    mRecentSessions = appSettings.value("recentSessions").toStringList();
    mRecentSessions.removeIf([](const QString& path) { return !QFileInfo::exists(path); });
    updateRecentSessionsMenu();

    auto* settingsMenu = menuBar()->addMenu(tr("Settings"));
    auto* preferencesAction = settingsMenu->addAction(tr("Preferences..."));
    preferencesAction->setShortcut(QKeySequence(QStringLiteral("Ctrl+,")));
    connect(preferencesAction, &QAction::triggered, this, &MainWindow::onOpenPreferences);
#ifdef __ANDROID__
    menuBar()->hide();
#endif

    // Load global calibration.json if it exists
    QString appDir = QCoreApplication::applicationDirPath();
    QString globalCalibPath = QDir(appDir).absoluteFilePath("calibration.json");
    if (QFile::exists(globalCalibPath)) {
        mGlobalCalibration = motioncam::CalibrationData::loadFromFile(globalCalibPath.toStdString());
        if (mGlobalCalibration.has_value()) {
            spdlog::info("Loaded global calibration from: {}", globalCalibPath.toStdString());
        }
    } else {
        // Create example calibration.json
        std::ofstream outFile(globalCalibPath.toStdString());
        if (outFile.is_open()) {
            outFile << motioncam::CalibrationData::createExampleJson();
            outFile.close();
            spdlog::info("Created example calibration.json at: {}", globalCalibPath.toStdString());
        }
    }
}

MainWindow::~MainWindow() {
    QElapsedTimer shutdownTimer;
    shutdownTimer.start();
    if (mGalleryPerformanceTestActive)
        spdlog::info("GALLERY_PERF event=window_shutdown_start");
    if (!mGalleryPerformanceTestActive) autoSaveSession();
    saveSettings();
    ++mGalleryGeneration;
    // The gallery is an independent top-level window, so it is not destroyed
    // automatically with MainWindow. Close it before waiting for its workers.
    if (mClipPlayer) mClipPlayer->closeImmediately();
    // Gallery workers may be waiting for a UI-thread handoff. Keep servicing
    // those cancellation handoffs until every worker has observed shutdown;
    // waiting directly on the UI thread can otherwise deadlock.
    for (;;) {
        bool allFinished = true;
        for (const auto& future : mGalleryTasks.futures())
            allFinished &= future.isFinished();
        if (allFinished) break;
        QApplication::processEvents(QEventLoop::ExcludeUserInputEvents, 20);
        QThread::msleep(1);
    }
    mGalleryTasks.waitForFinished();
    mThumbnailTasks.waitForFinished();

    // Wait for any ongoing processing
    if (mProcessingWatcher && mProcessingWatcher->isRunning()) {
        mProcessingWatcher->waitForFinished();
    }

    const auto archiveMounts = mArchiveTemporaryRoots.keys();
    for (const auto mountId : archiveMounts) {
        mFuseFilesystem->unmount(mountId);
        cleanupArchiveMount(mountId);
    }

    if (mGalleryPerformanceTestActive)
        spdlog::info("GALLERY_PERF event=window_shutdown_complete latency_ms={}",
                     shutdownTimer.elapsed());

#ifdef _WIN32
    if (mTaskbarList) {
        mTaskbarList->Release();
    }
    CoUninitialize();
#endif

    delete ui;
}

void MainWindow::saveSettings() {
    QSettings settings(PACKAGE_NAME, APP_NAME);
#ifdef __ANDROID__
    settings.setValue("android/mainSplitterHorizontal",mAndroidSplitterHorizontal);
    settings.setValue("android/mainSplitterVertical",mAndroidSplitterVertical);
#endif

    settings.setValue("draftMode", ui->draftModeCheckBox->checkState() == Qt::CheckState::Checked);
    settings.setValue("vignetteCorrection", QString::fromStdString(
        vignetteCorrectionModeToString(mRenderSettings.vignetteCorrection)));
    settings.setValue("normalizeGainMaps", ui->normalizeGainMapsCheckBox->isChecked());
    settings.setValue("vignetteOnlyColor", ui->vignetteOnlyColorCheckBox->checkState() == Qt::CheckState::Checked);
    settings.setValue("optimizeGainMaps", ui->optimizeGainMapsCheckBox->isChecked());
    settings.setValue("normalizeExposure", ui->normalizeExposureCheckBox->checkState() == Qt::CheckState::Checked);
    settings.setValue("smoothExposure", ui->smoothExposureCheckBox->checkState() == Qt::CheckState::Checked);
    settings.setValue("smoothWhiteBalance", ui->smoothWhiteBalanceCheckBox->checkState() == Qt::CheckState::Checked);
    settings.setValue("bakeIso", ui->bakeIsoCheckBox->checkState() == Qt::CheckState::Checked);
    settings.setValue("cfrConversion", ui->cfrConversionCheckBox->checkState() == Qt::CheckState::Checked);
    settings.setValue("rifeInterpolation", ui->rifeInterpolationCheckBox->isChecked());
    settings.setValue("detectDuplicateDngs", mDetectDuplicateDngs);
    settings.setValue("cropEnabled", ui->cropEnableCheckBox->checkState() == Qt::CheckState::Checked);
    settings.setValue("camModelOverrideEnabled", ui->camModelOverrideCheckBox->checkState() == Qt::CheckState::Checked);
    settings.setValue("logTransformEnabled", ui->logTransformCheckBox->checkState() == Qt::CheckState::Checked);
    settings.setValue("logHq", ui->logHqCheckBox->isChecked());
    settings.setValue("remosaicEnabled", ui->remosaicCheckBox->isChecked());
    settings.setValue("jpegCompression", ui->dngCompressionCheckBox->checkState() == Qt::CheckState::Checked);
    settings.setValue("jxlDistance", mRenderSettings.jxlDistance);
    const QString compressionMode = ui->dngCompressionModeComboBox->currentText();
    settings.setValue("cameraNativeFinalization", isCameraNativeFormat(compressionMode));
    settings.setValue("cameraNativeMode", compressionMode);
    settings.setValue("higherCfaHq", ui->higherCfaHqCheckBox->isChecked());
    settings.setValue("cachePath", mCacheRootFolder);
    settings.setValue("deleteOnUnmount", mDeleteOnUnmount);
    settings.setValue("cachePolicy", mCachePolicy == motioncam::CachePolicy::Quota ? "quota" : "off");
    settings.setValue("cacheQuotaBytes", static_cast<qulonglong>(mCacheQuotaBytes));
    settings.setValue("cacheCleanupIntervalSeconds", mCacheCleanupIntervalSeconds);
    settings.setValue("autoApplyClipSettings", mAutoApplyClipSettings);
    settings.setValue("galleryFpsIndicatorEnabled", mGalleryFpsIndicatorEnabled);
    settings.setValue("galleryHistogramEnabled", mGalleryHistogramEnabled);
    settings.setValue("galleryClippingEnabled", mGalleryClippingEnabled);
    settings.setValue("unmountOnFinalize", mUnmountOnFinalize);
    settings.setValue("finalizeSelectionToSingleDirectory", mFinalizeSelectionToSingleDirectory);
    settings.setValue("inheritHeroFrameSidecars", mInheritHeroFrameSidecars);
    settings.setValue("fuseMountingEnabled", mFuseMountingEnabled);
    settings.setValue("draftQuality", mRenderSettings.draftScale);
    settings.setValue("cfrTarget", QString::fromStdString(cfrTargetToString(mRenderSettings.cfrTarget)));
    settings.setValue("cropTarget", QString::fromStdString(mRenderSettings.cropTarget));
    settings.setValue("exposureCompensation", QString::fromStdString(mRenderSettings.exposureCompensation));
    settings.setValue("camModelOverride", QString::fromStdString(mRenderSettings.cameraModel));
    settings.setValue("levels", QString::fromStdString(mRenderSettings.levels));
    settings.setValue("logTransform", QString::fromStdString(logTransformModeToString(mRenderSettings.logTransform)));
    settings.setValue("quadBayerOption", QString::fromStdString(quadBayerModeToString(mRenderSettings.quadBayerOption)));
    settings.setValue("badPixelTreatment", QString::fromStdString(badPixelTreatmentToString(mRenderSettings.badPixelTreatment)));
    settings.setValue("cfaPhase", QString::fromStdString(mRenderSettings.cfaPhase));
}

void MainWindow::restoreSettings() {
    QSettings settings(PACKAGE_NAME, APP_NAME);

    ui->draftModeCheckBox->setCheckState(
        settings.value("draftMode").toBool() ? Qt::CheckState::Checked : Qt::CheckState::Unchecked);

    const QString vignetteMode = settings.contains("vignetteCorrection")
        ? settings.value("vignetteCorrection").toString()
        : (!settings.contains("applyVignetteCorrection") ||
           settings.value("applyVignetteCorrection").toBool() ? "Bake" : "Resample");
    mRenderSettings.vignetteCorrection = stringToVignetteCorrectionMode(vignetteMode.toStdString());
    ui->vignetteCorrectionComboBox->setCurrentText(vignetteMode);

    const bool normalizeGainMaps = settings.contains("normalizeGainMaps")
        ? settings.value("normalizeGainMaps").toBool()
        : settings.value("scaleRaw", false).toBool();
    ui->normalizeGainMapsCheckBox->setChecked(normalizeGainMaps);

    ui->vignetteOnlyColorCheckBox->setCheckState(
        !settings.contains("vignetteOnlyColor") ? Qt::CheckState::Checked :
        (settings.value("vignetteOnlyColor").toBool() ? Qt::CheckState::Checked : Qt::CheckState::Unchecked));

    ui->optimizeGainMapsCheckBox->setChecked(settings.value("optimizeGainMaps", false).toBool());

    ui->normalizeExposureCheckBox->setCheckState(
        !settings.contains("normalizeExposure") ? Qt::CheckState::Checked :
        (settings.value("normalizeExposure").toBool() ? Qt::CheckState::Checked : Qt::CheckState::Unchecked));

    ui->smoothExposureCheckBox->setCheckState(
        settings.value("smoothExposure").toBool() ? Qt::CheckState::Checked : Qt::CheckState::Unchecked);
    ui->smoothWhiteBalanceCheckBox->setCheckState(
        settings.value("smoothWhiteBalance").toBool() ? Qt::CheckState::Checked : Qt::CheckState::Unchecked);
    ui->bakeIsoCheckBox->setCheckState(
        settings.value("bakeIso").toBool() ? Qt::CheckState::Checked : Qt::CheckState::Unchecked);

    ui->cfrConversionCheckBox->setCheckState(
        !settings.contains("cfrConversion") ? Qt::CheckState::Checked :
        (settings.value("cfrConversion").toBool() ? Qt::CheckState::Checked : Qt::CheckState::Unchecked));
    ui->rifeInterpolationCheckBox->setChecked(RIFE_SUPPORTED && settings.value("rifeInterpolation", false).toBool());
    mDetectDuplicateDngs = settings.value("detectDuplicateDngs", true).toBool();

    ui->cropEnableCheckBox->setCheckState(
        settings.value("cropEnabled").toBool() ? Qt::CheckState::Checked : Qt::CheckState::Unchecked);

    ui->camModelOverrideCheckBox->setCheckState(
        !settings.contains("camModelOverrideEnabled") ? Qt::CheckState::Checked :
        (settings.value("camModelOverrideEnabled").toBool() ? Qt::CheckState::Checked : Qt::CheckState::Unchecked));

    ui->logTransformCheckBox->setCheckState(
        !settings.contains("logTransformEnabled") ? Qt::CheckState::Checked :
        (settings.value("logTransformEnabled").toBool() ? Qt::CheckState::Checked : Qt::CheckState::Unchecked));
    ui->logHqCheckBox->setChecked(settings.value("logHq", false).toBool());

    ui->remosaicCheckBox->setChecked(settings.value("remosaicEnabled", false).toBool());

    ui->dngCompressionCheckBox->setCheckState(
        settings.value("jpegCompression").toBool() ? Qt::CheckState::Checked : Qt::CheckState::Unchecked);
    mRenderSettings.jxlDistance = settings.value("jxlDistance", -1.0).toFloat();
    int compressionIndex = motioncam::isLossyJpegDct(mRenderSettings.jxlDistance) ? 1
        : mRenderSettings.jxlDistance < 0.0f ? 0
        : mRenderSettings.jxlDistance == 0.0f ? 2 : 3;
    bool restoredNativeMode = false;
    QString restoredNativeText;
    if (settings.contains("cameraNativeMode")) {
        QString savedMode = settings.value("cameraNativeMode").toString();
        if (savedMode.startsWith("Camera Native (") && savedMode.endsWith(')'))
            savedMode = savedMode.mid(15, savedMode.size() - 16);
        if (savedMode == "HEVC") savedMode = "HEVC 420 slow 14";
        if (savedMode == "AV1") savedMode = "AV1 420 3 7";
        if (savedMode == "AV1 HDR + Noise" || savedMode == "AV1 Synth Noise")
            savedMode = "AV1 420 2 12 synth noise";
        if (savedMode == "ProRes Standard") savedMode = "ProRes Std";
        const int savedIndex = ui->dngCompressionModeComboBox->findText(savedMode);
        if (savedIndex >= 4) {
            compressionIndex = savedIndex;
            restoredNativeMode = true;
        } else if (isCameraNativeFormat(savedMode)) {
            restoredNativeText = savedMode;
            restoredNativeMode = true;
        }
    } else if (settings.value("cameraNativeFinalization", false).toBool()) {
        compressionIndex = ui->dngCompressionModeComboBox->findText("HEVC 420 slow 14");
        restoredNativeMode = true;
    }
    if (restoredNativeText.isEmpty())
        ui->dngCompressionModeComboBox->setCurrentIndex(compressionIndex);
    else
        ui->dngCompressionModeComboBox->setCurrentText(restoredNativeText);
    if (!restoredNativeMode && mRenderSettings.jxlDistance > 0.0f &&
        std::abs(mRenderSettings.jxlDistance - 0.1f) > 0.0001f) {
        ui->dngCompressionModeComboBox->setCurrentText(
            QString("JPEG XL DCT %1").arg(mRenderSettings.jxlDistance));
    }
    if (!restoredNativeMode &&
        mRenderSettings.jxlDistance == motioncam::DNG_COMPRESSION_JPEG92_STRIP)
        ui->dngCompressionModeComboBox->setCurrentText("JPEG92 Lossless Strip");
    ui->higherCfaHqCheckBox->setChecked(
        settings.value("higherCfaHq", false).toBool());

    mCacheRootFolder = settings.value("cachePath").toString();
    mDeleteOnUnmount = settings.value("deleteOnUnmount", false).toBool();
    mCachePolicy = settings.value("cachePolicy", "quota").toString() == "off"
        ? motioncam::CachePolicy::Off : motioncam::CachePolicy::Quota;
    mCacheQuotaBytes = settings.value("cacheQuotaBytes",
        QVariant::fromValue<qulonglong>(30ULL * 1024 * 1024 * 1024)).toULongLong();
    mCacheCleanupIntervalSeconds = settings.value("cacheCleanupIntervalSeconds", 30).toInt();
    mAutoApplyClipSettings = settings.value("autoApplyClipSettings", true).toBool();
    mGalleryFpsIndicatorEnabled = settings.value("galleryFpsIndicatorEnabled", true).toBool();
    mGalleryHistogramEnabled = settings.value("galleryHistogramEnabled", false).toBool();
    mGalleryClippingEnabled = settings.value("galleryClippingEnabled", false).toBool();
    mUnmountOnFinalize = settings.value("unmountOnFinalize", true).toBool();
    mFinalizeSelectionToSingleDirectory =
        settings.value("finalizeSelectionToSingleDirectory", false).toBool();
    mInheritHeroFrameSidecars = settings.value("inheritHeroFrameSidecars", false).toBool();
    mFuseMountingEnabled = settings.value("fuseMountingEnabled", true).toBool();
    mRenderSettings.draftScale = std::max(1, settings.value("draftQuality").toInt());
    mRenderSettings.cfrTarget = stringToCFRTarget(!settings.contains("cfrTarget") ? "Prefer Integer" : settings.value("cfrTarget").toString().toStdString());
    mRenderSettings.exposureCompensation = (!settings.contains("exposureCompensation") ? "" : settings.value("exposureCompensation").toString().toStdString());
    mRenderSettings.quadBayerOption = stringToQuadBayerMode(!settings.contains("quadBayerOption") ? "Demosaic" : settings.value("quadBayerOption").toString().toStdString());
    mRenderSettings.badPixelTreatment = stringToBadPixelTreatment(
        settings.value("badPixelTreatment", "Bake").toString().toStdString());
    mRenderSettings.cfaPhase = (!settings.contains("cfaPhase") ? "Don't override CFA" : settings.value("cfaPhase").toString().toStdString());
    mRenderSettings.cropTarget = settings.value("cropTarget").toString().toStdString();
    mRenderSettings.cameraModel = (!settings.contains("camModelOverride") ? "Panasonic" : settings.value("camModelOverride").toString().toStdString());
    mRenderSettings.levels = (!settings.contains("levels") ? "Dynamic" : settings.value("levels").toString().toStdString());
    mRenderSettings.logTransform = stringToLogTransformMode(!settings.contains("logTransform") ? "Keep Input" : settings.value("logTransform").toString().toStdString());

    if(mRenderSettings.draftScale == 2)
        ui->draftQuality->setCurrentIndex(0);
    else if(mRenderSettings.draftScale == 4)
        ui->draftQuality->setCurrentIndex(1);
    else if(mRenderSettings.draftScale == 8)
        ui->draftQuality->setCurrentIndex(2);

    ui->cfrTarget->setCurrentText(QString::fromStdString(cfrTargetToString(mRenderSettings.cfrTarget)));
    ui->exposureCompensationLineEdit->setText(QString::fromStdString(mRenderSettings.exposureCompensation));
    ui->quadBayerComboBox->setCurrentText(QString::fromStdString(
        quadBayerModeToString(mRenderSettings.quadBayerOption)));
    ui->badPixelTreatmentComboBox->setCurrentText(QString::fromStdString(
        badPixelTreatmentToString(mRenderSettings.badPixelTreatment)));
    ui->cfaPhaseComboBox->setCurrentText(QString::fromStdString(mRenderSettings.cfaPhase));
    ui->cropTargetComboBox->setCurrentText(QString::fromStdString(mRenderSettings.cropTarget));
    ui->camModelOverrideComboBox->setCurrentText(QString::fromStdString(mRenderSettings.cameraModel));
    ui->levelsComboBox->setCurrentText(QString::fromStdString(mRenderSettings.levels));
    ui->logTransformComboBox->setCurrentText(QString::fromStdString(logTransformModeToString(mRenderSettings.logTransform)));

    updateUi();
}

void MainWindow::closeEvent(QCloseEvent* event) {
    ++mGalleryGeneration;
    if (mClipPlayer) mClipPlayer->closeImmediately();
    QMainWindow::closeEvent(event);
}

#ifdef __ANDROID__
void MainWindow::resizeEvent(QResizeEvent* event) {
    QMainWindow::resizeEvent(event);
    if (mAndroidComboOverlay) mAndroidComboOverlay->deleteLater();
    if (mClipPlayer && mClipPlayer->parentWidget() == this)
        mClipPlayer->setGeometry(rect());
    QTimer::singleShot(0, this, [this] {
        if (auto* splitter = findChild<QSplitter*>(QStringLiteral("androidMainSplitter"))) {
            auto* clips = ui->dragAndDropScrollArea;
            auto* settings = splitter->widget(1);
            const Qt::Orientation orientation = height() > width()
                ? Qt::Vertical : Qt::Horizontal;
            const bool orientationChanged = splitter->orientation() != orientation;
            if (orientationChanged) splitter->setOrientation(orientation);
            const int extent = orientation == Qt::Vertical ? splitter->height()
                                                           : splitter->width();
            const int available = std::max(2, extent - splitter->handleWidth());
            const int minimum = std::max(1, available / 6);
            if (orientation == Qt::Vertical) {
                clips->setMinimumWidth(0);
                settings->setMinimumWidth(0);
                clips->setMinimumHeight(minimum);
                settings->setMinimumHeight(minimum);
            } else {
                clips->setMinimumHeight(0);
                settings->setMinimumHeight(0);
                clips->setMinimumWidth(minimum);
                settings->setMinimumWidth(minimum);
            }
            const double fraction=orientation==Qt::Horizontal
                ?mAndroidSplitterHorizontal:mAndroidSplitterVertical;
            const int first=std::clamp(qRound(available*fraction),minimum,available-minimum);
            splitter->setSizes({first,available-first});
        }
        if (mSettingsDialog && mSettingsDialog->parentWidget() == ui->centralwidget)
            mSettingsDialog->setGeometry(ui->centralwidget->rect());
        for (const auto& dialog : mTimingDialogs)
            if (dialog && dialog->parentWidget() == ui->centralwidget)
                dialog->setGeometry(ui->centralwidget->rect());
    });
}

void MainWindow::updateAndroidOrientationPolicy() {
    bool landscapeOnly = false;
    if (!(mClipPlayer && mClipPlayer->isVisible())) {
        landscapeOnly = mSettingsDialog && mSettingsDialog->isVisible();
        for (const auto& dialog : mTimingDialogs)
            landscapeOnly |= dialog && dialog->isVisible();
    }
    const auto activity = QNativeInterface::QAndroidApplication::context();
    if (activity.isValid())
        activity.callMethod<void>("setMotionCamLandscapeOnly", landscapeOnly);
}

void MainWindow::showAndroidComboChoices(QComboBox* combo) {
    if (mAndroidComboOverlay) {
        mAndroidComboOverlay->hide();
        mAndroidComboOverlay->deleteLater();
    }
    auto* overlay = new QWidget(ui->centralwidget);
    overlay->setGeometry(ui->centralwidget->rect());
    overlay->setAttribute(Qt::WA_TranslucentBackground);
    overlay->setProperty("androidComboBackdrop", true);
    overlay->installEventFilter(this);
    auto* choices = new QListWidget(overlay);
    choices->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    choices->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    choices->setStyleSheet("QListWidget{background:#25282c;border:1px solid #555;}"
                           "QListWidget::item{padding:8px;color:white;}"
                           "QListWidget::item:selected{background:#3a5878;}");
    for (int i = 0; i < combo->count(); ++i) {
        auto* item = new QListWidgetItem(combo->itemText(i), choices);
        item->setData(Qt::UserRole, i);
    }
    const QPoint anchor = combo->mapTo(ui->centralwidget, QPoint(0, combo->height()));
    const int width = std::min(std::max(combo->width(), 180), overlay->width());
    const int height = std::min(std::max(combo->count() * 44 + 2, 44),
                                std::max(44, overlay->height() - 16));
    const int x = std::clamp(anchor.x(), 0, std::max(0, overlay->width() - width));
    const int y = anchor.y() + height <= overlay->height()
        ? std::max(0, anchor.y())
        : std::max(0, anchor.y() - combo->height() - height);
    choices->setGeometry(x, y, width, height);
    new motioncam::TouchScrolling(choices);
    connect(choices, &QListWidget::itemClicked, overlay,
            [this, combo, overlay](QListWidgetItem* item) {
        combo->setCurrentIndex(item->data(Qt::UserRole).toInt());
        overlay->hide();
        overlay->deleteLater();
    });
    mAndroidComboOverlay = overlay;
    overlay->show();
    overlay->raise();
}
#endif

bool MainWindow::eventFilter(QObject *watched, QEvent *event) {
#ifdef __ANDROID__
    if (watched->property("androidComboBackdrop").toBool() &&
        event->type() == QEvent::MouseButtonRelease) {
        if (auto* overlay = qobject_cast<QWidget*>(watched)) {
            overlay->hide();
            overlay->deleteLater();
        }
        return true;
    }
    if (auto* combo = qobject_cast<QComboBox*>(watched);
        combo && combo->property("androidInlineChoices").toBool() &&
        (event->type() == QEvent::MouseButtonPress ||
         event->type() == QEvent::MouseButtonRelease)) {
        if (event->type() == QEvent::MouseButtonRelease)
            showAndroidComboChoices(combo);
        return true;
    }
    if (auto* box = qobject_cast<QCheckBox*>(watched);
        box && box->property("androidIndicatorOnly").toBool() &&
        (event->type() == QEvent::MouseButtonPress ||
         event->type() == QEvent::MouseButtonRelease ||
         event->type() == QEvent::MouseButtonDblClick)) {
        auto* mouse = static_cast<QMouseEvent*>(event);
        QStyleOptionButton option;
        option.initFrom(box);
        option.text = box->text();
        const QRect indicator = box->style()->subElementRect(
            QStyle::SE_CheckBoxIndicator, &option, box).adjusted(-3, -3, 3, 3);
        if (!indicator.contains(mouse->position().toPoint())) {
            if (event->type() == QEvent::MouseButtonRelease) box->setDown(false);
            event->accept();
            return true;
        }
    }
#endif
    if (watched->property("sourceJsonStatus").toBool() &&
        event->type() == QEvent::MouseButtonDblClick) {
        // Cancel the refresh queued by the first click.  The release following
        // a double-click may still emit QPushButton::clicked, so suppress that
        // signal for the remainder of the platform double-click interval too.
        const int serial = watched->property("refreshClickSerial").toInt() + 1;
        watched->setProperty("refreshClickSerial", serial);
        watched->setProperty("refreshClickPending", false);
        watched->setProperty("suppressRefreshClick", true);
        QPointer<QObject> guardedWatched(watched);
        QTimer::singleShot(QApplication::doubleClickInterval(), this,
            [guardedWatched, serial] {
                if (guardedWatched &&
                    guardedWatched->property("refreshClickSerial").toInt() == serial)
                    guardedWatched->setProperty("suppressRefreshClick", false);
            });
        revealSourceJson(watched->property("sourceJsonPath").toString(),
                         watched->property("sourceClipPath").toString());
        return true;
    }
    if (auto* card = qobject_cast<QWidget*>(watched);
        card && card->property("clipCard").toBool() &&
        event->type() == QEvent::MouseButtonDblClick) {
        playMount(card->property("mountId").toInt());
        return true;
    }
    if (auto* card = qobject_cast<QWidget*>(watched);
        card && card->property("clipCard").toBool() &&
        event->type() == QEvent::MouseButtonRelease) {
#ifndef Q_OS_ANDROID
        if (auto* check = card->findChild<QCheckBox*>("clipSelection"))
            check->toggle();
#endif
        return true;
    }
    if (watched == ui->dragAndDropScrollArea) {
        if (event->type() == QEvent::DragEnter) {
            auto* dragEvent = static_cast<QDragEnterEvent*>(event);
            // On Wayland, reading the URL list can require a data transfer from
            // the drag source. Keep the drag-enter response independent of it.
            if (dragEvent->mimeData()->hasUrls())
                dragEvent->acceptProposedAction();
            return true;
        }
        else if (event->type() == QEvent::DragMove) {
            auto* dragEvent = static_cast<QDragMoveEvent*>(event);
            if (dragEvent->mimeData()->hasUrls())
                dragEvent->acceptProposedAction();
            return true;
        }
        else if (event->type() == QEvent::Drop) {
            auto* dropEvent = static_cast<QDropEvent*>(event);

            if (dropEvent->mimeData()->hasUrls()) {
                const auto urls = dropEvent->mimeData()->urls();

                QStringList filePaths;
                for (const auto& url : urls) {
                    auto filePath = url.toLocalFile();
                    if (filePath.endsWith(".mcraw", Qt::CaseInsensitive) ||
                        filePath.endsWith(".unspektra", Qt::CaseInsensitive) ||
                        filePath.endsWith(".7z", Qt::CaseInsensitive) ||
                        (filePath.contains("NATIVE", Qt::CaseInsensitive) &&
                         (filePath.endsWith(".mov", Qt::CaseInsensitive) ||
                          filePath.endsWith(".mp4", Qt::CaseInsensitive) ||
                          filePath.endsWith(".mkv", Qt::CaseInsensitive))) ||
                        filePath.endsWith(".dng", Qt::CaseInsensitive) ||
                        QFileInfo(filePath).isDir()) {
                        filePaths.append(filePath);
                    }
                }

                if (!filePaths.isEmpty()) {
                    dropEvent->acceptProposedAction();
                    mountFiles(filePaths);
                }
            }

            return true;
        }
    }

    return QMainWindow::eventFilter(watched, event);
}

void MainWindow::mountFile(const QString& filePath) {
#ifdef __ANDROID__
    const QFileInfo requested(filePath);
    if (requested.isSymLink() || !requested.exists())
        motioncam::reopenSeekableSourceLink(filePath);
#endif
    if (filePath.endsWith(".7z", Qt::CaseInsensitive)) {
        std::unique_ptr<QProgressDialog> progress;
#ifndef __ANDROID__
        if (!mImportBatchProgress) {
            progress = std::make_unique<QProgressDialog>(
                tr("Decompressing %1...").arg(QFileInfo(filePath).fileName()),
                QString(), 0, 0, this);
            progress->setWindowModality(Qt::WindowModal);
            progress->setCancelButton(nullptr);
            progress->setMinimumDuration(0);
            progress->show();
        }
#endif
        try {
            auto future = QtConcurrent::run([filePath] {
                return motioncam::extractReviewArchive(filePath);
            });
            while (!future.isFinished()) {
                QApplication::processEvents(QEventLoop::AllEvents, 20);
                QThread::msleep(10);
            }
            const auto extracted = future.result();
            if (progress) progress->close();
            mountFileImpl(extracted.sourcePath, filePath, extracted.temporaryRoot,
                          extracted.sidecarPath, extracted.gyroflowSidecarPath);
        } catch (const motioncam::InsufficientArchiveSpace& error) {
            if (progress) progress->close();
            mCancelImportBatch = mImportBatchActive;
            QMessageBox::critical(this, tr("Not enough temporary space"),
                                  QString::fromUtf8(error.what()));
        } catch (const std::exception& error) {
            if (progress) progress->close();
            QMessageBox::critical(this, tr("Archive import failed"),
                                  QString::fromUtf8(error.what()));
        }
        return;
    }
    mountFileImpl(filePath, filePath);
}

void MainWindow::mountFileImpl(const QString& filePath, const QString& importPath,
                               const QString& temporaryRoot,
                               const QString& sidecarPath,
                               const QString& gyroflowSidecarPath) {
#ifdef __ANDROID__
    const QString sidecarSyncError = motioncam::syncSidecarsForImport(filePath);
    if (!sidecarSyncError.isEmpty())
        spdlog::warn("{}", sidecarSyncError.toStdString());
#endif
    QElapsedTimer mountTimer;
    mountTimer.start();
    const int importIndex = mMountedFiles.size();
    if (mGalleryPerformanceTestActive)
        spdlog::info("GALLERY_PERF event=clip_import_start clip={} path={}",
                     importIndex, filePath.toStdString());
    spdlog::info("Mount timing [{}]: request started", filePath.toStdString());
    const QString normalizedPath = QFileInfo(importPath).absoluteFilePath();
    for (const auto& mounted : mMountedFiles)
        if (QFileInfo(mounted.importFile).absoluteFilePath() == normalizedPath) {
            if (!temporaryRoot.isEmpty()) QDir(temporaryRoot).removeRecursively();
            return;
        }
    if (mMountInProgress) {
        if (mMountPathInProgress == normalizedPath) {
            if (!temporaryRoot.isEmpty()) QDir(temporaryRoot).removeRecursively();
            return;
        }
        QTimer::singleShot(200, this, [this, importPath] { mountFile(importPath); });
        if (!temporaryRoot.isEmpty()) QDir(temporaryRoot).removeRecursively();
        return;
    }
    mMountInProgress = true;
    mMountPathInProgress = normalizedPath;
    // Extract just the filename from the path
    QFileInfo fileInfo(importPath);
    const QFileInfo stagedFileInfo(filePath);
    auto fileName = fileInfo.fileName();
    QString destinationRoot = mCacheRootFolder;
#ifdef __APPLE__
    if (destinationRoot.isEmpty()) {
        destinationRoot = QDir(QDir::homePath()).filePath("Mounts/MotionCamFuse");
    }
#else
    if (destinationRoot.isEmpty()) {
        destinationRoot = fileInfo.path();
    }
#endif
    // A sequence directory cannot be mounted onto itself: doing so hides the
    // source DNGs and makes every projected read recursively enter FUSE.
    const QString mountName = stagedFileInfo.isDir()
        ? fileInfo.fileName() + "-mounted"
        : fileInfo.baseName();
    auto dstPath = destinationRoot + "/" + mountName;
    motioncam::MountId mountId;

    std::unique_ptr<QProgressDialog> singleProgress;
    QProgressDialog* mountProgress = mImportBatchProgress.data();
#ifndef __ANDROID__
    if (!mountProgress) {
        singleProgress = std::make_unique<QProgressDialog>(
            tr("Reading and importing %1...").arg(fileInfo.fileName()), QString(), 0, 0, this);
        mountProgress = singleProgress.get();
        mountProgress->setWindowModality(Qt::WindowModal);
        mountProgress->setCancelButton(nullptr);
        mountProgress->setMinimumDuration(0);
        mountProgress->show();
        QApplication::processEvents();
    }
#endif
    QString mountError;
    auto settings = buildRenderSettings();
    settings.sourceSidecarPath = sidecarPath.toStdString();
    settings.sourceGyroflowSidecarPath = gyroflowSidecarPath.toStdString();
#ifdef __APPLE__
    if (mFuseMountingEnabled) cleanupStaleMacFuseMounts();
#elif defined(__linux__) && !defined(__ANDROID__)
    const qint64 cleanupStarted = mountTimer.elapsed();
    if (mFuseMountingEnabled && !cleanupStaleLinuxFuseMount(dstPath, mountError)) {
        if (singleProgress) mountProgress->close();
        mMountInProgress = false;
        mMountPathInProgress.clear();
        if (!temporaryRoot.isEmpty()) QDir(temporaryRoot).removeRecursively();
        QMessageBox::critical(this, tr("Error"), mountError);
        return;
    }
    spdlog::info("Mount timing [{}]: stale-mount check {:.1f} ms",
                 filePath.toStdString(),
                 static_cast<double>(mountTimer.elapsed() - cleanupStarted));
#endif
    const qint64 backendStarted = mountTimer.elapsed();
    if (mGalleryPerformanceTestActive)
        spdlog::info("GALLERY_PERF event=fuse_startup_start clip={} path={}",
                     importIndex, filePath.toStdString());
    const bool projectFiles = mFuseMountingEnabled;
    auto future = QtConcurrent::run([this, settings, filePath, dstPath, projectFiles, &mountError] {
        try {
            return mFuseFilesystem->mount(
                settings, filePath.toStdString(), dstPath.toStdString(), projectFiles);
        } catch (const std::exception& error) {
            mountError = QString::fromUtf8(error.what());
            spdlog::error("Mount failed [{}]: {}", filePath.toStdString(), error.what());
            return motioncam::InvalidMountId;
        }
    });
    while (!future.isFinished()) {
        QApplication::processEvents(QEventLoop::AllEvents, 20);
        QThread::msleep(10);
    }
    if (singleProgress) mountProgress->close();
    mountId = future.result();
    const qint64 fuseElapsed = mountTimer.elapsed() - backendStarted;
    if (mGalleryPerformanceTestActive)
        spdlog::info(
            "GALLERY_PERF event=fuse_startup_complete clip={} mount={} success={} latency_ms={}",
            importIndex, mountId, mountId != motioncam::InvalidMountId,
            fuseElapsed);
    spdlog::info("Mount timing [{}]: backend mount {:.1f} ms (total {:.1f} ms)",
                 filePath.toStdString(),
                 static_cast<double>(mountTimer.elapsed() - backendStarted),
                 static_cast<double>(mountTimer.elapsed()));
    mMountInProgress = false;
    mMountPathInProgress.clear();
    if (mountId == motioncam::InvalidMountId) {
#ifdef __ANDROID__
        motioncam::releaseSeekableSourceLink(importPath);
#endif
        if (!temporaryRoot.isEmpty()) QDir(temporaryRoot).removeRecursively();
        QMessageBox::critical(this, "Error",
            QString("There was an error importing the file. (error: %1)").arg(mountError));
        return;
    }

    // Get the scroll area's content widget and its layout
    auto* scrollContent = ui->dragAndDropScrollArea->widget();
    auto* scrollLayout = qobject_cast<QVBoxLayout*>(scrollContent->layout());

    // Create a widget to hold a filename label and buttons
    auto* fileWidget = new QWidget(scrollContent);

#ifdef __ANDROID__
    fileWidget->setMinimumHeight(156);
    fileWidget->setSizePolicy(QSizePolicy::Expanding,QSizePolicy::Fixed);
#else
    fileWidget->setFixedHeight(156);
#endif
    fileWidget->setProperty("filePath", filePath);
    fileWidget->setProperty("importPath", importPath);
    fileWidget->setProperty("sidecarPath", sidecarPath);
    fileWidget->setProperty("gyroflowSidecarPath", gyroflowSidecarPath);
    fileWidget->setProperty("mountId", mountId);
    fileWidget->setProperty("mountPath", dstPath);
    fileWidget->setProperty("projectedFiles", projectFiles);
    fileWidget->setObjectName(QStringLiteral("clipCard"));
    fileWidget->setProperty("clipCard", true);
    fileWidget->setCursor(Qt::PointingHandCursor);
    fileWidget->installEventFilter(this);

    auto* cardLayout = new QHBoxLayout(fileWidget);
    cardLayout->setContentsMargins(8, 8, 8, 8);
    cardLayout->setSpacing(10);

    auto* thumbnailContainer = new QWidget(fileWidget);
    thumbnailContainer->setObjectName(QStringLiteral("thumbnailContainer"));
    thumbnailContainer->setFixedSize(176, 140);
    thumbnailContainer->setAttribute(Qt::WA_TransparentForMouseEvents, true);
    auto* thumbnailLayout = new QHBoxLayout(thumbnailContainer);
    thumbnailLayout->setContentsMargins(0, 0, 0, 0);
    thumbnailLayout->setAlignment(Qt::AlignCenter);

    auto* thumbnailLabel = new QLabel(tr("Loading..."), thumbnailContainer);
    thumbnailLabel->setObjectName(QStringLiteral("thumbnailLabel"));
    thumbnailLabel->setFixedSize(176, 140);
    thumbnailLabel->setAlignment(Qt::AlignCenter);
    thumbnailLabel->setStyleSheet("background:#1a1a1a; border:1px solid #333;");
    thumbnailLayout->addWidget(thumbnailLabel);
    cardLayout->addWidget(thumbnailContainer, 0, Qt::AlignVCenter);

    auto* fileLayout = new QVBoxLayout();
    fileLayout->setContentsMargins(0, 2, 0, 2);
    fileLayout->setSpacing(4);
    cardLayout->addLayout(fileLayout, 1);

    // Create and add the filename label
    auto* fileLabel = new QLabel(fileInfo.baseName(), fileWidget);
    fileLabel->setToolTip(filePath); // Show full path on hover
#ifdef __ANDROID__
    fileLabel->setStyleSheet("font-weight: bold; font-size: 13pt;");
#else
    fileLabel->setStyleSheet("font-weight: bold; font-size: 12pt;");
#endif
    auto* titleLayout = new QHBoxLayout();
    titleLayout->setContentsMargins(0, 0, 0, 0);
    titleLayout->addWidget(fileLabel, 1);
    auto* localBadge = new QLabel(tr("LOCAL"), fileWidget);
    localBadge->setObjectName(QStringLiteral("localBadge"));
    localBadge->setStyleSheet("color:#f1c65a; border:1px solid #f1c65a; border-radius:3px; padding:1px 5px; font-weight:700;");
    localBadge->hide();
    auto* globalBadge = new QLabel(tr("GLOBAL"), fileWidget);
    globalBadge->setObjectName(QStringLiteral("globalBadge"));
    globalBadge->setStyleSheet("color:#a4b1c2; border:1px solid #46566b; border-radius:3px; padding:1px 5px; font-weight:600;");
    auto* localReset = new QPushButton(tr("×"), fileWidget);
    localReset->setObjectName(QStringLiteral("localReset"));
    localReset->setToolTip(tr("Reset local settings to global"));
    localReset->setFixedSize(20, 20);
    localReset->hide();
    titleLayout->addWidget(localReset);
    titleLayout->addWidget(localBadge);
    titleLayout->addWidget(globalBadge);
    fileLayout->addLayout(titleLayout);

    // The selection controls share only the metadata rows, leaving the title
    // and action rows free to reach the card's true right edge.
    auto* metadataLayout = new QHBoxLayout();
    metadataLayout->setContentsMargins(0, 0, 0, 0);
    metadataLayout->setSpacing(4);
    auto* metadataTextLayout = new QVBoxLayout();
    metadataTextLayout->setContentsMargins(0, 0, 0, 0);
    metadataTextLayout->setSpacing(0);
    metadataTextLayout->setAlignment(Qt::AlignTop);
    metadataLayout->addLayout(metadataTextLayout, 1);
    fileLayout->addLayout(metadataLayout);

    // Get file information from the FUSE filesystem
    auto fileInfoOpt = mFuseFilesystem->getFileInfo(mountId);
    if (fileInfoOpt.has_value()) {
        auto info = fileInfoOpt.value();

        auto* infoLabel1 = new QLabel(clipInfoFirstLine(info), fileWidget);
        styleClipMetadata(infoLabel1);
        infoLabel1->setFixedHeight(infoLabel1->fontMetrics().height() + 2);
        infoLabel1->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
        infoLabel1->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);
        infoLabel1->setProperty("infoLabel1", true);
        infoLabel1->setProperty("mountId", QVariant(mountId));
        metadataTextLayout->addWidget(infoLabel1);

        // Second row: FPS info and frame counts
        auto infoText2 = QString("<span style='color: #888888;'>Median / Average / Target FPS: %1 / %2 -> </span>"
                                 "<span style='color: white;'>%3</span>"
                                 "<span style='color: #888888;'> | Framecount: %4 | Dropped: -%5 | Duplicated: +%6</span>")
                                .arg(QString::number(info.frameRateInfo.medianFrameRate, 'f', 2))
                                .arg(QString::number(info.frameRateInfo.averageFrameRate, 'f', 2))
                                .arg(QString::number(info.fps, 'f', 2))
                                .arg(info.totalFrames)
                                .arg(info.droppedFrames)
                                .arg(info.duplicatedFrames);

        auto* infoLabel2 = new QLabel(infoText2, fileWidget);
        styleClipMetadata(infoLabel2);
        infoLabel2->setFixedHeight(infoLabel2->fontMetrics().height() + 4);
        infoLabel2->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
        infoLabel2->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);
        infoLabel2->setProperty("infoLabel2", true);
        infoLabel2->setProperty("mountId", QVariant(mountId));
        metadataTextLayout->addWidget(infoLabel2);
    }

    auto* missingMetadataLabel = new QLabel(fileWidget);
    missingMetadataLabel->setObjectName(QStringLiteral("missingMetadataLabel"));
    styleClipMetadata(missingMetadataLabel,QStringLiteral("color:#e3a35b;"));
    missingMetadataLabel->setFixedHeight(missingMetadataLabel->fontMetrics().height() + 1);
    missingMetadataLabel->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    metadataTextLayout->addWidget(missingMetadataLabel);

    // Create and add the source folder label
    QString sourceDirectory = fileInfo.path();
#ifdef __ANDROID__
    sourceDirectory = QSettings(PACKAGE_NAME, APP_NAME).value(
        "android/sourceDirectory/" + fileInfo.dir().dirName(),
        sourceDirectory).toString();
#endif
    auto* sourceLabel = new QLabel(QString("Source: %1").arg(sourceDirectory), fileWidget);
    sourceLabel->setObjectName(QStringLiteral("sourceDirectoryLabel"));
    fileWidget->setProperty("sourceDirectory", sourceDirectory);
    styleClipMetadata(sourceLabel,QStringLiteral("color:#666666;"));
    sourceLabel->setFixedHeight(sourceLabel->fontMetrics().height() + 1);
    sourceLabel->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
    metadataTextLayout->addWidget(sourceLabel);

    // Add spacer to maintain button position
    fileLayout->addSpacing(4);

    // Create horizontal layout for buttons
    auto* buttonLayout = new QHBoxLayout();
    buttonLayout->setContentsMargins(0, 0, 0, 0);
    buttonLayout->setSpacing(8);

    // Define consistent button size
    const int buttonWidth = 100;
    const int buttonHeight = 30;

    // Create and add the open button
    auto* openButton = new QPushButton("Open", fileWidget);
    openButton->setObjectName(QStringLiteral("openDestinationButton"));
    openButton->setEnabled(projectFiles);
    openButton->setVisible(projectFiles);
    openButton->setFixedSize(buttonWidth, buttonHeight);
    openButton->setIcon(QIcon(":/assets/folder_btn.png"));
    buttonLayout->addWidget(openButton);

    // Create and add the play button
    auto* playButton = new QPushButton("Play", fileWidget);
    playButton->setFixedSize(buttonWidth, buttonHeight);
    playButton->setIcon(QIcon(":/assets/play_btn.png"));
    buttonLayout->addWidget(playButton);

    // Create and add the remove button
    auto* removeButton = new QPushButton("Unmount", fileWidget);
    removeButton->setObjectName(QStringLiteral("removeClipButton"));
    if (!projectFiles) removeButton->setText(tr("Remove"));
    removeButton->setFixedSize(buttonWidth, buttonHeight);
    removeButton->setIcon(QIcon(":/assets/remove_btn.png"));
    buttonLayout->addWidget(removeButton);

    // Create and add the finalize button
    auto* finalizeButton = new QPushButton("Finalize", fileWidget);
    finalizeButton->setFixedSize(buttonWidth, buttonHeight);
    finalizeButton->setToolTip("Render all frames to disk with compression (if enabled)");
    finalizeButton->setEnabled(true);
    buttonLayout->addWidget(finalizeButton);

    auto* timingButton = new QPushButton(fileWidget);
    timingButton->setObjectName(QStringLiteral("frameTimingButton"));
    timingButton->setFixedSize(buttonHeight, buttonHeight);
    timingButton->setToolTip(tr("Show frame timing graph"));
    timingButton->setFlat(true);
    QPixmap timingPixmap(22, 22);
    timingPixmap.fill(Qt::transparent);
    {
        QPainter iconPainter(&timingPixmap);
        iconPainter.setRenderHint(QPainter::Antialiasing);
        iconPainter.setPen(QPen(QColor("#8fb7e8"), 1.7));
        iconPainter.drawPolyline(QPolygonF({{1, 16}, {5, 10}, {9, 14}, {14, 4}, {18, 9}, {21, 2}}));
        iconPainter.setPen(QPen(QColor("#64788f"), 1));
        iconPainter.drawLine(1, 20, 21, 20);
    }
    timingButton->setIcon(QIcon(timingPixmap));
    buttonLayout->addWidget(timingButton, 0, Qt::AlignVCenter);

    // Keep calibration controls flush against the right side of the action row.
    buttonLayout->addStretch();
    auto* calibButton = new QPushButton("Create JSON", fileWidget);
    calibButton->setFixedSize(buttonWidth, buttonHeight);
    calibButton->setProperty("calibButton", true);
    buttonLayout->addWidget(calibButton);

    // Create a container widget for status label and refresh button overlay
    auto* statusContainer = new QWidget(fileWidget);
    statusContainer->setObjectName(QStringLiteral("statusContainer"));
    statusContainer->setProperty("statusContainer", true);
    statusContainer->setFixedHeight(buttonHeight);
    statusContainer->setStyleSheet(QStringLiteral("background:transparent;"));

    // Create calibration status label (initially hidden)
    auto* calibStatusLabel = new QLabel("", statusContainer);
    styleClipMetadata(calibStatusLabel,QStringLiteral("font-weight:bold;"));
    calibStatusLabel->setProperty("calibStatusLabel", true);
    calibStatusLabel->setVisible(false);
    calibStatusLabel->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);

    // Create invisible refresh button on top of the status label
    auto* refreshButton = new QPushButton("", statusContainer);
    refreshButton->setStyleSheet("background: transparent; border: none;");
    refreshButton->setCursor(Qt::PointingHandCursor);
    refreshButton->setProperty("refreshButton", true);
    refreshButton->setProperty("sourceJsonStatus", true);
    refreshButton->setProperty("sourceJsonPath", sidecarPath);
    refreshButton->installEventFilter(this);
    refreshButton->setVisible(false);
    refreshButton->setToolTip(tr("Click to refresh calibration; double-click to show the source JSON"));

    buttonLayout->addWidget(statusContainer);

    // Connect refresh button to update calibration
    connect(refreshButton, &QPushButton::clicked, this,
            [this, fileWidget, refreshButton] {
        if (refreshButton->property("suppressRefreshClick").toBool()) return;
        if (refreshButton->property("refreshClickPending").toBool()) {
            refreshButton->setProperty("refreshClickPending", false);
            refreshButton->setProperty("refreshClickSerial",
                refreshButton->property("refreshClickSerial").toInt() + 1);
            revealSourceJson(refreshButton->property("sourceJsonPath").toString(),
                             refreshButton->property("sourceClipPath").toString());
            return;
        }
        refreshButton->setProperty("refreshClickPending", true);
        const int serial = refreshButton->property("refreshClickSerial").toInt() + 1;
        refreshButton->setProperty("refreshClickSerial", serial);
        QPointer<QPushButton> guardedButton(refreshButton);
        QPointer<QWidget> guardedFileWidget(fileWidget);
        QTimer::singleShot(QApplication::doubleClickInterval(), this,
            [this, guardedButton, guardedFileWidget, serial] {
                if (!guardedButton || !guardedFileWidget ||
                    guardedButton->property("refreshClickSerial").toInt() != serial)
                    return;
                guardedButton->setProperty("refreshClickPending", false);
                reloadCalibration(guardedFileWidget);
            });
    });

    // Add button layout to main layout
    fileLayout->addLayout(buttonLayout);
    fileLayout->addSpacing(6);

    auto* clipControls = new QVBoxLayout();
    clipControls->setContentsMargins(0, 0, 0, 0);
    clipControls->setSpacing(4);
    clipControls->addStretch();
    auto* clipCheckBox = new QCheckBox(fileWidget);
    clipCheckBox->setObjectName(QStringLiteral("clipSelection"));
    clipCheckBox->setToolTip(tr("Select this clip for per-clip settings"));
    clipCheckBox->setStyleSheet(QStringLiteral("background:transparent;"));
    clipControls->addWidget(clipCheckBox, 0, Qt::AlignHCenter);
    auto* clipNumber = new QLabel(QString::number(mMountedFiles.size() + 1), fileWidget);
    clipNumber->setObjectName(QStringLiteral("indexLabel"));
    styleClipMetadata(clipNumber,QStringLiteral("color:#7f93ad;"));
    clipNumber->setAlignment(Qt::AlignCenter);
    clipControls->addWidget(clipNumber, 0, Qt::AlignHCenter);
    clipControls->addStretch();
    metadataLayout->addLayout(clipControls);

    connect(timingButton, &QPushButton::clicked, this, [this, mountId, fileInfo] {
        const auto info = mFuseFilesystem->getFileInfo(mountId);
        if (!info || !info->presentationTimestamps ||
            info->presentationTimestamps->size() < 2 ||
            info->timingTimeBaseNum <= 0 || info->timingTimeBaseDen <= 0) {
            QMessageBox::information(this, tr("Frame timing"),
                                     tr("This clip has no usable presentation timestamps."));
            return;
        }
        auto* dialog = mTimingDialogs.value(mountId).data();
        if (!dialog) {
            dialog = new FrameTimingDialog(
                fileInfo.baseName(), *info->presentationTimestamps,
                info->timingTimeBaseNum, info->timingTimeBaseDen, info->fps,
                info->timingUsesCfrMapping, info->frameRateInfo,
#ifdef __ANDROID__
                ui->centralwidget);
#else
                this);
#endif
#ifdef __ANDROID__
            dialog->setWindowFlags(Qt::Widget);
            dialog->setAutoFillBackground(true);
            new motioncam::PageCrossfade(dialog);
#endif
            mTimingDialogs.insert(mountId, dialog);
            connect(dialog, &QObject::destroyed, this, [this, mountId] {
                mTimingDialogs.remove(mountId);
#ifdef __ANDROID__
                updateAndroidOrientationPolicy();
#endif
            });
        } else {
            dialog->updateTiming(*info->presentationTimestamps,
                                 info->timingTimeBaseNum, info->timingTimeBaseDen,
                                 info->fps, info->timingUsesCfrMapping,
                                 info->frameRateInfo);
        }
#ifdef __ANDROID__
        dialog->setGeometry(ui->centralwidget->rect());
        motioncam::PageCrossfade::show(dialog);
        updateAndroidOrientationPolicy();
#else
        dialog->show();
#endif
        dialog->raise();
        dialog->activateWindow();
    });

    connect(clipCheckBox, &QCheckBox::toggled, this, [this, fileWidget, mountId](bool selected) {
        if (selected) mSelectedMountIds.insert(mountId);
        else mSelectedMountIds.remove(mountId);
        fileWidget->setProperty("selected", selected);
        fileWidget->setStyleSheet(selected
            ? "QWidget#clipCard { background:#223246; border:1px solid #3a5878; border-radius:6px; }"
              "QWidget#clipCard QLabel { background:transparent; }"
              "QWidget#clipCard QWidget#thumbnailContainer { background:#223246; }"
              "QWidget#clipCard QWidget#statusContainer, QWidget#clipCard QCheckBox#clipSelection { background:transparent; }"
            : "QWidget#clipCard { background:transparent; border:1px solid transparent; border-radius:6px; }"
              "QWidget#clipCard QLabel { background:transparent; }"
              "QWidget#clipCard QWidget#thumbnailContainer { background:transparent; }"
              "QWidget#clipCard QWidget#statusContainer, QWidget#clipCard QCheckBox#clipSelection { background:transparent; }");
        updateSelectionUi();
    });
    connect(localReset, &QPushButton::clicked, this, [this, mountId, clipCheckBox] {
        mLocalSettings.remove(mountId);
        mFuseFilesystem->updateOptions(mountId, settingsForMount(mountId));
        updateFpsLabels();
        updateThumbnail(mountId);
        updateLocalBadge(mountId);
        // The checkbox owns the selection state and refreshes the settings UI.
        const bool wasSelected = clipCheckBox->isChecked();
        clipCheckBox->setChecked(false);
        if (!wasSelected) updateSelectionUi();
        autoSaveSession();
    });

    // Dynamic clip cards live before the designer's stretch/empty-state
    // items. Append at the end of that dynamic section so visual order matches
    // import/session order.
    int insertionIndex = 0;
    while (insertionIndex < scrollLayout->count()) {
        auto* widget = scrollLayout->itemAt(insertionIndex)->widget();
        if (!widget || (!widget->property("clipCard").toBool() &&
                        !(qobject_cast<QFrame*>(widget) &&
                          qobject_cast<QFrame*>(widget)->frameShape() == QFrame::HLine)))
            break;
        ++insertionIndex;
    }

    // Add separator if there are already mounted files
    if (!mMountedFiles.empty()) {
        auto* separator = new QFrame(scrollContent);

        separator->setFrameShape(QFrame::HLine);
        separator->setFrameShadow(QFrame::Plain);
        separator->setLineWidth(1);
        separator->setStyleSheet("QFrame { color: #e0e0e0; margin: 16px 0px; }");
        scrollLayout->insertWidget(insertionIndex++, separator);
    }

    // Add the file widget to the scroll area
    scrollLayout->insertWidget(insertionIndex, fileWidget);
    if (mImportBatchActive) {
        QTimer::singleShot(0, this, [this] {
            auto* area=ui->dragAndDropScrollArea;
            if(area->widget()&&area->widget()->layout())area->widget()->layout()->activate();
            auto* bar=area->verticalScrollBar();bar->setValue(bar->maximum());
            // Scroll ranges may receive one more geometry update after the
            // card's images/labels settle. Follow that final range as well.
            QTimer::singleShot(0,this,[this]{
                auto* finalBar=ui->dragAndDropScrollArea->verticalScrollBar();
                finalBar->setValue(finalBar->maximum());
            });
        });
    }
    updateClipIndices();
    if (mImportBatchActive &&
        (!mGalleryPerformanceTestActive || mPerformanceThumbnailRun)) {
        mDeferredImportThumbnails.append(mountId);
    } else if (!mGalleryPerformanceTestActive) {
        QTimer::singleShot(250, this, [this, mountId] { updateThumbnail(mountId); });
    } else if (mPerformanceThumbnailRun) {
        spdlog::info("GALLERY_PERF event=thumbnail_start mount={}", mountId);
        updateThumbnail(mountId);
    }

    for (auto* label : fileWidget->findChildren<QLabel*>())
        label->setAttribute(Qt::WA_TransparentForMouseEvents, true);

    // Hide the drag-drop label since we now have content
    ui->dragAndDropLabel->hide();

    // Connect buttons
    connect(openButton, &QPushButton::clicked, this, [this, fileWidget] {
        openMountedDirectory(fileWidget);
    });

    connect(playButton, &QPushButton::clicked, this, [this, mountId] {
        playMount(mountId);
    });

    connect(removeButton, &QPushButton::clicked, this, [this, fileWidget] {
        removeFile(fileWidget);
    });

    connect(finalizeButton, &QPushButton::clicked, this, [this, fileWidget] {
        finalizeFile(fileWidget);
    });

    connect(calibButton, &QPushButton::clicked, this, [this, fileWidget] {
        createCalibrationJson(fileWidget);
    });

    mMountedFiles.append(
        motioncam::MountedFile(mountId, filePath, importPath));
    if (!temporaryRoot.isEmpty()) mArchiveTemporaryRoots.insert(mountId, temporaryRoot);
    // Session loading writes the complete session after the batch. Rewriting
    // it for every imported clip adds quadratic JSON/file-system work and is
    // especially visible in automated multi-clip diagnostics.
    if (!mGalleryPerformanceTestActive && !mImportBatchActive) autoSaveSession();

    spdlog::info("Mount timing [{}]: UI/card/session complete {:.1f} ms",
                 filePath.toStdString(), static_cast<double>(mountTimer.elapsed()));
    if (mGalleryPerformanceTestActive) {
        const qint64 importElapsed = mountTimer.elapsed();
        spdlog::info(
            "GALLERY_PERF event=clip_import_complete clip={} mount={} latency_ms={} pre_fuse_ms={} fuse_ms={} post_fuse_ms={}",
            importIndex, mountId, importElapsed, backendStarted,
            fuseElapsed, importElapsed - backendStarted - fuseElapsed);
    }

    // During a batch, update the newly visible card immediately without
    // repeatedly reparsing the sidecars of every previously imported clip.
    updateCalibrationButtonStates(mImportBatchActive ? fileWidget : nullptr);
}

void MainWindow::mountFiles(
        const QStringList& filePaths,
        const std::function<void(const motioncam::MountedFile&)>& mounted) {
    if (filePaths.isEmpty()) return;
    if (filePaths.size() == 1) {
        const int before = mMountedFiles.size();
        mountFile(filePaths.front());
        if (mounted && mMountedFiles.size() > before)
            mounted(mMountedFiles.back());
        return;
    }

    mImportBatchActive = true;
    mCancelImportBatch = false;
#ifndef __ANDROID__
    QProgressDialog progress(
        tr("Importing %1 clips...").arg(filePaths.size()),
        QString(), 0, filePaths.size(), this);
    progress.setWindowModality(Qt::WindowModal);
    progress.setCancelButton(nullptr);
    progress.setMinimumDuration(0);
    progress.setAutoClose(false);
    progress.setAutoReset(false);
    if (auto* bar = progress.findChild<QProgressBar*>())
        bar->setFormat(tr("0 / %1").arg(filePaths.size()));
    mImportBatchProgress = &progress;
    progress.show();
    QApplication::processEvents();
#else
    mImportBatchProgress.clear();
#endif

    int imported = 0;
    for (int processed = 0; processed < filePaths.size(); ++processed) {
        const int before = mMountedFiles.size();
        mountFile(filePaths.at(processed));
        if (mMountedFiles.size() > before) {
            ++imported;
            if (mounted) mounted(mMountedFiles.back());
        }
#ifndef __ANDROID__
        progress.setValue(processed + 1);
        if (auto* bar = progress.findChild<QProgressBar*>())
            bar->setFormat(tr("%1 / %2").arg(imported).arg(filePaths.size()));
#endif
        QApplication::processEvents();
        if (mCancelImportBatch) break;
    }

    mImportBatchProgress.clear();
    mImportBatchActive = false;
    mCancelImportBatch = false;
#ifndef __ANDROID__
    progress.close();
#endif
    updateCalibrationButtonStates();
    if (!mGalleryPerformanceTestActive) autoSaveSession();
    const auto thumbnails = std::move(mDeferredImportThumbnails);
    mDeferredImportThumbnails.clear();
    for (const auto mountId : thumbnails) {
        if (mGalleryPerformanceTestActive)
            spdlog::info("GALLERY_PERF event=thumbnail_start mount={}", mountId);
        updateThumbnail(mountId);
    }
}

motioncam::RenderSettings MainWindow::settingsForMount(motioncam::MountId mountId) const {
    auto settings = mLocalSettings.value(mountId, mGlobalRenderSettings);
    // Source sidecars can be routed somewhere other than the conventional
    // path (for example when a clip was opened from an archive).  These paths
    // were previously attached only to the settings used for the initial
    // mount.  Preview renderers are rebuilt independently, so always carry
    // the per-card routing in every settings snapshot used for thumbnails and
    // gallery playback.
    if (const auto* fileWidget = fileWidgetForMount(mountId)) {
        settings.sourceSidecarPath =
            fileWidget->property("sidecarPath").toString().toStdString();
        settings.sourceGyroflowSidecarPath =
            fileWidget->property("gyroflowSidecarPath").toString().toStdString();
    }
    for (const auto& mounted : mMountedFiles) {
        if (mounted.mountId != mountId) continue;
        const auto mountedInfo = mFuseFilesystem->getFileInfo(mountId);
        if (mountedInfo && mountedInfo->sidecarState >= 0) {
            if (mountedInfo->orientation >= 0)
                settings.orientation = mountedInfo->orientation;
            if (mountedInfo->hasIgnoreForwardMatOverride)
                settings.ignoreForwardMat = mountedInfo->ignoreForwardMatOverride;
            break;
        }
        const auto sidecarPath = motioncam::vfs::sidecarPath(mounted.srcFile.toStdString());
        if (boost::filesystem::exists(sidecarPath)) {
            const auto sidecar = motioncam::CalibrationData::loadFromFile(sidecarPath.string());
            if (sidecar) {
                if (sidecar->hasOrientation) settings.orientation = sidecar->orientation;
                if (sidecar->hasIgnoreForwardMat) settings.ignoreForwardMat = sidecar->ignoreForwardMat;
            }
        }
        break;
    }
    return settings;
}

void MainWindow::startGalleryRender(motioncam::MountId mountId, double startSeconds,
                                    bool backfillThumbnails) {
    auto settings = previewRenderSettings(settingsForMount(mountId));
    const auto info = mFuseFilesystem->getFileInfo(mountId);
    if (!info) return;
    if (!mClipPlayer) return;
    // Gallery output follows the user's render settings. In particular, full
    // resolution remains the default and only the explicit proxy/draft option
    // reduces source resolution for faster playback.
    const auto generation = ++mGalleryGeneration;
    const double previewFps = info->fps > 0.0f ? info->fps : 24.0f;
    const bool isSequence = info->isSequence;
    const int thumbnailOrientation = isSequence
        ? (settings.orientation >= 0 ? settings.orientation : info->orientation) : -1;
    if (!isSequence) settings.orientation = -1;
    const int playbackFrames=std::max(1,info->totalFrames-info->droppedFrames+
        info->duplicatedFrames);
    const size_t playbackFirstFrame = static_cast<size_t>(std::clamp(
        std::round(startSeconds * previewFps), 0.0,
        static_cast<double>(playbackFrames-1)));
    const auto visibleRange=backfillThumbnails?mClipPlayer->visibleThumbnailOutputRange():QPair<int,int>{0,0};
    const size_t firstFrame = backfillThumbnails?size_t(visibleRange.first):playbackFirstFrame;
    QPointer<ClipPlayerDialog> player(mClipPlayer);
    const auto playbackTarget=mClipPlayer->playbackTarget();
    const auto incomingFrame=mClipPlayer->incomingFrame();
    const auto thumbnailCollectionEnabled=mClipPlayer->thumbnailCollectionEnabled();
    const auto thumbnailPrecacheEnabled=mClipPlayer->thumbnailPrecacheEnabled();
    const bool diagnostics = mGalleryPerformanceTestActive;
    const bool clippingEnabled = mGalleryClippingEnabled;
    const bool histogramEnabled = mClipPlayer->histogramEnabled() && !backfillThumbnails;
    const bool nativePresenter = mClipPlayer->nativeVulkanPresenter();
    const qreal thumbnailPixelRatio=mClipPlayer->devicePixelRatioF();
    const auto thumbnailRenderBounds=mClipPlayer->thumbnailRenderBounds();
    const bool directPresenter = mClipPlayer->directSequencePresentation();
    auto render = [this, mountId, settings, generation, firstFrame, visibleRange, playbackFrames, isSequence,
                   thumbnailOrientation,
                   backfillThumbnails, player, playbackTarget, incomingFrame,
                   thumbnailCollectionEnabled, thumbnailPrecacheEnabled,
                   diagnostics, clippingEnabled,
                   histogramEnabled, nativePresenter, directPresenter, thumbnailPixelRatio, thumbnailRenderBounds] {
        const auto taskStarted = std::chrono::steady_clock::now();
        size_t deliveredFrames = 0;
        size_t thumbnailFrames = 0;
        double displayDecodeMs = 0.0;
        double deliveryWaitMs = 0.0;
        if (diagnostics)
            spdlog::info("GALLERY_PERF event=gallery_render_task_start mount={} generation={} first_frame={}",
                         mountId, generation, firstFrame);
        const auto handoffStarted = std::chrono::steady_clock::now();
        std::unique_lock gallerySlot(mGalleryRenderMutex);
        const double handoffMs = std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - handoffStarted).count();
        if (mGalleryGeneration.load() != generation) {
            if (diagnostics)
                spdlog::info(
                    "GALLERY_PERF event=gallery_render_task_cancelled mount={} generation={} stage=handoff wait_ms={:.3f} total_ms={:.3f}",
                    mountId, generation, handoffMs,
                    std::chrono::duration<double, std::milli>(
                        std::chrono::steady_clock::now() - taskStarted).count());
            return;
        }
        if (diagnostics && handoffMs >= 1.0)
            spdlog::info(
                "GALLERY_PERF event=gallery_render_handoff mount={} generation={} wait_ms={:.3f}",
                mountId, generation, handoffMs);
        try {
            motioncam::PreviewOptions options;
            options.firstFrame = firstFrame;
            options.clippingEnabled = clippingEnabled;
            options.retainSourceSamples = histogramEnabled;
            options.skipFrame = [playbackTarget,incomingFrame,backfillThumbnails](size_t frame) {
                if(backfillThumbnails){
                    incomingFrame->store(static_cast<int>(frame));return false;
                }
                const bool skip=frame<static_cast<size_t>(std::max(0,playbackTarget->load()));
                if(!skip)incomingFrame->store(static_cast<int>(frame));
                return skip;
            };
            bool presentedFirstFrame = false;
            auto receiveFrame =
                [this, generation, player, mountId, settings, isSequence, incomingFrame, thumbnailCollectionEnabled,
                 thumbnailPixelRatio, thumbnailRenderBounds,
                 thumbnailPrecacheEnabled,
                 thumbnailOrientation,
                 clippingEnabled, directPresenter,
                 backfillThumbnails, &presentedFirstFrame,
                 &deliveredFrames, &thumbnailFrames, &displayDecodeMs, &deliveryWaitMs,
                 &taskStarted, diagnostics, nativePresenter](
                    motioncam::PreviewFrame&& preview) {
                    // A materialization already in progress may complete after
                    // the gallery closes. Do not touch the dialog or process
                    // that frame once its preview generation is cancelled.
                    if (mGalleryGeneration.load() != generation) return;
                    const auto decodeStarted = std::chrono::steady_clock::now();
                    const bool collectThumbnail = thumbnailCollectionEnabled->load();
                    const int rawOrientation = !isSequence
                        ? normalizedGalleryOrientation(preview.metadata.orientation) : 0;
                    // Direct presentation displays RGB888. Keep its output
                    // packed while collecting thumbnails, but preserve the
                    // full playback depth for FFmpeg sequences.
                    const bool rgb24 = (preview.displayBitDepth <= 8 ||
                        (collectThumbnail && directPresenter)) &&
                        !qEnvironmentVariableIsSet("MOTIONCAM_GALLERY_PERF_FORCE_RGB48");
                    const bool hashPreview =
                        qEnvironmentVariableIsSet("MOTIONCAM_GALLERY_PERF_HASH_PREVIEW");
                    motioncam::VulkanGalleryOutput gpuOutput;
                    gpuOutput.requireZeroCopy =
                        qEnvironmentVariableIsSet("MOTIONCAM_GALLERY_REQUIRE_ZERO_COPY");
                    gpuOutput.width = preview.width;
                    gpuOutput.height = preview.height;
                    // Direct presentation can use the GPU's rotated RGB24
                    // rows directly. FFmpeg expects source-oriented rows and
                    // performs its own rotation in the video filter graph.
                    gpuOutput.orientation = isSequence
                        ? (directPresenter && rgb24 && thumbnailOrientation >= 0
                            ? thumbnailOrientation : 0)
                        : (rawOrientation < 0 ? 0 : rawOrientation);
                    gpuOutput.clipping = clippingEnabled ? &preview.clipping : nullptr;
                    gpuOutput.hostPixels = &preview.galleryDisplayPixels;
                    // The GPU widget can remain allocated after switching to
                    // an FFmpeg sequence. That sequence still needs host rows.
                    const bool nativeGpuOutput=nativePresenter&&
                        (!isSequence||directPresenter)&&!hashPreview;
#ifdef Q_OS_ANDROID
                    constexpr bool cacheLiveThumbnail=false;
#else
                    const bool cacheLiveThumbnail=isSequence&&directPresenter&&
                        !backfillThumbnails&&thumbnailPrecacheEnabled->load();
#endif
                    gpuOutput.rgb24 = rgb24 || nativeGpuOutput;
                    std::vector<uint8_t> nativeThumbnail;
                    int nativeThumbnailFrame=-1;
                    if(cacheLiveThumbnail||collectThumbnail){
                        gpuOutput.thumbnailOrientation=isSequence
                            ?(thumbnailOrientation<0?0:thumbnailOrientation)
                            :(rawOrientation<0?0:rawOrientation);
                        const bool swap=gpuOutput.thumbnailOrientation==90||
                            gpuOutput.thumbnailOrientation==270;
                        const uint32_t orientedWidth=swap?preview.height:preview.width;
                        const uint32_t orientedHeight=swap?preview.width:preview.height;
                        // Fit in logical pixels first, exactly as the shared item
                        // delegate does, then generate at native display resolution.
                        // No per-frame smooth resize is needed on the UI thread.
                        const QSize sourceSize(orientedWidth,orientedHeight);
                        const QSize requested=sourceSize.scaled(
                            thumbnailRenderBounds->load(std::memory_order_relaxed),Qt::KeepAspectRatio)*thumbnailPixelRatio;
                        const QSize thumbnailSize=sourceSize.scaled(
                            requested.boundedTo(sourceSize),Qt::KeepAspectRatio);
                        gpuOutput.thumbnailWidth=std::max(1,thumbnailSize.width());
                        gpuOutput.thumbnailHeight=std::max(1,thumbnailSize.height());
                        gpuOutput.thumbnailPixels=&nativeThumbnail;
                        gpuOutput.thumbnailGeneration=generation;
                        gpuOutput.thumbnailSourceFrame=
                            static_cast<int>(preview.sourceFrameIndex);
                        // Paused work has no presentation deadline and must
                        // deliver its last thumbnail without a later frame.
                        if(!backfillThumbnails)
                            gpuOutput.completedThumbnailSourceFrame=&nativeThumbnailFrame;
                    }
                    if(nativeGpuOutput){
                        gpuOutput.gpuBuffer = &preview.galleryDisplayGpu;
                        // Match the CPU gallery's clockwise 90-degree pixel
                        // mapping. Rotating this case in the presenter mirrors
                        // the image horizontally on some Vulkan drivers.
                        gpuOutput.orientation=isSequence
                            ?(thumbnailOrientation==90?90:0)
                            :(rawOrientation<0?0:rawOrientation);
                        gpuOutput.rgb10a2=true;
                    }
                    std::vector<uint8_t> thumbnailSource;
                    if (clippingEnabled && collectThumbnail && !hashPreview)
                        thumbnailSource = preview.rgb;
                    bool gpuFinal = false;
                    applyGalleryPreviewColorTransform(
                        preview, settings.ignoreForwardMat,
                        gainMapOnlyDebug(settings),
                        hashPreview ? nullptr : &gpuOutput, &gpuFinal);
                    // Own the producing device immediately: cancellation or
                    // thumbnail fallback can exit before the playback handoff.
                    if(auto& frame=preview.galleryDisplayGpu;frame){
                        frame->sourceLease = std::make_shared<std::pair<std::shared_ptr<void>,std::shared_ptr<void>>>(
                            std::move(preview.galleryStateLease),std::move(preview.gallerySource.deviceLease));
                    }
                    const auto colorFinished = std::chrono::steady_clock::now();
                    if (hashPreview) gpuFinal = false;
                    const bool gpuPreOriented = gpuFinal && isSequence &&
                        gpuOutput.orientation != 0;
                    if (hashPreview) {
                        uint64_t hash = 1469598103934665603ull;
                        uint64_t sampledSum = 0, sampledWhite = 0, sampledCount = 0;
                        for (uint8_t value : preview.rgb) {
                            hash ^= value;
                            hash *= 1099511628211ull;
                        }
                        for (size_t index = 0; index + 1 < preview.rgb.size(); index += 2048) {
                            uint16_t code;
                            std::memcpy(&code, preview.rgb.data() + index, sizeof(code));
                            sampledSum += code;
                            sampledWhite += code >= 64000;
                            ++sampledCount;
                        }
                        spdlog::info("GALLERY_PERF event=gallery_rgb_hash mount={} generation={} timestamp={} hash={}",
                            mountId, generation, preview.timestamp, hash);
                        spdlog::info("GALLERY_PERF event=gallery_rgb_sample mean={} near_white={}/{}",
                            sampledCount ? double(sampledSum) / sampledCount : 0.0,
                            sampledWhite, sampledCount);
                    }
                    if(!gpuFinal&&clippingEnabled)
                        applyGalleryClipping(preview.rgb,preview.clipping);
                    QImage thumbnailImage;
                    if(!nativeThumbnail.empty()&&nativeThumbnail.size()>=
                       size_t(gpuOutput.thumbnailWidth)*gpuOutput.thumbnailHeight*3)
                        thumbnailImage=QImage(nativeThumbnail.data(),
                            static_cast<int>(gpuOutput.thumbnailWidth),
                            static_cast<int>(gpuOutput.thumbnailHeight),
                            static_cast<int>(gpuOutput.thumbnailWidth)*3,
                            QImage::Format_RGB888).copy();
                    if(cacheLiveThumbnail&&!gpuFinal&&thumbnailImage.isNull()&&
                       !preview.rgb.empty())
                        thumbnailImage=ClipPlayerDialog::makeRgb48Thumbnail(
                            preview.rgb.data(),static_cast<qsizetype>(preview.rgb.size()),
                            static_cast<int>(preview.width),
                            static_cast<int>(preview.height),thumbnailOrientation,thumbnailPixelRatio);
                    bool canBuildCpuThumbnail=true;
#ifdef Q_OS_ANDROID
                    // The GPU path produces thumbnails asynchronously. Do not
                    // launch a second full-frame transform/readback while its
                    // first result is still in flight.
                    canBuildCpuThumbnail=!nativeGpuOutput||!gpuFinal;
#endif
                    if ((clippingEnabled || nativeGpuOutput) && collectThumbnail &&
                        thumbnailImage.isNull()&&canBuildCpuThumbnail) {
                        if (gpuFinal)
                            applyGalleryPreviewColorTransform(preview, settings.ignoreForwardMat,
                                gainMapOnlyDebug(settings), nullptr, nullptr, &thumbnailSource);
                        if(gpuFinal&&clippingEnabled)
                            applyGalleryClipping(thumbnailSource,preview.clipping);
                        const auto& pixels = gpuFinal ? thumbnailSource : preview.rgb;
                        thumbnailImage = ClipPlayerDialog::makeRgb48Thumbnail(
                            pixels.data(), static_cast<qsizetype>(pixels.size()),
                            static_cast<int>(preview.width), static_cast<int>(preview.height),
                            isSequence ? thumbnailOrientation : rawOrientation,thumbnailPixelRatio);
                    }
                    preview.galleryCfa = {};
                    preview.galleryCfaBytes = {};
                    preview.gallerySource = {};
                    if (!gpuFinal) {
                        if (rgb24)
                            packGalleryRgb24(preview.rgb);
                    }
                    if (!isSequence) {
                        const int orientation = rawOrientation;
                        if (orientation == 90 || orientation == 180 || orientation == 270) {
                            const uint32_t sourceWidth = preview.width;
                            const uint32_t sourceHeight = preview.height;
                            if (!gpuFinal)
                                preview.rgb = rotateInterleaved(
                                    preview.rgb, sourceWidth, sourceHeight,
                                    rgb24 ? 3 : 6, orientation);
                            if (orientation == 90 || orientation == 270)
                                std::swap(preview.width, preview.height);
                        }
                        preview.metadata.orientation = 0;
                    }
                    const uint32_t width = preview.width, height = preview.height;
                    displayDecodeMs += std::chrono::duration<double, std::milli>(
                        std::chrono::steady_clock::now() - decodeStarted).count();
                    if (mGalleryGeneration.load() != generation) return;
                    const auto deliveryStarted = std::chrono::steady_clock::now();
                    // Keep the processed frame alive across queued thumbnail
                    // delivery without copying its full RGB48 payload. The
                    // playback handoff below is synchronous, so the view also
                    // remains valid while Qt and FFmpeg consume it.
                    auto rgb = std::make_shared<std::vector<uint8_t>>(
                        std::move(preview.rgb));
                    auto rgbPixels = std::move(preview.galleryDisplayPixels);
                    auto gpuBuffer = std::move(preview.galleryDisplayGpu);
                    if (gpuBuffer){
                        gpuBuffer->sourceFrameIndex=static_cast<int>(preview.sourceFrameIndex);
                        gpuBuffer->orientation=isSequence
                            ?(thumbnailOrientation==90?0:thumbnailOrientation):0;
                        gpuBuffer->staticFrame=!isSequence;
                        if (gpuBuffer->rejectSharedImage) {
                            auto reject = std::move(gpuBuffer->rejectSharedImage);
                            const bool strictZeroCopy = gpuOutput.requireZeroCopy;
                            gpuBuffer->rejectSharedImage = [this, generation, player,
                                    reject = std::move(reject), strictZeroCopy] {
                                if (!reject()) return false;
                                QMetaObject::invokeMethod(this, [this, generation, player, strictZeroCopy] {
                                    if (mGalleryGeneration.load() != generation || !player) return;
                                    if (strictZeroCopy)
                                        player->failRgb48Frames(QStringLiteral(
                                            "Shared Vulkan image import failed in strict zero-copy mode"));
                                    else
                                        player->reloadCurrentClip();
                                }, Qt::QueuedConnection);
                                return true;
                            };
                        }
                    }
                    const auto* rgbData = gpuBuffer ? nullptr
                        : rgbPixels ? rgbPixels.get() : rgb->data();
                    const size_t rgbSize = gpuBuffer ? 0u : rgbPixels
                        ? static_cast<size_t>(width) * height * 3u : rgb->size();
                    const QByteArray bytes = QByteArray::fromRawData(
                        reinterpret_cast<const char*>(rgbData),
                        static_cast<qsizetype>(rgbSize));
                    const int thumbnailOutputFrame=nativeThumbnailFrame>=0
                        ?nativeThumbnailFrame:static_cast<int>(preview.sourceFrameIndex);
                    if((collectThumbnail || cacheLiveThumbnail) &&
                       !thumbnailImage.isNull()){
                        ++thumbnailFrames;
                        QMetaObject::invokeMethod(this,[this,player,mountId,generation,
                                                        thumbnailCollectionEnabled,cacheLiveThumbnail,thumbnailImage,
                                                        thumbnailOutputFrame]{
                            if(mGalleryGeneration.load()==generation&&player&&player==mClipPlayer&&
                               player->currentMountId()==mountId&&
                               (thumbnailCollectionEnabled->load()||cacheLiveThumbnail))
                                player->setOutputFrameThumbnailImage(
                                    thumbnailOutputFrame,thumbnailImage,
                                    cacheLiveThumbnail);
                        },Qt::QueuedConnection);
                    } else if(collectThumbnail && !clippingEnabled &&
                             rgbData&&rgbSize>0){
                        ++thumbnailFrames;
                        QMetaObject::invokeMethod(this,[this,player,mountId,generation,
                                                        thumbnailCollectionEnabled,rgb,rgbPixels,rgbData,rgbSize,width,height,
                                                        thumbnailOutputFrame,gpuPreOriented,
                                                        thumbnailOrientation,thumbnailPixelRatio]{
                            if(mGalleryGeneration.load()==generation&&player&&player==mClipPlayer&&
                               player->currentMountId()==mountId&&thumbnailCollectionEnabled->load()) {
                                const QByteArray view = QByteArray::fromRawData(
                                    reinterpret_cast<const char*>(rgbData),
                                    static_cast<qsizetype>(rgbSize));
                                if (gpuPreOriented) {
                                    const bool swap = thumbnailOrientation == 90 ||
                                        thumbnailOrientation == 270;
                                    player->setOutputFrameThumbnailImage(thumbnailOutputFrame,
                                        ClipPlayerDialog::makeRgb48Thumbnail(
                                            rgbData, static_cast<qsizetype>(rgbSize),
                                            static_cast<int>(swap ? height : width),
                                            static_cast<int>(swap ? width : height), 0,thumbnailPixelRatio));
                                } else {
                                    player->setOutputFrameThumbnail(thumbnailOutputFrame,view,
                                        static_cast<int>(width),static_cast<int>(height));
                                }
                            }
                        },Qt::QueuedConnection);
                    }
                    // Thumbnail backfill must never enter the playback pipe.
                    // Doing so advances the visible frame/cursor as each strip
                    // image finishes rendering.
                    if(backfillThumbnails)return;
                    if (!presentedFirstFrame) {
                        const double readyMs = std::chrono::duration<double, std::milli>(
                            std::chrono::steady_clock::now() - taskStarted).count();
                        if (diagnostics)
                            spdlog::info(
                                "GALLERY_PERF event=gallery_first_frame_ready mount={} generation={} width={} height={} display_bpc={} latency_ms={:.3f}",
                                mountId, generation, width, height,
                                rgb24 ? 8 : 16, readyMs);
                        const auto presentationStarted = std::chrono::steady_clock::now();
                        QMetaObject::invokeMethod(this, [&, width, height] {
                            if (mGalleryGeneration.load() != generation) return;
                            if (player) player->presentRgb48Frame(
                                bytes, static_cast<int>(width), static_cast<int>(height),
                                preview.rawSamples, static_cast<int>(preview.rawWidth),
                                static_cast<int>(preview.rawHeight),
                                static_cast<int>(preview.rawChannels),
                                preview.rawBlack, preview.rawWhite,
                                preview.rawCfaSize, preview.rawCfaPhase,
                                preview.rawBlackLevels, rawOrientation,
                                rgb, gpuPreOriented, rgbPixels, gpuBuffer);
                        }, Qt::BlockingQueuedConnection);
                        presentedFirstFrame = true;
                        if (diagnostics)
                            spdlog::info(
                                "GALLERY_PERF event=gallery_first_frame_handoff mount={} generation={} presentation_ms={:.3f} total_ms={:.3f}",
                                mountId, generation,
                                std::chrono::duration<double, std::milli>(
                                    std::chrono::steady_clock::now() - presentationStarted).count(),
                                std::chrono::duration<double, std::milli>(
                                    std::chrono::steady_clock::now() - taskStarted).count());
                    }
                    const auto pushStarted = std::chrono::steady_clock::now();
                    auto pushResult = ClipPlayerDialog::FramePushResult::Retry;
                    size_t pushRetries = 0;
                    while (pushResult == ClipPlayerDialog::FramePushResult::Retry &&
                           mGalleryGeneration.load() == generation) {
                        QMetaObject::invokeMethod(this, [&] {
                            const auto uiStarted = std::chrono::steady_clock::now();
                            if (mGalleryGeneration.load() != generation)
                                pushResult = ClipPlayerDialog::FramePushResult::Stopped;
                            else if (player) pushResult = player->pushRgb48Frame(
                                bytes, static_cast<int>(width), static_cast<int>(height),
                                preview.rawSamples, static_cast<int>(preview.rawWidth),
                                static_cast<int>(preview.rawHeight),
                                static_cast<int>(preview.rawChannels),
                                preview.rawBlack, preview.rawWhite,
                                preview.rawCfaSize, preview.rawCfaPhase,
                                preview.rawBlackLevels, rawOrientation,
                                rgb, gpuPreOriented, rgbPixels, gpuBuffer,
                                static_cast<int>(preview.sourceFrameIndex));
                            else pushResult = ClipPlayerDialog::FramePushResult::Stopped;
                            if (diagnostics)
                                spdlog::info("GALLERY_PERF event=gallery_push_ui elapsed_ms={:.3f}",
                                    std::chrono::duration<double, std::milli>(
                                        std::chrono::steady_clock::now()-uiStarted).count());
                        }, Qt::BlockingQueuedConnection);
                        if (pushResult == ClipPlayerDialog::FramePushResult::Retry) {
                            ++pushRetries;
                            QThread::msleep(5);
                        }
                    }
                    if (diagnostics)
                        spdlog::info("GALLERY_PERF event=gallery_callback_detail color_ms={:.3f} prepare_ms={:.3f} push_ms={:.3f} push_retries={}",
                            std::chrono::duration<double, std::milli>(colorFinished-decodeStarted).count(),
                            std::chrono::duration<double, std::milli>(pushStarted-colorFinished).count(),
                            std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now()-pushStarted).count(),
                            pushRetries);
                    if (pushResult == ClipPlayerDialog::FramePushResult::Stopped)
                        throw std::runtime_error("Gallery decoder stopped accepting frames");
                    deliveryWaitMs += std::chrono::duration<double, std::milli>(
                        std::chrono::steady_clock::now() - deliveryStarted).count();
                    ++deliveredFrames;
                };
            const auto renderRange=[&](size_t begin,size_t last){
                if(begin>last||mGalleryGeneration.load()!=generation)return;
                options.firstFrame=begin;
                options.lastFrame=last;
                mFuseFilesystem->renderPreview(mountId,settings,options,
                    [this,generation](size_t,size_t,const std::string&){
                        return mGalleryGeneration.load()==generation;
                    },receiveFrame);
            };
            if(backfillThumbnails){
                const size_t end=std::min(size_t(playbackFrames-1),size_t(visibleRange.second));
                renderRange(firstFrame,end);
            }else renderRange(firstFrame,size_t(playbackFrames-1));
            if (mGalleryGeneration.load() == generation && !backfillThumbnails)
                QMetaObject::invokeMethod(this, [this, player, generation] {
                    if (mGalleryGeneration.load() != generation) return;
                    if (player) player->finishRgb48Frames();
                }, Qt::BlockingQueuedConnection);
            if (mGalleryGeneration.load() == generation && backfillThumbnails)
                emit galleryThumbnailBackfillFinished(
                    mountId, static_cast<int>(thumbnailFrames),
                    static_cast<qint64>(std::chrono::duration<double, std::milli>(
                        std::chrono::steady_clock::now() - taskStarted).count()));
            if (diagnostics) {
                const bool cancelled = mGalleryGeneration.load() != generation;
                spdlog::info(
                    "GALLERY_PERF event={} mount={} generation={} mode={} frames={} thumbnails={} thumbnail_fps={:.3f} decode_ms={:.3f} delivery_ms={:.3f} total_ms={:.3f}",
                    cancelled ? "gallery_render_task_cancelled" : "gallery_render_task_complete",
                    mountId, generation, backfillThumbnails ? "paused_backfill" : "playback",
                    deliveredFrames, thumbnailFrames,
                    thumbnailFrames * 1000.0 / std::max(1.0,
                        std::chrono::duration<double, std::milli>(
                            std::chrono::steady_clock::now() - taskStarted).count()),
                    displayDecodeMs, deliveryWaitMs,
                    std::chrono::duration<double, std::milli>(
                        std::chrono::steady_clock::now() - taskStarted).count());
            }
        } catch (const std::exception& error) {
            if (mGalleryGeneration.load() != generation) {
                if (diagnostics)
                    spdlog::info(
                        "GALLERY_PERF event=gallery_render_task_cancelled mount={} generation={} frames={} decode_ms={:.3f} delivery_ms={:.3f} total_ms={:.3f}",
                        mountId, generation, deliveredFrames, displayDecodeMs,
                        deliveryWaitMs,
                        std::chrono::duration<double, std::milli>(
                            std::chrono::steady_clock::now() - taskStarted).count());
                return;
            }
            const QString detail = QString::fromUtf8(error.what());
            spdlog::error("Gallery render failed: {}", error.what());
            QMetaObject::invokeMethod(this, [this, player, detail, generation] {
                if (mGalleryGeneration.load() != generation) return;
                if (player) player->failRgb48Frames(detail);
            }, Qt::BlockingQueuedConnection);
        }
    };
    mGalleryTasks.addFuture(QtConcurrent::run(std::move(render)));
}

void MainWindow::playMount(motioncam::MountId mountId, bool startRender) {
#ifdef __ANDROID__
    if(!mClipPlayer&&startRender){
        if(mAndroidGalleryOpening)return;
        mAndroidGalleryOpening=true;
        motioncam::stagePageCrossfade(this,motioncam::grabPageSnapshot(this),[this,mountId,startRender]{
            mAndroidGalleryOpening=false;
            showGallery(mountId,startRender);
        });
        return;
    }
#endif
    showGallery(mountId,startRender);
}

void MainWindow::showGallery(motioncam::MountId mountId, bool startRender) {
    if (mClipPlayer) {
        mClipPlayer->selectMount(mountId);
        mClipPlayer->raise();
        mClipPlayer->activateWindow();
        return;
    }
    QVector<ClipPlayerDialog::Clip> clips;
    for (const auto& mounted : mMountedFiles) {
        auto* card = fileWidgetForMount(mounted.mountId);
        const auto info = mFuseFilesystem->getFileInfo(mounted.mountId);
        if (!card || !info) continue;
        ClipPlayerDialog::Clip clip;
        clip.mountId = mounted.mountId;
        clip.title = QFileInfo(mounted.importFile).completeBaseName();
        clip.sourceFile = mounted.srcFile;
        // Independent DNGs form a still gallery, not a 24 fps video sequence.
        // Give each item a stable one-second viewing interval.
        clip.fps = info->fps > 0.0f ? info->fps : 24.0f;
        clip.durationSeconds = info->runtimeSeconds > 0.0f ? info->runtimeSeconds
                                                           : info->totalFrames / clip.fps;
        clip.sourceFrames = std::max(1,info->totalFrames-info->droppedFrames+
            info->duplicatedFrames);
        clip.width = info->width;
        clip.height = info->height;
        clip.nativeWidth = info->width;
        clip.nativeHeight = info->height;
        const int orientationOverride = settingsForMount(mounted.mountId).orientation;
        clip.orientation = info->isSequence
            ? (orientationOverride >= 0 ? orientationOverride : info->orientation)
            : -1;
        clip.isSequence = info->isSequence;
        clip.directPresentation = info->isSequence;
        clip.autoAdvance = info->isSequence && clip.sourceFrames > 1;
        clip.audioWav = info->audioWav;
        clip.duplicateFrames = info->duplicateFrameMask;
        clip.sourceFrameToOutput = info->sourceFrameToOutput;
        clip.sourceFrameDuplicated = info->sourceFrameDuplicated;
        clip.stillFrameNames = info->stillFrameNames;
        clip.selectedSourceFrames = mSelectedFrames.value(mounted.mountId);
        clips.push_back(std::move(clip));
    }
    if (clips.isEmpty()) return;
    mGalleryMountId = mountId;
    // A QWidget parent also establishes a transient-window relationship. Some
    // window managers then keep a maximized gallery permanently above the
    // main window. WA_DeleteOnClose handles ownership for this independent
    // top-level window; mClipPlayer is a QPointer and clears on deletion.
    mClipPlayer = new ClipPlayerDialog(std::move(clips), mountId, nullptr);
    mClipPlayer->setFpsIndicatorEnabled(mGalleryFpsIndicatorEnabled);
    mClipPlayer->setHistogramEnabled(mGalleryHistogramEnabled);
    mClipPlayer->setClippingEnabled(mGalleryClippingEnabled);
    connect(mClipPlayer, &ClipPlayerDialog::histogramEnabledChanged, this, [this](bool enabled) {
        mGalleryHistogramEnabled = enabled;
        QSettings(PACKAGE_NAME, APP_NAME).setValue("galleryHistogramEnabled", enabled);
        if (mClipPlayer) mClipPlayer->reloadCurrentClip();
    });
    connect(mClipPlayer, &ClipPlayerDialog::clippingEnabledChanged, this, [this](bool enabled) {
        mGalleryClippingEnabled = enabled;
        QSettings(PACKAGE_NAME, APP_NAME).setValue("galleryClippingEnabled", enabled);
        if (mClipPlayer) {
            mClipPlayer->reloadCurrentClip();
        }
    });
    connect(mClipPlayer, &ClipPlayerDialog::currentClipChanged, this,
            [this](int id, double startSeconds) {
                const auto previousMountId = mGalleryMountId;
                mGalleryMountId = id;
                if (previousMountId != motioncam::InvalidMountId &&
                    previousMountId != id)
                    mFuseFilesystem->releasePreviewResources(previousMountId);
                startGalleryRender(id, startSeconds);
            });
    connect(mClipPlayer, &ClipPlayerDialog::playbackClosed, this, [this] {
        if (!mClipPlayer) return;
        ++mGalleryGeneration;
        mGalleryMountId = motioncam::InvalidMountId;
        for (const auto& mounted : mMountedFiles)
            mFuseFilesystem->releasePreviewResources(mounted.mountId);
#ifdef __ANDROID__
        QTimer::singleShot(0, this, [this] {
            restoreSecondaryWindowsAfterGallery();
        });
#endif
    });
    connect(mClipPlayer,&ClipPlayerDialog::sourceFrameSelectionChanged,this,
        [this](int id,int frame,bool selected){
            if(selected)mSelectedFrames[id].insert(frame);
            else {mSelectedFrames[id].remove(frame);if(mSelectedFrames[id].isEmpty())mSelectedFrames.remove(id);}
            autoSaveSession();
        });
    connect(mClipPlayer,&ClipPlayerDialog::sourceFrameThumbnailRequested,this,
        &MainWindow::renderDroppedFrameThumbnail);
    connect(mClipPlayer,&ClipPlayerDialog::thumbnailBackfillRequested,this,
        [this](int id,double seconds){startGalleryRender(id,seconds,true);});
#ifdef __ANDROID__
    hideSecondaryWindowsForGallery();
    // Qt 6.7's Android backing store can dereference a null RHI when a
    // second full-screen QWidget surface is shown. Keep the gallery in the
    // existing activity surface and cover the entire main window instead.
    mClipPlayer->setParent(this, Qt::Widget);
    mClipPlayer->setGeometry(rect());
    mClipPlayer->setNavigationReturnSnapshot([this]{
        QWidget* destination=ui->centralwidget;
        for(const auto& page:mHiddenForGallery)if(page)destination=page;
        return motioncam::grabPageSnapshot(destination);
    });
    mClipPlayer->show();
    mClipPlayer->raise();
    updateAndroidOrientationPolicy();
#else
    mClipPlayer->show();
#endif
    if (startRender) startGalleryRender(mountId);
}

void MainWindow::renderDroppedFrameThumbnail(motioncam::MountId mountId,int sourceFrame){
    if(!mClipPlayer||sourceFrame<0)return;
    const auto generation=++mGalleryGeneration;
    auto settings=previewRenderSettings(settingsForMount(mountId));
    settings.options=static_cast<motioncam::FileRenderOptions>(
        static_cast<unsigned int>(settings.options)&
        ~static_cast<unsigned int>(motioncam::RENDER_OPT_FRAMERATE_CONVERSION));
    QPointer<ClipPlayerDialog> player(mClipPlayer);
    auto render=[this,mountId,sourceFrame,settings,generation,player]{
        std::unique_lock gallerySlot(mGalleryRenderMutex);
        if(mGalleryGeneration.load()!=generation)return;
        try{
            motioncam::PreviewOptions options;options.firstFrame=static_cast<size_t>(sourceFrame);
            options.skipFrame=[sourceFrame](size_t frame){return frame!=static_cast<size_t>(sourceFrame);};
            bool delivered=false;
            mFuseFilesystem->renderPreview(mountId,settings,options,
                [this,generation](size_t,size_t,const std::string&){return mGalleryGeneration.load()==generation;},
                [this,generation,player,mountId,sourceFrame,&delivered,settings](motioncam::PreviewFrame&& preview){
                    if(delivered||mGalleryGeneration.load()!=generation)return;
                    applyGalleryPreviewColorTransform(preview,
                        settings.ignoreForwardMat, gainMapOnlyDebug(settings));
                    packGalleryRgb24(preview.rgb);
                    auto& rgb=preview.rgb;const uint32_t width=preview.width,height=preview.height;
                    delivered=true;
                    const QByteArray bytes(reinterpret_cast<const char*>(rgb.data()),static_cast<qsizetype>(rgb.size()));
                    QMetaObject::invokeMethod(this,[this,player,mountId,generation,sourceFrame,bytes,width,height]{
                        if(mGalleryGeneration.load()==generation&&player&&player==mClipPlayer&&
                           player->currentMountId()==mountId&&player->thumbnailCollectionEnabled()->load())
                            player->presentDroppedSourceFrame(sourceFrame,bytes,
                            static_cast<int>(width),static_cast<int>(height));
                    },Qt::QueuedConnection);
                });
        }catch(const std::exception& error){spdlog::warn("Dropped-frame thumbnail failed: {}",error.what());}
        QMetaObject::invokeMethod(this,[this,player,mountId,generation]{
            if(mGalleryGeneration.load()!=generation||!player||player!=mClipPlayer||
               player->currentMountId()!=mountId||!player->thumbnailCollectionEnabled()->load())return;
            const auto info=mFuseFilesystem->getFileInfo(mountId);
            const double fps=info&&info->fps>0.0f?info->fps:24.0;
            startGalleryRender(mountId,std::max(0,player->playbackTarget()->load())/fps);
        },Qt::QueuedConnection);
    };
    mGalleryTasks.addFuture(QtConcurrent::run(std::move(render)));
}

void MainWindow::clearSelectedFrames(){
    if(mSelectedMountIds.isEmpty()){
        mSelectedFrames.clear();
        if(mClipPlayer)mClipPlayer->clearFrameSelections();
    }else{
        for(auto mountId:mSelectedMountIds)mSelectedFrames.remove(mountId);
        if(mClipPlayer)mClipPlayer->clearFrameSelections(mSelectedMountIds);
    }
    autoSaveSession();
}

void MainWindow::selectAllFrames(){
    mSelectedFrames.clear();
    for(const auto& mounted:mMountedFiles){
        const auto info=mFuseFilesystem->getFileInfo(mounted.mountId);
        if(!info)continue;
        const int sourceCount=info->sourceFrameToOutput
            ?static_cast<int>(info->sourceFrameToOutput->size())
            :std::max(1,info->totalFrames-info->droppedFrames+info->duplicatedFrames);
        auto& selected=mSelectedFrames[mounted.mountId];
        for(int source=0;source<sourceCount;++source)selected.insert(source);
    }
    if(mClipPlayer)mClipPlayer->selectAllFrameSelections();
    autoSaveSession();
}

void MainWindow::finalizeSelectedFrames(){
    QList<motioncam::MountId> renderIds;
    if(mSelectedMountIds.isEmpty()){
        for(const auto& mounted:mMountedFiles)if(!mSelectedFrames.value(mounted.mountId).isEmpty())renderIds.append(mounted.mountId);
    }else{
        for(auto id:mSelectedMountIds)if(!mSelectedFrames.value(id).isEmpty())renderIds.append(id);
    }
    if(renderIds.isEmpty()){
        QMessageBox::information(this,tr("Finalize Selected Frames"),tr("No frames are selected in the target clips."));return;
    }
    const bool compressionEnabled=ui->dngCompressionCheckBox->isChecked();
    const QString compressionMode=ui->dngCompressionModeComboBox->currentText();
    float compressionDistance=mRenderSettings.jxlDistance;
    if(compressionEnabled){
        if(isCameraNativeFormat(compressionMode)||isSevenZipFinalizeFormat(compressionMode)||
           isMetadataOnlyFinalizeFormat(compressionMode)){
            QMessageBox::warning(this,tr("Finalize Selected Frames"),
                tr("The selected finalize format cannot represent an individual DNG frame "
                   "selection. Choose a JPEG or JPEG XL DNG format."));
            return;
        }
        if(!dngFinalizeCompressionDistance(compressionMode,compressionDistance)){
            QMessageBox::warning(this,tr("Invalid finalize format"),
                tr("Enter a complete JPEG XL format such as ‘JPEG XL DCT 0.1’."));
            return;
        }
    }
    QString sharedOutputDirectory;
    if(mFinalizeSelectionToSingleDirectory){
        sharedOutputDirectory=QFileDialog::getExistingDirectory(
            this,tr("Select Output Directory for Selected Frames"),QString(),
            QFileDialog::ShowDirsOnly|QFileDialog::DontResolveSymlinks);
        if(sharedOutputDirectory.isEmpty())return;
    }
    for(auto mountId:renderIds){
        auto* card=fileWidgetForMount(mountId);if(!card)continue;
        const QString output=mFinalizeSelectionToSingleDirectory?sharedOutputDirectory:
            card->property("mountPath").toString()+QStringLiteral("-selection");
        spdlog::info("Selected-frame finalize: mount={} output={} hero_sidecars={}",
                     mountId, output.toStdString(), mInheritHeroFrameSidecars);
        const QString temporary=output+QStringLiteral(".finalizing-")+QUuid::createUuid().toString(QUuid::WithoutBraces);
        QDir().mkpath(temporary);
        const auto selected=mSelectedFrames.value(mountId);
        auto original=settingsForMount(mountId);auto sourceSettings=original;
        sourceSettings.options=static_cast<motioncam::FileRenderOptions>(
            static_cast<unsigned int>(sourceSettings.options)&
            ~static_cast<unsigned int>(motioncam::RENDER_OPT_FRAMERATE_CONVERSION));
        sourceSettings.jxlDistance=compressionDistance;
        mFuseFilesystem->updateOptions(mountId,sourceSettings);
        motioncam::FinalizeOptions options;
        options.skipDngFrame=[selected](size_t frame){return !selected.contains(static_cast<int>(frame));};
        options.detectDuplicateDngs=false;
        // Selected frames may omit the neighbors RIFE needs for a duplicated
        // frame, so leave interpolation to whole-clip finalization.
        options.interpolateDuplicatedFrames=false;
        options.jxlDistance=compressionDistance;
        QProgressDialog progress(tr("Finalizing selected frames…"),tr("Cancel"),0,selected.size(),this);
        progress.setWindowModality(Qt::WindowModal);progress.setMinimumDuration(0);
#ifdef __ANDROID__
        AndroidFinalizeWakeLock finalizeWakeLock;
        showAndroidProgress(progress,this);
#endif
        try{
            mFuseFilesystem->finalize(mountId,temporary.toStdString(),compressionEnabled,options,
                [&progress](size_t completed,size_t count,const std::string& name){
                    progress.setMaximum(static_cast<int>(count));progress.setValue(static_cast<int>(completed));
                    if(!name.empty())progress.setLabelText(
#ifdef __ANDROID__
                        androidFinalizeEntryName(name)
#else
                        QString::fromStdString(name)
#endif
                    );
                    QApplication::processEvents();return !progress.wasCanceled();
                });
            mFuseFilesystem->updateOptions(mountId,original);
            if(progress.wasCanceled()){QDir(temporary).removeRecursively();continue;}
            if(mInheritHeroFrameSidecars){
                QString sidecarError;
                if(!inheritHeroFrameSidecars(temporary,output,mFinalizeSelectionToSingleDirectory,
                                             sidecarError))
                    throw std::runtime_error(sidecarError.toStdString());
            }
            if(motioncam::effectiveBadPixelTreatment(original.badPixelTreatment,
                    original.vignetteCorrection) == motioncam::BadPixelTreatment::OptionsJsonOnly &&
                !(original.options & motioncam::RENDER_OPT_REMOSAIC_TO_BAYER)){
                QString sidecar=card->property("sidecarPath").toString();
                if(sidecar.isEmpty())
                    sidecar=QString::fromStdString(motioncam::vfs::sidecarPath(
                        card->property("filePath").toString().toStdString()).string());
                writeFinalizedCfaBadPixelOptions(temporary,output,sidecar,true);
            }
            if(mFinalizeSelectionToSingleDirectory){
                QString mergeError;
                if(!mergeDirectoryContentsFlat(temporary,output,mergeError))
                    throw std::runtime_error(mergeError.toStdString());
                QDir(temporary).removeRecursively();
            }else if(!QFileInfo::exists(output)){
                if(!QDir(QFileInfo(output).absolutePath()).rename(temporary,output))
                    throw std::runtime_error("Could not publish selection folder");
            }else{
                QString mergeError;
                if(!mergeDirectoryContents(temporary,output,mergeError))
                    throw std::runtime_error(mergeError.toStdString());
                QDir(temporary).removeRecursively();
            }
        }catch(const std::exception& error){
            mFuseFilesystem->updateOptions(mountId,original);QDir(temporary).removeRecursively();
            if(progress.wasCanceled()||std::string(error.what())=="Finalization cancelled")return;
            QMessageBox::critical(this,tr("Finalize Selected Frames"),QString::fromUtf8(error.what()));return;
        }
    }
    QMessageBox::information(this,tr("Finalize Selected Frames"),
        mFinalizeSelectionToSingleDirectory
            ? tr("Selected frames were saved to %1.").arg(QDir::toNativeSeparators(sharedOutputDirectory))
            : tr("Selected frames were saved to each clip's -selection folder."));
}

void MainWindow::startGalleryPerformanceTest(
        const QString& sessionPath, int playbackMilliseconds,
        bool generateThumbnails) {
    const bool mountedMode = qEnvironmentVariable("MOTIONCAM_GALLERY_PERF_MODE") ==
        QStringLiteral("mounted");
    if (mountedMode) generateThumbnails = false;
    const QFileInfo session(sessionPath);
    if (!session.isFile()) {
        spdlog::error("GALLERY_PERF event=suite_failed reason=session_not_found path={}",
                      session.absoluteFilePath().toStdString());
        QCoreApplication::exit(2);
        return;
    }

    const auto suiteStarted = std::chrono::steady_clock::now();
    mGalleryPerformanceTestActive = true;
    spdlog::info("GALLERY_PERF event=suite_start session={} mode={} playback_ms={} thumbnails={}",
                 session.absoluteFilePath().toStdString(),
                 mountedMode ? "mounted" : "gallery", playbackMilliseconds,
                 generateThumbnails);
    auto thumbnailsFinished = std::make_shared<QSet<motioncam::MountId>>();
    if (generateThumbnails)
        connect(this, &MainWindow::thumbnailPerformanceFinished, this,
        [thumbnailsFinished](motioncam::MountId mountId, bool success, qint64 elapsedMs) {
            spdlog::info(
                "GALLERY_PERF event=thumbnail_complete mount={} success={} latency_ms={}",
                mountId, success, elapsedMs);
            thumbnailsFinished->insert(mountId);
        });
    const auto importStarted = std::chrono::steady_clock::now();
    spdlog::info("GALLERY_PERF event=import_thumbnail_pipeline_start");
    mPerformanceThumbnailRun = generateThumbnails;
    loadSessionFromFile(session.absoluteFilePath());
    mPerformanceThumbnailRun = false;
    const double importMs = std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - importStarted).count();
    spdlog::info("GALLERY_PERF event=session_import_complete clips={} latency_ms={:.3f}",
                 mMountedFiles.size(), importMs);
    if (mMountedFiles.isEmpty()) {
        spdlog::error("GALLERY_PERF event=suite_failed reason=no_mounted_clips");
        QCoreApplication::exit(3);
        return;
    }

    if (!mountedMode)
        spdlog::info(
            "GALLERY_PERF event=cold_gallery_start clips={} thumbnails_enabled={} thumbnails_complete={} import_ms={:.3f}",
            mMountedFiles.size(), generateThumbnails, thumbnailsFinished->size(),
            std::chrono::duration<double, std::milli>(
                std::chrono::steady_clock::now() - importStarted).count());

    // Keep mounted reads off the UI thread. FUSE callbacks and mount setup can
    // need that thread while a client is blocked waiting for a frame.
    if (mountedMode) {
        struct MountedSampleClip {
            int index;
            motioncam::MountId mount;
            QString path;
            float jxlDistance;
        };
        QVector<MountedSampleClip> clips;
        for (int i = 0; i < mMountedFiles.size(); ++i) {
            const auto mount = mMountedFiles[i].mountId;
            auto* card = fileWidgetForMount(mount);
            clips.push_back({i, mount,
                card ? card->property("mountPath").toString() : QString(),
                settingsForMount(mount).jxlDistance});
        }
        QPointer<MainWindow> window(this);
        QThreadPool::globalInstance()->start([clips = std::move(clips), window] {
            QElapsedTimer suiteTimer;
            suiteTimer.start();
            int samples = 0;
            int readFailures = 0;
            int previewFailures = 0;
            spdlog::info("GALLERY_PERF event=mounted_sequence_suite_start clips={}",
                         clips.size());
            for (const auto& clip : clips) {
                QElapsedTimer enumerationTimer;
                enumerationTimer.start();
                const auto files = QDir(clip.path).entryInfoList(
                    QStringList() << "*.dng" << "*.DNG",
                    QDir::Files | QDir::Readable, QDir::Name);
                spdlog::info("GALLERY_PERF event=mounted_sequence_enumerated clip={} mount={} files={} latency_ms={}",
                             clip.index, clip.mount, files.size(), enumerationTimer.elapsed());
                if (files.isEmpty()) { ++readFailures; continue; }
                QVector<QPair<const char*, int>> positions{{"first", 0}};
                if (files.size() > 1) positions.push_back({"sequential", 1});
                if (files.size() > 2) positions.push_back({"sequential", 2});
                const int midpoint = files.size() / 2;
                if (midpoint > 2) positions.push_back({"seek", midpoint});
                if (midpoint + 1 < files.size() && midpoint > 2)
                    positions.push_back({"after_seek", midpoint + 1});
                bool sequentialCountValid = false;
                const int sequentialCount = qEnvironmentVariableIntValue(
                    "MOTIONCAM_GALLERY_PERF_MOUNTED_SEQUENTIAL_COUNT",
                    &sequentialCountValid);
                if (sequentialCountValid && sequentialCount > 0) {
                    positions.clear();
                    for (int frame = 0; frame < std::min(sequentialCount,
                                                         static_cast<int>(files.size())); ++frame)
                        positions.push_back({"sequential", frame});
                }
                for (const auto& [access, frame] : positions) {
                    QElapsedTimer timer;
                    timer.start();
                    QFile file(files[frame].absoluteFilePath());
                    std::vector<uint8_t> dng;
                    const bool opened = file.open(QIODevice::ReadOnly);
                    bool complete = false;
                    if (opened && file.size() > 0) {
                        const auto expected = static_cast<size_t>(file.size());
                        dng.resize(expected);
                        size_t received = 0;
                        while (received < expected) {
                            const auto count = file.read(
                                reinterpret_cast<char*>(dng.data() + received),
                                static_cast<qint64>(std::min<size_t>(
                                    expected - received, 4 * 1024 * 1024)));
                            if (count <= 0) break;
                            received += static_cast<size_t>(count);
                        }
                        complete = received == expected;
                        dng.resize(received);
                    }
                    const qint64 readMs = timer.elapsed();
                    if (complete && qEnvironmentVariableIsSet(
                            "MOTIONCAM_GALLERY_PERF_COMPRESS_JXL")) {
                        const size_t inputBytes = dng.size();
                        timer.restart();
                        bool compressed = true;
                        try {
                            motioncam::RenderSettings settings;
                            settings.jxlDistance = clip.jxlDistance;
                            motioncam::vfs::DngFinalizeOptions options;
                            options.writeTiming = false;
                            options.compression = true;
                            options.packToWhiteLevel = qEnvironmentVariableIsSet(
                                "MOTIONCAM_GALLERY_PERF_PACK_TO_WHITE_LEVEL");
                            options.sourceName = "gallery_perf_jxl";
                            motioncam::vfs::finalizeDng(dng, settings, options);
                        } catch (const std::exception& error) {
                            spdlog::error("GALLERY_PERF event=jxl_encode_failed clip={} frame={} reason={}",
                                          clip.index, frame, error.what());
                            compressed = false;
                        }
                        spdlog::info("GALLERY_PERF event=jxl_encode_sample clip={} frame={} success={} distance={} input_bytes={} output_bytes={} encode_ms={}",
                                     clip.index, frame, compressed, clip.jxlDistance,
                                     inputBytes, dng.size(), timer.elapsed());
                        complete = compressed;
                    }
                    timer.restart();
                    const size_t bytes = dng.size();
                    motioncam::DNGImageLayout layout;
                    const bool layoutValid = complete &&
                        motioncam::DNGDecoder::getImageLayout(dng, layout);
                    const uint32_t linearizationEntries = complete
                        ? motioncam::DNGDecoder::getLinearizationTableCount(dng) : 0;
                    const qint64 layoutMs = timer.elapsed();
                    if (clip.index == 0 && frame == 0) {
                        const QString dumpPath = qEnvironmentVariable(
                            "MOTIONCAM_GALLERY_PERF_DUMP_DNG");
                        if (!dumpPath.isEmpty()) {
                            QFile dump(dumpPath);
                            if (dump.open(QIODevice::WriteOnly))
                                dump.write(reinterpret_cast<const char*>(dng.data()),
                                           static_cast<qint64>(dng.size()));
                        }
                    }
                    const qint64 previewStartedMs = timer.elapsed();
                    motioncam::PreviewFrame preview;
                    const bool skipPreview = qEnvironmentVariableIsSet(
                        "MOTIONCAM_GALLERY_PERF_SKIP_PREVIEW_DECODE");
                    const bool decoded = complete && (skipPreview ||
                        motioncam::DNGDecoder::decodePreview(
                            std::move(dng), motioncam::RenderSettings{},
                            preview, false));
                    const qint64 previewMs = timer.elapsed() - previewStartedMs;
                    const bool hashPreview = qEnvironmentVariableIsSet(
                        "MOTIONCAM_GALLERY_PERF_HASH_PREVIEW");
                    uint64_t pixelHash = hashPreview ? 1469598103934665603ull : 0;
                    if (decoded && hashPreview)
                        for (const uint8_t value : preview.rgb) {
                            pixelHash ^= value;
                            pixelHash *= 1099511628211ull;
                        }
                    const qint64 decodeMs = timer.elapsed();
                    if (qEnvironmentVariableIsSet("MOTIONCAM_GALLERY_PERF_PROFILE"))
                        spdlog::info("GALLERY_PERF event=mounted_decode_breakdown clip={} frame={} layout_ms={} dump_ms={} preview_ms={} hash_ms={} total_ms={}",
                                     clip.index, frame, layoutMs,
                                     previewStartedMs - layoutMs, previewMs,
                                     decodeMs - previewStartedMs - previewMs, decodeMs);
                    ++samples;
                    if (!complete) ++readFailures;
                    if (complete && !decoded) ++previewFailures;
                    spdlog::info("GALLERY_PERF event=mounted_sequence_sample clip={} mount={} access={} frame={} read_success={} preview_decoded={} bytes={} read_ms={} decode_ms={} width={} height={} cfa={} channels={} linearization_entries={} pixel_hash={}",
                                 clip.index, clip.mount, access, frame, complete, decoded,
                                 bytes, readMs, decodeMs, preview.width,
                                 preview.height,
                                 layoutValid && layout.pixels == motioncam::DNGPixelLayout::CFA,
                                 layoutValid ? layout.samplesPerPixel : 0,
                                 linearizationEntries,
                                 pixelHash);
                }
                if (qEnvironmentVariableIsSet("MOTIONCAM_GALLERY_PERF_PARALLEL_READS") &&
                    files.size() >= 8) {
                    bool countValid = false;
                    const int requestedCount = qEnvironmentVariableIntValue(
                        "MOTIONCAM_GALLERY_PERF_PARALLEL_READ_COUNT", &countValid);
                    const int parallelCount = std::min(
                        static_cast<int>(files.size()) - 3,
                        countValid ? std::clamp(requestedCount, 1, 16) : 4);
                    std::vector<std::thread> readers;
                    std::atomic<int> concurrentFailures{0};
                    for (int frame = 3; frame < 3 + parallelCount; ++frame) {
                        const QString path = files[frame].absoluteFilePath();
                        readers.emplace_back([clip, frame, path, &concurrentFailures] {
                            QElapsedTimer timer;
                            timer.start();
                            QFile file(path);
                            qint64 received = 0;
                            bool complete = file.open(QIODevice::ReadOnly);
                            if (complete) {
                                std::vector<char> buffer(4 * 1024 * 1024);
                                while (received < file.size()) {
                                    const qint64 count = file.read(buffer.data(),
                                        std::min<qint64>(buffer.size(), file.size() - received));
                                    if (count <= 0) { complete = false; break; }
                                    received += count;
                                }
                            }
                            if (!complete) ++concurrentFailures;
                            spdlog::info("GALLERY_PERF event=mounted_parallel_sample clip={} frame={} read_success={} bytes={} read_ms={}",
                                         clip.index, frame, complete, received, timer.elapsed());
                        });
                    }
                    for (auto& reader : readers) reader.join();
                    samples += parallelCount;
                    readFailures += concurrentFailures.load();
                }
            }
            spdlog::info("GALLERY_PERF event=mounted_sequence_suite_complete samples={} read_failures={} preview_failures={} latency_ms={}",
                         samples, readFailures, previewFailures, suiteTimer.elapsed());
            if (window) QMetaObject::invokeMethod(window, [readFailures, previewFailures] {
                QCoreApplication::exit(readFailures || previewFailures ? 4 : 0);
            }, Qt::QueuedConnection);
        });
        return;
    }

    struct RunState {
        enum class Phase { Init, Playback, Seek, SeekPlayback, ThumbnailBackfill };
        QVector<motioncam::MountId> mounts;
        int clipIndex = 0;
        int playbackMs = 3000;
        int presentedFrames = 0;
        int lastPresentedIndex = -1;
        qint64 firstPresentedNs = -1;
        qint64 lastPresentedNs = -1;
        int firstPresentedIndex = -1;
        std::vector<qint64> presentationIntervalsNs;
        int actionSerial = 0;
        Phase phase = Phase::Init;
        QElapsedTimer actionTimer;
        QElapsedTimer sampleTimer;
    };
    auto state = std::make_shared<RunState>();
    state->playbackMs = std::max(250, playbackMilliseconds);
    for (const auto& file : mMountedFiles) state->mounts.push_back(file.mountId);

    // Exercise full-resolution nearest-neighbour reconstruction by default so
    // the HQ demosaic cannot hide gallery scheduling and presentation costs.
    // A proxy-preserving diagnostic run can opt out for scale-specific tests.
    const bool preserveGalleryProxy = qEnvironmentVariableIsSet(
        "MOTIONCAM_GALLERY_PERF_PRESERVE_PROXY");
    const bool forceHighQuality = qEnvironmentVariableIsSet(
        "MOTIONCAM_GALLERY_PERF_FORCE_HQ");
    for (const auto mountId : state->mounts) {
        auto settings = settingsForMount(mountId);
        settings.options = static_cast<motioncam::FileRenderOptions>(
            static_cast<unsigned int>(settings.options) &
            ~(static_cast<unsigned int>(preserveGalleryProxy
                    ? motioncam::RENDER_OPT_HIGHER_CFA_HQ
                    : (motioncam::RENDER_OPT_DRAFT |
                       motioncam::RENDER_OPT_HIGHER_CFA_HQ))));
        if (forceHighQuality)
            settings.options = static_cast<motioncam::FileRenderOptions>(
                settings.options | motioncam::RENDER_OPT_HIGHER_CFA_HQ);
        if (!preserveGalleryProxy) settings.draftScale = 1;
        spdlog::info("GALLERY_PERF event=gallery_clip_settings mount={} draft={} scale={} hq={}",
            mountId, bool(settings.options & motioncam::RENDER_OPT_DRAFT),
            settings.draftScale,
            bool(settings.options & motioncam::RENDER_OPT_HIGHER_CFA_HQ));
        mLocalSettings.insert(mountId, settings);
        mFuseFilesystem->updateOptions(mountId, settings);
    }
    const QString requestedViewport = qEnvironmentVariable("MOTIONCAM_GALLERY_PERF_VIEWPORT");
    spdlog::info(
        "GALLERY_PERF event=gallery_mode resolution={} hq={} window={}",
        preserveGalleryProxy ? "proxy" : "full", forceHighQuality,
        requestedViewport.isEmpty() ? "maximized" : requestedViewport.toStdString());

    QElapsedTimer gallerySetupTimer;
    gallerySetupTimer.start();
    auto galleryStartupTimer = std::make_shared<QElapsedTimer>();
    galleryStartupTimer->start();
    spdlog::info("GALLERY_PERF event=gallery_setup_start clips={}", state->mounts.size());
    const bool previousClippingEnabled = mGalleryClippingEnabled;
    // Create the player without starting a hidden warm-up render. The first
    // render is started below after all diagnostic signal hooks are installed.
    playMount(state->mounts.front(), false);
    if (!mClipPlayer) {
        spdlog::error("GALLERY_PERF event=suite_failed reason=gallery_not_created");
        QCoreApplication::exit(4);
        return;
    }
    const bool cappedPlayback=qEnvironmentVariableIsSet("MOTIONCAM_GALLERY_PERF_CAPPED");
    mClipPlayer->setPerformanceUncapped(!cappedPlayback);
    if(qEnvironmentVariableIsSet("MOTIONCAM_GALLERY_PERF_AUDIO"))
        mClipPlayer->setPerformanceAudioEnabled(true);
    spdlog::info("GALLERY_PERF event=playback_cadence mode={}",
                 cappedPlayback?"capped":"uncapped");
    if (qEnvironmentVariableIsSet("MOTIONCAM_GALLERY_PERF_CLIPPING"))
        mClipPlayer->setClippingEnabled(true);
    mClipPlayer->setPerformanceOverlayPinned(
        !qEnvironmentVariableIsSet("MOTIONCAM_GALLERY_PERF_AUTO_FADE"));
    mClipPlayer->setHistogramEnabled(!qEnvironmentVariableIsSet(
        "MOTIONCAM_GALLERY_PERF_NO_HISTOGRAM"));
    if (qEnvironmentVariableIsSet("MOTIONCAM_GALLERY_PERF_EXPAND_HISTOGRAM"))
        mClipPlayer->setHistogramExpanded(true);
    spdlog::info("GALLERY_PERF event=gallery_histogram enabled={} overlay_pinned=true",
                 mClipPlayer->histogramEnabled());
    mClipPlayer->setAutomaticAdvanceEnabled(false);
    // The optional decode-focused run excludes thumbnail collection and row
    // layout work. The ordinary performance run includes both.
    const bool showThumbnailStrip = !qEnvironmentVariableIsSet(
        "MOTIONCAM_GALLERY_PERF_HIDE_THUMBNAILS") &&
        !qEnvironmentVariableIsSet("MOTIONCAM_GALLERY_PERF_OPEN_THUMBNAILS_DURING_PLAYBACK");
    mClipPlayer->setThumbnailStripVisible(showThumbnailStrip);
    spdlog::info("GALLERY_PERF event=thumbnail_strip enabled={}", showThumbnailStrip);
    // Playback samples target the most demanding normal UI geometry. Keep the
    // gallery maximized after setup and after the resize/cancellation preflight
    // so the measured FFmpeg surface matches a maximized interactive gallery.
    const QStringList viewportParts = requestedViewport.split('x');
    bool widthValid = false, heightValid = false;
    const int viewportWidth = viewportParts.value(0).toInt(&widthValid);
    const int viewportHeight = viewportParts.value(1).toInt(&heightValid);
    if (viewportParts.size() == 2 && widthValid && heightValid &&
        viewportWidth >= 640 && viewportHeight >= 360) {
        mClipPlayer->showNormal();
        mClipPlayer->resize(viewportWidth, viewportHeight);
        spdlog::info("GALLERY_PERF event=gallery_viewport requested={}x{}",
            viewportWidth, viewportHeight);
    } else if (qEnvironmentVariableIsSet("MOTIONCAM_GALLERY_PERF_FULLSCREEN")) {
        mClipPlayer->showFullScreen();
    } else {
        mClipPlayer->showMaximized();
    }
    spdlog::info("GALLERY_PERF event=gallery_setup_complete latency_ms={}",
                 gallerySetupTimer.elapsed());

    auto finishSuite = [this, state, suiteStarted, previousClippingEnabled] {
        const double totalMs = std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - suiteStarted).count();
        spdlog::info("GALLERY_PERF event=suite_complete clips={} total_ms={:.3f}",
                     state->mounts.size(), totalMs);
        if (mClipPlayer) mClipPlayer->setClippingEnabled(previousClippingEnabled);
        if (mClipPlayer) {
            mClipPlayer->closeImmediately();
        }
        mGalleryClippingEnabled = previousClippingEnabled;
#ifdef __ANDROID__
        spdlog::default_logger()->flush();
        if (qEnvironmentVariableIsSet("MOTIONCAM_GALLERY_PERF_KEEP_ALIVE")) {
            spdlog::info("GALLERY_PERF event=suite_waiting_for_external_stop");
            spdlog::default_logger()->flush();
            return;
        }
#endif
        if (mClipPlayer) mClipPlayer->deleteLater();
        // Let Qt finish the native Vulkan child-window teardown before the
        // Android process starts destroying its HWUI/Vulkan resources.
        QTimer::singleShot(750, qApp, &QCoreApplication::quit);
    };

    auto startClip = std::make_shared<std::function<void()>>();
    auto startSample = std::make_shared<std::function<void(bool)>>();
    *startClip = [this, state, startClip, finishSuite] {
        if (state->clipIndex >= state->mounts.size()) {
            finishSuite();
            return;
        }
        const auto mountId = state->mounts[state->clipIndex];
        state->phase = RunState::Phase::Init;
        state->presentedFrames = 0;
        state->lastPresentedIndex = -1;
        state->firstPresentedIndex = -1;
        state->presentationIntervalsNs.clear();
        state->actionTimer.restart();
        const int serial = ++state->actionSerial;
        spdlog::info("GALLERY_PERF event=clip_init_start clip={} mount={}",
                     state->clipIndex, mountId);
        if (mClipPlayer) {
            if (mClipPlayer->currentMountId() == mountId)
                mClipPlayer->reloadCurrentClip();
            else
                mClipPlayer->selectMount(mountId);
        }
        QTimer::singleShot(120000, this, [state, serial, mountId] {
            if (state->actionSerial != serial) return;
            spdlog::error("GALLERY_PERF event=timeout phase=init mount={}", mountId);
            QCoreApplication::exit(5);
        });
    };
    *startSample = [this, state, startClip, startSample](bool afterSeek) {
        // Clip navigation deliberately preserves a paused gallery when the
        // thumbnail strip is open. Performance samples, however, explicitly
        // measure playback and must not inherit the preceding backfill pause.
        if (mClipPlayer) mClipPlayer->setPlaybackPaused(false);
        state->phase = afterSeek ? RunState::Phase::SeekPlayback
                                 : RunState::Phase::Playback;
        state->presentedFrames = 0;
        state->lastPresentedIndex = -1;
        state->firstPresentedNs = -1;
        state->lastPresentedNs = -1;
        state->firstPresentedIndex = -1;
        state->presentationIntervalsNs.clear();
        state->sampleTimer.restart();
        if(qEnvironmentVariableIsSet("MOTIONCAM_GALLERY_PERF_AUTO_FADE"))
            mClipPlayer->setPerformanceOverlayPinned(false);
        const int serial = ++state->actionSerial;
        if (!afterSeek && qEnvironmentVariableIsSet(
                "MOTIONCAM_GALLERY_PERF_OPEN_THUMBNAILS_DURING_PLAYBACK"))
            QTimer::singleShot(std::max(0, qEnvironmentVariableIntValue(
                "MOTIONCAM_GALLERY_PERF_OPEN_THUMBNAILS_DURING_PLAYBACK")),
                this, [this, state, serial] {
                if (state->actionSerial != serial || !mClipPlayer) return;
                spdlog::info("GALLERY_PERF event=thumbnail_open_during_playback");
                mClipPlayer->setThumbnailStripVisible(true);
            });
        QTimer::singleShot(state->playbackMs, this,
            [this, state, startClip, startSample, afterSeek, serial] {
                if (state->actionSerial != serial || !mClipPlayer) return;
                if (mClipPlayer->currentMountId() != state->mounts[state->clipIndex]) {
                    spdlog::error("GALLERY_PERF event=suite_failed reason=unexpected_clip expected={} actual={}",
                        state->mounts[state->clipIndex], mClipPlayer->currentMountId());
                    QCoreApplication::exit(9);
                    return;
                }
                const qint64 elapsedNs = std::max<qint64>(1, state->sampleTimer.nsecsElapsed());
                const qint64 activeNs = state->firstPresentedNs >= 0 &&
                                        state->lastPresentedNs > state->firstPresentedNs
                    ? state->lastPresentedNs - state->firstPresentedNs : 0;
                const double targetFps=mClipPlayer->currentFrameRate();
                const double presentedFps=activeNs>0
                    ?(state->presentedFrames-1)*1.0e9/activeNs:0.0;
                const int sourceAdvance=state->firstPresentedIndex>=0
                    ?state->lastPresentedIndex-state->firstPresentedIndex:0;
                const double sourceFps=activeNs>0
                    ?sourceAdvance*1.0e9/activeNs:0.0;
                const double driftMs=targetFps>0&&activeNs>0
                    ?(activeNs-sourceAdvance*1.0e9/targetFps)/1.0e6:0.0;
                auto intervals=state->presentationIntervalsNs;
                std::sort(intervals.begin(),intervals.end());
                const auto percentile=[&](double fraction){
                    return intervals.empty()?0.0:
                        intervals[std::min(intervals.size()-1,
                            static_cast<size_t>(fraction*(intervals.size()-1)))]+0.0;
                };
                spdlog::info(
                    "GALLERY_PERF event=playback_sample clip={} mount={} after_seek={} elapsed_ms={:.3f} active_ms={:.3f} frames={} presented_fps={:.4f} source_fps={:.4f} target_fps={:.4f} drift_ms={:.3f} interval_p50_ms={:.3f} interval_p95_ms={:.3f} interval_max_ms={:.3f}",
                    state->clipIndex, mClipPlayer->currentMountId(), afterSeek,
                    elapsedNs/1.0e6, activeNs/1.0e6, state->presentedFrames,
                    presentedFps, sourceFps, targetFps, driftMs,
                    percentile(0.5)/1.0e6, percentile(0.95)/1.0e6,
                    intervals.empty()?0.0:intervals.back()/1.0e6);
                if (!afterSeek) {
                    state->phase = RunState::Phase::Seek;
                    state->actionTimer.restart();
                    const int seekSerial = ++state->actionSerial;
                    bool seekSecondsValid = false;
                    const double requestedSeekSeconds = qEnvironmentVariable(
                        "MOTIONCAM_GALLERY_PERF_SEEK_SECONDS").toDouble(&seekSecondsValid);
                    const double target = seekSecondsValid ?
                        std::clamp(requestedSeekSeconds, 0.0,
                            mClipPlayer->currentDurationSeconds()) :
                        mClipPlayer->currentDurationSeconds() * 0.5;
                    spdlog::info("GALLERY_PERF event=seek_start clip={} mount={} target_s={:.3f}",
                                 state->clipIndex, mClipPlayer->currentMountId(), target);
                    mClipPlayer->seekToSeconds(target);
                    QTimer::singleShot(120000, this,
                        [state, seekSerial, mountId = mClipPlayer->currentMountId()] {
                            if (state->actionSerial != seekSerial) return;
                            spdlog::error("GALLERY_PERF event=timeout phase=seek mount={}",
                                          mountId);
                            QCoreApplication::exit(6);
                        });
                } else {
                    if (qEnvironmentVariableIsSet("MOTIONCAM_GALLERY_PERF_SKIP_BACKFILL") ||
                        qEnvironmentVariableIsSet("MOTIONCAM_GALLERY_PERF_HIDE_THUMBNAILS")) {
                        ++state->clipIndex;
                        (*startClip)();
                        return;
                    }
                    state->phase = RunState::Phase::ThumbnailBackfill;
                    state->actionTimer.restart();
                    const int backfillSerial = ++state->actionSerial;
                    spdlog::info(
                        "GALLERY_PERF event=paused_thumbnail_backfill_start clip={} mount={}",
                        state->clipIndex, mClipPlayer->currentMountId());
                    const bool alreadyPaused=mClipPlayer->isPlaybackPaused();
                    mClipPlayer->setPlaybackPaused(true);
                    if(alreadyPaused)mClipPlayer->requestThumbnailBackfill();
                    QTimer::singleShot(120000, this,
                        [state, backfillSerial, mountId=mClipPlayer->currentMountId()] {
                            if(state->actionSerial!=backfillSerial)return;
                            spdlog::error(
                                "GALLERY_PERF event=timeout phase=thumbnail_backfill mount={}",
                                mountId);
                            QCoreApplication::exit(8);
                        });
                }
            });
    };

    connect(mClipPlayer, &ClipPlayerDialog::framePresented, this,
        [state](int mountId, int frameIndex) {
            if (state->clipIndex >= state->mounts.size() ||
                mountId != state->mounts[state->clipIndex]) return;
            if (state->phase == RunState::Phase::Playback ||
                state->phase == RunState::Phase::SeekPlayback) {
                if (frameIndex <= state->lastPresentedIndex)
                    spdlog::error("GALLERY_PERF event=frame_order_error clip={} previous={} current={}",
                        state->clipIndex, state->lastPresentedIndex, frameIndex);
                state->lastPresentedIndex = frameIndex;
                const qint64 presentedNs=state->sampleTimer.nsecsElapsed();
                if(state->firstPresentedNs<0){
                    state->firstPresentedNs=presentedNs;
                    state->firstPresentedIndex=frameIndex;
                }
                if(state->lastPresentedNs>=0)
                    state->presentationIntervalsNs.push_back(
                        presentedNs-state->lastPresentedNs);
                state->lastPresentedNs=presentedNs;
                ++state->presentedFrames;
            }
        });
    connect(mClipPlayer, &ClipPlayerDialog::firstFramePresented, this,
        [this, state, startClip, startSample, galleryStartupTimer](int mountId) {
            if (state->clipIndex >= state->mounts.size() ||
                mountId != state->mounts[state->clipIndex]) return;
            if (state->phase == RunState::Phase::Init) {
                ++state->actionSerial;
                mClipPlayer->setPerformancePixelOverlayEnabled(true);
                if (state->clipIndex == 0)
                    spdlog::info(
                        "GALLERY_PERF event=gallery_startup_first_frame clip={} mount={} latency_ms={}",
                        state->clipIndex, mountId, galleryStartupTimer->elapsed());
                spdlog::info("GALLERY_PERF event=clip_init_complete clip={} mount={} latency_ms={}",
                             state->clipIndex, mountId, state->actionTimer.elapsed());
                const auto info = mFuseFilesystem->getFileInfo(mountId);
                if (info && !info->isSequence) {
                    spdlog::info("GALLERY_PERF event=still_complete clip={} mount={}",
                                 state->clipIndex, mountId);
                    if (qEnvironmentVariableIsSet("MOTIONCAM_GALLERY_PERF_GESTURES")) {
                        mClipPlayer->runPerformanceGestureTest([state, startClip] {
                            ++state->clipIndex;
                            (*startClip)();
                        });
                        return;
                    }
                    ++state->clipIndex;
                    (*startClip)();
                    return;
                }
                (*startSample)(false);
            } else if (state->phase == RunState::Phase::Seek) {
                ++state->actionSerial;
                spdlog::info("GALLERY_PERF event=seek_complete clip={} mount={} latency_ms={}",
                             state->clipIndex, mountId, state->actionTimer.elapsed());
                (*startSample)(true);
            }
        });
    connect(this, &MainWindow::galleryThumbnailBackfillFinished, this,
        [this,state,startClip](motioncam::MountId mountId,int frames,qint64 elapsedMs){
            if(state->phase!=RunState::Phase::ThumbnailBackfill||
               state->clipIndex>=state->mounts.size()||mountId!=state->mounts[state->clipIndex])return;
            ++state->actionSerial;
            spdlog::info(
                "GALLERY_PERF event=paused_thumbnail_backfill_complete clip={} mount={} frames={} elapsed_ms={} thumbnail_fps={:.3f}",
                state->clipIndex,mountId,frames,elapsedMs,
                frames*1000.0/std::max<qint64>(1,elapsedMs));
            ++state->clipIndex;
            (*startClip)();
        });

    // Maximizing is asynchronous on X11/Wayland. Starting rawvideo before the
    // window manager and layouts publish the final viewport can configure
    // FFmpeg for the constructor's provisional geometry; with no retained
    // frame yet, the ordinary resize-refresh path intentionally has nothing
    // to restart. Let geometry settle, then begin the measured first render.
    QTimer::singleShot(500, this, [startClip] { (*startClip)(); });
}

void MainWindow::openMountedDirectory(QWidget* fileWidget) {
    auto mountPath = fileWidget->property("mountPath").toString();
    if (mountPath.isEmpty()) {
        QMessageBox::warning(this, "Error", "Mount path not found");
        return;
    }

    bool success = false;

#ifdef _WIN32
    success = QProcess::startDetached("explorer", QStringList() << QDir::toNativeSeparators(mountPath));
#elif __APPLE__
    success = QProcess::startDetached("/usr/bin/open", QStringList() << mountPath);
#elif defined(__linux__) && !defined(__ANDROID__)
    success = QProcess::startDetached("xdg-open", QStringList() << mountPath);
#endif

    if (!success)
        QMessageBox::warning(this, "Error", QString("Failed to open directory: %1").arg(mountPath));
}

void MainWindow::revealSourceJson(const QString& jsonPath,
                                  const QString& clipPath) {
#ifdef __ANDROID__
    if (clipPath.isEmpty() ||
        motioncam::sourceDirectoryForImport(clipPath).isEmpty()) {
        QMessageBox::information(this, tr("Source directory required"),
            tr("Link this clip's source directory before editing its JSON."));
        return;
    }
    const QString error = motioncam::editSidecarInExternalApp(
        clipPath, QFileInfo(jsonPath).fileName());
    if (!error.isEmpty()) {
        QMessageBox::critical(this, tr("Could not open JSON"), error);
        return;
    }
    for (const auto& mounted : mMountedFiles) {
        if (mounted.srcFile != clipPath) continue;
        mAndroidExternalJsonCard = fileWidgetForMount(mounted.mountId);
        mAndroidExternalEditorLeftApp = false;
        break;
    }
    return;
#endif
    const QFileInfo jsonInfo(jsonPath);
    if (jsonPath.isEmpty() || !jsonInfo.exists()) {
        QMessageBox::warning(this, tr("Error"), tr("Source JSON file not found: %1").arg(jsonPath));
        return;
    }

    bool success = false;
#ifdef _WIN32
    success = QProcess::startDetached(
        "explorer", QStringList() << "/select," << QDir::toNativeSeparators(jsonInfo.absoluteFilePath()));
#elif __APPLE__
    success = QProcess::startDetached("/usr/bin/open", QStringList() << "-R" << jsonInfo.absoluteFilePath());
#elif defined(__linux__) && !defined(__ANDROID__)
    // FileManager1 is implemented by the major Linux file managers and asks
    // them to reveal/select the item rather than merely opening its parent.
    // Fall back to xdg-open when the desktop does not expose that interface.
    auto* revealProcess = new QProcess(this);
    const QString directory = jsonInfo.absolutePath();
    const auto openDirectory = [this, directory] {
        if (!QProcess::startDetached("xdg-open", QStringList() << directory))
            QMessageBox::warning(this, tr("Error"),
                                 tr("Failed to open source JSON directory: %1").arg(directory));
    };
    connect(revealProcess, &QProcess::finished, this,
            [revealProcess, openDirectory](int exitCode, QProcess::ExitStatus exitStatus) {
        if (exitStatus != QProcess::NormalExit || exitCode != 0) openDirectory();
        revealProcess->deleteLater();
    });
    connect(revealProcess, &QProcess::errorOccurred, this,
            [revealProcess, openDirectory](QProcess::ProcessError error) {
        if (error == QProcess::FailedToStart) {
            openDirectory();
            revealProcess->deleteLater();
        }
    });
    revealProcess->start("dbus-send", QStringList()
        << "--session"
        << "--print-reply"
        << "--dest=org.freedesktop.FileManager1"
        << "/org/freedesktop/FileManager1"
        << "org.freedesktop.FileManager1.ShowItems"
        << QString("array:string:%1").arg(QUrl::fromLocalFile(jsonInfo.absoluteFilePath()).toString())
        << "string:");
    return;
#endif

    if (!success)
        QMessageBox::warning(this, tr("Error"),
                             tr("Failed to open source JSON directory: %1").arg(jsonInfo.absolutePath()));
}

void MainWindow::removeFile(QWidget* fileWidget, bool releaseMount) {
    auto* scrollContent = ui->dragAndDropScrollArea->widget();
    auto* scrollLayout = qobject_cast<QVBoxLayout*>(scrollContent->layout());
    const QString mountPath = fileWidget->property("mountPath").toString();

    int fileWidgetIndex = scrollLayout->indexOf(fileWidget);
    removeAdjacentClipSeparator(scrollLayout,fileWidgetIndex);

    scrollLayout->removeWidget(fileWidget);
    fileWidget->deleteLater();

    // Unmount the file
    bool ok = false;
    auto mountId = fileWidget->property("mountId").toInt(&ok);
    if(ok) {
        // The player owns a snapshot of the mounted clip list. Close it before
        // changing that list so it cannot keep rendering or navigate back to
        // an unmounted clip. closeEvent also cancels the active generation.
        if (mClipPlayer) mClipPlayer->closeImmediately();
        if (const auto cancellation = mThumbnailCancellations.take(mountId))
            cancellation->store(true);
        if (auto* dialog = mTimingDialogs.value(mountId).data()) dialog->close();
        if (releaseMount) {
            mFuseFilesystem->unmount(mountId);
            cleanupArchiveMount(mountId);
        }
        mSelectedMountIds.remove(mountId);
        mSelectedFrames.remove(mountId);
        mLocalSettings.remove(mountId);

#ifdef _WIN32
        if (releaseMount && mDeleteOnUnmount && !mountPath.isEmpty()) {
            QThread::msleep(100);
            QDir mountDirectory(mountPath);
            if (mountDirectory.exists() && !mountDirectory.removeRecursively())
                spdlog::warn("Could not delete local DNG output after unmount: {}",
                             mountPath.toStdString());
        }
#endif

        auto it = std::find_if(
            mMountedFiles.begin(), mMountedFiles.end(),
            [mountId](const motioncam::MountedFile& f) { return f.mountId == mountId; });
        if(it != mMountedFiles.end()) {
#ifdef __ANDROID__
            motioncam::releaseSeekableSourceLink(
                it->importFile);
#endif
            mMountedFiles.erase(it);
        }
    }

    // If all files are removed, show the drag-drop label again
    if (mMountedFiles.empty()) {
        ui->dragAndDropLabel->show();
    }
    updateClipIndices();
    updateSelectionUi();
    if (mMountedFiles.empty() && !mClearingSession && !mGalleryPerformanceTestActive)
        QFile::remove(autoSessionPath());
    else
        autoSaveSession();
}

void MainWindow::cleanupArchiveMount(motioncam::MountId mountId) {
    const QString root = mArchiveTemporaryRoots.take(mountId);
    if (!root.isEmpty() && QDir(root).exists() && !QDir(root).removeRecursively())
        spdlog::warn("Could not remove archive staging directory: {}", root.toStdString());
}

void MainWindow::keyPressEvent(QKeyEvent* event) {
    if (event->matches(QKeySequence::SelectAll)) {
        for (const auto& file : mMountedFiles) {
            if (auto* card = fileWidgetForMount(file.mountId))
                if (auto* check = card->findChild<QCheckBox*>("clipSelection"))
                    check->setChecked(true);
        }
        event->accept();
        return;
    }
    QMainWindow::keyPressEvent(event);
}

#ifdef _WIN32
void MainWindow::discardFile(QWidget* fileWidget) {
    auto mountPath = fileWidget->property("mountPath").toString();
    if (mountPath.isEmpty()) {
        spdlog::error("Discard failed: mount path not found");
        return;
    }

    const auto answer = QMessageBox::warning(
        this,
        "Discard rendered files?",
        QString("Unmount and permanently delete all files in:\n%1").arg(mountPath),
        QMessageBox::Discard | QMessageBox::Cancel,
        QMessageBox::Cancel);
    if (answer != QMessageBox::Discard) {
        return;
    }

    // First unmount
    bool ok = false;
    auto mountId = fileWidget->property("mountId").toInt(&ok);
    if(ok) {
        mFuseFilesystem->unmount(mountId);
        cleanupArchiveMount(mountId);
    }

    // Give Windows a moment to release file handles after unmounting
    QThread::msleep(100);

    // Delete the entire mount directory recursively
    QDir mountDir(mountPath);
    if (mountDir.exists()) {
        // Try to remove directory (includes all files)
        bool removed = mountDir.removeRecursively();

        if (!removed) {
            // If first attempt failed, wait a bit longer and try again
            // (files might still be in use)
            QThread::msleep(500);
            removed = mountDir.removeRecursively();
        }

        if (removed) {
            spdlog::info("Discarded clip: removed directory and all files: {}", mountPath.toStdString());
        } else {
            // Directory couldn't be removed (likely files still in use)
            // Try to delete individual files
            QStringList allFiles = mountDir.entryList(QDir::Files | QDir::NoDotAndDotDot);
            int deletedCount = 0;

            for (const QString& fileName : allFiles) {
                QString filePath = mountDir.absoluteFilePath(fileName);
                if (QFile::remove(filePath)) {
                    deletedCount++;
                }
            }

            // Try one more time to remove the directory
            if (mountDir.rmdir(".")) {
                spdlog::info("Discarded clip: deleted {} files and removed directory: {}",
                            deletedCount, mountPath.toStdString());
            } else {
                spdlog::warn("Discarded clip: deleted {} files but directory remains (files may be in use): {}",
                            deletedCount, mountPath.toStdString());
            }
        }
    }

    // Remove from UI (same as removeFile)
    auto* scrollContent = ui->dragAndDropScrollArea->widget();
    auto* scrollLayout = qobject_cast<QVBoxLayout*>(scrollContent->layout());

    int fileWidgetIndex = scrollLayout->indexOf(fileWidget);
    removeAdjacentClipSeparator(scrollLayout,fileWidgetIndex);

    scrollLayout->removeWidget(fileWidget);
    fileWidget->deleteLater();

    // Remove from mounted files list
    if(ok) {
        auto it = std::find_if(
            mMountedFiles.begin(), mMountedFiles.end(),
            [mountId](const motioncam::MountedFile& f) { return f.mountId == mountId; });
        if(it != mMountedFiles.end()) {
#ifdef __ANDROID__
            motioncam::releaseSeekableSourceLink(
                it->importFile);
#endif
            mMountedFiles.erase(it);
        }
    }

    if (mMountedFiles.empty()) {
        ui->dragAndDropLabel->show();
    }
}
#endif

std::optional<QString> MainWindow::ensureRifeRuntime() {
    if (!RIFE_SUPPORTED) return std::nullopt;
    const QString dataRoot = QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation);
    const QString runtimeRoot = rifeRuntimeRoot();
#ifdef _WIN32
    const QString runtimePython = QDir(runtimeRoot).absoluteFilePath(".venv/Scripts/python.exe");
#else
    const QString runtimePython = QDir(runtimeRoot).absoluteFilePath(".venv/bin/python");
#endif
    const QString readyMarker = QDir(runtimeRoot).absoluteFilePath(".motioncam-ready");
    if (QFileInfo::exists(readyMarker) && QFileInfo::exists(runtimePython) &&
        QFileInfo::exists(QDir(runtimeRoot).absoluteFilePath("inference_img.py")))
        return runtimeRoot;

    const auto answer = QMessageBox::question(this, "Install frame interpolation",
        "MotionCam Fuse needs to download and install its private RIFE runtime. "
        "PyTorch and the model dependencies can require several gigabytes.\n\n"
        "Install it now?", QMessageBox::Yes | QMessageBox::No);
    if (answer != QMessageBox::Yes) return std::nullopt;

    QString bootstrapPython = QStandardPaths::findExecutable("python3");
#ifdef _WIN32
    if (bootstrapPython.isEmpty()) bootstrapPython = QStandardPaths::findExecutable("python");
#endif
    if (bootstrapPython.isEmpty()) {
        QMessageBox::critical(this, "Frame interpolation",
            "Python 3 was not found. Install Python 3, then try interpolation again.");
        return std::nullopt;
    }

    QDir().mkpath(dataRoot);
    const QString archivePath = QDir(dataRoot).absoluteFilePath("rife-download.zip");
    QProgressDialog progress("Downloading RIFE...", "Cancel", 0, 0, this);
    progress.setWindowModality(Qt::WindowModal);
    progress.setMinimumDuration(0);
    progress.setAutoClose(false);
#ifdef __ANDROID__
    showAndroidProgress(progress,this);
#else
    progress.show();
#endif

    QNetworkAccessManager network;
    QNetworkRequest request(QUrl(QString("https://github.com/may-son/RIFE-FixDropFrames-and-ConvertFPS/archive/%1.zip")
        .arg(RIFE_REVISION)));
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                         QNetworkRequest::NoLessSafeRedirectPolicy);
    QNetworkReply* reply = network.get(request);
    QSaveFile archive(archivePath);
    if (!archive.open(QIODevice::WriteOnly)) {
        reply->abort();
        reply->deleteLater();
        QMessageBox::critical(this, "Frame interpolation", "Could not create the RIFE download file.");
        return std::nullopt;
    }
    bool writeFailed = false;
    QEventLoop loop;
    connect(reply, &QNetworkReply::readyRead, this, [&] {
        const QByteArray chunk = reply->readAll();
        if (archive.write(chunk) != chunk.size()) { writeFailed = true; reply->abort(); }
    });
    connect(reply, &QNetworkReply::downloadProgress, this, [&](qint64 received, qint64 total) {
        if (total > 0) { progress.setMaximum(1000); progress.setValue(static_cast<int>(received * 1000 / total)); }
        QApplication::processEvents();
        if (progress.wasCanceled()) reply->abort();
    });
    connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
    loop.exec();
    if (reply->bytesAvailable()) {
        const QByteArray chunk = reply->readAll();
        writeFailed |= archive.write(chunk) != chunk.size();
    }
    if (reply->error() != QNetworkReply::NoError || writeFailed) {
        const QString error = progress.wasCanceled() ? "Installation cancelled." : reply->errorString();
        archive.cancelWriting(); reply->deleteLater();
        QMessageBox::warning(this, "Frame interpolation", writeFailed ? "Could not save the RIFE download." : error);
        return std::nullopt;
    }
    if (!archive.commit()) {
        reply->deleteLater();
        QMessageBox::critical(this, "Frame interpolation", "Could not save the RIFE download.");
        return std::nullopt;
    }
    reply->deleteLater();

    QFile downloadedArchive(archivePath);
    if (!downloadedArchive.open(QIODevice::ReadOnly)) {
        QMessageBox::critical(this, "Frame interpolation", "Could not verify the RIFE download.");
        return std::nullopt;
    }
    QCryptographicHash archiveHash(QCryptographicHash::Sha256);
    while (!downloadedArchive.atEnd()) {
        const QByteArray chunk = downloadedArchive.read(1024 * 1024);
        if (chunk.isEmpty() && downloadedArchive.error() != QFileDevice::NoError) {
            downloadedArchive.close();
            QFile::remove(archivePath);
            QMessageBox::critical(this, "Frame interpolation", "Could not verify the RIFE download.");
            return std::nullopt;
        }
        archiveHash.addData(chunk);
    }
    downloadedArchive.close();
    const QByteArray actualHash = archiveHash.result().toHex();
    if (actualHash != QByteArray(RIFE_ARCHIVE_SHA256)) {
        QFile::remove(archivePath);
        QMessageBox::critical(this, "Frame interpolation",
            "The downloaded RIFE archive failed its SHA-256 integrity check. "
            "The file was deleted and will not be installed.\n\nExpected: " +
            QString(RIFE_ARCHIVE_SHA256) + "\nReceived: " + QString::fromLatin1(actualHash));
        return std::nullopt;
    }

    QDir(runtimeRoot).removeRecursively();
    QDir().mkpath(runtimeRoot);
    progress.setRange(0, 0);
    progress.setLabelText("Preparing RIFE and installing Python dependencies...");
    const QString setupCode = QString::fromUtf8(R"PY(
import pathlib, subprocess, sys, zipfile
archive, root = pathlib.Path(sys.argv[1]), pathlib.Path(sys.argv[2])
with zipfile.ZipFile(archive) as z:
    for item in z.infolist():
        parts = pathlib.PurePosixPath(item.filename).parts[1:]
        if not parts or '..' in parts: continue
        target = root.joinpath(*parts)
        if item.is_dir(): target.mkdir(parents=True, exist_ok=True)
        else:
            target.parent.mkdir(parents=True, exist_ok=True)
            with z.open(item) as source, target.open('wb') as output: output.write(source.read())
subprocess.check_call([sys.executable, '-m', 'venv', str(root / '.venv')])
python = root / '.venv' / ('Scripts/python.exe' if sys.platform == 'win32' else 'bin/python')
subprocess.check_call([str(python), '-m', 'pip', 'install', '--upgrade', 'pip'])
subprocess.check_call([str(python), '-m', 'pip', 'install', '-r', str(root / 'requirements.txt')])
)PY");
    QProcess setup;
    setup.setProcessChannelMode(QProcess::MergedChannels);
    setup.start(bootstrapPython, {"-c", setupCode, archivePath, runtimeRoot});
    if (!setup.waitForStarted()) {
        QMessageBox::critical(this, "Frame interpolation", "Could not start the Python installer.");
        return std::nullopt;
    }
    QByteArray setupDiagnostic;
    while (setup.state() != QProcess::NotRunning) {
        setup.waitForFinished(100);
        setupDiagnostic += setup.readAll();
        if (setupDiagnostic.size() > 16000) setupDiagnostic = setupDiagnostic.right(16000);
        QApplication::processEvents();
        if (progress.wasCanceled()) {
            setup.kill(); setup.waitForFinished();
            QMessageBox::information(this, "Frame interpolation", "Installation cancelled.");
            return std::nullopt;
        }
    }
    setupDiagnostic += setup.readAll();
    if (setup.exitStatus() != QProcess::NormalExit || setup.exitCode() != 0) {
        QMessageBox::critical(this, "Frame interpolation",
            "RIFE installation failed:\n\n" + QString::fromUtf8(setupDiagnostic).right(4000));
        return std::nullopt;
    }
    QFile marker(readyMarker);
    if (!marker.open(QIODevice::WriteOnly) || marker.write(RIFE_REVISION) < 0) {
        QMessageBox::critical(this, "Frame interpolation", "Could not finish the RIFE installation.");
        return std::nullopt;
    }
    QFile::remove(archivePath);
    return runtimeRoot;
}

void MainWindow::finalizeCameraNative(QWidget* fileWidget, const QString& requestedMode,
                                      bool metadataOnly) {
    QString mode=requestedMode;
    const QString audioCodec=finalizeAudioCodec(mode);
    const QStringList modeParts = mode.simplified().split(' ', Qt::SkipEmptyParts);
    const bool hevc = !modeParts.isEmpty() &&
        modeParts.front().compare("HEVC", Qt::CaseInsensitive) == 0;
    const bool av1 = !modeParts.isEmpty() &&
        modeParts.front().compare("AV1", Qt::CaseInsensitive) == 0;
    const bool hdrNoise = av1 && mode.endsWith("synth noise", Qt::CaseInsensitive);
    const bool proRes = mode.startsWith("ProRes ", Qt::CaseInsensitive);
    const bool cineForm = mode.startsWith("CineForm ", Qt::CaseInsensitive);
    const bool cineFormRgb = mode.compare("CineForm RGB", Qt::CaseInsensitive) == 0;
    QString chroma = "420";
    QString encoderPreset;
    QString encoderCrf;
    int av1Preset = 0;
    int av1Crf = 0;
    if (hevc || av1) {
        bool validPreset = true;
        bool validCrf = true;
        if (modeParts.size() == (hdrNoise ? 6 : 4)) {
            chroma = modeParts[1];
            encoderPreset = modeParts[2];
            encoderCrf = modeParts[3];
            if (av1) {
                av1Preset = encoderPreset.toInt(&validPreset);
                av1Crf = encoderCrf.toInt(&validCrf);
                validPreset = validPreset && av1Preset >= -2 && av1Preset <= 13;
                validCrf = validCrf && av1Crf >= 0 && av1Crf <= 63;
            } else {
                encoderCrf.toFloat(&validCrf);
            }
        } else {
            validPreset = validCrf = false;
        }
        const bool validChroma = chroma == "420" || chroma == "422" || chroma == "444";
        if (!validChroma || !validPreset || !validCrf || (hdrNoise && chroma != "420")) {
            QMessageBox::warning(this, tr("Invalid finalize format"),
                tr("Use ‘HEVC 420|422|444 preset CRF’ or ‘AV1 420|422|444 preset CRF’. "
                   "AV1 synth noise is available only with 420."));
            return;
        }
    } else if ((proRes && mode != "ProRes LT" && mode != "ProRes Std" &&
                mode != "ProRes HQ" && mode != "ProRes 444" && mode != "ProRes XQ") ||
               (cineForm && mode != "CineForm 422" && mode != "CineForm RGB")) {
        QMessageBox::warning(this, tr("Invalid finalize format"),
                             tr("Select one of the listed ProRes or CineForm formats."));
        return;
    }
    const QString srcFile = fileWidget->property("filePath").toString();
    const QString mountPath = fileWidget->property("mountPath").toString();
    bool mountOk = false;
    const auto mountId = fileWidget->property("mountId").toInt(&mountOk);
    if (!mountOk || srcFile.isEmpty() || mountPath.isEmpty()) {
        QMessageBox::warning(this, "Camera Native finalization", "Mount information is unavailable.");
        return;
    }

    const QFileInfo inputInfo(srcFile);
    QString calibrationPath = fileWidget->property("sidecarPath").toString();
    if (calibrationPath.isEmpty()) calibrationPath = inputInfo.isDir()
        ? QDir(srcFile).absoluteFilePath(inputInfo.fileName() + ".json")
        : inputInfo.absolutePath() + "/" + inputInfo.completeBaseName() + ".json";
    QString gyroflowPath = fileWidget->property("gyroflowSidecarPath").toString();
    if (gyroflowPath.isEmpty()) gyroflowPath = inputInfo.isDir()
        ? QDir(srcFile).absoluteFilePath(inputInfo.fileName() + "_gyroflow.json")
        : inputInfo.absolutePath() + "/" + inputInfo.completeBaseName() + "_gyroflow.json";
    const auto sourceCalibration = QFile::exists(calibrationPath)
        ? CalibrationData::loadFromFile(calibrationPath.toStdString())
        : std::nullopt;

    // Camera Native is deliberately fed linear RGB. LOG60 is applied once by
    // FFmpeg immediately before the ordered-dithered RGB-to-YUV conversion.
    auto stagingSettings = buildRenderSettings();
    // Per-source JSON is authoritative for display orientation and therefore
    // also overrides orientation inherited from the source container/DNG.
    stagingSettings.orientation = settingsForMount(mountId).orientation;
    stagingSettings.options &= ~motioncam::RENDER_OPT_REMOSAIC_TO_BAYER;
    stagingSettings.options &= ~motioncam::RENDER_OPT_JPEG_COMPRESSION;
    stagingSettings.options &= ~motioncam::RENDER_OPT_LOG_TRANSFORM;
    stagingSettings.draftScale = 1;
    stagingSettings.logTransform = motioncam::LogTransformMode::Disabled;
    // ignoreForwardMat is a gallery display preference. Camera Native output
    // must retain the source/calibration ForwardMatrix tags in its sidecar.
    stagingSettings.ignoreForwardMat = false;
    // Camera model identity is not represented by the Camera Native sidecar;
    // do not apply its DNG-only BaselineExposure compensation during staging.
    stagingSettings.cameraModel.clear();
    stagingSettings.cameraNativeStaging = true;
    mFuseFilesystem->updateOptions(mountId, stagingSettings);

    const auto info = mFuseFilesystem->getFileInfo(mountId);
    if (!info) {
        mFuseFilesystem->updateOptions(mountId, buildRenderSettings());
        QMessageBox::warning(this, "Camera Native finalization", "Clip information is unavailable.");
        return;
    }
    const int outputOrientation = stagingSettings.orientation >= 0
        ? stagingSettings.orientation : info->orientation;
    // FileInfo::dataType is a display label and deliberately continues to say
    // "Quad Bayer CFA" even when the selected finalization path demosaics it.
    // Inspect source CFA metadata so every CFA source has demosaic enabled
    // before Camera Native staging requests a linear RGB sequence.
    bool unsupportedBayer = false;
    const bool demosaicCfa =
        stagingSettings.quadBayerOption == motioncam::QuadBayerMode::Demosaic ||
        stagingSettings.quadBayerOption == motioncam::QuadBayerMode::DemosaicColor ||
        stagingSettings.quadBayerOption == motioncam::QuadBayerMode::DemosaicOCL;
    try {
        const auto& calibration = sourceCalibration;
        const int cfaSizeOverride = calibration && calibration->hasCfaSize && calibration->cfaSize > 0
            ? calibration->cfaSize : 0;
        if (srcFile.endsWith(".mcraw", Qt::CaseInsensitive) ||
            srcFile.endsWith(".unspektra", Qt::CaseInsensitive)) {
            Decoder decoder(srcFile.toStdString());
            const auto frames = decoder.getFrames();
            if (!frames.empty()) {
                std::vector<uint8_t> frameData;
                nlohmann::json frameJson;
                decoder.loadFrame(frames.front(), frameData, frameJson);
                const auto metadata = CameraFrameMetadata::parse(frameJson);
                const int cfaSize = cfaSizeOverride > 0 ? cfaSizeOverride : metadata.cfaSize;
                unsupportedBayer = cfaSize >= 2 && !demosaicCfa;
            }
        } else if (DNGDecoder::isDNGSequence(srcFile.toStdString())) {
            DNGDecoder decoder(srcFile.toStdString());
            int cfaSize = 0;
            std::array<uint8_t, 4> cfaPhase{};
            const bool hasCfa = decoder.getCFAMetadata(0, cfaSize, cfaPhase);
            if (cfaSizeOverride > 0) cfaSize = cfaSizeOverride;
            if (hasCfa || cfaSizeOverride > 0)
                unsupportedBayer = cfaSize >= 2 && !demosaicCfa;
        }
    } catch (const std::exception& e) {
        spdlog::warn("Could not preflight Camera Native CFA metadata: {}", e.what());
    }
    if (unsupportedBayer) {
        mFuseFilesystem->updateOptions(mountId, buildRenderSettings());
        QMessageBox::warning(this, "Camera Native finalization",
            "Camera Native requires an RGB sequence. Enable CFA demosaic before finalizing.");
        return;
    }

    const QFileInfo sourceInfo(srcFile);
    QString codecSuffix;
    if (hevc) codecSuffix = "_HEVC";
    else if (av1) codecSuffix = hdrNoise ? "_AV1_HDR_NOISE" : "_AV1";
    else if (mode.contains("ProRes LT")) codecSuffix = "_PRORES_LT";
    else if (mode.contains("ProRes Std")) codecSuffix = "_PRORES_STANDARD";
    else if (mode.contains("ProRes HQ")) codecSuffix = "_PRORES_HQ";
    else if (mode.contains("ProRes 444")) codecSuffix = "_PRORES_444";
    else if (mode.contains("ProRes XQ")) codecSuffix = "_PRORES_XQ";
    else if (cineFormRgb) codecSuffix = "_CINEFORM_RGB";
    else if (cineForm) codecSuffix = "_CINEFORM_422";
    const QString outputBase = metadataOnly
        ? sourceInfo.completeBaseName() + "_METADATA"
        : sourceInfo.completeBaseName() + "_LOG60_NATIVE" + codecSuffix;
    const bool compressedSourceAudio=QFile::exists(QDir(mountPath).absoluteFilePath("audio.flac"))||
        QFile::exists(QDir(mountPath).absoluteFilePath("audio.opus"));
    const bool compressedOutputAudio=!audioCodec.isEmpty()||compressedSourceAudio;
    if(!metadataOnly && compressedOutputAudio&&(proRes||cineForm)){
        QMessageBox::warning(this,tr("Unsupported audio and video combination"),
            tr("This FFmpeg build cannot embed FLAC or Opus with ProRes or CineForm "
               "in MOV or MP4. Select an HEVC or AV1 format, or use uncompressed "
               "audio with this video format."));
        return;
    }
    const QString containerExtension = (av1||compressedOutputAudio) ? ".mp4" : ".mov";
    const QDir outputDir(QFileInfo(mountPath).absolutePath());
    const QString outputPath = outputDir.absoluteFilePath(outputBase + containerExtension);
    const QString jsonPath = outputDir.absoluteFilePath(outputBase + ".json");
    const QString outputGyroflowPath = outputDir.absoluteFilePath(outputBase + "_gyroflow.json");
    const QString gcsvPath = outputDir.absoluteFilePath(outputBase + ".gcsv");
    const QString audioBasePath = outputDir.absoluteFilePath(outputBase);
    bool hasExistingAudio = false;
    for (const char* extension : {"wav", "flac", "opus", "aac"})
        hasExistingAudio |= QFile::exists(audioBasePath + "." + extension);
    if (((!metadataOnly && QFile::exists(outputPath)) || QFile::exists(jsonPath) ||
         (metadataOnly && (QFile::exists(gcsvPath) || hasExistingAudio)) ||
         (!metadataOnly && QFile::exists(gyroflowPath) && QFile::exists(outputGyroflowPath))) &&
        QMessageBox::question(this, metadataOnly ? "Replace metadata output?"
                                                : "Replace Camera Native output?",
            QString("Replace existing output for %1?").arg(outputBase),
            QMessageBox::Yes | QMessageBox::No, QMessageBox::No) != QMessageBox::Yes) {
        mFuseFilesystem->updateOptions(mountId, buildRenderSettings());
        return;
    }

    QTemporaryDir staging(outputDir.absoluteFilePath(".camera-native-XXXXXX"));
    if (!staging.isValid()) {
        mFuseFilesystem->updateOptions(mountId, buildRenderSettings());
        QMessageBox::critical(this, "Camera Native finalization", "Could not create the staging directory.");
        return;
    }

    const bool hasDuplicateFrames = info->duplicateFrameMask &&
        std::any_of(info->duplicateFrameMask->begin(), info->duplicateFrameMask->end(),
                    [](bool duplicate) { return duplicate; });
    const bool interpolateFrames = RIFE_SUPPORTED && ui->rifeInterpolationCheckBox->isChecked() &&
        (info->duplicatedFrames > 0 || hasDuplicateFrames);
    const auto rifeRuntime = interpolateFrames ? ensureRifeRuntime() : std::optional<QString>{QString{}};
    if (!rifeRuntime) {
        mFuseFilesystem->updateOptions(mountId, buildRenderSettings());
        return;
    }

    QProgressDialog progress(metadataOnly ? "Collecting per-clip metadata..."
                                          : "Rendering linear RGB sequence...", "Cancel", 0,
        std::max(1, info->totalFrames - info->droppedFrames + info->duplicatedFrames), this);
    progress.setWindowModality(Qt::WindowModal);
    progress.setMinimumDuration(0);
    progress.setAutoClose(false);
    progress.setAutoReset(false);
#ifdef __ANDROID__
    AndroidFinalizeWakeLock finalizeWakeLock;
    showAndroidProgress(progress,this);
#endif

    try {
        motioncam::FinalizeOptions finalizeOptions;
        finalizeOptions.interpolateDuplicatedFrames = interpolateFrames;
        finalizeOptions.detectDuplicateDngs = false;
        finalizeOptions.rifeDirectory = rifeRuntime->toStdString();
        QDir stageDir(staging.path());

        QString ffmpeg;
#ifdef _WIN32
        const QString bundled = QCoreApplication::applicationDirPath() + "/ffmpeg.exe";
#else
        const QString bundled = QCoreApplication::applicationDirPath() + "/ffmpeg";
#endif
        if (QFileInfo(bundled).isExecutable()) ffmpeg = bundled;
        else ffmpeg = QStandardPaths::findExecutable("ffmpeg");
        if (!metadataOnly && ffmpeg.isEmpty())
            throw std::runtime_error(
                "Could not find the FFmpeg executable. Install it on PATH or place it beside MotionCam Fuse.");
        auto probeEncoder = [&ffmpeg](const QString& name, QByteArray* help = nullptr) {
            QProcess probe;
            probe.setProcessChannelMode(QProcess::MergedChannels);
            probe.start(ffmpeg, {"-hide_banner", "-h", "encoder=" + name});
            if (!probe.waitForStarted() || !probe.waitForFinished(10000)) {
                probe.kill();
                probe.waitForFinished();
                return false;
            }
            const QByteArray output = probe.readAll();
            if (help) *help = output;
            return probe.exitCode() == 0 && output.contains(("Encoder " + name).toUtf8());
        };
        QString av1Encoder;
        QString requiredEncoder = proRes ? "prores_ks" : cineForm ? "cfhd" : QString{};
        if (!metadataOnly && av1) {
            QByteArray svtHelp;
            const bool hasSvt = probeEncoder("libsvtav1", &svtHelp);
            const QByteArray requiredPixelFormat =
                (chroma == "422" ? QByteArray("yuv422p10le")
                                 : chroma == "444" ? QByteArray("yuv444p10le")
                                                    : QByteArray("yuv420p10le"));
            if (hasSvt && svtHelp.contains(requiredPixelFormat)) {
                av1Encoder = "libsvtav1";
            } else if (chroma != "420" && probeEncoder("libaom-av1")) {
                if (av1Preset < 0 || av1Preset > 8)
                    throw std::runtime_error(
                        "The libaom AV1 fallback requires a preset from 0 through 8.");
                av1Encoder = "libaom-av1";
            } else {
                throw std::runtime_error(
                    ("This FFmpeg executable cannot encode AV1 " + chroma +
                     " at 10-bit. Install an FFmpeg build with a compatible libsvtav1 or libaom-av1.")
                        .toStdString());
            }
        } else if (!metadataOnly && !requiredEncoder.isEmpty() && !probeEncoder(requiredEncoder)) {
            throw std::runtime_error(
                ("This FFmpeg executable does not provide the " + requiredEncoder +
                 " encoder. Install a compatible FFmpeg build or place it beside MotionCam Fuse.")
                    .toStdString());
        }

        struct NativeFrame {
            std::vector<uint8_t> rgb;
            Timestamp timestamp = 0;
            DNGFrameMetadata colorMetadata;
            std::vector<GainMap> gainMaps;
            std::vector<GainMap> deferredGainMaps;
            bool duplicateFrame = false;
            bool syntheticFrame = false;
        };
        std::mutex queueMutex;
        std::condition_variable queueChanged;
        std::deque<NativeFrame> frameQueue;
        std::atomic_bool cancelled{false};
        bool renderDone = false;
        std::exception_ptr renderError;
        size_t renderCompleted = 0, renderCount = 1;
        std::string renderLabel;

        std::thread renderer([&] {
            try {
                mFuseFilesystem->finalize(mountId, staging.path().toStdString(), false,
                    finalizeOptions,
                    [&](size_t completed, size_t count, const std::string& name) {
                        {
                            std::lock_guard lock(queueMutex);
                            renderCompleted = completed;
                            renderCount = count;
                            renderLabel = name;
                        }
                        queueChanged.notify_all();
                        return !cancelled.load();
                    },
                    [&](const std::vector<uint8_t>& dng, Timestamp timestamp) {
                        NativeFrame frame;
                        frame.timestamp = timestamp;
                        PreviewFrame decoded;
                        RenderSettings decodeSettings;
                        DNGImageLayout layout;
                        if (!DNGDecoder::getImageLayout(dng, layout) ||
                            layout.pixels == DNGPixelLayout::CFA ||
                            !DNGDecoder::decodePreview(dng, decodeSettings, decoded, false) ||
                            decoded.width != static_cast<uint32_t>(info->width) ||
                            decoded.height != static_cast<uint32_t>(info->height))
                            throw std::runtime_error("Could not extract RGB16 from rendered frame");
                        if (!metadataOnly) frame.rgb = std::move(decoded.rgb);
                        frame.colorMetadata = std::move(decoded.metadata);
                        DNGDecoder::getGainMaps(dng, 2, frame.gainMaps);
                        DNGDecoder::getGainMaps(dng, 3, frame.deferredGainMaps);
                        frame.duplicateFrame = DNGDecoder::isDuplicateFrame(dng);
                        frame.syntheticFrame = DNGDecoder::isSyntheticFrame(dng);
                        if (stagingSettings.options & motioncam::RENDER_OPT_APPLY_VIGNETTE_CORRECTION) {
                            frame.gainMaps.clear();
                            if (!(stagingSettings.options & motioncam::RENDER_OPT_VIGNETTE_ONLY_COLOR))
                                frame.deferredGainMaps.clear();
                        }
                        std::unique_lock lock(queueMutex);
                        queueChanged.wait(lock, [&] {
                            return frameQueue.size() < 3 || cancelled.load();
                        });
                        if (cancelled.load()) throw std::runtime_error("Finalization cancelled");
                        frameQueue.push_back(std::move(frame));
                        lock.unlock();
                        queueChanged.notify_all();
                    }, false);
            } catch (...) {
                renderError = std::current_exception();
            }
            {
                std::lock_guard lock(queueMutex);
                renderDone = true;
            }
            queueChanged.notify_all();
        });
        struct RendererGuard {
            std::atomic_bool& cancelled;
            std::condition_variable& changed;
            std::thread& thread;
            ~RendererGuard() {
                cancelled.store(true);
                changed.notify_all();
                if (thread.joinable()) thread.join();
            }
        } rendererGuard{cancelled, queueChanged, renderer};

        // Audio is the first finalized entry. Once the first video frame is
        // queued, its staged WAV (if any) is ready and FFmpeg can start.
        while (true) {
            std::unique_lock lock(queueMutex);
            if (!frameQueue.empty() || renderDone) break;
            queueChanged.wait_for(lock, std::chrono::milliseconds(50));
            const auto completed = renderCompleted;
            const auto count = renderCount;
            const auto label = renderLabel;
            lock.unlock();
            progress.setMaximum(static_cast<int>(count));
            progress.setValue(static_cast<int>(completed));
            if (!label.empty()) progress.setLabelText(
#ifdef __ANDROID__
                androidFinalizeEntryName(label)
#else
                QString::fromStdString(label)
#endif
            );
            QApplication::processEvents();
            if (progress.wasCanceled()) cancelled.store(true);
        }
        if (renderError) std::rethrow_exception(renderError);
        if (frameQueue.empty()) throw std::runtime_error("The rendered sequence contains no DNG frames");

        const int frameCount = info->totalFrames - info->droppedFrames + info->duplicatedFrames;

        // Feed our parser's RGB output directly to FFmpeg. This avoids both
        // FFmpeg's unsupported 16-bit RGB DNG path and a sequence-sized raw
        // intermediate file.
        const QString partialPath = stageDir.absoluteFilePath(outputBase + containerExtension);
        const QString partialJsonPath = stageDir.absoluteFilePath(outputBase + ".json");
        const QString partialGyroflowPath = stageDir.absoluteFilePath(outputBase + "_gyroflow.json");
        if (!metadataOnly && QFile::exists(gyroflowPath) &&
            !QFile::copy(gyroflowPath, partialGyroflowPath))
            throw std::runtime_error("Could not stage the Gyroflow lens profile");
        for(const QString& name:{QStringLiteral("audio.wav"),QStringLiteral("audio.flac"),QStringLiteral("audio.opus"),QStringLiteral("audio.aac")}){
            const QString source=QDir(mountPath).absoluteFilePath(name);
            if(QFile::exists(source)){
                const QString target=stageDir.absoluteFilePath(name);
                if(!QFile::copy(source,target))throw std::runtime_error("Could not stage source audio");
                break;
            }
        }
        const bool convertToCfr =
            stagingSettings.options & motioncam::RENDER_OPT_FRAMERATE_CONVERSION;
        QStringList args{"-hide_banner", "-y"};
        // Attach a real track display matrix to the NUT input and prevent
        // FFmpeg from baking it into pixels. A plain output `rotate` metadata
        // tag is discarded when encoding a new MOV/MP4 stream.
        if (outputOrientation == 90 || outputOrientation == 180 || outputOrientation == 270)
            args << "-display_rotation:v:0" << QString::number(-outputOrientation)
                 << "-noautorotate";
        args << "-f" << "nut" << "-i" << "pipe:0";
        QString audioPath;
        for(const QString& name:{QStringLiteral("audio.wav"),QStringLiteral("audio.flac"),QStringLiteral("audio.opus"),QStringLiteral("audio.aac")}){
            const QString candidate=stageDir.absoluteFilePath(name);
            if(QFile::exists(candidate)){audioPath=candidate;break;}
        }
        if (!audioPath.isEmpty())
            args << "-i" << audioPath;
        const QString log60Lut =
            "lutrgb=r=log(1+60*val/maxval)/log(61)*maxval:"
            "g=log(1+60*val/maxval)/log(61)*maxval:"
            "b=log(1+60*val/maxval)/log(61)*maxval";
        const bool proRes444 = mode.contains("ProRes 444") || mode.contains("ProRes XQ");
        const QString pixelFormat = cineFormRgb ? "gbrp12le"
            : proRes444 ? "yuv444p10le"
            : (proRes || cineForm || chroma == "422") ? "yuv422p10le"
            : chroma == "444" ? "yuv444p10le" : "yuv420p10le";
        const QString videoFilter = cineFormRgb
            ? log60Lut +
                ",zscale=matrixin=gbr:matrix=gbr:rangein=full:range=full:dither=ordered,format=gbrp12le"
            : log60Lut +
                ",zscale=matrixin=gbr:matrix=2020_ncl:rangein=full:range=full:dither=ordered,format=" +
                pixelFormat;
        args << "-map" << "0:v:0";
        if (!audioPath.isEmpty()){
            args << "-map" << "1:a:0" << "-c:a"
                 << (audioCodec.isEmpty()?QStringLiteral("copy"):
                     audioCodec=="flac"?QStringLiteral("flac"):QStringLiteral("libopus"));
            if(audioCodec=="opus")args << "-b:a" << "288k";
        }
        args << "-vf" << videoFilter;
        if (av1) {
            // SVT 4 defaults to parallelism level 6 (305 PPCS at 4K), which
            // can exhaust a desktop process's memory during encoder startup.
            // Level 4 keeps preset/quality unchanged while bounding its frame
            // pipeline (148 PPCS in SVT-AV1 4.1).
            if (av1Encoder == "libsvtav1") {
                // Keep seeks within two seconds even for short native clips.
                QString svtParams =
                    "lp=4:keyint=2s:tune=1:sharpness=0:enable-overlays=1:scd=1:scm=0";
                if (hdrNoise)
                    svtParams =
                        "lp=4:keyint=2s:tune=5:sharpness=0:noise=8:enable-overlays=1:scd=1:scm=0";
                args << "-c:v" << av1Encoder << "-crf" << QString::number(av1Crf)
                     << "-preset" << QString::number(av1Preset)
                     << "-svtav1-params" << svtParams;
            } else {
                const QString av1Profile = chroma == "422" ? "2" : "1";
                args << "-c:v" << "libaom-av1" << "-crf" << QString::number(av1Crf)
                     << "-b:v" << "0" << "-cpu-used" << QString::number(av1Preset)
                     << "-profile:v" << av1Profile << "-row-mt" << "1"
                     << "-tune" << "psnr";
            }
        } else if (proRes) {
            const QString profile = mode.contains("ProRes LT") ? "1"
                : mode.contains("ProRes Std") ? "2"
                : mode.contains("ProRes HQ") ? "3"
                : mode.contains("ProRes 444") ? "4" : "5";
            args << "-c:v" << "prores_ks" << "-profile:v" << profile
                 << "-alpha_bits" << "0";
        } else if (cineForm) {
            args << "-c:v" << "cfhd" << "-quality" << "film3+";
        } else {
            const QString hevcProfile = chroma == "422" ? "main422-10"
                : chroma == "444" ? "main444-10" : "main10";
            args << "-c:v" << "libx265" << "-preset" << encoderPreset
                 << "-crf" << encoderCrf << "-profile:v" << hevcProfile
                 << "-x265-params"
                 << "range=full:colorprim=bt2020:colormatrix=bt2020nc:psy-rd=1.0";
        }
        args << "-pix_fmt" << pixelFormat
             << "-color_range" << "pc";
        if (cineFormRgb)
            args << "-colorspace" << "rgb" << "-color_primaries" << "bt2020";
        else
            args << "-colorspace" << "bt2020nc" << "-color_primaries" << "bt2020";
        args
             << "-fps_mode" << (convertToCfr ? "cfr" : "passthrough");
        if (convertToCfr)
            args << "-r" << QString::number(info->fps, 'g', 9);
        else
            // Match the MP4 track scale: fine enough for source VFR timing,
            // without exposing NUT's 1 ns time base to the encoder.
            args << "-enc_time_base:v" << "1:1000000";
        args << "-movflags" << "+write_colr"
             // MOV permits a finer track time scale than FFmpeg's Matroska
             // muxer, preserving sub-millisecond VFR presentation times.
             << "-video_track_timescale" << "1000000";
        args << "-frames:v" << QString::number(frameCount) << partialPath;

        progress.setRange(0, frameCount);
        progress.setValue(0);
        progress.setLabelText(metadataOnly ? "Collecting per-clip metadata..."
                                           : "Encoding LOG60 Camera Native frame 1...");
        progress.show();
        QApplication::processEvents();
        QProcess encoder;
        encoder.setProcessChannelMode(QProcess::MergedChannels);
        if (!metadataOnly) encoder.start(ffmpeg, args, QIODevice::ReadWrite);
        if (!metadataOnly && !encoder.waitForStarted())
            throw std::runtime_error("Could not start FFmpeg: " + encoder.errorString().toStdString());
        QByteArray diagnostic;
        // SVT-AV1 needs a sane nominal rate, but FFmpeg must not use the VFR
        // average as a synchronization target. The timestamps remain the
        // authority in passthrough mode.
        const double encoderNominalFps = convertToCfr
            ? info->fps : info->frameRateInfo.medianFrameRate;
        std::unique_ptr<NutVideoPipe> videoPipe;
        if (!metadataOnly)
            videoPipe = std::make_unique<NutVideoPipe>(
                encoder, diagnostic, info->width, info->height, encoderNominalFps);
        DNGFrameMetadata colorMetadata;
        nlohmann::json dynamicFrames = nlohmann::json::array();
        nlohmann::json gainMapFormats = nlohmann::json::array();
        nlohmann::json gainMapPayloads = nlohmann::json::array();
        std::optional<DNGFrameMetadata> nativeDefaults;
        std::unordered_map<std::string, size_t> gainMapFormatIds;
        std::unordered_map<std::string, size_t> gainMapPayloadIds;
        auto identityMatrix = [](const std::array<float, 9>& matrix) {
            for (size_t i = 0; i < matrix.size(); ++i) {
                const float expected = (i % 4) == 0 ? 1.0f : 0.0f;
                if (std::abs(matrix[i] - expected) > 1.0e-6f) return false;
            }
            return true;
        };
        auto appendDynamicMetadata = [&](const NativeFrame& frame) {
            const auto& metadata = frame.colorMetadata;
            if (!nativeDefaults) nativeDefaults = metadata;
            const auto& defaults = *nativeDefaults;
            nlohmann::json item;
            item["timestampNs"] = frame.timestamp;
            if (metadata.iso != defaults.iso) item["iso"] = metadata.iso;
            if (metadata.exposureTime != defaults.exposureTime)
                item["shutterSpeedSeconds"] = metadata.exposureTime;
            if (metadata.baselineExposure != defaults.baselineExposure)
                item["baselineExposure"] = metadata.baselineExposure;
            if (metadata.asShotNeutral != defaults.asShotNeutral)
                item["asShotNeutral"] = metadata.asShotNeutral;
            if (frame.duplicateFrame) item["duplicateFrame"] = true;
            if (frame.syntheticFrame) item["syntheticFrame"] = true;
            if (metadata.tiffOrientation != defaults.tiffOrientation)
                item["tiffOrientation"] = metadata.tiffOrientation;
            auto emitMatrixOverride = [&](const char* name, bool present,
                                          const std::array<float, 9>& value,
                                          bool defaultPresent,
                                          const std::array<float, 9>& defaultValue,
                                          bool retainIdentity,
                                          bool retainDefaultIdentity = false) {
                const bool include = present && (retainIdentity || !identityMatrix(value));
                const bool defaultInclude = defaultPresent &&
                    (retainDefaultIdentity || !identityMatrix(defaultValue));
                if (include != defaultInclude || (include && value != defaultValue))
                    item[name] = include ? nlohmann::json(value) : nlohmann::json(nullptr);
            };
            emitMatrixOverride("colorMatrix1", metadata.hasColorMatrix1, metadata.colorMatrix1,
                defaults.hasColorMatrix1, defaults.colorMatrix1, false);
            emitMatrixOverride("colorMatrix2", metadata.hasColorMatrix2, metadata.colorMatrix2,
                defaults.hasColorMatrix2, defaults.colorMatrix2, false);
            emitMatrixOverride("forwardMatrix1", metadata.hasForwardMatrix1, metadata.forwardMatrix1,
                defaults.hasForwardMatrix1, defaults.forwardMatrix1, false);
            emitMatrixOverride("forwardMatrix2", metadata.hasForwardMatrix2, metadata.forwardMatrix2,
                defaults.hasForwardMatrix2, defaults.forwardMatrix2, false);
            const bool retainCamera1 = metadata.hasCameraCalibration2 &&
                !identityMatrix(metadata.cameraCalibration2);
            const bool retainCamera2 = metadata.hasCameraCalibration1 &&
                !identityMatrix(metadata.cameraCalibration1);
            const bool retainDefaultCamera1 = defaults.hasCameraCalibration2 &&
                !identityMatrix(defaults.cameraCalibration2);
            const bool retainDefaultCamera2 = defaults.hasCameraCalibration1 &&
                !identityMatrix(defaults.cameraCalibration1);
            emitMatrixOverride("cameraCalibration1", metadata.hasCameraCalibration1,
                metadata.cameraCalibration1, defaults.hasCameraCalibration1,
                defaults.cameraCalibration1, retainCamera1, retainDefaultCamera1);
            emitMatrixOverride("cameraCalibration2", metadata.hasCameraCalibration2,
                metadata.cameraCalibration2, defaults.hasCameraCalibration2,
                defaults.cameraCalibration2, retainCamera2, retainDefaultCamera2);
            if (metadata.calibrationIlluminant1 != defaults.calibrationIlluminant1)
                item["calibrationIlluminant1"] = metadata.calibrationIlluminant1;
            if (metadata.calibrationIlluminant2 != defaults.calibrationIlluminant2)
                item["calibrationIlluminant2"] = metadata.calibrationIlluminant2;

            auto serializeGainMaps = [&](const std::vector<GainMap>& maps) {
                nlohmann::json result = nlohmann::json::array();
                for (const auto& map : maps) {
                    QByteArray rawValues;
                    rawValues.resize(static_cast<qsizetype>(map.data.size() * sizeof(float)));
                    auto* destination = reinterpret_cast<unsigned char*>(rawValues.data());
                    for (size_t index = 0; index < map.data.size(); ++index) {
                        uint32_t bits = 0;
                        std::memcpy(&bits, &map.data[index], sizeof(bits));
                        destination[index * 4] = static_cast<unsigned char>(bits);
                        destination[index * 4 + 1] = static_cast<unsigned char>(bits >> 8);
                        destination[index * 4 + 2] = static_cast<unsigned char>(bits >> 16);
                        destination[index * 4 + 3] = static_cast<unsigned char>(bits >> 24);
                    }
                    // qCompress prepends the uncompressed size. Strip that
                    // prefix so the payload is an ordinary zlib stream that
                    // can be decoded outside Qt.
                    QByteArray compressed = qCompress(rawValues, 9);
                    compressed.remove(0, 4);
                    nlohmann::json format = {
                        {"top", map.top}, {"left", map.left},
                        {"bottom", map.bottom}, {"right", map.right},
                        {"plane", map.plane}, {"planes", map.planes},
                        {"rowPitch", map.rowPitch}, {"colPitch", map.colPitch},
                        {"width", map.width}, {"height", map.height},
                        {"channels", map.channels},
                        // Gain-map bounds remain in the uncropped sensor
                        // coordinate system. Camera-native RGB starts at the
                        // rectangle's top/left, so retain that coordinate
                        // extent for reconstruction during ingest.
                        {"coordinateWidth", map.coordinateWidth},
                        {"coordinateHeight", map.coordinateHeight},
                        {"spacingV", map.spacingV}, {"spacingH", map.spacingH},
                        {"originV", map.originV}, {"originH", map.originH}
                    };
                    const std::string formatKey = format.dump();
                    auto [formatIt, newFormat] = gainMapFormatIds.emplace(
                        formatKey, gainMapFormats.size());
                    if (newFormat) gainMapFormats.push_back(std::move(format));

                    const std::string encoded = compressed.toBase64(
                        QByteArray::Base64Encoding | QByteArray::OmitTrailingEquals).toStdString();
                    const std::string payloadKey = std::to_string(map.data.size()) + ":" + encoded;
                    auto [payloadIt, newPayload] = gainMapPayloadIds.emplace(
                        payloadKey, gainMapPayloads.size());
                    if (newPayload) gainMapPayloads.push_back({
                        {"valueCount", map.data.size()},
                        {"valuesEncoding", "float32-le+zlib+base64"},
                        {"values", encoded}
                    });
                    result.push_back({
                        {"format", formatIt->second},
                        {"payload", payloadIt->second}
                    });
                }
                return result;
            };
            item["gainMaps"] = serializeGainMaps(frame.gainMaps);
            if (!frame.deferredGainMaps.empty())
                item["deferredGainMaps"] = serializeGainMaps(frame.deferredGainMaps);
            dynamicFrames.push_back(std::move(item));
        };
        std::optional<NativeFrame> pendingFrame;
        int frameIndex = 0;
        while (true) {
            if (progress.wasCanceled()) {
                cancelled.store(true);
                queueChanged.notify_all();
                encoder.kill();
                encoder.waitForFinished();
                throw std::runtime_error("Finalization cancelled");
            }

            NativeFrame frame;
            bool haveFrame = false;
            bool done = false;
            size_t completed = 0, count = 1;
            std::string label;
            {
                std::unique_lock lock(queueMutex);
                if (frameQueue.empty() && !renderDone)
                    queueChanged.wait_for(lock, std::chrono::milliseconds(50));
                if (!frameQueue.empty()) {
                    frame = std::move(frameQueue.front());
                    frameQueue.pop_front();
                    haveFrame = true;
                }
                done = renderDone && frameQueue.empty();
                completed = renderCompleted;
                count = renderCount;
                label = renderLabel;
            }
            queueChanged.notify_all();

            if (haveFrame) {
                if (!pendingFrame) colorMetadata = frame.colorMetadata;
                if (pendingFrame) {
                    const int64_t duration = frame.timestamp - pendingFrame->timestamp;
                    if (videoPipe) videoPipe->writeFrame(pendingFrame->rgb, pendingFrame->timestamp,
                        std::max<int64_t>(1, duration));
                    appendDynamicMetadata(*pendingFrame);
                    ++frameIndex;
                    progress.setValue(frameIndex);
                }
                pendingFrame = std::move(frame);
            }
            if (done) {
                if (renderError) std::rethrow_exception(renderError);
                break;
            }
            progress.setMaximum(std::max(frameCount, static_cast<int>(count)));
            if (!haveFrame) progress.setValue(static_cast<int>(completed));
            progress.setLabelText(haveFrame
                ? QString(metadataOnly ? "Collecting metadata for frame %1 of %2..."
                                       : "Rendering and encoding LOG60 Camera Native frame %1 of %2...")
                    .arg(frameIndex + 1).arg(frameCount)
                : QString::fromStdString(label));
            QApplication::processEvents();
        }
        if (!pendingFrame) throw std::runtime_error("The rendered sequence contains no frames");
        if (videoPipe) videoPipe->writeFrame(pendingFrame->rgb, pendingFrame->timestamp,
            std::max<int64_t>(1, static_cast<int64_t>(std::llround(1e9 / info->fps))));
        appendDynamicMetadata(*pendingFrame);
        ++frameIndex;
        if (frameIndex != frameCount)
            throw std::runtime_error("Rendered frame count did not match the clip metadata");
        if (progress.wasCanceled()) throw std::runtime_error("Finalization cancelled");
        if (videoPipe) videoPipe->finish();
        if (!metadataOnly) encoder.closeWriteChannel();
        if (!metadataOnly) progress.setLabelText(QString("Finishing LOG60 Camera Native %1...")
            .arg(containerExtension==".mp4" ? "MP4" : "MOV"));
        while (!metadataOnly && encoder.state() != QProcess::NotRunning) {
            encoder.waitForFinished(100);
            QApplication::processEvents();
            diagnostic += encoder.readAll();
            if (diagnostic.size() > 8000) diagnostic = diagnostic.right(4000);
            if (progress.wasCanceled()) {
                encoder.kill();
                encoder.waitForFinished();
                throw std::runtime_error("Finalization cancelled");
            }
        }
        if (!metadataOnly && (encoder.exitStatus() != QProcess::NormalExit || encoder.exitCode() != 0)) {
            diagnostic += encoder.readAll();
            throw std::runtime_error(("FFmpeg failed:\n" +
                QString::fromUtf8(diagnostic.right(4000))).toStdString());
        }

        // The source sidecar is authoritative for explicit calibration
        // overrides. Keep those values independently of the temporary staging
        // DNG: its RGB conversion is an implementation detail and must not be
        // allowed to strip ForwardMatrix tags from the Camera Native sidecar.
        if (sourceCalibration) {
            if (sourceCalibration->hasColorMatrix1) {
                colorMetadata.colorMatrix1 = sourceCalibration->colorMatrix1;
                colorMetadata.hasColorMatrix1 = true;
            }
            if (sourceCalibration->hasColorMatrix2) {
                colorMetadata.colorMatrix2 = sourceCalibration->colorMatrix2;
                colorMetadata.hasColorMatrix2 = true;
            }
            if (sourceCalibration->hasForwardMatrix1) {
                colorMetadata.forwardMatrix1 = sourceCalibration->forwardMatrix1;
                colorMetadata.hasForwardMatrix1 = true;
            }
            if (sourceCalibration->hasForwardMatrix2) {
                colorMetadata.forwardMatrix2 = sourceCalibration->forwardMatrix2;
                colorMetadata.hasForwardMatrix2 = true;
            }
            if (sourceCalibration->hasCameraCalibration1) {
                colorMetadata.cameraCalibration1 = sourceCalibration->cameraCalibration1;
                colorMetadata.hasCameraCalibration1 = true;
            }
            if (sourceCalibration->hasCameraCalibration2) {
                colorMetadata.cameraCalibration2 = sourceCalibration->cameraCalibration2;
                colorMetadata.hasCameraCalibration2 = true;
            }
            if (sourceCalibration->hasAsShotNeutral) {
                colorMetadata.asShotNeutral = sourceCalibration->asShotNeutral;
                colorMetadata.hasAsShotNeutral = true;
            }
        }

        nlohmann::json sidecar;
        if (!metadataOnly) {
            sidecar["transferFunction"] = "LOG60";
            sidecar["dataLevels"] = "Full";
            sidecar["videoCodec"] = av1 ? "AV1" : proRes ? "ProRes" : cineForm ? "CineForm" : "HEVC";
            sidecar["encoder"] = av1 ? av1Encoder.toStdString()
                : proRes ? "prores_ks" : cineForm ? "cfhd" : "libx265";
            sidecar["pixelFormat"] = pixelFormat.toStdString();
            if (hdrNoise) sidecar["noiseSynthesis"] = 8;
        }
        if (colorMetadata.hasColorMatrix1 && !identityMatrix(colorMetadata.colorMatrix1)) sidecar["colorMatrix1"] = colorMetadata.colorMatrix1;
        if (colorMetadata.hasColorMatrix2 && !identityMatrix(colorMetadata.colorMatrix2)) sidecar["colorMatrix2"] = colorMetadata.colorMatrix2;
        if (colorMetadata.hasForwardMatrix1 && !identityMatrix(colorMetadata.forwardMatrix1)) sidecar["forwardMatrix1"] = colorMetadata.forwardMatrix1;
        if (colorMetadata.hasForwardMatrix2 && !identityMatrix(colorMetadata.forwardMatrix2)) sidecar["forwardMatrix2"] = colorMetadata.forwardMatrix2;
        const bool includeCamera1 = colorMetadata.hasCameraCalibration1 &&
            (!identityMatrix(colorMetadata.cameraCalibration1) ||
             (colorMetadata.hasCameraCalibration2 && !identityMatrix(colorMetadata.cameraCalibration2)));
        const bool includeCamera2 = colorMetadata.hasCameraCalibration2 &&
            (!identityMatrix(colorMetadata.cameraCalibration2) ||
             (colorMetadata.hasCameraCalibration1 && !identityMatrix(colorMetadata.cameraCalibration1)));
        if (includeCamera1) sidecar["cameraCalibration1"] = colorMetadata.cameraCalibration1;
        if (includeCamera2) sidecar["cameraCalibration2"] = colorMetadata.cameraCalibration2;
        if (colorMetadata.calibrationIlluminant1) sidecar["calibrationIlluminant1"] = colorMetadata.calibrationIlluminant1;
        if (colorMetadata.calibrationIlluminant2) sidecar["calibrationIlluminant2"] = colorMetadata.calibrationIlluminant2;
        if (colorMetadata.hasAsShotNeutral) sidecar["asShotNeutral"] = colorMetadata.asShotNeutral;
        if (nativeDefaults) {
            sidecar["iso"] = nativeDefaults->iso;
            sidecar["shutterSpeedSeconds"] = nativeDefaults->exposureTime;
            sidecar["baselineExposure"] = nativeDefaults->baselineExposure;
            sidecar["tiffOrientation"] = nativeDefaults->tiffOrientation;
        }
        sidecar["dynamic"] = {
            {"gainMapFormats", std::move(gainMapFormats)},
            {"gainMapPayloads", std::move(gainMapPayloads)},
            {"frames", std::move(dynamicFrames)}
        };
        std::ofstream jsonFile(partialJsonPath.toStdString(), std::ios::trunc);
        if (!jsonFile)
            throw std::runtime_error("Could not create Camera Native JSON sidecar");
        jsonFile << sidecar.dump(2) << '\n';
        if (!jsonFile)
            throw std::runtime_error("Could not write Camera Native JSON sidecar");
        jsonFile.close();

        if (metadataOnly) {
            const QString sourceGcsv = sourceInfo.absolutePath() + "/" +
                sourceInfo.completeBaseName() + ".gcsv";
            const QString partialGcsv = stageDir.absoluteFilePath(outputBase + ".gcsv");
            if (QFile::exists(sourceGcsv)) {
                copyFileAtomically(sourceGcsv, partialGcsv);
            } else if (srcFile.endsWith(".mcraw", Qt::CaseInsensitive)) {
                Decoder decoder(srcFile.toStdString());
                if (decoder.hasGyroData()) {
                    std::vector<MotionSample> samples;
                    decoder.loadGyroData(samples);
                    if (!samples.empty()) {
                        std::ofstream gcsv(partialGcsv.toStdString(), std::ios::trunc);
                        if (!gcsv) throw std::runtime_error("Could not create GCSV sidecar");
                        gcsv << "GYROFLOW IMU LOG\nversion,1.2\nid,MotionCam\n"
                             << "orientation,XYZ\nvendor,MotionCam\n"
                             << "tscale,0.000000001\ngscale,1\nt,gx,gy,gz\n"
                             << std::setprecision(9);
                        const auto& sourceFrames = decoder.getFrames();
                        if (sourceFrames.empty())
                            throw std::runtime_error("Could not find the first source frame for GCSV timing");
                        const int64_t firstTimestamp = sourceFrames.front();
                        for (const auto& sample : samples)
                            gcsv << sample.timestampNs - firstTimestamp << ','
                                 << sample.x << ',' << sample.y << ',' << sample.z << '\n';
                        if (!gcsv) throw std::runtime_error("Could not write GCSV sidecar");
                    }
                }
            }

            QString partialAudio;
            QString outputAudio;
            for (const QString& extension : {QStringLiteral("wav"), QStringLiteral("flac"),
                                             QStringLiteral("opus"), QStringLiteral("aac")}) {
                const QString candidate = stageDir.absoluteFilePath("audio." + extension);
                if (QFile::exists(candidate)) {
                    partialAudio = stageDir.absoluteFilePath(outputBase + "." + extension);
                    outputAudio = audioBasePath + "." + extension;
                    copyFileAtomically(candidate, partialAudio);
                    break;
                }
            }

            std::vector<std::pair<QString, QString>> outputs{{partialJsonPath, jsonPath}};
            if (QFile::exists(partialGcsv)) outputs.emplace_back(partialGcsv, gcsvPath);
            if (!partialAudio.isEmpty()) outputs.emplace_back(partialAudio, outputAudio);
            if (progress.wasCanceled()) throw std::runtime_error("Finalization cancelled");

            // Treat all files produced by this mode as one replacement set.
            // A rerun may switch audio extensions or have no gyro data.
            QStringList oldPaths{jsonPath, gcsvPath};
            for (const char* extension : {"wav", "flac", "opus", "aac"})
                oldPaths << audioBasePath + "." + extension;
            const QString backupId = ".metadata-backup-" +
                QUuid::createUuid().toString(QUuid::WithoutBraces);
            std::vector<std::pair<QString, QString>> backups;
            std::vector<QString> committed;
            try {
                for (const QString& destination : oldPaths) {
                    if (!QFile::exists(destination)) continue;
                    const QString backup = destination + backupId;
                    if (!QFile::rename(destination, backup))
                        throw std::runtime_error("Could not preserve existing metadata output");
                    backups.emplace_back(backup, destination);
                }
                for (const auto& [partial, destination] : outputs) {
                    if (!QFile::rename(partial, destination))
                        throw std::runtime_error("Could not commit metadata output");
                    committed.push_back(destination);
                }
            } catch (...) {
                for (const auto& destination : committed) QFile::remove(destination);
                for (const auto& [backup, destination] : backups)
                    QFile::rename(backup, destination);
                throw;
            }
            for (const auto& [backup, destination] : backups) QFile::remove(backup);
            progress.close();
            QMessageBox::information(this, "Metadata finalization",
                QString("Created %1 output file(s) beside the source clip.").arg(outputs.size()));
            mFuseFilesystem->updateOptions(mountId, buildRenderSettings());
            return;
        }

        // Commit the pair together. Existing outputs are retained until both
        // temporary files are complete and are restored if either rename fails.
        const QString backupId = ".camera-native-backup-" + QUuid::createUuid().toString(QUuid::WithoutBraces);
        const QString mediaBackup = outputPath + backupId;
        const QString jsonBackup = jsonPath + backupId;
        const QString gyroflowBackup = outputGyroflowPath + backupId;
        const bool hadMedia = QFile::exists(outputPath);
        const bool hadJson = QFile::exists(jsonPath);
        const bool hadGyroflow = QFile::exists(outputGyroflowPath);
        if ((hadMedia && !QFile::rename(outputPath, mediaBackup)) ||
            (hadJson && !QFile::rename(jsonPath, jsonBackup)) ||
            (hadGyroflow && !QFile::rename(outputGyroflowPath, gyroflowBackup))) {
            if (QFile::exists(mediaBackup)) QFile::rename(mediaBackup, outputPath);
            if (QFile::exists(jsonBackup)) QFile::rename(jsonBackup, jsonPath);
            if (QFile::exists(gyroflowBackup))
                QFile::rename(gyroflowBackup, outputGyroflowPath);
            throw std::runtime_error("Could not preserve the existing Camera Native output");
        }
        const bool mediaCommitted = QFile::rename(partialPath, outputPath);
        const bool jsonCommitted = mediaCommitted && QFile::rename(partialJsonPath, jsonPath);
        const bool gyroflowCommitted = jsonCommitted &&
            (!QFile::exists(gyroflowPath) ||
             QFile::rename(partialGyroflowPath, outputGyroflowPath));
        if (!gyroflowCommitted) {
            if (mediaCommitted) QFile::remove(outputPath);
            if (jsonCommitted) QFile::remove(jsonPath);
            if (QFile::exists(mediaBackup)) QFile::rename(mediaBackup, outputPath);
            if (QFile::exists(jsonBackup)) QFile::rename(jsonBackup, jsonPath);
            if (QFile::exists(gyroflowBackup))
                QFile::rename(gyroflowBackup, outputGyroflowPath);
            throw std::runtime_error("Could not commit the completed Camera Native video and JSON");
        }
        QFile::remove(mediaBackup);
        QFile::remove(jsonBackup);
        QFile::remove(gyroflowBackup);

        progress.close();
#ifdef __ANDROID__
        showAndroidNotice(tr("Camera Native finalization"),
            tr("Created:\n%1\n%2").arg(outputPath, jsonPath));
#else
        QMessageBox::information(this, "Camera Native finalization",
            QString("Created:\n%1\n%2").arg(outputPath, jsonPath));
#endif
    } catch (const std::exception& e) {
        progress.close();
        if (std::string(e.what()) != "Finalization cancelled") {
#ifdef __ANDROID__
            showAndroidNotice(tr("Camera Native finalization"),
                              QString::fromStdString(e.what()));
#else
            QMessageBox::critical(this, "Camera Native finalization", QString::fromStdString(e.what()));
#endif
        }
    }

    // Restore the mounted view; Camera Native settings affect finalization only.
    mFuseFilesystem->updateOptions(mountId, buildRenderSettings());
}

void MainWindow::finalizeFile(QWidget* fileWidget) {
    QString compressionMode = ui->dngCompressionModeComboBox->currentText();
    const QString requestedMode = compressionMode;
    const QString audioCodec=ui->dngCompressionCheckBox->isChecked()
        ?finalizeAudioCodec(compressionMode):QString{};
    const bool archiveDngs = ui->dngCompressionCheckBox->isChecked() &&
        isSevenZipFinalizeFormat(compressionMode);
    if (ui->dngCompressionCheckBox->isChecked()) {
        if (isCameraNativeFormat(compressionMode) ||
            isMetadataOnlyFinalizeFormat(compressionMode)) {
            finalizeCameraNative(fileWidget, requestedMode,
                                 isMetadataOnlyFinalizeFormat(compressionMode));
            return;
        }
        if (archiveDngs) {
            // 7z finalization archives the original MCRAW below. It does not
            // pass through the DNG rendering pipeline.
        } else {
            float distance = 0.0f;
            if (!dngFinalizeCompressionDistance(compressionMode, distance)) {
                QMessageBox::warning(this, tr("Invalid finalize format"),
                    tr("Enter a complete JPEG XL format such as ‘JPEG XL DCT 0.1’."));
                return;
            }
            mRenderSettings.jxlDistance = distance;
        }
    }
    auto mountPath = fileWidget->property("mountPath").toString();
    auto srcFile = fileWidget->property("filePath").toString();

    if (mountPath.isEmpty() || srcFile.isEmpty()) {
        spdlog::error("Finalize failed: mount information not found (mountPath: {}, srcFile: {})",
                     mountPath.toStdString(), srcFile.toStdString());
        return;
    }

    // DirectLog audio is decoded for the mounted view. Keep its original
    // compressed packets when the persistent format permits the same codec.
    QString sourceAudioCodec;
    if (srcFile.contains("NATIVE", Qt::CaseInsensitive) &&
        (srcFile.endsWith(".mov", Qt::CaseInsensitive) ||
         srcFile.endsWith(".mp4", Qt::CaseInsensitive) ||
         srcFile.endsWith(".mkv", Qt::CaseInsensitive))) {
        AVFormatContext* sourceFormat = nullptr;
        if (avformat_open_input(&sourceFormat, srcFile.toUtf8().constData(), nullptr, nullptr) >= 0) {
            if (avformat_find_stream_info(sourceFormat, nullptr) >= 0) {
                const int stream = av_find_best_stream(sourceFormat, AVMEDIA_TYPE_AUDIO,
                                                       -1, -1, nullptr, 0);
                if (stream >= 0) {
                    switch (sourceFormat->streams[stream]->codecpar->codec_id) {
                    case AV_CODEC_ID_FLAC: sourceAudioCodec = "flac"; break;
                    case AV_CODEC_ID_OPUS: sourceAudioCodec = "opus"; break;
                    case AV_CODEC_ID_AAC: sourceAudioCodec = "aac"; break;
                    default: break;
                    }
                }
            }
            avformat_close_input(&sourceFormat);
        }
    }

    // Get mount ID and file info
    bool ok = false;
    auto mountId = fileWidget->property("mountId").toInt(&ok);
    if (!ok) {
        spdlog::error("Finalize failed: invalid mount ID");
        return;
    }

    auto fileInfo = mFuseFilesystem->getFileInfo(mountId);
    if (!fileInfo.has_value()) {
        spdlog::error("Finalize failed: could not get file information");
        return;
    }

    int totalFrames = fileInfo->totalFrames - fileInfo->droppedFrames + fileInfo->duplicatedFrames;
    if (totalFrames <= 0) {
        QMessageBox::warning(this, "Finalize failed", "The clip contains no renderable frames.");
        return;
    }

    if (archiveDngs &&
        QFileInfo(srcFile).suffix().compare("mcraw", Qt::CaseInsensitive) != 0) {
        QMessageBox::warning(this, tr("Finalize failed"),
            tr("7z finalization is only available for MCRAW source clips. "
               "DNG sequences and DirectLog clips cannot be archived with this format."));
        return;
    }

    QString archivePath;
    QString archiveSidecarPath;
    QString archiveGyroflowPath;
    QString sourceSidecarPath;
    QString sourceGyroflowPath;
    QString archiver;
    if (archiveDngs) {
        archivePath = mountPath + ".7z";
        const QString importPath = fileWidget->property("importPath").toString();
        if (importPath.endsWith(".7z", Qt::CaseInsensitive)) {
            const QFileInfo sourceArchive(importPath);
            archivePath = QDir(sourceArchive.absolutePath()).absoluteFilePath(
                sourceArchive.completeBaseName() + "_finalized.7z");
        }
        const QFileInfo archiveInfo(archivePath);
        const QString archiveBase = QDir(archiveInfo.absolutePath()).absoluteFilePath(
            archiveInfo.completeBaseName());
        archiveSidecarPath = archiveBase + ".json";
        archiveGyroflowPath = archiveBase + "_gyroflow.json";
        sourceSidecarPath = fileWidget->property("sidecarPath").toString();
        sourceGyroflowPath = fileWidget->property("gyroflowSidecarPath").toString();
        if (sourceSidecarPath.isEmpty())
            sourceSidecarPath = QString::fromStdString(
                motioncam::vfs::sidecarPath(srcFile.toStdString()).string());
        if (sourceGyroflowPath.isEmpty())
            sourceGyroflowPath = QString::fromStdString(
                motioncam::vfs::gyroflowSidecarPath(srcFile.toStdString()).string());
        archiver = sevenZipExecutable();
        if (archiver.isEmpty()) {
            QMessageBox::critical(this, tr("Finalize failed"),
                tr("7-Zip was not found. Install 7-Zip (7zz, 7z, or 7za) and try again."));
            return;
        }
        if ((QFileInfo::exists(archivePath) ||
             (QFileInfo::exists(sourceSidecarPath) && QFileInfo::exists(archiveSidecarPath) &&
              QFileInfo(sourceSidecarPath).absoluteFilePath() != QFileInfo(archiveSidecarPath).absoluteFilePath()) ||
             (QFileInfo::exists(sourceGyroflowPath) && QFileInfo::exists(archiveGyroflowPath) &&
              QFileInfo(sourceGyroflowPath).absoluteFilePath() != QFileInfo(archiveGyroflowPath).absoluteFilePath())) &&
            QMessageBox::question(this, tr("Replace finalized archive?"),
                tr("Replace the existing 7z output and sidecars for:\n%1").arg(archivePath),
                QMessageBox::Yes | QMessageBox::No, QMessageBox::No) != QMessageBox::Yes)
            return;
    }

    const bool hasDuplicateFrames = fileInfo->duplicateFrameMask &&
        std::any_of(fileInfo->duplicateFrameMask->begin(),
                    fileInfo->duplicateFrameMask->end(),
                    [](bool duplicate) { return duplicate; });
    const bool interpolateFrames = RIFE_SUPPORTED && !archiveDngs &&
        ui->rifeInterpolationCheckBox->isChecked() &&
        (fileInfo->duplicatedFrames > 0 || hasDuplicateFrames);
    const auto rifeRuntime = interpolateFrames ? ensureRifeRuntime() : std::optional<QString>{QString{}};
    if (!rifeRuntime) return;

    QString tempPath;
    QString finalizedFolderName;
    std::unique_ptr<QTemporaryDir> audioStaging;
    const QString retainedOutputPath = mountPath + "-finalized";
    QDir tempDir;
#ifdef __ANDROID__
    const bool androidDirectOutput = !archiveDngs;
    if (androidDirectOutput) {
        const QString folderName = QFileInfo(mountPath).fileName() + "-finalized";
        const QString destinationMode=QSettings(PACKAGE_NAME,APP_NAME).value(
            "android/finalizeDestination","documents").toString();
        const QString destination = motioncam::prepareFinalizeOutput(folderName,
            destinationMode,fileWidget->property("importPath").toString());
        if (destination == "choose") {
            mPendingAndroidFinalizeCard=fileWidget;
            return;
        }
        if (!destination.startsWith("ready:")) {
            showAndroidNotice(tr("Finalize failed"), destination.mid(6));
            return;
        }
        finalizedFolderName = destination.mid(6);
        if (!audioCodec.isEmpty() || !sourceAudioCodec.isEmpty()) {
            audioStaging = std::make_unique<QTemporaryDir>();
            if (!audioStaging->isValid()) {
                motioncam::cancelFinalizeOutput();
                showAndroidNotice(tr("Finalize failed"), tr("Could not prepare audio staging."));
                return;
            }
        }
    }
#else
    constexpr bool androidDirectOutput = false;
#endif
    if (!androidDirectOutput) {
        // Desktop finalization uses a local transaction directory. Android
        // streams completed entries straight to its SAF destination.
        tempPath = mountPath + ".finalizing-" +
            QUuid::createUuid().toString(QUuid::WithoutBraces);
        if (!mUnmountOnFinalize && QFileInfo::exists(retainedOutputPath) &&
            QMessageBox::question(this, "Replace finalized output?",
                QString("Replace the existing finalized output at:\n%1").arg(retainedOutputPath),
                QMessageBox::Yes | QMessageBox::No, QMessageBox::No) != QMessageBox::Yes)
            return;
        tempDir.setPath(tempPath);
        if (!tempDir.mkpath(".")) {
            spdlog::error("Failed to create temp directory: {}", tempPath.toStdString());
#ifdef __ANDROID__
            showAndroidNotice(tr("Finalize failed"),
                              tr("Could not create the temporary render directory."));
#else
            QMessageBox::critical(this, "Finalize failed", "Could not create the temporary render directory.");
#endif
            return;
        }
        spdlog::info("Using temp directory: {}", tempPath.toStdString());
    }

    // Create progress dialog
    QProgressDialog progress(
        archiveDngs ? "Preparing MCRAW archive..." : "Rendering DNG sequence...",
        "Cancel", 0, totalFrames, this);
    progress.setWindowModality(Qt::WindowModal);
    progress.setMinimumDuration(0);
    progress.setAutoClose(false);
    progress.setAutoReset(false);
#ifdef __ANDROID__
    AndroidFinalizeWakeLock finalizeWakeLock;
    showAndroidProgress(progress,this);
#endif
    progress.setValue(0);

    // Get current render config
    auto settings = buildRenderSettings();
    const bool exportBadPixelOptions = !archiveDngs &&
        motioncam::effectiveBadPixelTreatment(settings.badPixelTreatment,
            settings.vignetteCorrection) == motioncam::BadPixelTreatment::OptionsJsonOnly &&
        !(settings.options & motioncam::RENDER_OPT_REMOSAIC_TO_BAYER);
    QString badPixelSidecar = fileWidget->property("sidecarPath").toString();
    if (badPixelSidecar.isEmpty()) badPixelSidecar = QString::fromStdString(
        motioncam::vfs::sidecarPath(srcFile.toStdString()).string());

    bool enableCompression = !archiveDngs &&
        (settings.options & motioncam::RENDER_OPT_JPEG_COMPRESSION);
    motioncam::FinalizeOptions finalizeOptions;
    finalizeOptions.interpolateDuplicatedFrames = interpolateFrames;
    finalizeOptions.detectDuplicateDngs = false;
    finalizeOptions.rifeDirectory = rifeRuntime->toStdString();
    finalizeOptions.jxlDistance = settings.jxlDistance;

    spdlog::info("Starting finalize for {} ({} frames from fileInfo, compression: {})",
                 srcFile.toStdString(), totalFrames, enableCompression ? "enabled" : "disabled");
    spdlog::info("Compression checkbox state: {}", enableCompression);


    // Render frame-based formats directly to disk (bypassing ProjectedFS).
    // MCRAW archival deliberately skips this operation.
    bool mountReleased = false;
    try {
        if (!archiveDngs) {
            auto updateFinalizeProgress = [&](size_t completed, size_t count,
                                               const std::string& name) {
                    progress.setMaximum(static_cast<int>(count));
                    progress.setValue(static_cast<int>(completed));
                    if (!name.empty()) {
                        progress.setLabelText(
                            QString("Finalizing—step %1 of %2:\n%3")
                                .arg(std::min(completed + 1, count)).arg(count)
                                .arg(
#ifdef __ANDROID__
                                    androidFinalizeEntryName(name)
#else
                                    QString::fromStdString(name)
#endif
                                ));
                    }
                    QApplication::processEvents();
                    return !progress.wasCanceled();
                };
#ifdef __ANDROID__
            if (androidDirectOutput) {
                auto* processing = dynamic_cast<motioncam::ProcessingFileSystem*>(
                    mFuseFilesystem.get());
                if (!processing)
                    throw std::runtime_error("Android processing filesystem is unavailable");
                const auto writeEntry = [](const std::string& name,
                                            const std::vector<uint8_t>& bytes) {
                    if (!motioncam::writeFinalizeOutputEntry(QString::fromStdString(name), bytes))
                        throw std::runtime_error("Could not write finalized entry " + name +
                            " to the selected output folder");
                };
                bool inspectedCfa = false;
                processing->finalizeToEntries(mountId, enableCompression,
                    finalizeOptions, updateFinalizeProgress,
                    [&](const std::string& name, const std::vector<uint8_t>& bytes) {
                        const QString entryName = QString::fromStdString(name);
                        if (audioStaging && entryName.startsWith("audio.") &&
                            QStringList{"wav", "flac", "opus", "aac"}.contains(
                                QFileInfo(entryName).suffix().toLower())) {
                            QFile audio(QDir(audioStaging->path()).filePath(entryName));
                            if (!audio.open(QIODevice::WriteOnly) ||
                                audio.write(reinterpret_cast<const char*>(bytes.data()), bytes.size()) !=
                                    static_cast<qint64>(bytes.size()) || !audio.flush())
                                throw std::runtime_error("Could not stage finalized audio");
                            return;
                        }
                        writeEntry(name, bytes);
                        if (!inspectedCfa && entryName.endsWith(".dng", Qt::CaseInsensitive)) {
                            inspectedCfa = true;
                            if (exportBadPixelOptions) {
                                const auto json = motioncam::vfs::finalizedCfaBadPixelOptions(
                                    badPixelSidecar.toStdString(), bytes);
                                if (!json.empty()) writeEntry(
                                    (finalizedFolderName + ".json").toStdString(),
                                    std::vector<uint8_t>(json.begin(), json.end()));
                            }
                        }
                    });
            } else
#endif
                mFuseFilesystem->finalize(mountId, tempPath.toStdString(),
                    enableCompression, finalizeOptions, updateFinalizeProgress);
            if((!audioCodec.isEmpty() || !sourceAudioCodec.isEmpty()) &&
               !progress.wasCanceled()){
                const QString audioDirectory = audioStaging ? audioStaging->path() : tempPath;
                QString sourceAudio;
                for(const QString& name:{QStringLiteral("audio.wav"),QStringLiteral("audio.flac"),QStringLiteral("audio.opus"),QStringLiteral("audio.aac")}){
                    const QString candidate=QDir(audioDirectory).absoluteFilePath(name);
                    if(QFile::exists(candidate)){sourceAudio=candidate;break;}
                }
                const bool copySource = !sourceAudioCodec.isEmpty() &&
                    (audioCodec.isEmpty() || audioCodec == sourceAudioCodec);
                const QString targetCodec = copySource ? sourceAudioCodec : audioCodec;
                if(!sourceAudio.isEmpty() && !targetCodec.isEmpty() &&
                   (copySource || !sourceAudio.endsWith("."+targetCodec,Qt::CaseInsensitive))){
                    const QString target=QDir(audioDirectory).absoluteFilePath("audio."+targetCodec);
                    progress.setLabelText((copySource ? "Copying " : "Encoding ")+
                                          targetCodec.toUpper()+" audio...");
                    AudioWriter::convertFile((copySource ? srcFile : sourceAudio).toStdString(),
                        target.toStdString(), targetCodec.toStdString(), copySource, [&] {
                            QApplication::processEvents();
                            return !progress.wasCanceled();
                        });
                    if (sourceAudio != target) QFile::remove(sourceAudio);
                }
            }
        }

        if (!progress.wasCanceled()) {
            if (!archiveDngs)
                spdlog::info("Rendered {} frames to temp directory", totalFrames);
            if (!androidDirectOutput && exportBadPixelOptions)
                writeFinalizedCfaBadPixelOptions(tempPath,
                    mUnmountOnFinalize ? mountPath : retainedOutputPath, badPixelSidecar);

#ifdef __ANDROID__
            if (androidDirectOutput) {
                if (audioStaging) {
                    for (const auto& name : QDir(audioStaging->path()).entryList(QDir::Files)) {
                        QFile audio(QDir(audioStaging->path()).filePath(name));
                        if (!audio.open(QIODevice::ReadOnly))
                            throw std::runtime_error("Could not read finalized audio");
                        const auto data = audio.readAll();
                        if (audio.error() != QFileDevice::NoError ||
                            !motioncam::writeFinalizeOutputEntry(name,
                                std::vector<uint8_t>(data.begin(), data.end())))
                            throw std::runtime_error("Could not write finalized audio to the selected output folder");
                    }
                }
                motioncam::finishFinalizeOutput();
                progress.close();
                showAndroidNotice(tr("Finalize complete"),
                    tr("The rendered files were written to the selected output folder."));
                if (!mUnmountOnFinalize) return;
                mFuseFilesystem->unmount(mountId);
                cleanupArchiveMount(mountId);
                mountReleased = true;
            } else
#endif
            if (archiveDngs) {
                progress.setRange(0, 0);
                progress.setLabelText("Compressing original MCRAW with 7z LZMA2 Ultra...");
                QApplication::processEvents();
                const QString transactionId = ".partial-" +
                    QUuid::createUuid().toString(QUuid::WithoutBraces);
                const QString partialArchive = archivePath + transactionId;
                const QString partialSidecar = archiveSidecarPath + transactionId;
                const QString partialGyroflow = archiveGyroflowPath + transactionId;
                RemoveFilesOnExit partialFiles{{partialArchive, partialSidecar, partialGyroflow}};
                QProcess compressor;
                const QFileInfo sourceInfo(srcFile);
                compressor.setWorkingDirectory(sourceInfo.absolutePath());
                compressor.setProcessChannelMode(QProcess::MergedChannels);
                compressor.start(archiver, {"a", "-t7z", "-m0=lzma2", "-mx=9",
                    "-mmt=on", "-ms=on", "-sse", "-y", partialArchive,
                    sourceInfo.fileName()});
                if (!compressor.waitForStarted())
                    throw std::runtime_error("Could not start 7-Zip");
                QByteArray diagnostic;
                while (compressor.state() != QProcess::NotRunning) {
                    compressor.waitForFinished(100);
                    diagnostic += compressor.readAll();
                    if (diagnostic.size() > 16000) diagnostic = diagnostic.right(8000);
                    QApplication::processEvents();
                    if (progress.wasCanceled()) {
                        compressor.kill();
                        compressor.waitForFinished();
                        QFile::remove(partialArchive);
                        throw std::runtime_error("Finalization cancelled");
                    }
                }
                diagnostic += compressor.readAll();
                if (compressor.exitStatus() != QProcess::NormalExit || compressor.exitCode() != 0) {
                    QFile::remove(partialArchive);
                    throw std::runtime_error(("7-Zip failed:\n" +
                        QString::fromUtf8(diagnostic.right(8000))).toStdString());
                }
                if (!QFileInfo::exists(partialArchive))
                    throw std::runtime_error("7-Zip did not create the output archive");

                copyFileAtomically(sourceSidecarPath, partialSidecar);
                copyFileAtomically(sourceGyroflowPath, partialGyroflow);
                const QString backupId = ".backup-" +
                    QUuid::createUuid().toString(QUuid::WithoutBraces);
                const QString archiveBackup = archivePath + backupId;
                const QString sidecarBackup = archiveSidecarPath + backupId;
                const QString gyroflowBackup = archiveGyroflowPath + backupId;
                const bool stageSidecar = QFileInfo::exists(partialSidecar);
                const bool stageGyroflow = QFileInfo::exists(partialGyroflow);
                const bool hadArchive = QFileInfo::exists(archivePath);
                // Existing companions belong to the archive being replaced.
                // Preserve them even when this render has no replacement so a
                // successful commit removes stale metadata, while a failed
                // commit can still restore the complete previous output.
                const bool hadSidecar = QFileInfo::exists(archiveSidecarPath);
                const bool hadGyroflow = QFileInfo::exists(archiveGyroflowPath);
                auto preserve = [](const QString& path, const QString& backup, bool exists) {
                    return !exists || QFile::rename(path, backup);
                };
                if (!preserve(archivePath, archiveBackup, hadArchive) ||
                    !preserve(archiveSidecarPath, sidecarBackup, hadSidecar) ||
                    !preserve(archiveGyroflowPath, gyroflowBackup, hadGyroflow)) {
                    if (QFileInfo::exists(archiveBackup)) QFile::rename(archiveBackup, archivePath);
                    if (QFileInfo::exists(sidecarBackup)) QFile::rename(sidecarBackup, archiveSidecarPath);
                    if (QFileInfo::exists(gyroflowBackup)) QFile::rename(gyroflowBackup, archiveGyroflowPath);
                    throw std::runtime_error("Could not preserve existing 7z output files");
                }
                const bool archiveCommitted = QFile::rename(partialArchive, archivePath);
                const bool sidecarCommitted = archiveCommitted &&
                    (!stageSidecar || QFile::rename(partialSidecar, archiveSidecarPath));
                const bool gyroflowCommitted = sidecarCommitted &&
                    (!stageGyroflow || QFile::rename(partialGyroflow, archiveGyroflowPath));
                if (!gyroflowCommitted) {
                    if (archiveCommitted) QFile::remove(archivePath);
                    if (sidecarCommitted && stageSidecar) QFile::remove(archiveSidecarPath);
                    if (QFileInfo::exists(archiveBackup)) QFile::rename(archiveBackup, archivePath);
                    if (QFileInfo::exists(sidecarBackup)) QFile::rename(sidecarBackup, archiveSidecarPath);
                    if (QFileInfo::exists(gyroflowBackup)) QFile::rename(gyroflowBackup, archiveGyroflowPath);
                    QFile::remove(partialArchive);
                    QFile::remove(partialSidecar);
                    QFile::remove(partialGyroflow);
                    throw std::runtime_error("Could not commit the completed 7z archive and sidecars");
                }
                QFile::remove(archiveBackup);
                QFile::remove(sidecarBackup);
                QFile::remove(gyroflowBackup);
                if (!tempDir.removeRecursively())
                    spdlog::warn("Could not remove temporary DNG render: {}", tempPath.toStdString());

                if (!mUnmountOnFinalize) {
                    progress.close();
#ifdef __ANDROID__
                    showAndroidNotice(tr("Finalize complete"),
                        QString("The finalized archive was created at:\n%1").arg(archivePath));
#else
                    QMessageBox::information(this, "Finalize complete",
                        QString("The clip remains mounted. Finalized archive was created at:\n%1")
                            .arg(archivePath));
#endif
                    return;
                }

                mFuseFilesystem->unmount(mountId);
                cleanupArchiveMount(mountId);
                mountReleased = true;
#ifdef _WIN32
                QThread::msleep(150);
#elif __APPLE__
                if (fileWidget->property("projectedFiles").toBool() &&
                    !waitForMacFuseUnmount(mountPath, 10000))
                    throw std::runtime_error("Timed out waiting for the macOS FUSE mount to close");
#endif
                QDir mountDir(mountPath);
                if (mountDir.exists() && !mountDir.removeRecursively())
                    throw std::runtime_error("Could not remove the unmounted output directory");
                spdlog::info("Finalize complete: original MCRAW archived to {}",
                             archivePath.toStdString());
            } else {
                // Now move files from temp to final location
                progress.setLabelText("Moving files to final location...");
                QApplication::processEvents();

                if (!mUnmountOnFinalize) {
                const QFileInfo retainedInfo(retainedOutputPath);
                const bool removedExisting = !retainedInfo.exists() ||
                    (retainedInfo.isDir() ? QDir(retainedOutputPath).removeRecursively()
                                          : QFile::remove(retainedOutputPath));
                if (!removedExisting)
                    throw std::runtime_error("Could not replace the existing finalized output directory");
                QDir parentDir(QFileInfo(retainedOutputPath).absolutePath());
                if (!parentDir.rename(tempPath, retainedOutputPath))
                    throw std::runtime_error("Could not move the completed render into the finalized output directory");
                progress.close();
                QMessageBox::information(this, "Finalize complete",
                    QString("The clip remains mounted. Finalized output was created at:\n%1")
                        .arg(retainedOutputPath));
                spdlog::info("Finalize complete: {} frames rendered to {} without unmounting",
                             totalFrames, retainedOutputPath.toStdString());
                return;
                }

            // Keep the active mount intact until rendering has fully succeeded.
                mFuseFilesystem->unmount(mountId);
                cleanupArchiveMount(mountId);
                mountReleased = true;
#ifdef _WIN32
            QThread::msleep(150);
#elif __APPLE__
            if (fileWidget->property("projectedFiles").toBool() &&
                !waitForMacFuseUnmount(mountPath, 10000)) {
                throw std::runtime_error("Timed out waiting for the macOS FUSE mount to close");
            }
#endif

                QDir mountDir(mountPath);
                if (mountDir.exists() && !mountDir.removeRecursively()) {
                    throw std::runtime_error("Could not remove the unmounted output directory");
                }

                QDir parentDir(QFileInfo(mountPath).absolutePath());
                if (!parentDir.rename(tempPath, mountPath)) {
                    throw std::runtime_error("Could not move the completed render into place");
                }

                spdlog::info("Finalize complete: {} frames rendered to {}",
                             totalFrames, mountPath.toStdString());
            }
        } else {
            spdlog::info("Finalize cancelled by user");
            if (!androidDirectOutput) tempDir.removeRecursively();
#ifdef __ANDROID__
            if (androidDirectOutput) motioncam::cancelFinalizeOutput();
#endif
            return;
        }

    } catch (const std::exception& e) {
        spdlog::error("Finalize failed: {}", e.what());
#ifdef __ANDROID__
        if (androidDirectOutput) motioncam::cancelFinalizeOutput();
#endif
        if (!mountReleased) {
            if (!androidDirectOutput) tempDir.removeRecursively();
        }
        progress.close();
        if (std::string(e.what()) == "Finalization cancelled") {
            return;
        }
        QString message = QString::fromStdString(e.what());
        if (mountReleased && tempDir.exists()) {
            message += QString("\n\nThe completed temporary render was preserved at:\n%1")
                           .arg(tempPath);
        }
#ifdef __ANDROID__
        showAndroidNotice(tr("Finalize failed"), message);
#else
        QMessageBox::critical(this, "Finalize failed", message);
#endif
        return;
    }

    // Reuse normal clip removal after the output transaction has committed.
    removeFile(fileWidget, false);
}

void MainWindow::updateUi() {
    // Exposure smoothing operates on the normalized exposure correction.
    ui->smoothExposureCheckBox->setEnabled(ui->normalizeExposureCheckBox->isChecked());
    if (!ui->normalizeExposureCheckBox->isChecked())
        ui->smoothExposureCheckBox->setChecked(false);

    // Draft quality only enabled when draft mode is on
    const QSignalBlocker draftQualitySignals(ui->draftQuality);
    if(ui->draftModeCheckBox->checkState() == Qt::CheckState::Checked) {
        if(ui->draftQuality->currentIndex() < 0) {
            ui->draftQuality->setCurrentIndex(0);
            mRenderSettings.draftScale = 2;
        }
        ui->draftQuality->setEnabled(true);
        ui->higherCfaHqCheckBox->setEnabled(true);
        ui->quadBayerComboBox->setEnabled(true);
    } else {
        ui->draftQuality->setCurrentIndex(-1);
        mRenderSettings.draftScale = 1;
        ui->draftQuality->setEnabled(false);
        ui->higherCfaHqCheckBox->setEnabled(true);
        ui->quadBayerComboBox->setEnabled(true);
    }

    if(ui->cropEnableCheckBox->checkState() == Qt::CheckState::Checked)
        ui->cropTargetComboBox->setEnabled(true);
    else
        ui->cropTargetComboBox->setEnabled(false);

    if(ui->camModelOverrideCheckBox->checkState() == Qt::CheckState::Checked) {
        ui->camModelOverrideComboBox->setEnabled(true);
        if (ui->camModelOverrideComboBox->currentText() == "")
            ui->camModelOverrideComboBox->setCurrentText("Panasonic");
    } else {
        ui->camModelOverrideComboBox->setCurrentText("");
        ui->camModelOverrideComboBox->setEnabled(false);
    }

    // Bit depth reduction combobox only enabled when checkbox is checked
    if(ui->logTransformCheckBox->checkState() == Qt::CheckState::Checked) {
        ui->logTransformComboBox->setEnabled(true);
        ui->logHqCheckBox->setEnabled(true);
        if (ui->logTransformComboBox->currentText() == "")
            ui->logTransformComboBox->setCurrentText("Keep Input");
    } else {
        ui->logTransformComboBox->setCurrentText("");
        ui->logTransformComboBox->setEnabled(false);
        ui->logHqCheckBox->setEnabled(false);
    }

    // Normalization applies to both baked pixels and retained output gain maps.
    // The flat-field diagnostic is meaningful only when gain maps are baked.
    if(ui->vignetteCorrectionComboBox->currentText() == "Bake") {
        if(ui->normalizeGainMapsCheckBox->isChecked()) {
            ui->debugVignetteCheckBox->setEnabled(false);
            ui->debugVignetteCheckBox->setChecked(false);
        } else {
            ui->debugVignetteCheckBox->setEnabled(true);
        }
    } else {
        ui->debugVignetteCheckBox->setEnabled(false);
        ui->debugVignetteCheckBox->setChecked(false);
    }
    const bool retainsGainMaps = ui->vignetteCorrectionComboBox->currentText() != "Exclude";
    ui->vignetteOnlyColorCheckBox->setEnabled(retainsGainMaps);
    ui->optimizeGainMapsCheckBox->setEnabled(retainsGainMaps);

    // Update calibration button states
    updateCalibrationButtonStates();
}

void MainWindow::updateFpsLabels() {
    // Get the scroll area's content widget
    auto* scrollContent = ui->dragAndDropScrollArea->widget();
    if (!scrollContent) {
        return;
    }

    // Find all info labels in the scroll area
    auto allLabels = scrollContent->findChildren<QLabel*>();

    for (auto* label : allLabels) {
        bool ok = false;
        auto mountId = label->property("mountId").toInt(&ok);

        if (!ok || mountId < 0) {
            continue;
        }

        // Get the updated info
        auto fileInfoOpt = mFuseFilesystem->getFileInfo(mountId);
        if (!fileInfoOpt.has_value()) {
            continue;
        }

        const auto& info = fileInfoOpt.value();

        // Update first info label (runtime, resolution, data type, levels)
        if (label->property("infoLabel1").toBool()) {
            label->setText(clipInfoFirstLine(info));
        }
        // Update second info label (FPS and frame counts)
        else if (label->property("infoLabel2").toBool()) {
            auto infoText2 = QString("<span style='color: #888888;'>Median / Average / Target FPS: %1 / %2 -> </span>"
                                     "<span style='color: white;'>%3</span>"
                                     "<span style='color: #888888;'> | Framecount: %4 | Dropped: -%5 | Duplicated: +%6</span>")
                                    .arg(QString::number(info.frameRateInfo.medianFrameRate, 'f', 2))
                                    .arg(QString::number(info.frameRateInfo.averageFrameRate, 'f', 2))
                                    .arg(QString::number(info.fps, 'f', 2))
                                    .arg(info.totalFrames)
                                    .arg(info.droppedFrames)
                                    .arg(info.duplicatedFrames);

            label->setText(infoText2);
        }
    }

    for (auto it = mTimingDialogs.begin(); it != mTimingDialogs.end(); ++it) {
        if (!it.value()) continue;
        const auto info = mFuseFilesystem->getFileInfo(it.key());
        if (!info || !info->presentationTimestamps) continue;
        it.value()->updateTiming(*info->presentationTimestamps,
                                 info->timingTimeBaseNum, info->timingTimeBaseDen,
                                 info->fps, info->timingUsesCfrMapping,
                                 info->frameRateInfo);
    }
}

void MainWindow::onRenderSettingsChanged(Qt::CheckState checkState) {
    updateUi();

    scheduleOptionsUpdate();
}

void MainWindow::scheduleOptionsUpdate() {
    if (auto* changedWidget = qobject_cast<QWidget*>(sender())) {
        changedWidget->setProperty("localOverride", false);
        changedWidget->style()->unpolish(changedWidget);
        changedWidget->style()->polish(changedWidget);
    }
    if (mSelectedMountIds.isEmpty()) mGlobalRenderSettings = buildRenderSettings();
    if (mMountedFiles.isEmpty()) return;
    if (mAutoApplyClipSettings) applyAutoSettings();
    else markSettingsDirty();
}

void MainWindow::applyAutoSettings() {
    if (mProcessingInProgress || mProcessingWatcher->isRunning()) {
        mOptionsUpdatePending = true;
        return;
    }

    if (mSelectedMountIds.isEmpty()) {
        mRenderSettings = mGlobalRenderSettings;
        QList<motioncam::MountId> mounts;
        for (const auto& file : mMountedFiles) {
            if (mLocalSettings.contains(file.mountId)) continue;
            mounts.push_back(file.mountId);
        }
        clearApplyFeedback();
        autoSaveSession();
        if (mounts.isEmpty()) {
            updateFpsLabels();
            return;
        }
        const auto settings = mGlobalRenderSettings;
        mOptionsUpdatePending = false;
        mProcessingInProgress = true;
        onProcessingStarted();
        mProcessingWatcher->setFuture(QtConcurrent::run(
            [this, mounts = std::move(mounts), settings] {
                for (const auto mountId : mounts)
                    mFuseFilesystem->updateOptions(mountId, settings);
            }));
        return;
    }

    onApplySelected();
}

void MainWindow::updateApplyButtonsVisibility() {
    const bool visible = !mAutoApplyClipSettings;
    mApplySelectedButton->setVisible(visible);
    mApplyAllButton->setVisible(visible);
}

void MainWindow::markSettingsDirty() {
    if (mMountedFiles.isEmpty()) return;
    mSettingsDirty = true;
    mApplySelectedButton->setEnabled(!mSelectedMountIds.isEmpty());
    mApplyAllButton->setEnabled(true);
    mApplySelectedButton->setText(tr("Apply to Selected *"));
    mApplyAllButton->setText(tr("Apply to All *"));
    const QString pending = QStringLiteral(
        "QPushButton { background:#d1a53a; color:#111; border:1px solid #e4bd5d; border-radius:4px; }"
        "QPushButton:hover { background:#e1b64a; }");
    mApplySelectedButton->setStyleSheet(pending);
    mApplyAllButton->setStyleSheet(pending);
}

void MainWindow::clearApplyFeedback() {
    mSettingsDirty = false;
    mApplySelectedButton->setText(tr("Apply to Selected"));
    mApplyAllButton->setText(tr("Apply to All"));
    mApplySelectedButton->setStyleSheet(mApplySelectedButtonBaseStyle);
    mApplyAllButton->setStyleSheet(mApplyAllButtonBaseStyle);
}

QWidget* MainWindow::fileWidgetForMount(motioncam::MountId mountId) const {
    auto* content = ui->dragAndDropScrollArea->widget();
    if (!content) return nullptr;
    for (auto* widget : content->findChildren<QWidget*>(QString(), Qt::FindDirectChildrenOnly)) {
        bool ok = false;
        if (widget->property("mountId").toInt(&ok) == mountId && ok) return widget;
    }
    return nullptr;
}

void MainWindow::updateLocalBadge(motioncam::MountId mountId) {
    auto* card = fileWidgetForMount(mountId);
    if (!card) return;
    const bool local = mLocalSettings.contains(mountId);
    if (auto* badge = card->findChild<QLabel*>("localBadge")) badge->setVisible(local);
    if (auto* badge = card->findChild<QLabel*>("globalBadge")) badge->setVisible(!local);
    if (auto* reset = card->findChild<QPushButton*>("localReset")) reset->setVisible(local);
}

void MainWindow::updateClipIndices() {
    auto* content = ui->dragAndDropScrollArea->widget();
    auto* layout = content ? qobject_cast<QVBoxLayout*>(content->layout()) : nullptr;
    if (!layout) return;
    int number = 1;
    for (int i = 0; i < layout->count(); ++i) {
        auto* card = layout->itemAt(i)->widget();
        if (!card || !card->property("mountId").isValid()) continue;
        if (auto* label = card->findChild<QLabel*>("indexLabel"))
            label->setText(QStringLiteral("%1.").arg(number++, 2, 10, QChar('0')));
    }
}

void MainWindow::updateSelectionUi() {
    const int count = mSelectedMountIds.size();
    mSelectedFilesLabel->setText(count == 0
        ? tr("0 selected — editing GLOBAL settings")
        : tr("%1 selected — editing LOCAL settings").arg(count));
    mApplySelectedButton->setEnabled(count > 0);
    mApplyAllButton->setEnabled(!mMountedFiles.isEmpty());
    updateCalibrationButtonStates();
    const auto settings = count == 0
        ? mGlobalRenderSettings
        : mLocalSettings.value(*mSelectedMountIds.constBegin(), mGlobalRenderSettings);
    mRenderSettings = settings;
    const QSignalBlocker b1(ui->draftModeCheckBox), b2(ui->vignetteCorrectionComboBox),
        b3(ui->vignetteOnlyColorCheckBox), b4(ui->optimizeGainMapsCheckBox),
        b5(ui->normalizeGainMapsCheckBox), b6(ui->debugVignetteCheckBox),
        b7(ui->normalizeExposureCheckBox), b8(ui->smoothExposureCheckBox),
        b9(ui->smoothWhiteBalanceCheckBox), b10(ui->bakeIsoCheckBox),
        b11(ui->cfrConversionCheckBox), b12(ui->cropEnableCheckBox),
        b13(ui->camModelOverrideCheckBox), b14(ui->logTransformCheckBox),
        b14a(ui->logHqCheckBox);
    const QSignalBlocker b15(ui->cfrTarget), b16(ui->cropTargetComboBox),
        b17(ui->camModelOverrideComboBox), b18(ui->levelsComboBox),
        b19(ui->exposureCompensationLineEdit), b20(ui->logTransformComboBox),
        b21(ui->quadBayerComboBox), b22(ui->cfaPhaseComboBox),
        b23(ui->draftQuality), b24(ui->remosaicCheckBox),
        b25(ui->higherCfaHqCheckBox), b26(ui->dngCompressionCheckBox),
        b27(ui->dngCompressionModeComboBox), b28(ui->badPixelTreatmentComboBox);
    for (auto* box : {ui->draftModeCheckBox,
                      ui->vignetteOnlyColorCheckBox, ui->optimizeGainMapsCheckBox,
                      ui->normalizeGainMapsCheckBox, ui->debugVignetteCheckBox,
                      ui->normalizeExposureCheckBox, ui->smoothExposureCheckBox,
                      ui->smoothWhiteBalanceCheckBox, ui->bakeIsoCheckBox,
                      ui->cfrConversionCheckBox, ui->cropEnableCheckBox,
                      ui->camModelOverrideCheckBox, ui->logTransformCheckBox,
                      ui->logHqCheckBox,
                      ui->remosaicCheckBox, ui->higherCfaHqCheckBox,
                      ui->dngCompressionCheckBox})
        box->setTristate(false);
    for (auto* widget : {static_cast<QWidget*>(ui->cfrTarget),
                         static_cast<QWidget*>(ui->cropTargetComboBox),
                         static_cast<QWidget*>(ui->camModelOverrideComboBox),
                         static_cast<QWidget*>(ui->levelsComboBox),
                         static_cast<QWidget*>(ui->exposureCompensationLineEdit),
                         static_cast<QWidget*>(ui->logTransformComboBox),
                         static_cast<QWidget*>(ui->quadBayerComboBox),
                         static_cast<QWidget*>(ui->badPixelTreatmentComboBox),
                         static_cast<QWidget*>(ui->vignetteCorrectionComboBox),
                         static_cast<QWidget*>(ui->cfaPhaseComboBox),
                         static_cast<QWidget*>(ui->draftQuality)}) {
        widget->setProperty("localOverride", false);
        widget->style()->unpolish(widget);
        widget->style()->polish(widget);
    }
    auto checked = [&settings](motioncam::FileRenderOptions option) {
        return static_cast<bool>(settings.options & option);
    };
    ui->draftModeCheckBox->setChecked(checked(motioncam::RENDER_OPT_DRAFT));
    ui->vignetteCorrectionComboBox->setCurrentText(QString::fromStdString(
        vignetteCorrectionModeToString(settings.vignetteCorrection)));
    ui->vignetteOnlyColorCheckBox->setChecked(checked(motioncam::RENDER_OPT_VIGNETTE_ONLY_COLOR));
    ui->optimizeGainMapsCheckBox->setChecked(checked(motioncam::RENDER_OPT_OPTIMIZE_GAIN_MAPS));
    ui->normalizeGainMapsCheckBox->setChecked(checked(motioncam::RENDER_OPT_NORMALIZE_SHADING_MAP));
    ui->debugVignetteCheckBox->setChecked(checked(motioncam::RENDER_OPT_DEBUG_SHADING_MAP));
    ui->normalizeExposureCheckBox->setChecked(checked(motioncam::RENDER_OPT_NORMALIZE_EXPOSURE));
    ui->smoothExposureCheckBox->setChecked(checked(motioncam::RENDER_OPT_SMOOTH_EXPOSURE));
    ui->smoothWhiteBalanceCheckBox->setChecked(checked(motioncam::RENDER_OPT_SMOOTH_WHITE_BALANCE));
    ui->bakeIsoCheckBox->setChecked(checked(motioncam::RENDER_OPT_BAKE_ISO));
    ui->cfrConversionCheckBox->setChecked(checked(motioncam::RENDER_OPT_FRAMERATE_CONVERSION));
    ui->cropEnableCheckBox->setChecked(checked(motioncam::RENDER_OPT_CROPPING));
    ui->camModelOverrideCheckBox->setChecked(checked(motioncam::RENDER_OPT_CAMMODEL_OVERRIDE));
    ui->logTransformCheckBox->setChecked(checked(motioncam::RENDER_OPT_LOG_TRANSFORM));
    ui->logHqCheckBox->setChecked(checked(motioncam::RENDER_OPT_LOG_HQ));
    ui->remosaicCheckBox->setChecked(checked(motioncam::RENDER_OPT_REMOSAIC_TO_BAYER));
    ui->higherCfaHqCheckBox->setChecked(checked(motioncam::RENDER_OPT_HIGHER_CFA_HQ));
    ui->dngCompressionCheckBox->setChecked(checked(motioncam::RENDER_OPT_JPEG_COMPRESSION));
    ui->cfrTarget->setCurrentText(QString::fromStdString(cfrTargetToString(settings.cfrTarget)));
    ui->cropTargetComboBox->setCurrentText(QString::fromStdString(settings.cropTarget));
    ui->camModelOverrideComboBox->setCurrentText(QString::fromStdString(settings.cameraModel));
    ui->levelsComboBox->setCurrentText(QString::fromStdString(settings.levels));
    ui->exposureCompensationLineEdit->setText(QString::fromStdString(settings.exposureCompensation));
    ui->logTransformComboBox->setCurrentText(
        QString::fromStdString(logTransformModeToString(settings.logTransform)));
    ui->quadBayerComboBox->setCurrentText(
        QString::fromStdString(quadBayerModeToString(settings.quadBayerOption)));
    ui->badPixelTreatmentComboBox->setCurrentText(
        QString::fromStdString(badPixelTreatmentToString(settings.badPixelTreatment)));
    ui->cfaPhaseComboBox->setCurrentText(QString::fromStdString(settings.cfaPhase));
    ui->draftQuality->setCurrentIndex(settings.draftScale == 2 ? 0
        : settings.draftScale == 4 ? 1 : settings.draftScale == 8 ? 2 : -1);
    // Camera Native modes are global finalization choices rather than fields
    // in RenderSettings. Preserve them while clip selection changes.
    if (!isCameraNativeFormat(ui->dngCompressionModeComboBox->currentText()) &&
        !isMetadataOnlyFinalizeFormat(ui->dngCompressionModeComboBox->currentText()) &&
        !isSevenZipFinalizeFormat(ui->dngCompressionModeComboBox->currentText())) {
        if (motioncam::isLossyJpegDct(settings.jxlDistance))
            ui->dngCompressionModeComboBox->setCurrentIndex(1);
        else if (settings.jxlDistance == motioncam::DNG_COMPRESSION_JPEG92_STRIP)
            ui->dngCompressionModeComboBox->setCurrentText("JPEG92 Lossless Strip");
        else if (settings.jxlDistance < 0.0f)
            ui->dngCompressionModeComboBox->setCurrentIndex(0);
        else if (settings.jxlDistance == 0.0f)
            ui->dngCompressionModeComboBox->setCurrentIndex(2);
        else if (std::abs(settings.jxlDistance - 0.1f) <= 0.0001f)
            ui->dngCompressionModeComboBox->setCurrentIndex(3);
        else
            ui->dngCompressionModeComboBox->setCurrentText(
                QString("JPEG XL DCT %1").arg(settings.jxlDistance));
    }

    if (count > 1) {
        auto mixedFlag = [this, &settings](motioncam::FileRenderOptions option) {
            const bool first = static_cast<bool>(settings.options & option);
            for (auto selectedId : mSelectedMountIds) {
                const auto current = mLocalSettings.value(selectedId, mGlobalRenderSettings);
                if (static_cast<bool>(current.options & option) != first) return true;
            }
            return false;
        };
        auto markCheck = [](QCheckBox* box, bool mixed) {
            box->setTristate(mixed);
            if (mixed) box->setCheckState(Qt::PartiallyChecked);
        };
        markCheck(ui->draftModeCheckBox, mixedFlag(motioncam::RENDER_OPT_DRAFT));
        markCheck(ui->vignetteOnlyColorCheckBox, mixedFlag(motioncam::RENDER_OPT_VIGNETTE_ONLY_COLOR));
        markCheck(ui->optimizeGainMapsCheckBox, mixedFlag(motioncam::RENDER_OPT_OPTIMIZE_GAIN_MAPS));
        markCheck(ui->normalizeGainMapsCheckBox, mixedFlag(motioncam::RENDER_OPT_NORMALIZE_SHADING_MAP));
        markCheck(ui->debugVignetteCheckBox, mixedFlag(motioncam::RENDER_OPT_DEBUG_SHADING_MAP));
        markCheck(ui->normalizeExposureCheckBox, mixedFlag(motioncam::RENDER_OPT_NORMALIZE_EXPOSURE));
        markCheck(ui->smoothExposureCheckBox, mixedFlag(motioncam::RENDER_OPT_SMOOTH_EXPOSURE));
        markCheck(ui->smoothWhiteBalanceCheckBox, mixedFlag(motioncam::RENDER_OPT_SMOOTH_WHITE_BALANCE));
        markCheck(ui->bakeIsoCheckBox, mixedFlag(motioncam::RENDER_OPT_BAKE_ISO));
        markCheck(ui->cfrConversionCheckBox, mixedFlag(motioncam::RENDER_OPT_FRAMERATE_CONVERSION));
        markCheck(ui->cropEnableCheckBox, mixedFlag(motioncam::RENDER_OPT_CROPPING));
        markCheck(ui->camModelOverrideCheckBox, mixedFlag(motioncam::RENDER_OPT_CAMMODEL_OVERRIDE));
        markCheck(ui->logTransformCheckBox, mixedFlag(motioncam::RENDER_OPT_LOG_TRANSFORM));
        markCheck(ui->logHqCheckBox, mixedFlag(motioncam::RENDER_OPT_LOG_HQ));
        markCheck(ui->remosaicCheckBox, mixedFlag(motioncam::RENDER_OPT_REMOSAIC_TO_BAYER));
        markCheck(ui->higherCfaHqCheckBox, mixedFlag(motioncam::RENDER_OPT_HIGHER_CFA_HQ));
        markCheck(ui->dngCompressionCheckBox, mixedFlag(motioncam::RENDER_OPT_JPEG_COMPRESSION));
        auto markValue = [this, &settings](QWidget* widget, auto getter) {
            const auto first = getter(settings);
            bool mixed = false;
            for (auto selectedId : mSelectedMountIds)
                mixed |= getter(mLocalSettings.value(selectedId, mGlobalRenderSettings)) != first;
            widget->setProperty("localOverride", mixed);
            widget->style()->unpolish(widget);
            widget->style()->polish(widget);
        };
        markValue(ui->cfrTarget, [](const auto& s) { return cfrTargetToString(s.cfrTarget); });
        markValue(ui->draftQuality, [](const auto& s) { return s.draftScale; });
        markValue(ui->cropTargetComboBox, [](const auto& s) { return s.cropTarget; });
        markValue(ui->camModelOverrideComboBox, [](const auto& s) { return s.cameraModel; });
        markValue(ui->levelsComboBox, [](const auto& s) { return s.levels; });
        markValue(ui->exposureCompensationLineEdit, [](const auto& s) { return s.exposureCompensation; });
        markValue(ui->logTransformComboBox, [](const auto& s) { return s.logTransform; });
        markValue(ui->quadBayerComboBox, [](const auto& s) { return s.quadBayerOption; });
        markValue(ui->badPixelTreatmentComboBox, [](const auto& s) { return s.badPixelTreatment; });
        markValue(ui->vignetteCorrectionComboBox, [](const auto& s) { return s.vignetteCorrection; });
        markValue(ui->cfaPhaseComboBox, [](const auto& s) { return s.cfaPhase; });
    }
    updateUi();
}

void MainWindow::onApplySelected() {
    if (mSelectedMountIds.isEmpty()) return;
    if (mProcessingInProgress || mProcessingWatcher->isRunning()) {
        mOptionsUpdatePending = true;
        return;
    }
    const auto edited = buildRenderSettings();
    QList<QPair<motioncam::MountId, motioncam::RenderSettings>> updates;
    for (auto id : mSelectedMountIds) {
        auto settings = edited;
        const auto previous = mLocalSettings.value(id, mGlobalRenderSettings);
        auto preserveMixedFlag = [&](QCheckBox* box, motioncam::FileRenderOptions option) {
            if (box->checkState() != Qt::PartiallyChecked) return;
            if (previous.options & option) settings.options |= option;
            else settings.options = static_cast<motioncam::FileRenderOptions>(settings.options & ~option);
        };
        preserveMixedFlag(ui->draftModeCheckBox, motioncam::RENDER_OPT_DRAFT);
        preserveMixedFlag(ui->vignetteOnlyColorCheckBox, motioncam::RENDER_OPT_VIGNETTE_ONLY_COLOR);
        preserveMixedFlag(ui->optimizeGainMapsCheckBox, motioncam::RENDER_OPT_OPTIMIZE_GAIN_MAPS);
        preserveMixedFlag(ui->normalizeGainMapsCheckBox, motioncam::RENDER_OPT_NORMALIZE_SHADING_MAP);
        preserveMixedFlag(ui->debugVignetteCheckBox, motioncam::RENDER_OPT_DEBUG_SHADING_MAP);
        preserveMixedFlag(ui->normalizeExposureCheckBox, motioncam::RENDER_OPT_NORMALIZE_EXPOSURE);
        preserveMixedFlag(ui->smoothExposureCheckBox, motioncam::RENDER_OPT_SMOOTH_EXPOSURE);
        preserveMixedFlag(ui->smoothWhiteBalanceCheckBox, motioncam::RENDER_OPT_SMOOTH_WHITE_BALANCE);
        preserveMixedFlag(ui->bakeIsoCheckBox, motioncam::RENDER_OPT_BAKE_ISO);
        preserveMixedFlag(ui->cfrConversionCheckBox, motioncam::RENDER_OPT_FRAMERATE_CONVERSION);
        preserveMixedFlag(ui->cropEnableCheckBox, motioncam::RENDER_OPT_CROPPING);
        preserveMixedFlag(ui->camModelOverrideCheckBox, motioncam::RENDER_OPT_CAMMODEL_OVERRIDE);
        preserveMixedFlag(ui->logTransformCheckBox, motioncam::RENDER_OPT_LOG_TRANSFORM);
        preserveMixedFlag(ui->logHqCheckBox, motioncam::RENDER_OPT_LOG_HQ);
        preserveMixedFlag(ui->remosaicCheckBox, motioncam::RENDER_OPT_REMOSAIC_TO_BAYER);
        preserveMixedFlag(ui->higherCfaHqCheckBox, motioncam::RENDER_OPT_HIGHER_CFA_HQ);
        preserveMixedFlag(ui->dngCompressionCheckBox, motioncam::RENDER_OPT_JPEG_COMPRESSION);
        if (ui->cfrTarget->property("localOverride").toBool()) settings.cfrTarget = previous.cfrTarget;
        if (ui->draftQuality->property("localOverride").toBool()) settings.draftScale = previous.draftScale;
        if (ui->cropTargetComboBox->property("localOverride").toBool()) settings.cropTarget = previous.cropTarget;
        if (ui->camModelOverrideComboBox->property("localOverride").toBool()) settings.cameraModel = previous.cameraModel;
        if (ui->levelsComboBox->property("localOverride").toBool()) settings.levels = previous.levels;
        if (ui->exposureCompensationLineEdit->property("localOverride").toBool())
            settings.exposureCompensation = previous.exposureCompensation;
        if (ui->logTransformComboBox->property("localOverride").toBool()) settings.logTransform = previous.logTransform;
        if (ui->quadBayerComboBox->property("localOverride").toBool()) settings.quadBayerOption = previous.quadBayerOption;
        if (ui->badPixelTreatmentComboBox->property("localOverride").toBool()) settings.badPixelTreatment = previous.badPixelTreatment;
        if (ui->vignetteCorrectionComboBox->property("localOverride").toBool()) settings.vignetteCorrection = previous.vignetteCorrection;
        if (ui->cfaPhaseComboBox->property("localOverride").toBool()) settings.cfaPhase = previous.cfaPhase;
        mLocalSettings.insert(id, settings);
        updates.push_back(qMakePair(id, settings));
        updateLocalBadge(id);
    }
    clearApplyFeedback();
    autoSaveSession();
    if (updates.isEmpty()) {
        updateFpsLabels();
        return;
    }
    mOptionsUpdatePending = false;
    mProcessingInProgress = true;
    onProcessingStarted();
    mProcessingWatcher->setFuture(QtConcurrent::run(
        [this, updates = std::move(updates)] {
            for (const auto& update : updates)
                mFuseFilesystem->updateOptions(update.first, update.second);
        }));
}

void MainWindow::onApplyAll() {
    if (mMountedFiles.isEmpty()) return;
    mGlobalRenderSettings = buildRenderSettings();
    mRenderSettings = mGlobalRenderSettings;
    mLocalSettings.clear();
    for (const auto& file : mMountedFiles) {
        mFuseFilesystem->updateOptions(file.mountId, mGlobalRenderSettings);
        updateLocalBadge(file.mountId);
        updateThumbnail(file.mountId);
    }
    updateFpsLabels();
    clearApplyFeedback();
    autoSaveSession();
}

void MainWindow::updateThumbnail(motioncam::MountId mountId) {
    if (mImportBatchActive) {
        if (!mDeferredImportThumbnails.contains(mountId))
            mDeferredImportThumbnails.append(mountId);
        return;
    }
    auto* card = fileWidgetForMount(mountId);
    auto* label = card ? card->findChild<QLabel*>("thumbnailLabel") : nullptr;
    if (!label) return;
    // Keep background thumbnail decoding out of playback benchmark samples.
    if (mGalleryPerformanceTestActive && !mPerformanceThumbnailRun) {
        label->setText(tr("Performance test"));
        return;
    }
    // Keep the last completed image visible while its replacement renders.
    // Only the initial load needs a textual placeholder.
    if (!label->property("hasThumbnail").toBool())
        label->setText(tr("Generating..."));
    if (const auto previous = mThumbnailCancellations.value(mountId))
        previous->store(true);
    const auto cancelled = std::make_shared<std::atomic_bool>(false);
    mThumbnailCancellations.insert(mountId, cancelled);
    QPointer<QLabel> guardedLabel(label);
    auto settings = thumbnailRenderSettings(settingsForMount(mountId));
    const auto info = mFuseFilesystem->getFileInfo(mountId);
    if (settings.orientation < 0) {
        if (info) settings.orientation = info->orientation;
    }
    const auto started = std::chrono::steady_clock::now();
    const bool performanceRun = mPerformanceThumbnailRun;
    auto render = [this, mountId, settings, guardedLabel, cancelled, started,
                   performanceRun] {
        QImage image;
        try {
            motioncam::PreviewOptions options;
            options.skipFrame = [](size_t frame) { return frame > 0; };
            mFuseFilesystem->renderPreview(
                mountId, settings, options,
                [cancelled](size_t, size_t, const std::string&) {
                    return !cancelled->load();
                },
                [&](motioncam::PreviewFrame&& preview) {
                    if (cancelled->load() || !image.isNull()) return;
                    applyGalleryPreviewColorTransform(
                        preview, settings.ignoreForwardMat,
                        gainMapOnlyDebug(settings));
                    const int orientation = settings.orientation >= 0
                        ? settings.orientation : preview.metadata.orientation;
                    image = previewImage(preview.rgb, preview.width, preview.height,
                                         orientation);
                });
        } catch (const std::exception& error) {
            if (!cancelled->load())
                spdlog::warn("Could not generate processed thumbnail: {}", error.what());
        }
        if (cancelled->load()) return;
        QMetaObject::invokeMethod(this, [this, mountId, guardedLabel,
                                        image = std::move(image), cancelled,
                                        started, performanceRun] {
            if (cancelled->load()) return;
            const qint64 elapsedMs = static_cast<qint64>(
                std::chrono::duration<double, std::milli>(
                    std::chrono::steady_clock::now() - started).count());
            if (performanceRun)
                emit thumbnailPerformanceFinished(mountId, !image.isNull(), elapsedMs);
            if (!guardedLabel) return;
            if (!image.isNull()) {
                const QPixmap preview = QPixmap::fromImage(image).scaled(
                    QSize(176, 140), Qt::KeepAspectRatio, Qt::SmoothTransformation);
                guardedLabel->setFixedSize(preview.size());
                guardedLabel->setPixmap(preview);
                guardedLabel->setProperty("hasThumbnail", true);
            } else if (!guardedLabel->property("hasThumbnail").toBool()) {
                guardedLabel->setText(QObject::tr("No preview"));
            }
        }, Qt::QueuedConnection);
    };
    mThumbnailTasks.addFuture(QtConcurrent::run(std::move(render)));
}

void MainWindow::onProcessingStarted() {
#ifdef _WIN32
    if (mTaskbarList) {
        HWND hwnd = reinterpret_cast<HWND>(winId());
        mTaskbarList->SetProgressState(hwnd, TBPF_NORMAL);
        mTaskbarList->SetProgressValue(hwnd, 0, mMountedFiles.size());
    }
#endif
}

void MainWindow::onProcessingProgress(int current, int total) {
#ifdef _WIN32
    if (mTaskbarList) {
        HWND hwnd = reinterpret_cast<HWND>(winId());
        mTaskbarList->SetProgressValue(hwnd, current, total);
    }
#endif
}

void MainWindow::onProcessingFinished() {
#ifdef _WIN32
    if (mTaskbarList) {
        HWND hwnd = reinterpret_cast<HWND>(winId());
        mTaskbarList->SetProgressState(hwnd, TBPF_NOPROGRESS);
    }
#endif
    mProcessingInProgress = false;

    if (mOptionsUpdatePending) {
        // QFutureWatcher may still report isRunning() from inside its finished
        // callback. Start the coalesced update on the next event-loop turn.
        QTimer::singleShot(0, this, &MainWindow::scheduleOptionsUpdate);
        return;
    }

    // Update FPS labels after processing completes
    updateFpsLabels();
    for (const auto& file : mMountedFiles)
        updateThumbnail(file.mountId);
    if (mClipPlayer && mGalleryMountId != motioncam::InvalidMountId) {
        for (const auto& mounted : mMountedFiles) {
            const auto info = mFuseFilesystem->getFileInfo(mounted.mountId);
            if (!info) continue;
            const double fps = info->isSequence && info->fps > 0.0f ? info->fps : 1.0;
            const int frames = std::max(1, info->totalFrames - info->droppedFrames +
                info->duplicatedFrames);
            const double duration = info->runtimeSeconds > 0.0f
                ? info->runtimeSeconds : frames / fps;
            mClipPlayer->updateClipInfo(mounted.mountId, fps, duration, frames,
                info->width, info->height, info->duplicateFrameMask,
                info->sourceFrameToOutput,info->sourceFrameDuplicated);
        }
        mClipPlayer->reloadCurrentClip();
    }
}

void MainWindow::onDraftModeQualityChanged(int index) {
    if(index == 0)
        mRenderSettings.draftScale = 2;
    else if(index == 1)
        mRenderSettings.draftScale = 4;
    else if(index == 2)
        mRenderSettings.draftScale = 8;

    scheduleOptionsUpdate();
}

void MainWindow::onCFRTargetChanged(std::string input) {
    mRenderSettings.cfrTarget = stringToCFRTarget(input);
    scheduleOptionsUpdate();
}

void MainWindow::onCropTargetChanged(std::string input) {
    mRenderSettings.cropTarget = input;
    scheduleOptionsUpdate();
}

void MainWindow::onCamModelOverrideChanged(std::string input) {
    mRenderSettings.cameraModel = input;
    scheduleOptionsUpdate();
}

void MainWindow::onLevelsChanged(std::string input) {
    mRenderSettings.levels = input;
    scheduleOptionsUpdate();
}

void MainWindow::onLogTransformChanged(std::string input) {
    mRenderSettings.logTransform = stringToLogTransformMode(input);
    scheduleOptionsUpdate();
}

void MainWindow::onExposureCompensationChanged(std::string input) {
    mRenderSettings.exposureCompensation = input;
    scheduleOptionsUpdate();
}

void MainWindow::onQuadBayerChanged(std::string input) {
    mRenderSettings.quadBayerOption = stringToQuadBayerMode(input);
    scheduleOptionsUpdate();
}

void MainWindow::onCfaPhaseChanged(std::string input) {
    mRenderSettings.cfaPhase = input;
    scheduleOptionsUpdate();
}

void MainWindow::onOpenPreferences() {
#ifdef __ANDROID__
    if (mSettingsDialog) {
        mSettingsDialog->raise();
        mSettingsDialog->activateWindow();
        return;
    }
    auto* dialog = new SettingsDialog(ui->centralwidget);
    mSettingsDialog = dialog;
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    dialog->setWindowFlags(Qt::Widget);
    dialog->setAutoFillBackground(true);
    new motioncam::PageCrossfade(dialog);
#else
    SettingsDialog stackDialog(this);
    auto* dialog = &stackDialog;
#endif
    dialog->setCacheFolder(mCacheRootFolder);
    dialog->setAutoApplyClipSettings(mAutoApplyClipSettings);
    dialog->setGalleryFpsIndicatorEnabled(mGalleryFpsIndicatorEnabled);
    dialog->setDetectDuplicateDngs(mDetectDuplicateDngs);
    dialog->setUnmountOnFinalize(mUnmountOnFinalize);
    dialog->setFinalizeSelectionToSingleDirectory(mFinalizeSelectionToSingleDirectory);
    dialog->setInheritHeroFrameSidecars(mInheritHeroFrameSidecars);
    dialog->setFuseMountingEnabled(mFuseMountingEnabled);
    dialog->setFuseMountingAvailable(mFuseMountingAvailable);
#ifdef _WIN32
    dialog->setDeleteOnUnmount(mDeleteOnUnmount);
    dialog->setCachePolicyMode(mCachePolicy == motioncam::CachePolicy::Quota ? "quota" : "off");
    dialog->setCacheQuotaGb(static_cast<double>(mCacheQuotaBytes) / (1024.0 * 1024.0 * 1024.0));
    dialog->setCacheCleanupIntervalSeconds(mCacheCleanupIntervalSeconds);
#endif

#ifdef __ANDROID__
    QSettings androidPreferences(PACKAGE_NAME, APP_NAME);
    dialog->setAndroidFinalizeDestination(
        androidPreferences.value("android/finalizeDestination", "documents").toString(),
        motioncam::legacyCustomFinalizeDirectory());
    connect(dialog, &QDialog::finished, this, [this, dialog](int result) {
        if (result == QDialog::Accepted) applyPreferences(*dialog);
        mSettingsDialog = nullptr;
        updateAndroidOrientationPolicy();
        dialog->deleteLater();
    });
    dialog->setGeometry(ui->centralwidget->rect());
    motioncam::PageCrossfade::show(dialog);
    updateAndroidOrientationPolicy();
#else
    if (dialog->exec() == QDialog::Accepted) applyPreferences(*dialog);
#endif
}

#ifdef __ANDROID__
void MainWindow::hideSecondaryWindowsForGallery() {
    mHiddenForGallery.clear();
    auto hide = [this](QWidget* window) {
        if (window && window->isVisible()) {
            mHiddenForGallery.append(window);
            window->setProperty("suppressNavigationFade", true);
            window->hide();
            window->setProperty("suppressNavigationFade", false);
        }
    };
    hide(mSettingsDialog);
    for (const auto& dialog : mTimingDialogs) hide(dialog);
}

void MainWindow::restoreSecondaryWindowsAfterGallery() {
    for (const auto& window : mHiddenForGallery) {
        if (window) {
            window->setGeometry(ui->centralwidget->rect());
            window->show();
            window->raise();
        }
    }
    mHiddenForGallery.clear();
    updateAndroidOrientationPolicy();
}
#endif

void MainWindow::applyPreferences(SettingsDialog& dialog) {
    const bool wasAutoApply = mAutoApplyClipSettings;
    mCacheRootFolder = dialog.getCacheFolder();
    mAutoApplyClipSettings = dialog.getAutoApplyClipSettings();
    mGalleryFpsIndicatorEnabled = dialog.getGalleryFpsIndicatorEnabled();
    mDetectDuplicateDngs = dialog.getDetectDuplicateDngs();
    if (mClipPlayer) mClipPlayer->setFpsIndicatorEnabled(mGalleryFpsIndicatorEnabled);
    mUnmountOnFinalize = dialog.getUnmountOnFinalize();
    mFinalizeSelectionToSingleDirectory = dialog.getFinalizeSelectionToSingleDirectory();
    mInheritHeroFrameSidecars = dialog.getInheritHeroFrameSidecars();
#ifdef __ANDROID__
    QSettings(PACKAGE_NAME, APP_NAME).setValue(
        "android/finalizeDestination", dialog.getAndroidFinalizeDestination());
#endif
    const bool requestedProjection = dialog.getFuseMountingEnabled() && mFuseMountingAvailable;
    if (requestedProjection != mFuseMountingEnabled) {
        std::vector<motioncam::MountId> changedMounts;
        try {
            for (const auto& mounted : mMountedFiles) {
                mFuseFilesystem->setProjectionEnabled(mounted.mountId, requestedProjection);
                changedMounts.push_back(mounted.mountId);
            }
            mFuseMountingEnabled = requestedProjection;
            for (const auto mountId : changedMounts) {
                if (auto* card = fileWidgetForMount(mountId)) {
                    card->setProperty("projectedFiles", requestedProjection);
                    if (auto* button = card->findChild<QPushButton*>(QStringLiteral("openDestinationButton"))) {
                        button->setEnabled(requestedProjection);
                        button->setVisible(requestedProjection);
                    }
                    if(auto* button=card->findChild<QPushButton*>(QStringLiteral("removeClipButton")))
                        button->setText(requestedProjection?tr("Unmount"):tr("Remove"));
                }
            }
        } catch (const std::exception& error) {
            for (auto it = changedMounts.rbegin(); it != changedMounts.rend(); ++it) {
                try { mFuseFilesystem->setProjectionEnabled(*it, mFuseMountingEnabled); }
                catch (const std::exception& rollbackError) {
                    spdlog::error("Failed to restore projection for mount {}: {}", *it, rollbackError.what());
                }
            }
            QMessageBox::warning(this, tr("Mount setting"),
                                 tr("Could not change the active DNG mounts: %1")
                                     .arg(QString::fromUtf8(error.what())));
        }
    }
    updateApplyButtonsVisibility();
#ifdef _WIN32
    mDeleteOnUnmount = dialog.getDeleteOnUnmount();
    mCachePolicy = dialog.getCachePolicyMode() == "off"
        ? motioncam::CachePolicy::Off : motioncam::CachePolicy::Quota;
    mCacheQuotaBytes = static_cast<std::uint64_t>(
        dialog.getCacheQuotaGb() * 1024.0 * 1024.0 * 1024.0);
    mCacheCleanupIntervalSeconds = dialog.getCacheCleanupIntervalSeconds();
    mFuseFilesystem->setCachePolicy(mCachePolicy);
    mFuseFilesystem->setCacheQuotaBytes(mCacheQuotaBytes);
    mFuseFilesystem->cleanupCacheExpired();
    if (mCacheCleanupTimer) {
        if (mCachePolicy == motioncam::CachePolicy::Quota && mCacheCleanupIntervalSeconds > 0)
            mCacheCleanupTimer->start(mCacheCleanupIntervalSeconds * 1000);
        else
            mCacheCleanupTimer->stop();
    }
#endif
    saveSettings();
    if (mAutoApplyClipSettings && !wasAutoApply)
        applyAutoSettings();
    else if (!mAutoApplyClipSettings && wasAutoApply)
        clearApplyFeedback();
}

bool MainWindow::saveSessionToFile(const QString& path) {
    auto encode = [](const motioncam::RenderSettings& settings) {
        QJsonObject object;
        object["options"] = static_cast<int>(settings.options);
        object["draftScale"] = settings.draftScale;
        object["cfrTarget"] = QString::fromStdString(cfrTargetToString(settings.cfrTarget));
        object["cropTarget"] = QString::fromStdString(settings.cropTarget);
        object["cameraModel"] = QString::fromStdString(settings.cameraModel);
        object["levels"] = QString::fromStdString(settings.levels);
        object["logTransform"] = QString::fromStdString(logTransformModeToString(settings.logTransform));
        object["exposureCompensation"] = QString::fromStdString(settings.exposureCompensation);
        object["quadBayerOption"] = QString::fromStdString(quadBayerModeToString(settings.quadBayerOption));
        object["badPixelTreatment"] = QString::fromStdString(badPixelTreatmentToString(settings.badPixelTreatment));
        object["vignetteCorrection"] = QString::fromStdString(vignetteCorrectionModeToString(settings.vignetteCorrection));
        object["cfaPhase"] = QString::fromStdString(settings.cfaPhase);
        object["jxlDistance"] = settings.jxlDistance;
        object["orientation"] = normalizedGalleryOrientation(settings.orientation);
        object["ignoreForwardMat"] = settings.ignoreForwardMat;
        return object;
    };
    QJsonObject root;
    root["version"] = 3;
    root["sessionFile"] = mCurrentSessionFile;
    root["globalSettings"] = encode(mGlobalRenderSettings);
    root["cacheFolder"] = mCacheRootFolder;
    QJsonArray clips;
    for (const auto& file : mMountedFiles) {
        QJsonObject clip;
        clip["path"] = file.importFile;
        if (mLocalSettings.contains(file.mountId))
            clip["localSettings"] = encode(mLocalSettings.value(file.mountId));
        QJsonArray selectedFrames;
        QList<int> ordered=mSelectedFrames.value(file.mountId).values();
        std::sort(ordered.begin(),ordered.end());
        for(int frame:ordered)selectedFrames.append(frame);
        clip["selectedFrames"] = selectedFrames;
        clips.append(clip);
    }
    root["clips"] = clips;
    const QByteArray json=QJsonDocument(root).toJson();
    bool saved=false;
#ifdef __ANDROID__
    if(path.startsWith("content:")){
        const QString error=motioncam::writeDocument(path,json);
        saved=error.isEmpty();
        if(!saved)spdlog::error("Could not save session document: {}",error.toStdString());
    }else
#endif
    {
        QSaveFile file(path);
        saved=file.open(QIODevice::WriteOnly)&&file.write(json)==json.size()&&file.commit();
    }
    if (!saved) {
        QMessageBox::warning(this, tr("Save Session"), tr("Could not save %1").arg(path));
        return false;
    }
    if (QFileInfo(path).absoluteFilePath() != QFileInfo(autoSessionPath()).absoluteFilePath()) {
        mCurrentSessionFile = path;
        addRecentSession(path);
        autoSaveSession();
    }
    return true;
}

void MainWindow::clearSession() {
    mClearingSession = true;
    while (!mMountedFiles.isEmpty()) {
        auto* card = fileWidgetForMount(mMountedFiles.front().mountId);
        if (card) removeFile(card);
        else {
            const auto mountId = mMountedFiles.front().mountId;
#ifdef __ANDROID__
            motioncam::releaseSeekableSourceLink(
                mMountedFiles.front().importFile);
#endif
            mFuseFilesystem->unmount(mountId);
            cleanupArchiveMount(mountId);
            mMountedFiles.removeFirst();
        }
    }
    mLocalSettings.clear();
    mSelectedMountIds.clear();
    mSelectedFrames.clear();
    updateSelectionUi();
    mClearingSession = false;
}

void MainWindow::loadSessionFromFile(const QString& path) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        QMessageBox::warning(this, tr("Load Session"), tr("Could not open %1").arg(path));
        return;
    }
    const auto document = QJsonDocument::fromJson(file.readAll());
    if (!document.isObject()) {
        QMessageBox::warning(this, tr("Load Session"), tr("Invalid session file"));
        return;
    }
    auto decode = [](const QJsonObject& object) {
        motioncam::RenderSettings settings;
        settings.options = static_cast<motioncam::FileRenderOptions>(object["options"].toInt());
        settings.draftScale = object["draftScale"].toInt(1);
        settings.cfrTarget = stringToCFRTarget(object["cfrTarget"].toString("Prefer Integer").toStdString());
        settings.cropTarget = object["cropTarget"].toString().toStdString();
        settings.cameraModel = object["cameraModel"].toString("Panasonic").toStdString();
        settings.levels = object["levels"].toString("Dynamic").toStdString();
        settings.logTransform = stringToLogTransformMode(object["logTransform"].toString("Keep Input").toStdString());
        settings.exposureCompensation = object["exposureCompensation"].toString().toStdString();
        settings.quadBayerOption = stringToQuadBayerMode(object["quadBayerOption"].toString("Demosaic").toStdString());
        settings.badPixelTreatment = stringToBadPixelTreatment(object["badPixelTreatment"].toString("Bake").toStdString());
        settings.vignetteCorrection = stringToVignetteCorrectionMode(object["vignetteCorrection"].toString(
            settings.options & motioncam::RENDER_OPT_APPLY_VIGNETTE_CORRECTION ? "Bake" : "Resample").toStdString());
        settings.cfaPhase = object["cfaPhase"].toString("Don't override CFA").toStdString();
        settings.jxlDistance = static_cast<float>(object["jxlDistance"].toDouble(-1.0));
        settings.orientation = normalizedGalleryOrientation(object["orientation"].toInt(-1));
        settings.ignoreForwardMat = object["ignoreForwardMat"].toBool(false);
        return settings;
    };
    const auto root = document.object();
    const auto clips = root["clips"].toArray();
    clearSession();
    mCacheRootFolder = root["cacheFolder"].toString();
    mGlobalRenderSettings = decode(root["globalSettings"].toObject());
    mRenderSettings = mGlobalRenderSettings;
    updateSelectionUi();
    QStringList clipPaths;
    QHash<QString, QJsonObject> clipsByPath;
    for (const auto& value : clips) {
        const auto clip = value.toObject();
        const QString clipPath = clip["path"].toString();
#ifdef __ANDROID__
        if (!QFileInfo::exists(clipPath))
            motioncam::reopenSeekableSourceLink(clipPath);
#endif
        if (!QFileInfo::exists(clipPath)) continue;
        clipPaths.append(clipPath);
        clipsByPath.insert(QFileInfo(clipPath).absoluteFilePath(), clip);
    }
    mountFiles(clipPaths, [this, &clipsByPath, &decode](
            const motioncam::MountedFile& mounted) {
        const auto clip = clipsByPath.value(
            QFileInfo(mounted.importFile).absoluteFilePath());
        if (clip.contains("localSettings") && !mMountedFiles.isEmpty()) {
            const auto id = mounted.mountId;
            const auto local = decode(clip["localSettings"].toObject());
            mLocalSettings.insert(id, local);
            mFuseFilesystem->updateOptions(id, settingsForMount(id));
            updateLocalBadge(id);
        }
        QSet<int> selected;
        for(const auto& value:clip["selectedFrames"].toArray())
            if(value.isDouble()&&value.toInt()>=0)selected.insert(value.toInt());
        if(!selected.isEmpty())mSelectedFrames.insert(mounted.mountId,std::move(selected));
    });
    // Cards are created with global settings before their local overrides
    // are restored. Refresh their metadata from the updated backend now.
    updateFpsLabels();
    updateCalibrationButtonStates();
    const bool snapshot=QFileInfo(path).absoluteFilePath()==QFileInfo(autoSessionPath()).absoluteFilePath();
    mCurrentSessionFile=snapshot?document.object().value("sessionFile").toString(path):path;
    if(mCurrentSessionFile.isEmpty())mCurrentSessionFile=path;
    if(!snapshot)addRecentSession(path);
    autoSaveSession();
}

QString MainWindow::sessionDirectory() const {
    const QString path = QDir(QStandardPaths::writableLocation(
        QStandardPaths::AppLocalDataLocation)).filePath("sessions");
    QDir().mkpath(path);
    return path;
}

QString MainWindow::autoSessionPath() const {
    return QDir(sessionDirectory()).filePath("last_session.json");
}

void MainWindow::autoSaveSession() {
    // Automated diagnostics may load purpose-built subsets. They must never
    // replace the user's resumable interactive session.
    if (mGalleryPerformanceTestActive || mClearingSession || mMountedFiles.isEmpty()) return;
    saveSessionToFile(autoSessionPath());
}

void MainWindow::promptToResumeSession() {
    const QString path = autoSessionPath();
    if (!QFileInfo::exists(path)) return;
#ifdef __ANDROID__
    // Embedded in the active Qt surface to avoid Android popup-window crashes.
    auto* overlay=new AndroidPopup(this,tr("Resume Session"));
    auto* question=new QLabel(tr("Resume the last session or start a new one?"),overlay->panel);
    question->setWordWrap(true);overlay->content->addWidget(question);
    auto* buttons=new QDialogButtonBox(overlay->panel);
    auto* resume=buttons->addButton(tr("Resume"),QDialogButtonBox::AcceptRole);
    auto* newSession=buttons->addButton(tr("New Session"),QDialogButtonBox::RejectRole);
    overlay->content->addWidget(buttons);
    connect(newSession, &QPushButton::clicked, overlay, [overlay] {
        overlay->hide();
        overlay->deleteLater();
    });
    connect(resume, &QPushButton::clicked, overlay, [this, overlay, path] {
        overlay->hide();
        overlay->deleteLater();
        QTimer::singleShot(0, this, [this, path] { loadSessionFromFile(path); });
    });
    overlay->open();
#else
    QMessageBox prompt(this);
    prompt.setIcon(QMessageBox::Question);
    prompt.setWindowTitle(tr("Resume Session"));
    prompt.setText(tr("Resume the last session or start a new one?"));
    auto* resume = prompt.addButton(tr("Resume"), QMessageBox::AcceptRole);
    prompt.addButton(tr("New Session"), QMessageBox::RejectRole);
    prompt.setDefaultButton(resume);
    prompt.exec();
    if (prompt.clickedButton() == resume) loadSessionFromFile(path);
#endif
    // An empty new session leaves the last snapshot available on the next start.
}

#ifdef __ANDROID__
void MainWindow::showAndroidNotice(const QString& title, const QString& message) {
    if (QApplication::applicationState() != Qt::ApplicationActive || !isVisible()) {
        mPendingAndroidNoticeTitle = title;
        mPendingAndroidNoticeMessage = message;
        return;
    }
    auto* overlay=new AndroidPopup(this,title);
    auto* body=new QLabel(message,overlay->panel);body->setTextFormat(Qt::PlainText);body->setWordWrap(true);
    overlay->content->addWidget(body);
    auto* buttons=new QDialogButtonBox(QDialogButtonBox::Ok,overlay->panel);
    overlay->content->addWidget(buttons);
    connect(buttons,&QDialogButtonBox::accepted,overlay,&QWidget::deleteLater);
    overlay->open();
}
#endif

#ifdef __APPLE__
void MainWindow::cleanupStaleMacFuseMounts() {
    QSet<QString> activePaths;
    for (const auto& mounted : mMountedFiles) {
        if (auto* card = fileWidgetForMount(mounted.mountId))
            activePaths.insert(QFileInfo(card->property("mountPath").toString()).absoluteFilePath());
    }
    for (const auto& mountPoint : listMotionCamFuseMounts()) {
        if (activePaths.contains(QFileInfo(mountPoint).absoluteFilePath())) continue;
        unmountMacFusePath(mountPoint);
        QDir().rmdir(mountPoint);
    }
}

void MainWindow::forceUnmountAllMacFuseMounts() {
    if (QMessageBox::question(this, tr("Force Unmount All"),
            tr("Force unmount all MotionCamFuse volumes and clear the session?"),
            QMessageBox::Yes | QMessageBox::No, QMessageBox::No) != QMessageBox::Yes)
        return;
    auto mounts = listMotionCamFuseMounts();
    std::sort(mounts.begin(), mounts.end(), [](const QString& a, const QString& b) {
        return a.size() > b.size();
    });
    for (const auto& mountPoint : mounts) {
        unmountMacFusePath(mountPoint);
        QDir().rmdir(mountPoint);
    }
    clearSession();
}
#endif

void MainWindow::addRecentSession(const QString& path) {
    const QString normalized = path.startsWith("content:")?path:QFileInfo(path).absoluteFilePath();
    if (normalized == QFileInfo(autoSessionPath()).absoluteFilePath()) return;
    mRecentSessions.removeAll(normalized);
    mRecentSessions.prepend(normalized);
    while (mRecentSessions.size() > 10) mRecentSessions.removeLast();
    QSettings(PACKAGE_NAME, APP_NAME).setValue("recentSessions", mRecentSessions);
    updateRecentSessionsMenu();
}

void MainWindow::updateRecentSessionsMenu() {
    if (!mRecentSessionsMenu) return;
    mRecentSessionsMenu->clear();
    if (mRecentSessions.isEmpty()) {
        auto* empty = mRecentSessionsMenu->addAction(tr("No recent sessions"));
        empty->setEnabled(false);
    }
    for (const QString& path : mRecentSessions) {
        QString name=QFileInfo(path).completeBaseName();
#ifdef __ANDROID__
        if(path.startsWith("content:")){
            const QString displayName=motioncam::documentDisplayName(path);
            if(!displayName.isEmpty())name=QFileInfo(displayName).completeBaseName();
        }
#endif
        auto* action = mRecentSessionsMenu->addAction(name);
        action->setToolTip(path);
        connect(action, &QAction::triggered, this, [this, path] {
            if (path.startsWith("content:")||QFileInfo::exists(path)) {
                if (confirmSaveBeforeReplacingSession()) loadSessionFromFile(path);
            }
            else {
                mRecentSessions.removeAll(path);
                QSettings(PACKAGE_NAME, APP_NAME).setValue("recentSessions", mRecentSessions);
                updateRecentSessionsMenu();
            }
        });
    }
    mRecentSessionsMenu->addSeparator();
    mRecentSessionsMenu->addAction(tr("Clear Recent Sessions"), this,
                                   &MainWindow::onClearRecentSessions);
}

void MainWindow::onClearRecentSessions() {
    mRecentSessions.clear();
    QSettings(PACKAGE_NAME, APP_NAME).setValue("recentSessions", mRecentSessions);
    updateRecentSessionsMenu();
}

void MainWindow::onNewSession() {
    if (!confirmSaveBeforeReplacingSession()) return;
    clearSession();
    mCurrentSessionFile.clear();
    clearApplyFeedback();
}
void MainWindow::onLoadSession() {
    const QUrl selected=QFileDialog::getOpenFileUrl(this,tr("Load Session"),
        QUrl::fromLocalFile(sessionDirectory()),tr("JSON files (*.json)"));
    const QString path=selected.isLocalFile()?selected.toLocalFile():selected.toString(QUrl::FullyEncoded);
    if(!path.isEmpty()&&confirmSaveBeforeReplacingSession())loadSessionFromFile(path);
}

QString MainWindow::promptSessionSavePath() {
    const QString initial=mCurrentSessionFile.isEmpty()?sessionDirectory():mCurrentSessionFile;
    const QUrl selected=QFileDialog::getSaveFileUrl(this,tr("Save Session As"),
        initial.startsWith("content:")?QUrl(initial):QUrl::fromLocalFile(initial),
        tr("JSON files (*.json)"));
    QString path=selected.isLocalFile()?selected.toLocalFile():selected.toString(QUrl::FullyEncoded);
    if(!path.isEmpty()&&!path.startsWith("content:")&&
       !path.endsWith(".json",Qt::CaseInsensitive))path+=".json";
    return path;
}

bool MainWindow::confirmSaveBeforeReplacingSession() {
    if (mMountedFiles.isEmpty()) return true;
    QMessageBox prompt(this);
    prompt.setIcon(QMessageBox::Question);
    prompt.setWindowTitle(tr("Save Session"));
    prompt.setText(tr("Save the current session before continuing?"));
    auto* save = prompt.addButton(tr("Save"), QMessageBox::AcceptRole);
    prompt.addButton(tr("Discard"), QMessageBox::DestructiveRole);
    auto* cancel = prompt.addButton(QMessageBox::Cancel);
    prompt.setDefaultButton(save);
    prompt.exec();
    if (prompt.clickedButton() == cancel || !prompt.clickedButton()) return false;
    if (prompt.clickedButton() != save) return true;
    QString path = mCurrentSessionFile;
    if (path.isEmpty()) {
        path = promptSessionSavePath();
        if (path.isEmpty()) return false;
    }
    return saveSessionToFile(path);
}
void MainWindow::onSaveSession() {
    if (mCurrentSessionFile.isEmpty()) onSaveSessionAs();
    else saveSessionToFile(mCurrentSessionFile);
}
void MainWindow::onSaveSessionAs() {
    QString path = promptSessionSavePath();
    if (!path.isEmpty()) {
        saveSessionToFile(path);
    }
}

void MainWindow::onSetDefaultSettings(bool checked) {
    ui->draftModeCheckBox->setCheckState(Qt::CheckState::Unchecked);
    ui->vignetteCorrectionComboBox->setCurrentText("Bake");
    ui->normalizeGainMapsCheckBox->setCheckState(Qt::CheckState::Unchecked);
    ui->debugVignetteCheckBox->setCheckState(Qt::CheckState::Unchecked);
    ui->vignetteOnlyColorCheckBox->setCheckState(Qt::CheckState::Checked);
    ui->optimizeGainMapsCheckBox->setCheckState(Qt::CheckState::Unchecked);
    ui->normalizeExposureCheckBox->setCheckState(Qt::CheckState::Checked);
    ui->smoothExposureCheckBox->setCheckState(Qt::CheckState::Unchecked);
    ui->smoothWhiteBalanceCheckBox->setCheckState(Qt::CheckState::Unchecked);
    ui->bakeIsoCheckBox->setCheckState(Qt::CheckState::Unchecked);
    ui->cfrConversionCheckBox->setCheckState(Qt::CheckState::Checked);
    ui->cropEnableCheckBox->setCheckState(Qt::CheckState::Unchecked);
    ui->camModelOverrideCheckBox->setCheckState(Qt::CheckState::Checked);
    ui->logTransformCheckBox->setCheckState(Qt::CheckState::Checked);
    ui->logHqCheckBox->setChecked(false);
    ui->higherCfaHqCheckBox->setChecked(false);
    ui->dngCompressionModeComboBox->setCurrentIndex(0);
    ui->dngCompressionCheckBox->setChecked(true);

    mRenderSettings.draftScale = 1;
    mRenderSettings.jxlDistance = -1.0f;
    mRenderSettings.cfrTarget = stringToCFRTarget("Prefer Integer");
    mRenderSettings.exposureCompensation.clear();
    mRenderSettings.cameraModel = "Panasonic";
    mRenderSettings.levels = "Dynamic";
    mRenderSettings.logTransform = stringToLogTransformMode("Keep Input");
    mRenderSettings.quadBayerOption = stringToQuadBayerMode("Demosaic");
    mRenderSettings.badPixelTreatment = BadPixelTreatment::Bake;
    mRenderSettings.vignetteCorrection = VignetteCorrectionMode::Bake;
    mRenderSettings.cfaPhase = "Don't override CFA";

    ui->cfrTarget->setCurrentText(QString::fromStdString(cfrTargetToString(mRenderSettings.cfrTarget)));
    ui->exposureCompensationLineEdit->setText(QString::fromStdString(mRenderSettings.exposureCompensation));
    ui->camModelOverrideComboBox->setCurrentText(QString::fromStdString(mRenderSettings.cameraModel));
    ui->levelsComboBox->setCurrentText(QString::fromStdString(mRenderSettings.levels));
    ui->cropTargetComboBox->setCurrentText(QString::fromStdString(mRenderSettings.cropTarget));
    ui->logTransformComboBox->setCurrentText(QString::fromStdString(logTransformModeToString(mRenderSettings.logTransform)));
    ui->quadBayerComboBox->setCurrentText(QString::fromStdString(quadBayerModeToString(mRenderSettings.quadBayerOption)));
    ui->badPixelTreatmentComboBox->setCurrentText(QString::fromStdString(
        badPixelTreatmentToString(mRenderSettings.badPixelTreatment)));
    ui->cfaPhaseComboBox->setCurrentText(QString::fromStdString(mRenderSettings.cfaPhase));

    updateUi();
}

void MainWindow::createCalibrationJson(QWidget* fileWidget) {
    auto filePath = fileWidget->property("filePath").toString();
    if (filePath.isEmpty()) {
        QMessageBox::warning(this, "Error", "File path not found");
        return;
    }

    QFileInfo fileInfo(filePath);
    QString jsonPath = fileWidget->property("sidecarPath").toString();
    if (jsonPath.isEmpty()) jsonPath = fileInfo.isDir()
        ? fileInfo.absoluteFilePath() + "/" + fileInfo.fileName() + ".json"
        : fileInfo.absolutePath() + "/" + fileInfo.completeBaseName() + ".json";
#ifdef __ANDROID__
    const QString sourceDirectory = motioncam::sourceDirectoryForImport(filePath);
    if (sourceDirectory.isEmpty()) {
        mPendingAndroidJsonCard = fileWidget;
        if (auto* scanButton = findChild<QPushButton*>("scanSidecarDirectoryButton"))
            scanButton->click();
        return;
    }
    fileWidget->setProperty("sourceDirectory", sourceDirectory);
    if (auto* label = fileWidget->findChild<QLabel*>("sourceDirectoryLabel"))
        label->setText(tr("Source: %1").arg(sourceDirectory));
#endif

    // When exactly one clip with a valid sidecar is selected, the calibration
    // buttons on the other clips copy that sidecar instead of creating a new
    // one from the global template.
    for (auto* candidate : fileWidget->findChildren<QPushButton*>()) {
        if (!candidate->property("calibButton").toBool()) continue;
        const QString sourcePath = candidate->property("copyJsonSourcePath").toString();
        if (!sourcePath.isEmpty()) {
#ifdef __ANDROID__
            QFile sourceJson(sourcePath);
            if (!sourceJson.open(QIODevice::ReadOnly)) {
                QMessageBox::critical(this, tr("Error"), sourceJson.errorString());
                return;
            }
            const QString writeError = motioncam::writeSidecarToSource(
                filePath, QFileInfo(jsonPath).fileName(),
                QString::fromUtf8(sourceJson.readAll()));
            if (!writeError.isEmpty()) {
                QMessageBox::critical(this, tr("Error"), writeError);
                return;
            }
#endif
            if (!QFile::copy(sourcePath, jsonPath)) {
                QMessageBox::critical(this, tr("Error"),
                    tr("Failed to copy calibration file:\n%1\n\nto:\n%2")
                        .arg(sourcePath, jsonPath));
                return;
            }
            reloadCalibration(fileWidget);
            return;
        }
        break;
    }

    // Check if JSON already exists
    if (QFile::exists(jsonPath)) {
        auto reply = QMessageBox::question(this, "File Exists",
            QString("Calibration file already exists:\n%1\n\nOverwrite?").arg(jsonPath),
            QMessageBox::Yes | QMessageBox::No);
        if (reply != QMessageBox::Yes) {
            return;
        }
    }

    // Read the global calibration.json file directly to preserve all fields (including disabled ones)
    std::string jsonContent;
    QString appDir = QCoreApplication::applicationDirPath();
    QString globalCalibPath = QDir(appDir).absoluteFilePath("calibration.json");

    if (QFile::exists(globalCalibPath)) {
        // Read the global calibration.json file
        std::ifstream inFile(globalCalibPath.toStdString());
        if (inFile.is_open()) {
            std::stringstream buffer;
            buffer << inFile.rdbuf();
            jsonContent = buffer.str();
            inFile.close();
        } else {
            // Fallback to example if can't read
            jsonContent = motioncam::CalibrationData::createExampleJson();
        }
    } else {
        // Use example template if global calibration doesn't exist
        jsonContent = motioncam::CalibrationData::createExampleJson();
    }

    // The global calibration may predate fields added to the generated
    // template. Preserve its values and ordering, but add any missing example
    // fields so newly-created per-clip sidecars are current.
    try {
        auto calibration = nlohmann::ordered_json::parse(jsonContent);
        const auto example = nlohmann::ordered_json::parse(
            motioncam::CalibrationData::createExampleJson());
        for (const auto& [key, value] : example.items())
            if (!calibration.contains(key)) calibration[key] = value;
        jsonContent = calibration.dump(2);
    } catch (const std::exception& e) {
        spdlog::warn("Could not merge new calibration template fields: {}", e.what());
    }

#ifdef __ANDROID__
    const QString writeError = motioncam::writeSidecarToSource(
        filePath, QFileInfo(jsonPath).fileName(),
        QString::fromStdString(jsonContent));
    if (!writeError.isEmpty()) {
        QMessageBox::critical(this, tr("Error"), writeError);
        return;
    }
#endif

    // Write JSON file
    std::ofstream outFile(jsonPath.toStdString());
    if (!outFile.is_open()) {
        QMessageBox::critical(this, "Error", QString("Failed to create calibration file:\n%1").arg(jsonPath));
        return;
    }

    outFile << jsonContent;
    outFile.close();

    // Reload the newly-created sidecar for any already-mounted clip.
    reloadCalibration(fileWidget);
}

void MainWindow::reloadCalibration(QWidget* fileWidget) {
    bool ok = false;
    const auto mountId = fileWidget->property("mountId").toInt(&ok);
    if (!ok) return;

    // A refresh is also the explicit rescan action for sidecars edited outside
    // Fuse. Reload every mounted clip: more than one JSON may have been edited
    // since the last rescan, and limiting invalidation to the clicked card
    // leaves the remaining processed thumbnails stale. Preserve each clip's
    // global/local settings and its routed sidecar path.
    for (const auto& mounted : mMountedFiles) {
#ifdef __ANDROID__
        const QString sidecarSyncError = motioncam::syncSidecarsForImport(
            mounted.srcFile);
        if (!sidecarSyncError.isEmpty())
            spdlog::warn("{}", sidecarSyncError.toStdString());
#endif
        mFuseFilesystem->updateOptions(
            mounted.mountId, settingsForMount(mounted.mountId));
    }

    // FileInfo (including sidecar validity) is rebuilt by updateOptions.  Read
    // it only after the reload so Create JSON and the clickable Loaded/Ignored
    // status immediately reflect the file that was just written or edited.
    updateCalibrationButtonStates();
    for (const auto& mounted : mMountedFiles)
        updateThumbnail(mounted.mountId);
    updateFpsLabels();

    // A settings change reaches onProcessingFinished(), which refreshes an
    // open per-clip player.  Sidecar-only changes must do the same even though
    // the RenderSettings themselves did not change.
    if (mClipPlayer && mGalleryMountId != motioncam::InvalidMountId) {
        for (const auto& mounted : mMountedFiles)
            mClipPlayer->invalidateThumbnails(mounted.mountId);
        for (const auto& mounted : mMountedFiles) {
            const auto info = mFuseFilesystem->getFileInfo(mounted.mountId);
            if (!info) continue;
            const double fps = info->isSequence && info->fps > 0.0f ? info->fps : 1.0;
            const int frames = std::max(1, info->totalFrames - info->droppedFrames +
                info->duplicatedFrames);
            const double duration = info->runtimeSeconds > 0.0f
                ? info->runtimeSeconds : frames / fps;
            mClipPlayer->updateClipInfo(mounted.mountId, fps, duration, frames,
                info->width, info->height, info->duplicateFrameMask,
                info->sourceFrameToOutput, info->sourceFrameDuplicated);
        }
        mClipPlayer->reloadCurrentClip();
    }
}

void MainWindow::updateCalibrationButtonStates(QWidget* onlyFileWidget) {
    auto* scrollContent = ui->dragAndDropScrollArea->widget();
    if (!scrollContent) return;

    auto fileWidgets = onlyFileWidget
        ? QList<QWidget*>{onlyFileWidget}
        : scrollContent->findChildren<QWidget*>(QString(), Qt::FindDirectChildrenOnly);

    QString selectedJsonPath;
    QWidget* selectedFileWidget = nullptr;
    if (mSelectedMountIds.size() == 1) {
        selectedFileWidget = fileWidgetForMount(*mSelectedMountIds.constBegin());
        if (selectedFileWidget) {
            const QFileInfo selectedInfo(selectedFileWidget->property("filePath").toString());
            QString candidate = selectedFileWidget->property("sidecarPath").toString();
            if (candidate.isEmpty()) candidate = selectedInfo.isDir()
                ? selectedInfo.absoluteFilePath() + "/" + selectedInfo.fileName() + ".json"
                : selectedInfo.absolutePath() + "/" + selectedInfo.completeBaseName() + ".json";
            if (QFile::exists(candidate)) {
                bool valid = false;
                const auto selectedMountId = selectedFileWidget->property("mountId").toInt();
                const auto info = mFuseFilesystem->getFileInfo(selectedMountId);
                if (info && info->sidecarState >= 0)
                    valid = info->sidecarState == 1;
                else
                    valid = motioncam::CalibrationData::loadFromFile(
                        candidate.toStdString()).has_value();
                if (valid) selectedJsonPath = candidate;
            }
        }
    }

    for (auto* fileWidget : fileWidgets) {
        // Skip if not a file widget (e.g., separators)
        if (!fileWidget->property("filePath").isValid()) {
            continue;
        }

        auto filePath = fileWidget->property("filePath").toString();
        QFileInfo fileInfo(filePath);
        QString jsonPath = fileWidget->property("sidecarPath").toString();
        if (jsonPath.isEmpty()) jsonPath = fileInfo.isDir()
            ? fileInfo.absoluteFilePath() + "/" + fileInfo.fileName() + ".json"
            : fileInfo.absolutePath() + "/" + fileInfo.completeBaseName() + ".json";

        if (auto* label = fileWidget->findChild<QLabel*>(QStringLiteral("missingMetadataLabel"))) {
            QStringList missing;
            const auto info = mFuseFilesystem->getFileInfo(
                fileWidget->property("mountId").toInt());
            if (info) {
                if (!info->hasGainMaps) missing << tr("gain maps");
                if (!info->hasColorMatrices) missing << tr("color matrices");
                if (!info->hasForwardMatrices) missing << tr("forward matrices");
                if (!info->hasAsShotNeutral) missing << tr("AsShotNeutral");
            }
            label->setText(missing.isEmpty() ? QString()
                : tr("no %1").arg(missing.join(", no ")));
            label->setToolTip(tr("Metadata availability for the first source frame."));
            label->setVisible(!missing.isEmpty());
#ifndef __ANDROID__
            fileWidget->setFixedHeight(missing.isEmpty() ? 156 : 166);
#endif
        }

        // Find the calibration button, status container, label, and refresh button
        QPushButton* actualCalibButton = nullptr;
        QPushButton* actualRefreshButton = nullptr;
        QWidget* statusContainer = nullptr;
        for (auto* btn : fileWidget->findChildren<QPushButton*>()) {
            if (btn->property("calibButton").toBool()) {
                actualCalibButton = btn;
            }
            if (btn->property("refreshButton").toBool()) {
                actualRefreshButton = btn;
            }
        }

        for (auto* widget : fileWidget->findChildren<QWidget*>()) {
            if (widget->property("statusContainer").toBool()) {
                statusContainer = widget;
                break;
            }
        }

        QLabel* actualStatusLabel = nullptr;
        for (auto* lbl : fileWidget->findChildren<QLabel*>()) {
            if (lbl->property("calibStatusLabel").toBool()) {
                actualStatusLabel = lbl;
                break;
            }
        }

        if (!actualCalibButton || !actualStatusLabel || !actualRefreshButton || !statusContainer) {
            continue;
        }

        actualRefreshButton->setProperty("sourceJsonPath", jsonPath);
        actualRefreshButton->setProperty("sourceClipPath", filePath);

        const bool canCopySelectedJson = !selectedJsonPath.isEmpty() &&
                                         fileWidget != selectedFileWidget;
        actualCalibButton->setText(canCopySelectedJson ? tr("Copy JSON") : tr("Create JSON"));
        actualCalibButton->setToolTip(canCopySelectedJson
            ? tr("Copy the selected clip's calibration JSON to this clip")
            : QString());
        actualCalibButton->setProperty("copyJsonSourcePath",
            canCopySelectedJson ? selectedJsonPath : QString());

        // Check if JSON exists and is valid. MCRAW mounting already parsed the
        // complete sidecar, which can contain large per-frame gain-map data;
        // reuse that result instead of parsing it again on the UI thread.
        if (QFile::exists(jsonPath)) {
            std::optional<bool> mountedSidecarValid;
            bool mountOk = false;
            const auto mountId = fileWidget->property("mountId").toInt(&mountOk);
            if (mountOk) {
                const auto info = mFuseFilesystem->getFileInfo(mountId);
                if (info && info->sidecarState >= 0)
                    mountedSidecarValid = info->sidecarState == 1;
            }
            const bool calibrationValid = mountedSidecarValid
                ? *mountedSidecarValid
                : motioncam::CalibrationData::loadFromFile(jsonPath.toStdString()).has_value();

            if (calibrationValid) {
                // Valid calibration found
                actualCalibButton->setVisible(false);
                actualStatusLabel->setText("Calibration Loaded");
                styleClipMetadata(actualStatusLabel,QStringLiteral("font-weight:bold;color:#00AA00;"));
                actualStatusLabel->setVisible(true);

                // Adjust label size and position
                actualStatusLabel->adjustSize();
                actualStatusLabel->move(0, (statusContainer->height() - actualStatusLabel->height()) / 2);

                // Show refresh button and position it to overlay the label
                int labelWidth = actualStatusLabel->fontMetrics().horizontalAdvance(actualStatusLabel->text());
                actualRefreshButton->setGeometry(0, 0, labelWidth + 20, statusContainer->height());
                actualRefreshButton->setVisible(true);
                actualRefreshButton->raise();

                statusContainer->setFixedWidth(labelWidth + 20);
                statusContainer->setVisible(true);
            } else {
                // Invalid calibration
                actualCalibButton->setVisible(false);
                actualStatusLabel->setText("Calibration Ignored");
                styleClipMetadata(actualStatusLabel,QStringLiteral("font-weight:bold;color:#AA0000;"));
                actualStatusLabel->setVisible(true);

                // Adjust label size and position
                actualStatusLabel->adjustSize();
                actualStatusLabel->move(0, (statusContainer->height() - actualStatusLabel->height()) / 2);

                // Show refresh button and position it to overlay the label
                int labelWidth = actualStatusLabel->fontMetrics().horizontalAdvance(actualStatusLabel->text());
                actualRefreshButton->setGeometry(0, 0, labelWidth + 20, statusContainer->height());
                actualRefreshButton->setVisible(true);
                actualRefreshButton->raise();

                statusContainer->setFixedWidth(labelWidth + 20);
                statusContainer->setVisible(true);
            }
        } else {
            // No JSON file
            actualCalibButton->setVisible(true);
            actualStatusLabel->setVisible(false);
            actualRefreshButton->setVisible(false);
            statusContainer->setVisible(false);
        }
    }
}
