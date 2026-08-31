#include "mainwindow.h"
#include "FrameTimingDialog.h"
#include "ui_mainwindow.h"
#include "settingsdialog.h"
#include "CalibrationData.h"
#include "CameraFrameMetadata.h"
#include "CameraMetadata.h"
#include "DNGDecoder.h"
#include "Utils.h"
#include "VirtualFileSystemImpl.h"

#include <motioncam/Decoder.hpp>

// Prevent Windows.h macros from interfering with std::max/min
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#endif

#include <QDragEnterEvent>
#include <QDropEvent>
#include <QKeyEvent>

using namespace motioncam;
#include <QMimeData>
#include <QPushButton>
#include <QAction>
#include <QMenuBar>
#include <QFileInfo>
#include <QProcess>
#include <QPointer>
#include <QMessageBox>
#include <QFileDialog>
#include <QSettings>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QDir>
#include <QSignalBlocker>
#include <QLabel>
#include <QPainter>
#include <QFrame>
#include <QProgressDialog>
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
#elif __linux__
#include "linux/FuseFileSystemImpl_Linux.h"
#include <cerrno>
#include <sys/stat.h>
#endif

namespace {
    constexpr auto PACKAGE_NAME = "com.motioncam";
    constexpr auto APP_NAME = "MotionCam FS";
    constexpr auto RIFE_REVISION = "b0542ef99f380f0fe17a1b44151ac6935e8b82d4";
    constexpr auto RIFE_ARCHIVE_SHA256 =
        "35bcf9b169e69f8aee5dfb26005424579109b7d9421bb16e3b675a5142b485bd";

    bool isCameraNativeFormat(const QString& mode) {
        const QString trimmed = mode.trimmed();
        return trimmed.startsWith("HEVC ", Qt::CaseInsensitive) ||
               trimmed.startsWith("AV1 ", Qt::CaseInsensitive) ||
               trimmed.startsWith("ProRes ", Qt::CaseInsensitive) ||
               trimmed.startsWith("CineForm ", Qt::CaseInsensitive);
    }

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

#ifdef __linux__
    bool cleanupStaleLinuxFuseMount(const QString& mountPath, QString& errorMessage) {
        const QByteArray nativePath = QFile::encodeName(QDir::cleanPath(mountPath));
        struct stat pathStat {};
        if (::lstat(nativePath.constData(), &pathStat) == 0 || errno != ENOTCONN) {
            return true;
        }

        QProcess fusermount;
        fusermount.start("fusermount3", {"-u", QDir::cleanPath(mountPath)});
        if (!fusermount.waitForStarted() || !fusermount.waitForFinished(10000) ||
            fusermount.exitStatus() != QProcess::NormalExit || fusermount.exitCode() != 0) {
            const QString details =
                QString::fromLocal8Bit(fusermount.readAllStandardError()).trimmed();
            errorMessage = QString("A stale FUSE mount exists at %1 and could not be detached%2.")
                               .arg(QDir::cleanPath(mountPath),
                                    details.isEmpty() ? QString() : QString(": %1").arg(details));
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

    // Build options bitfield
    settings.options = motioncam::RENDER_OPT_NONE;

    if(ui->draftModeCheckBox->checkState() == Qt::CheckState::Checked)
        settings.options |= motioncam::RENDER_OPT_DRAFT;

    if(ui->vignetteCorrectionCheckBox->checkState() == Qt::CheckState::Checked)
        settings.options |= motioncam::RENDER_OPT_APPLY_VIGNETTE_CORRECTION;

    if(ui->vignetteOnlyColorCheckBox->checkState() == Qt::CheckState::Checked)
        settings.options |= motioncam::RENDER_OPT_VIGNETTE_ONLY_COLOR;

    if(ui->optimizeGainMapsCheckBox->checkState() == Qt::CheckState::Checked)
        settings.options |= motioncam::RENDER_OPT_OPTIMIZE_GAIN_MAPS;

    if(ui->scaleRawCheckBox->checkState() == Qt::CheckState::Checked)
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
    QTimer::singleShot(0, this, &MainWindow::cleanupStaleMacFuseMounts);
#elif __linux__
    mFuseFilesystem = std::make_unique<motioncam::FuseFileSystemImpl_Linux>();
#endif

    // Enable drag and drop on the scroll area
    ui->dragAndDropScrollArea->setAcceptDrops(true);
    ui->dragAndDropScrollArea->installEventFilter(this);

    restoreSettings();
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
    connect(ui->vignetteCorrectionCheckBox, &QCheckBox::checkStateChanged, this, &MainWindow::onRenderSettingsChanged);
    connect(ui->scaleRawCheckBox, &QCheckBox::checkStateChanged, this, &MainWindow::onRenderSettingsChanged);
    connect(ui->debugVignetteCheckBox, &QCheckBox::checkStateChanged, this, &MainWindow::onRenderSettingsChanged);
    connect(ui->vignetteOnlyColorCheckBox, &QCheckBox::checkStateChanged, this, &MainWindow::onRenderSettingsChanged);
    connect(ui->optimizeGainMapsCheckBox, &QCheckBox::checkStateChanged, this, &MainWindow::onRenderSettingsChanged);
    connect(ui->normalizeExposureCheckBox, &QCheckBox::checkStateChanged, this, &MainWindow::onRenderSettingsChanged);
    connect(ui->smoothExposureCheckBox, &QCheckBox::checkStateChanged, this, &MainWindow::onRenderSettingsChanged);
    connect(ui->smoothWhiteBalanceCheckBox, &QCheckBox::checkStateChanged, this, &MainWindow::onRenderSettingsChanged);
    connect(ui->bakeIsoCheckBox, &QCheckBox::checkStateChanged, this, &MainWindow::onRenderSettingsChanged);
    connect(ui->cfrConversionCheckBox, &QCheckBox::checkStateChanged, this, &MainWindow::onRenderSettingsChanged);
    connect(ui->rifeInterpolationCheckBox, &QCheckBox::checkStateChanged, this, [this](Qt::CheckState) { saveSettings(); });
    connect(ui->detectDuplicateDngsCheckBox, &QCheckBox::checkStateChanged, this, [this](Qt::CheckState) { saveSettings(); });
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
    connect(ui->remosaicCheckBox, &QCheckBox::checkStateChanged, this, &MainWindow::onRenderSettingsChanged);
    connect(ui->higherCfaHqCheckBox, &QCheckBox::checkStateChanged, this, &MainWindow::onRenderSettingsChanged);
    connect(ui->dngCompressionCheckBox, &QCheckBox::checkStateChanged, this, &MainWindow::onRenderSettingsChanged);
    connect(ui->dngCompressionModeComboBox, &QComboBox::currentTextChanged, this,
            [this](const QString& text) {
        const QString mode = text.trimmed();
        if (mode.compare("JPEG 92 Lossless", Qt::CaseInsensitive) == 0) {
            mRenderSettings.jxlDistance = -1.0f;
        } else if (mode.compare("JPEG DCT 12b", Qt::CaseInsensitive) == 0 ||
                   mode.compare("CinemaDNG 12-bit Lossy", Qt::CaseInsensitive) == 0 ||
                   mode.compare("JPEG DCT Lossy", Qt::CaseInsensitive) == 0) {
            mRenderSettings.jxlDistance = motioncam::DNG_COMPRESSION_JPEG_DCT;
        } else if (mode.compare("JPEG XL Lossless", Qt::CaseInsensitive) == 0) {
            mRenderSettings.jxlDistance = 0.0f;
        } else {
            float distance = 0.0f;
            if (parseJxlDctDistance(mode, distance))
                mRenderSettings.jxlDistance = distance;
        }
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
    autoSaveSession();
    saveSettings();
    mThumbnailTasks.waitForFinished();

    // Wait for any ongoing processing
    if (mProcessingWatcher && mProcessingWatcher->isRunning()) {
        mProcessingWatcher->waitForFinished();
    }

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

    settings.setValue("draftMode", ui->draftModeCheckBox->checkState() == Qt::CheckState::Checked);
    settings.setValue("applyVignetteCorrection", ui->vignetteCorrectionCheckBox->checkState() == Qt::CheckState::Checked);
    settings.setValue("scaleRaw", ui->scaleRawCheckBox->checkState() == Qt::CheckState::Checked);
    settings.setValue("vignetteOnlyColor", ui->vignetteOnlyColorCheckBox->checkState() == Qt::CheckState::Checked);
    settings.setValue("optimizeGainMaps", ui->optimizeGainMapsCheckBox->isChecked());
    settings.setValue("normalizeExposure", ui->normalizeExposureCheckBox->checkState() == Qt::CheckState::Checked);
    settings.setValue("smoothExposure", ui->smoothExposureCheckBox->checkState() == Qt::CheckState::Checked);
    settings.setValue("smoothWhiteBalance", ui->smoothWhiteBalanceCheckBox->checkState() == Qt::CheckState::Checked);
    settings.setValue("bakeIso", ui->bakeIsoCheckBox->checkState() == Qt::CheckState::Checked);
    settings.setValue("cfrConversion", ui->cfrConversionCheckBox->checkState() == Qt::CheckState::Checked);
    settings.setValue("rifeInterpolation", ui->rifeInterpolationCheckBox->isChecked());
    settings.setValue("detectDuplicateDngs", ui->detectDuplicateDngsCheckBox->isChecked());
    settings.setValue("cropEnabled", ui->cropEnableCheckBox->checkState() == Qt::CheckState::Checked);
    settings.setValue("camModelOverrideEnabled", ui->camModelOverrideCheckBox->checkState() == Qt::CheckState::Checked);
    settings.setValue("logTransformEnabled", ui->logTransformCheckBox->checkState() == Qt::CheckState::Checked);
    settings.setValue("remosaicEnabled", ui->remosaicCheckBox->isChecked());
    settings.setValue("jpegCompression", ui->dngCompressionCheckBox->checkState() == Qt::CheckState::Checked);
    settings.setValue("jxlDistance", mRenderSettings.jxlDistance);
    const QString compressionMode = ui->dngCompressionModeComboBox->currentText();
    settings.setValue("cameraNativeFinalization", isCameraNativeFormat(compressionMode));
    settings.setValue("cameraNativeMode", compressionMode);
    settings.setValue("higherCfaHq", ui->higherCfaHqCheckBox->isChecked());
    settings.setValue("cachePath", mCacheRootFolder);
    settings.setValue("deleteOnUnmount", mDeleteOnUnmount);
    settings.setValue("playerPath", mPlayerPath);
    settings.setValue("cachePolicy", mCachePolicy == motioncam::CachePolicy::Quota ? "quota" : "off");
    settings.setValue("cacheQuotaBytes", static_cast<qulonglong>(mCacheQuotaBytes));
    settings.setValue("cacheCleanupIntervalSeconds", mCacheCleanupIntervalSeconds);
    settings.setValue("autoApplyClipSettings", mAutoApplyClipSettings);
    settings.setValue("unmountOnFinalize", mUnmountOnFinalize);
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

    ui->vignetteCorrectionCheckBox->setCheckState(
        !settings.contains("applyVignetteCorrection") ? Qt::CheckState::Checked :
        (settings.value("applyVignetteCorrection").toBool() ? Qt::CheckState::Checked : Qt::CheckState::Unchecked));

    ui->scaleRawCheckBox->setCheckState(
        settings.value("scaleRaw").toBool() ? Qt::CheckState::Checked : Qt::CheckState::Unchecked);

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
    ui->rifeInterpolationCheckBox->setChecked(settings.value("rifeInterpolation", false).toBool());
    ui->detectDuplicateDngsCheckBox->setChecked(settings.value("detectDuplicateDngs", true).toBool());

    ui->cropEnableCheckBox->setCheckState(
        settings.value("cropEnabled").toBool() ? Qt::CheckState::Checked : Qt::CheckState::Unchecked);

    ui->camModelOverrideCheckBox->setCheckState(
        !settings.contains("camModelOverrideEnabled") ? Qt::CheckState::Checked :
        (settings.value("camModelOverrideEnabled").toBool() ? Qt::CheckState::Checked : Qt::CheckState::Unchecked));

    ui->logTransformCheckBox->setCheckState(
        !settings.contains("logTransformEnabled") ? Qt::CheckState::Checked :
        (settings.value("logTransformEnabled").toBool() ? Qt::CheckState::Checked : Qt::CheckState::Unchecked));

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
        compressionIndex = 4;
        restoredNativeMode = true;
    }
    if (restoredNativeText.isEmpty())
        ui->dngCompressionModeComboBox->setCurrentIndex(compressionIndex);
    else
        ui->dngCompressionModeComboBox->setCurrentText(restoredNativeText);
    if (!restoredNativeMode && mRenderSettings.jxlDistance > 0.0f &&
        std::abs(mRenderSettings.jxlDistance - 0.3f) > 0.0001f) {
        ui->dngCompressionModeComboBox->setCurrentText(
            QString("JPEG XL DCT %1").arg(mRenderSettings.jxlDistance));
    }
    ui->higherCfaHqCheckBox->setChecked(
        settings.value("higherCfaHq", false).toBool());

    mCacheRootFolder = settings.value("cachePath").toString();
    mDeleteOnUnmount = settings.value("deleteOnUnmount", false).toBool();
    mPlayerPath = settings.value("playerPath").toString();
    mCachePolicy = settings.value("cachePolicy", "quota").toString() == "off"
        ? motioncam::CachePolicy::Off : motioncam::CachePolicy::Quota;
    mCacheQuotaBytes = settings.value("cacheQuotaBytes",
        QVariant::fromValue<qulonglong>(30ULL * 1024 * 1024 * 1024)).toULongLong();
    mCacheCleanupIntervalSeconds = settings.value("cacheCleanupIntervalSeconds", 30).toInt();
    mAutoApplyClipSettings = settings.value("autoApplyClipSettings", true).toBool();
    mUnmountOnFinalize = settings.value("unmountOnFinalize", true).toBool();
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

bool MainWindow::eventFilter(QObject *watched, QEvent *event) {
    if (auto* card = qobject_cast<QWidget*>(watched);
        card && card->property("clipCard").toBool() &&
        event->type() == QEvent::MouseButtonRelease) {
        if (auto* check = card->findChild<QCheckBox*>("clipSelection"))
            check->toggle();
        return true;
    }
    if (watched == ui->dragAndDropScrollArea) {
        if (event->type() == QEvent::DragEnter) {
            auto* dragEvent = static_cast<QDragEnterEvent*>(event);

            if (dragEvent->mimeData()->hasUrls()) {
                const auto urls = dragEvent->mimeData()->urls();

                // Check if at least one file has the extension we want
                for (const auto& url : urls) {
                    auto filePath = url.toLocalFile();

                    // Accept MCRAW files, MOV/MP4 files with NATIVE suffix, or DNG files/directories
                    if (filePath.endsWith(".mcraw", Qt::CaseInsensitive) ||
                        (filePath.contains("NATIVE", Qt::CaseInsensitive) &&
                         (filePath.endsWith(".mov", Qt::CaseInsensitive) ||
                          filePath.endsWith(".mp4", Qt::CaseInsensitive) ||
                          filePath.endsWith(".mkv", Qt::CaseInsensitive))) ||
                        filePath.endsWith(".dng", Qt::CaseInsensitive) ||
                        QFileInfo(filePath).isDir()) {
                        dragEvent->acceptProposedAction();
                        return true;
                    }
                }
            }

            return true;
        }
        else if (event->type() == QEvent::Drop) {
            auto* dropEvent = static_cast<QDropEvent*>(event);

            if (dropEvent->mimeData()->hasUrls()) {
                const auto urls = dropEvent->mimeData()->urls();

                for (const auto& url : urls) {
                    auto filePath = url.toLocalFile();
                    if (filePath.endsWith(".mcraw", Qt::CaseInsensitive) ||
                        (filePath.contains("NATIVE", Qt::CaseInsensitive) &&
                         (filePath.endsWith(".mov", Qt::CaseInsensitive) ||
                          filePath.endsWith(".mp4", Qt::CaseInsensitive) ||
                          filePath.endsWith(".mkv", Qt::CaseInsensitive))) ||
                        filePath.endsWith(".dng", Qt::CaseInsensitive) ||
                        QFileInfo(filePath).isDir()) {
                        mountFile(filePath);
                    }
                }

                dropEvent->acceptProposedAction();
            }

            return true;
        }
    }

    return QMainWindow::eventFilter(watched, event);
}

void MainWindow::mountFile(const QString& filePath) {
    const QString normalizedPath = QFileInfo(filePath).absoluteFilePath();
    for (const auto& mounted : mMountedFiles)
        if (QFileInfo(mounted.srcFile).absoluteFilePath() == normalizedPath)
            return;
    if (mMountInProgress) {
        QTimer::singleShot(200, this, [this, filePath] { mountFile(filePath); });
        return;
    }
    mMountInProgress = true;
    // Extract just the filename from the path
    QFileInfo fileInfo(filePath);
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
    const QString mountName = fileInfo.isDir()
        ? fileInfo.fileName() + "-mounted"
        : fileInfo.baseName();
    auto dstPath = destinationRoot + "/" + mountName;
    motioncam::MountId mountId;

    QProgressDialog mountProgress(
        tr("Reading and mounting %1...").arg(fileInfo.fileName()), QString(), 0, 0, this);
    mountProgress.setWindowModality(Qt::WindowModal);
    mountProgress.setCancelButton(nullptr);
    mountProgress.setMinimumDuration(0);
    mountProgress.show();
    QApplication::processEvents();
    QString mountError;
    const auto settings = buildRenderSettings();
#ifdef __APPLE__
    cleanupStaleMacFuseMounts();
#elif __linux__
    if (!cleanupStaleLinuxFuseMount(dstPath, mountError)) {
        mountProgress.close();
        mMountInProgress = false;
        QMessageBox::critical(this, tr("Error"), mountError);
        return;
    }
#endif
    auto future = QtConcurrent::run([this, settings, filePath, dstPath, &mountError] {
        try {
            return mFuseFilesystem->mount(
                settings, filePath.toStdString(), dstPath.toStdString());
        } catch (const std::exception& error) {
            mountError = QString::fromUtf8(error.what());
            return motioncam::InvalidMountId;
        }
    });
    while (!future.isFinished()) {
        QApplication::processEvents(QEventLoop::AllEvents, 20);
        QThread::msleep(10);
    }
    mountProgress.close();
    mountId = future.result();
    mMountInProgress = false;
    if (mountId == motioncam::InvalidMountId) {
        QMessageBox::critical(this, "Error", QString("There was an error mounting the file. (error: %1)").arg(mountError));
        return;
    }

    // Get the scroll area's content widget and its layout
    auto* scrollContent = ui->dragAndDropScrollArea->widget();
    auto* scrollLayout = qobject_cast<QVBoxLayout*>(scrollContent->layout());

    // Create a widget to hold a filename label and buttons
    auto* fileWidget = new QWidget(scrollContent);

    fileWidget->setFixedHeight(156);
    fileWidget->setProperty("filePath", filePath);
    fileWidget->setProperty("mountId", mountId);
    fileWidget->setProperty("mountPath", dstPath);
    fileWidget->setObjectName(QStringLiteral("clipCard"));
    fileWidget->setProperty("clipCard", true);
    fileWidget->setCursor(Qt::PointingHandCursor);
    fileWidget->installEventFilter(this);

    auto* cardLayout = new QHBoxLayout(fileWidget);
    cardLayout->setContentsMargins(8, 8, 8, 8);
    cardLayout->setSpacing(10);

    auto* thumbnailContainer = new QWidget(fileWidget);
    thumbnailContainer->setObjectName(QStringLiteral("thumbnailContainer"));
    thumbnailContainer->setFixedSize(176, 112);
    thumbnailContainer->setAttribute(Qt::WA_TransparentForMouseEvents, true);
    auto* thumbnailLayout = new QHBoxLayout(thumbnailContainer);
    thumbnailLayout->setContentsMargins(0, 0, 0, 0);
    thumbnailLayout->setAlignment(Qt::AlignCenter);

    auto* thumbnailLabel = new QLabel(tr("Loading..."), thumbnailContainer);
    thumbnailLabel->setObjectName(QStringLiteral("thumbnailLabel"));
    thumbnailLabel->setFixedSize(176, 112);
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
    fileLabel->setStyleSheet("font-weight: bold; font-size: 12pt;");
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
    metadataTextLayout->setSpacing(4);
    metadataLayout->addLayout(metadataTextLayout, 1);
    fileLayout->addLayout(metadataLayout);

    // Get file information from the FUSE filesystem
    auto fileInfoOpt = mFuseFilesystem->getFileInfo(mountId);
    if (fileInfoOpt.has_value()) {
        auto info = fileInfoOpt.value();

        // Format runtime as MM:SS
        int minutes = static_cast<int>(info.runtimeSeconds) / 60;
        int seconds = static_cast<int>(info.runtimeSeconds) % 60;
        QString runtimeStr = QString("%1:%2").arg(minutes).arg(seconds, 2, 10, QChar('0'));

        // Extract bit depth and log indicator from levelsInfo for styling
        QString levelsStr = QString::fromStdString(info.levelsInfo);
        QString rawPart;
        int rawIdx = levelsStr.lastIndexOf('b');
        while (rawIdx > 0 && levelsStr.at(rawIdx - 1).isDigit())
            --rawIdx;
        if (rawIdx >= 0) {
            rawPart = levelsStr.mid(rawIdx);
        }

        // First row: Runtime, Resolution, Data Type, Levels
        auto infoText1 = QString("<span style='color: #888888;'>Runtime: </span><span style='color: white;'>%1</span>"
                                 "<span style='color: #888888;'> | Resolution: %2x%3 | Data Type: %4 | Levels: %5</span>")
                                .arg(runtimeStr)
                                .arg(info.width)
                                .arg(info.height)
                                .arg(QString::fromStdString(info.dataType))
                                .arg(levelsStr.left(rawIdx));

        // Add styled RAW part if it exists
        if (!rawPart.isEmpty()) {
            infoText1 += QString("<span style='color: white;'>%1</span>").arg(rawPart);
        }

        auto* infoLabel1 = new QLabel(infoText1, fileWidget);
        infoLabel1->setStyleSheet("font-size: 9pt;");
        infoLabel1->setMinimumHeight(infoLabel1->fontMetrics().height() + 4);
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
        infoLabel2->setStyleSheet("font-size: 9pt;");
        infoLabel2->setMinimumHeight(infoLabel2->fontMetrics().height() + 4);
        infoLabel2->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);
        infoLabel2->setProperty("infoLabel2", true);
        infoLabel2->setProperty("mountId", QVariant(mountId));
        metadataTextLayout->addWidget(infoLabel2);
    }

    // Create and add the source folder label
    auto* sourceLabel = new QLabel(QString("Source: %1").arg(fileInfo.path()), fileWidget);
    sourceLabel->setStyleSheet("font-size: 9pt; color: #666666;");
    metadataTextLayout->addWidget(sourceLabel);

    // Add spacer to maintain button position
    fileLayout->addSpacing(8);

    // Create horizontal layout for buttons
    auto* buttonLayout = new QHBoxLayout();
    buttonLayout->setContentsMargins(0, 0, 0, 0);
    buttonLayout->setSpacing(8);

    // Define consistent button size
    const int buttonWidth = 100;
    const int buttonHeight = 30;

    // Create and add the open button
    auto* openButton = new QPushButton("Open", fileWidget);
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
    calibStatusLabel->setStyleSheet("font-size: 9pt; font-weight: bold;");
    calibStatusLabel->setProperty("calibStatusLabel", true);
    calibStatusLabel->setVisible(false);
    calibStatusLabel->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);

    // Create invisible refresh button on top of the status label
    auto* refreshButton = new QPushButton("", statusContainer);
    refreshButton->setStyleSheet("background: transparent; border: none;");
    refreshButton->setCursor(Qt::PointingHandCursor);
    refreshButton->setProperty("refreshButton", true);
    refreshButton->setVisible(false);
    refreshButton->setToolTip("Refresh Calibration");

    buttonLayout->addWidget(statusContainer);

    // Connect refresh button to update calibration
    connect(refreshButton, &QPushButton::clicked, this, [this] {
        updateFpsLabels();
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
    clipNumber->setStyleSheet("color:#7f93ad; font-size:9pt;");
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
                info->timingUsesCfrMapping, this);
            mTimingDialogs.insert(mountId, dialog);
            connect(dialog, &QObject::destroyed, this, [this, mountId] {
                mTimingDialogs.remove(mountId);
            });
        } else {
            dialog->updateTiming(*info->presentationTimestamps,
                                 info->timingTimeBaseNum, info->timingTimeBaseDen,
                                 info->fps, info->timingUsesCfrMapping);
        }
        dialog->show();
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
    connect(localReset, &QPushButton::clicked, this, [this, mountId] {
        mLocalSettings.remove(mountId);
        mFuseFilesystem->updateOptions(mountId, mGlobalRenderSettings);
        updateLocalBadge(mountId);
        updateSelectionUi();
        autoSaveSession();
    });

    // Add separator if there are already mounted files
    if (!mMountedFiles.empty()) {
        auto* separator = new QFrame(scrollContent);

        separator->setFrameShape(QFrame::HLine);
        separator->setFrameShadow(QFrame::Plain);
        separator->setLineWidth(1);
        separator->setStyleSheet("QFrame { color: #e0e0e0; margin: 16px 0px; }");
        scrollLayout->insertWidget(0, separator);
    }

    // Add the file widget to the scroll area
    scrollLayout->insertWidget(0, fileWidget);
    updateClipIndices();
    QTimer::singleShot(250, this, [this, mountId] { updateThumbnail(mountId); });

    for (auto* label : fileWidget->findChildren<QLabel*>())
        label->setAttribute(Qt::WA_TransparentForMouseEvents, true);

    // Hide the drag-drop label since we now have content
    ui->dragAndDropLabel->hide();

    // Connect buttons
    connect(openButton, &QPushButton::clicked, this, [this, fileWidget] {
        openMountedDirectory(fileWidget);
    });

    connect(playButton, &QPushButton::clicked, this, [this, filePath] {
        playFile(filePath);
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
        motioncam::MountedFile(mountId, filePath));
    autoSaveSession();

    // Update calibration button state
    updateCalibrationButtonStates();
}

void MainWindow::playFile(const QString& path) {
    bool success = false;

#ifdef _WIN32
    const QString playerPath = mPlayerPath.isEmpty()
        ? QDir(QCoreApplication::applicationDirPath()).absoluteFilePath("../Player/MotionCamPlayer.exe")
        : mPlayerPath;
    success = QProcess::startDetached(QDir::cleanPath(playerPath), QStringList() << path);
#elif __APPLE__
    success = mPlayerPath.isEmpty()
        ? QProcess::startDetached("/usr/bin/open", QStringList() << "-a" << "MotionCam Player" << path)
        : QProcess::startDetached("/usr/bin/open", QStringList() << "-a" << mPlayerPath << path);
#elif __linux__
    if (!mPlayerPath.isEmpty()) success = QProcess::startDetached(mPlayerPath, QStringList() << path);
#endif

    if (!success)
        QMessageBox::warning(this, "Error", QString("Failed to launch player with file: %1").arg(path));
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
#elif __linux__
    success = QProcess::startDetached("xdg-open", QStringList() << mountPath);
#endif

    if (!success)
        QMessageBox::warning(this, "Error", QString("Failed to open directory: %1").arg(mountPath));
}

void MainWindow::removeFile(QWidget* fileWidget) {
    auto* scrollContent = ui->dragAndDropScrollArea->widget();
    auto* scrollLayout = qobject_cast<QVBoxLayout*>(scrollContent->layout());
    const QString mountPath = fileWidget->property("mountPath").toString();

    // Find and remove the separator above this file widget if it exists
    int fileWidgetIndex = scrollLayout->indexOf(fileWidget);
    if (fileWidgetIndex > 0) {
        auto* itemAbove = scrollLayout->itemAt(fileWidgetIndex - 1);
        if (itemAbove && itemAbove->widget()) {
            auto* widgetAbove = itemAbove->widget();
            // Check if it's a separator (QFrame with HLine shape)
            auto* frame = qobject_cast<QFrame*>(widgetAbove);
            if (frame && frame->frameShape() == QFrame::HLine) {
                scrollLayout->removeWidget(frame);
                frame->deleteLater();
            }
        }
    }

    scrollLayout->removeWidget(fileWidget);
    fileWidget->deleteLater();

    // Unmount the file
    bool ok = false;
    auto mountId = fileWidget->property("mountId").toInt(&ok);
    if(ok) {
        if (auto* dialog = mTimingDialogs.value(mountId).data()) dialog->close();
        mFuseFilesystem->unmount(mountId);
        mSelectedMountIds.remove(mountId);
        mLocalSettings.remove(mountId);

#ifdef _WIN32
        if (mDeleteOnUnmount && !mountPath.isEmpty()) {
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
        if(it != mMountedFiles.end())

            mMountedFiles.erase(it);
    }

    // If all files are removed, show the drag-drop label again
    if (mMountedFiles.empty()) {
        ui->dragAndDropLabel->show();
    }
    updateClipIndices();
    updateSelectionUi();
    autoSaveSession();
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
    if (fileWidgetIndex > 0) {
        auto* itemAbove = scrollLayout->itemAt(fileWidgetIndex - 1);
        if (itemAbove && itemAbove->widget()) {
            auto* widgetAbove = itemAbove->widget();
            auto* frame = qobject_cast<QFrame*>(widgetAbove);
            if (frame && frame->frameShape() == QFrame::HLine) {
                scrollLayout->removeWidget(frame);
                frame->deleteLater();
            }
        }
    }

    scrollLayout->removeWidget(fileWidget);
    fileWidget->deleteLater();

    // Remove from mounted files list
    if(ok) {
        auto it = std::find_if(
            mMountedFiles.begin(), mMountedFiles.end(),
            [mountId](const motioncam::MountedFile& f) { return f.mountId == mountId; });
        if(it != mMountedFiles.end())
            mMountedFiles.erase(it);
    }

    if (mMountedFiles.empty()) {
        ui->dragAndDropLabel->show();
    }
}
#endif

std::optional<QString> MainWindow::ensureRifeRuntime() {
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
    progress.show();

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

void MainWindow::finalizeCameraNative(QWidget* fileWidget, const QString& mode) {
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

    // Camera Native is deliberately fed linear RGB. LOG60 is applied once by
    // FFmpeg immediately before RGB-to-YUV conversion, without dithering.
    auto stagingSettings = buildRenderSettings();
    stagingSettings.options &= ~motioncam::RENDER_OPT_REMOSAIC_TO_BAYER;
    stagingSettings.options &= ~motioncam::RENDER_OPT_JPEG_COMPRESSION;
    stagingSettings.options &= ~motioncam::RENDER_OPT_LOG_TRANSFORM;
    stagingSettings.draftScale = 1;
    stagingSettings.logTransform = motioncam::LogTransformMode::Disabled;
    stagingSettings.cameraNativeStaging = true;
    mFuseFilesystem->updateOptions(mountId, stagingSettings);

    const auto info = mFuseFilesystem->getFileInfo(mountId);
    if (!info) {
        mFuseFilesystem->updateOptions(mountId, buildRenderSettings());
        QMessageBox::warning(this, "Camera Native finalization", "Clip information is unavailable.");
        return;
    }
    // FileInfo::dataType is a display label and deliberately continues to say
    // "Quad Bayer CFA" even when the selected finalization path demosaics it.
    // Inspect source CFA metadata so every CFA source has demosaic enabled
    // before Camera Native staging requests a linear RGB sequence.
    bool unsupportedBayer = false;
    const bool demosaicCfa =
        stagingSettings.quadBayerOption == motioncam::QuadBayerMode::Demosaic ||
        stagingSettings.quadBayerOption == motioncam::QuadBayerMode::DemosaicOCL;
    try {
        const QFileInfo inputInfo(srcFile);
        const QString calibrationPath = inputInfo.isDir()
            ? QDir(srcFile).absoluteFilePath(inputInfo.fileName() + ".json")
            : inputInfo.absolutePath() + "/" + inputInfo.completeBaseName() + ".json";
        const auto calibration = QFile::exists(calibrationPath)
            ? CalibrationData::loadFromFile(calibrationPath.toStdString())
            : std::nullopt;
        const int cfaSizeOverride = calibration && calibration->hasCfaSize && calibration->cfaSize > 0
            ? calibration->cfaSize : 0;
        if (srcFile.endsWith(".mcraw", Qt::CaseInsensitive)) {
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
    const QString outputBase = sourceInfo.completeBaseName() + "_LOG60_NATIVE" + codecSuffix;
    const QString containerExtension = av1 ? ".mp4" : ".mov";
    const QDir outputDir(QFileInfo(mountPath).absolutePath());
    const QString outputPath = outputDir.absoluteFilePath(outputBase + containerExtension);
    const QString jsonPath = outputDir.absoluteFilePath(outputBase + ".json");
    if ((QFile::exists(outputPath) || QFile::exists(jsonPath)) &&
        QMessageBox::question(this, "Replace Camera Native output?",
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

    const bool detectDuplicateDngs = ui->detectDuplicateDngsCheckBox->isChecked();
    const bool interpolateFrames = ui->rifeInterpolationCheckBox->isChecked() &&
        (info->duplicatedFrames > 0 || detectDuplicateDngs);
    const auto rifeRuntime = interpolateFrames ? ensureRifeRuntime() : std::optional<QString>{QString{}};
    if (!rifeRuntime) {
        mFuseFilesystem->updateOptions(mountId, buildRenderSettings());
        return;
    }

    QProgressDialog progress("Rendering linear RGB sequence...", "Cancel", 0,
        std::max(1, info->totalFrames - info->droppedFrames + info->duplicatedFrames), this);
    progress.setWindowModality(Qt::WindowModal);
    progress.setMinimumDuration(0);
    progress.setAutoClose(false);
    progress.setAutoReset(false);

    try {
        motioncam::FinalizeOptions finalizeOptions;
        finalizeOptions.interpolateDuplicatedFrames = interpolateFrames;
        finalizeOptions.detectDuplicateDngs = detectDuplicateDngs;
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
        if (ffmpeg.isEmpty())
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
        if (av1) {
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
        } else if (!requiredEncoder.isEmpty() && !probeEncoder(requiredEncoder)) {
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
                        uint32_t width = 0, height = 0;
                        if (!DNGDecoder::extractUncompressedRGB16(dng, frame.rgb, width, height) ||
                            width != static_cast<uint32_t>(info->width) ||
                            height != static_cast<uint32_t>(info->height) ||
                            !DNGDecoder::getColorMetadata(dng, frame.colorMetadata))
                            throw std::runtime_error("Could not extract RGB16 from rendered frame");
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
            if (!label.empty()) progress.setLabelText(QString::fromStdString(label));
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
        const bool convertToCfr =
            stagingSettings.options & motioncam::RENDER_OPT_FRAMERATE_CONVERSION;
        QStringList args{"-hide_banner", "-y", "-f", "nut", "-i", "pipe:0"};
        const QString audioPath = stageDir.absoluteFilePath("audio.wav");
        if (QFile::exists(audioPath))
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
            ? log60Lut + ",format=gbrp12le"
            : log60Lut +
                ",zscale=matrixin=gbr:matrix=2020_ncl:rangein=full:range=full:dither=none,format=" +
                pixelFormat;
        args << "-map" << "0:v:0";
        if (QFile::exists(audioPath))
            args << "-map" << "1:a:0" << "-c:a" << "copy";
        args << "-vf" << videoFilter;
        if (av1) {
            // SVT 4 defaults to parallelism level 6 (305 PPCS at 4K), which
            // can exhaust a desktop process's memory during encoder startup.
            // Level 4 keeps preset/quality unchanged while bounding its frame
            // pipeline (148 PPCS in SVT-AV1 4.1).
            if (av1Encoder == "libsvtav1") {
                QString svtParams = "lp=4:keyint=10s:tune=0:enable-overlays=1:scd=1:scm=0";
                if (hdrNoise)
                    svtParams = "lp=4:keyint=10s:tune=5:noise=8:enable-overlays=1:scd=1:scm=0";
                args << "-c:v" << av1Encoder << "-crf" << QString::number(av1Crf)
                     << "-preset" << QString::number(av1Preset)
                     << "-svtav1-params" << svtParams;
            } else {
                const QString av1Profile = chroma == "422" ? "2" : "1";
                args << "-c:v" << "libaom-av1" << "-crf" << QString::number(av1Crf)
                     << "-b:v" << "0" << "-cpu-used" << QString::number(av1Preset)
                     << "-profile:v" << av1Profile << "-row-mt" << "1";
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
                 << "-x265-params" << "range=full:colorprim=bt2020:colormatrix=bt2020nc";
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
        progress.setLabelText("Encoding LOG60 Camera Native frame 1...");
        progress.show();
        QApplication::processEvents();
        QProcess encoder;
        encoder.setProcessChannelMode(QProcess::MergedChannels);
        encoder.start(ffmpeg, args, QIODevice::ReadWrite);
        if (!encoder.waitForStarted())
            throw std::runtime_error("Could not start FFmpeg: " + encoder.errorString().toStdString());
        QByteArray diagnostic;
        // SVT-AV1 needs a sane nominal rate, but FFmpeg must not use the VFR
        // average as a synchronization target. The timestamps remain the
        // authority in passthrough mode.
        const double encoderNominalFps = convertToCfr
            ? info->fps : info->frameRateInfo.medianFrameRate;
        NutVideoPipe videoPipe(encoder, diagnostic, info->width, info->height, encoderNominalFps);
        DNGFrameMetadata colorMetadata;
        nlohmann::json dynamicFrames = nlohmann::json::array();
        nlohmann::json gainMapFormats = nlohmann::json::array();
        nlohmann::json gainMapPayloads = nlohmann::json::array();
        std::unordered_map<std::string, size_t> gainMapFormatIds;
        std::unordered_map<std::string, size_t> gainMapPayloadIds;
        auto appendDynamicMetadata = [&](const NativeFrame& frame) {
            const auto& metadata = frame.colorMetadata;
            nlohmann::json item;
            item["timestampNs"] = frame.timestamp;
            item["iso"] = metadata.iso;
            item["shutterSpeedSeconds"] = metadata.exposureTime;
            item["baselineExposure"] = metadata.baselineExposure;
            item["asShotNeutral"] = metadata.asShotNeutral;
            item["duplicateFrame"] = frame.duplicateFrame;
            item["syntheticFrame"] = frame.syntheticFrame;

            const uint32_t channels = std::max<uint32_t>(1, metadata.blackLevelCount);
            item["blackLevel"] = nlohmann::json::array();
            item["whiteLevel"] = nlohmann::json::array();
            for (uint32_t channel = 0; channel < channels; ++channel) {
                const uint32_t black = metadata.blackLevelCount
                    ? std::min(channel, metadata.blackLevelCount - 1) : 0;
                item["blackLevel"].push_back(static_cast<double>(metadata.blackLevel[black]));
                const uint32_t white = metadata.whiteLevelCount
                    ? std::min(channel, metadata.whiteLevelCount - 1) : 0;
                item["whiteLevel"].push_back(static_cast<double>(metadata.whiteLevel[white]));
            }
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
                        {"coordinateWidth", map.right - map.left == static_cast<uint32_t>(info->width)
                            ? map.left + map.right : std::max(map.right, static_cast<uint32_t>(info->width))},
                        {"coordinateHeight", map.bottom - map.top == static_cast<uint32_t>(info->height)
                            ? map.top + map.bottom : std::max(map.bottom, static_cast<uint32_t>(info->height))},
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
                    videoPipe.writeFrame(pendingFrame->rgb, pendingFrame->timestamp,
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
                ? QString("Rendering and encoding LOG60 Camera Native frame %1 of %2...")
                    .arg(frameIndex + 1).arg(frameCount)
                : QString::fromStdString(label));
            QApplication::processEvents();
        }
        if (!pendingFrame) throw std::runtime_error("The rendered sequence contains no video frames");
        videoPipe.writeFrame(pendingFrame->rgb, pendingFrame->timestamp,
            std::max<int64_t>(1, static_cast<int64_t>(std::llround(1e9 / info->fps))));
        appendDynamicMetadata(*pendingFrame);
        ++frameIndex;
        if (frameIndex != frameCount)
            throw std::runtime_error("Rendered frame count did not match the clip metadata");
        videoPipe.finish();
        encoder.closeWriteChannel();
        progress.setLabelText(QString("Finishing LOG60 Camera Native %1...")
            .arg(av1 ? "MP4" : "MOV"));
        while (encoder.state() != QProcess::NotRunning) {
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
        if (encoder.exitStatus() != QProcess::NormalExit || encoder.exitCode() != 0) {
            diagnostic += encoder.readAll();
            throw std::runtime_error(("FFmpeg failed:\n" +
                QString::fromUtf8(diagnostic.right(4000))).toStdString());
        }

        nlohmann::json sidecar;
        sidecar["transferFunction"] = "LOG60";
        sidecar["dataLevels"] = "Full";
        sidecar["videoCodec"] = av1 ? "AV1" : proRes ? "ProRes" : cineForm ? "CineForm" : "HEVC";
        sidecar["encoder"] = av1 ? av1Encoder.toStdString()
            : proRes ? "prores_ks" : cineForm ? "cfhd" : "libx265";
        sidecar["pixelFormat"] = pixelFormat.toStdString();
        if (hdrNoise) sidecar["noiseSynthesis"] = 8;
        if (colorMetadata.hasColorMatrix1) sidecar["colorMatrix1"] = colorMetadata.colorMatrix1;
        if (colorMetadata.hasColorMatrix2) sidecar["colorMatrix2"] = colorMetadata.colorMatrix2;
        if (colorMetadata.hasForwardMatrix1) sidecar["forwardMatrix1"] = colorMetadata.forwardMatrix1;
        if (colorMetadata.hasForwardMatrix2) sidecar["forwardMatrix2"] = colorMetadata.forwardMatrix2;
        if (colorMetadata.hasAsShotNeutral) sidecar["asShotNeutral"] = colorMetadata.asShotNeutral;
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

        // Commit the pair together. Existing outputs are retained until both
        // temporary files are complete and are restored if either rename fails.
        const QString backupId = ".camera-native-backup-" + QUuid::createUuid().toString(QUuid::WithoutBraces);
        const QString mediaBackup = outputPath + backupId;
        const QString jsonBackup = jsonPath + backupId;
        const bool hadMedia = QFile::exists(outputPath);
        const bool hadJson = QFile::exists(jsonPath);
        if ((hadMedia && !QFile::rename(outputPath, mediaBackup)) ||
            (hadJson && !QFile::rename(jsonPath, jsonBackup))) {
            if (QFile::exists(mediaBackup)) QFile::rename(mediaBackup, outputPath);
            throw std::runtime_error("Could not preserve the existing Camera Native output");
        }
        const bool mediaCommitted = QFile::rename(partialPath, outputPath);
        const bool jsonCommitted = mediaCommitted && QFile::rename(partialJsonPath, jsonPath);
        if (!jsonCommitted) {
            if (mediaCommitted) QFile::remove(outputPath);
            if (QFile::exists(mediaBackup)) QFile::rename(mediaBackup, outputPath);
            if (QFile::exists(jsonBackup)) QFile::rename(jsonBackup, jsonPath);
            throw std::runtime_error("Could not commit the completed Camera Native video and JSON");
        }
        QFile::remove(mediaBackup);
        QFile::remove(jsonBackup);

        progress.close();
        QMessageBox::information(this, "Camera Native finalization",
            QString("Created:\n%1\n%2").arg(outputPath, jsonPath));
    } catch (const std::exception& e) {
        progress.close();
        if (std::string(e.what()) != "Finalization cancelled")
            QMessageBox::critical(this, "Camera Native finalization", QString::fromStdString(e.what()));
    }

    // Restore the mounted view; Camera Native settings affect finalization only.
    mFuseFilesystem->updateOptions(mountId, buildRenderSettings());
}

void MainWindow::finalizeFile(QWidget* fileWidget) {
    const QString compressionMode = ui->dngCompressionModeComboBox->currentText();
    if (ui->dngCompressionCheckBox->isChecked()) {
        if (isCameraNativeFormat(compressionMode)) {
            finalizeCameraNative(fileWidget, compressionMode);
            return;
        }
        const QString mode = compressionMode.trimmed();
        if (mode.compare("JPEG 92 Lossless", Qt::CaseInsensitive) == 0) {
            mRenderSettings.jxlDistance = -1.0f;
        } else if (mode.compare("JPEG DCT 12b", Qt::CaseInsensitive) == 0 ||
                   mode.compare("CinemaDNG 12-bit Lossy", Qt::CaseInsensitive) == 0 ||
                   mode.compare("JPEG DCT Lossy", Qt::CaseInsensitive) == 0) {
            mRenderSettings.jxlDistance = motioncam::DNG_COMPRESSION_JPEG_DCT;
        } else if (mode.compare("JPEG XL Lossless", Qt::CaseInsensitive) == 0) {
            mRenderSettings.jxlDistance = 0.0f;
        } else {
            float distance = 0.0f;
            if (!parseJxlDctDistance(mode, distance)) {
                QMessageBox::warning(this, tr("Invalid finalize format"),
                    tr("Enter a complete JPEG XL format such as ‘JPEG XL DCT 0.3’."));
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

    // Use a temporary directory to avoid ProjectedFS locks
    // We'll write to temp, then move files to the final location
    QString tempPath = mountPath + ".finalizing-" +
        QUuid::createUuid().toString(QUuid::WithoutBraces);
    const QString retainedOutputPath = mountPath + "-finalized";
    if (!mUnmountOnFinalize && QFileInfo::exists(retainedOutputPath) &&
        QMessageBox::question(this, "Replace finalized output?",
            QString("Replace the existing finalized output at:\n%1").arg(retainedOutputPath),
            QMessageBox::Yes | QMessageBox::No, QMessageBox::No) != QMessageBox::Yes) {
        return;
    }
    QDir tempDir(tempPath);

    // Create temp directory
    if (!tempDir.mkpath(".")) {
        spdlog::error("Failed to create temp directory: {}", tempPath.toStdString());
        QMessageBox::critical(this, "Finalize failed", "Could not create the temporary render directory.");
        return;
    }

    spdlog::info("Using temp directory: {}", tempPath.toStdString());

    const bool detectDuplicateDngs = ui->detectDuplicateDngsCheckBox->isChecked();
    const bool interpolateFrames = ui->rifeInterpolationCheckBox->isChecked() &&
        (fileInfo->duplicatedFrames > 0 || detectDuplicateDngs);
    const auto rifeRuntime = interpolateFrames ? ensureRifeRuntime() : std::optional<QString>{QString{}};
    if (!rifeRuntime) return;

    // Create progress dialog
    QProgressDialog progress("Rendering DNG sequence...", "Cancel", 0, totalFrames, this);
    progress.setWindowModality(Qt::WindowModal);
    progress.setMinimumDuration(0);
    progress.setAutoClose(false);
    progress.setAutoReset(false);
    progress.setValue(0);

    // Get current render config
    auto settings = buildRenderSettings();
    bool enableCompression = settings.options & motioncam::RENDER_OPT_JPEG_COMPRESSION;
    motioncam::FinalizeOptions finalizeOptions;
    finalizeOptions.interpolateDuplicatedFrames = interpolateFrames;
    finalizeOptions.detectDuplicateDngs = detectDuplicateDngs;
    finalizeOptions.rifeDirectory = rifeRuntime->toStdString();
    finalizeOptions.jxlDistance = settings.jxlDistance;

    spdlog::info("Starting finalize for {} ({} frames from fileInfo, compression: {})",
                 srcFile.toStdString(), totalFrames, enableCompression ? "enabled" : "disabled");
    spdlog::info("Compression checkbox state: {}", enableCompression);


    // Render all frames directly to disk (bypassing ProjectedFS)
    // This allows compression to work correctly with variable file sizes
    bool mountReleased = false;
    try {
        mFuseFilesystem->finalize(
            mountId,
            tempPath.toStdString(),
            enableCompression,
            finalizeOptions,
            [&](size_t completed, size_t count, const std::string& name) {
                progress.setMaximum(static_cast<int>(count));
                progress.setValue(static_cast<int>(completed));
                if (!name.empty()) {
                    progress.setLabelText(
                        QString("Finalizing—step %1 of %2: %3")
                            .arg(std::min(completed + 1, count)).arg(count)
                            .arg(QString::fromStdString(name)));
                }
                QApplication::processEvents();
                return !progress.wasCanceled();
            });

        if (!progress.wasCanceled()) {
            spdlog::info("Rendered {} frames to temp directory", totalFrames);

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
            mountReleased = true;
#ifdef _WIN32
            QThread::msleep(150);
#elif __APPLE__
            if (!waitForMacFuseUnmount(mountPath, 10000)) {
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
        } else {
            spdlog::info("Finalize cancelled by user");
            tempDir.removeRecursively();
            return;
        }

    } catch (const std::exception& e) {
        spdlog::error("Finalize failed: {}", e.what());
        if (!mountReleased) {
            tempDir.removeRecursively();
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
        QMessageBox::critical(
            this,
            "Finalize failed",
            message);
        return;
    }

    // Remove from UI
    auto* scrollContent = ui->dragAndDropScrollArea->widget();
    auto* scrollLayout = qobject_cast<QVBoxLayout*>(scrollContent->layout());

    int fileWidgetIndex = scrollLayout->indexOf(fileWidget);
    if (fileWidgetIndex > 0) {
        auto* itemAbove = scrollLayout->itemAt(fileWidgetIndex - 1);
        if (itemAbove && itemAbove->widget()) {
            auto* widgetAbove = itemAbove->widget();
            auto* frame = qobject_cast<QFrame*>(widgetAbove);
            if (frame && frame->frameShape() == QFrame::HLine) {
                scrollLayout->removeWidget(frame);
                frame->deleteLater();
            }
        }
    }

    scrollLayout->removeWidget(fileWidget);
    fileWidget->deleteLater();

    // Remove from mounted files list
    auto it = std::find_if(
        mMountedFiles.begin(), mMountedFiles.end(),
        [mountId](const motioncam::MountedFile& f) { return f.mountId == mountId; });
    if(it != mMountedFiles.end())
        mMountedFiles.erase(it);

    if (mMountedFiles.empty()) {
        ui->dragAndDropLabel->show();
    }
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
        ui->quadBayerComboBox->setEnabled(false);
    } else {
        ui->draftQuality->setCurrentIndex(-1);
        mRenderSettings.draftScale = 1;
        ui->draftQuality->setEnabled(false);
        ui->higherCfaHqCheckBox->setEnabled(false);
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
        if (ui->logTransformComboBox->currentText() == "")
            ui->logTransformComboBox->setCurrentText("Keep Input");
    } else {
        ui->logTransformComboBox->setCurrentText("");
        ui->logTransformComboBox->setEnabled(false);
    }

    // Pixel normalization is bake-only; reduce-to-color can also transform a
    // deferred OpcodeList2 gain map.
    if(ui->vignetteCorrectionCheckBox->checkState() == Qt::CheckState::Checked) {
        ui->scaleRawCheckBox->setEnabled(true);
        if(ui->scaleRawCheckBox->checkState() == Qt::CheckState::Checked) {
            ui->debugVignetteCheckBox->setEnabled(false);
            ui->debugVignetteCheckBox->setChecked(false);
        } else {
            ui->debugVignetteCheckBox->setEnabled(true);
        }
    } else {
        ui->scaleRawCheckBox->setEnabled(false);
        ui->scaleRawCheckBox->setChecked(false);
        ui->debugVignetteCheckBox->setEnabled(false);
        ui->debugVignetteCheckBox->setChecked(false);
    }
    ui->vignetteOnlyColorCheckBox->setEnabled(true);

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
            // Format runtime as MM:SS
            int minutes = static_cast<int>(info.runtimeSeconds) / 60;
            int seconds = static_cast<int>(info.runtimeSeconds) % 60;
            QString runtimeStr = QString("%1:%2").arg(minutes).arg(seconds, 2, 10, QChar('0'));

            // Extract bit depth and log indicator from levelsInfo for styling
            QString levelsStr = QString::fromStdString(info.levelsInfo);
            QString rawPart;
            int rawIdx = levelsStr.lastIndexOf('b');
            while (rawIdx > 0 && levelsStr.at(rawIdx - 1).isDigit())
                --rawIdx;
            if (rawIdx >= 0) {
                rawPart = levelsStr.mid(rawIdx);
            }

            auto infoText1 = QString("<span style='color: #888888;'>Runtime: </span><span style='color: white;'>%1</span>"
                                     "<span style='color: #888888;'> | Resolution: %2x%3 | Data Type: %4 | Levels: %5</span>")
                                    .arg(runtimeStr)
                                    .arg(info.width)
                                    .arg(info.height)
                                    .arg(QString::fromStdString(info.dataType))
                                    .arg(levelsStr.left(rawIdx));

            if (!rawPart.isEmpty()) {
                infoText1 += QString("<span style='color: white;'>%1</span>").arg(rawPart);
            }

            label->setText(infoText1);
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
                                 info->fps, info->timingUsesCfrMapping);
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
    if (mSelectedMountIds.isEmpty()) {
        mRenderSettings = mGlobalRenderSettings;
        for (const auto& file : mMountedFiles) {
            if (mLocalSettings.contains(file.mountId)) continue;
            mFuseFilesystem->updateOptions(file.mountId, mGlobalRenderSettings);
        }
        updateFpsLabels();
        clearApplyFeedback();
        autoSaveSession();
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
    for (int i = layout->count() - 1; i >= 0; --i) {
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
    const QSignalBlocker b1(ui->draftModeCheckBox), b2(ui->vignetteCorrectionCheckBox),
        b3(ui->vignetteOnlyColorCheckBox), b4(ui->optimizeGainMapsCheckBox),
        b5(ui->scaleRawCheckBox), b6(ui->debugVignetteCheckBox),
        b7(ui->normalizeExposureCheckBox), b8(ui->smoothExposureCheckBox),
        b9(ui->smoothWhiteBalanceCheckBox), b10(ui->bakeIsoCheckBox),
        b11(ui->cfrConversionCheckBox), b12(ui->cropEnableCheckBox),
        b13(ui->camModelOverrideCheckBox), b14(ui->logTransformCheckBox);
    const QSignalBlocker b15(ui->cfrTarget), b16(ui->cropTargetComboBox),
        b17(ui->camModelOverrideComboBox), b18(ui->levelsComboBox),
        b19(ui->exposureCompensationLineEdit), b20(ui->logTransformComboBox),
        b21(ui->quadBayerComboBox), b22(ui->cfaPhaseComboBox),
        b23(ui->draftQuality), b24(ui->remosaicCheckBox),
        b25(ui->higherCfaHqCheckBox), b26(ui->dngCompressionCheckBox),
        b27(ui->dngCompressionModeComboBox), b28(ui->badPixelTreatmentComboBox);
    for (auto* box : {ui->draftModeCheckBox, ui->vignetteCorrectionCheckBox,
                      ui->vignetteOnlyColorCheckBox, ui->optimizeGainMapsCheckBox,
                      ui->scaleRawCheckBox, ui->debugVignetteCheckBox,
                      ui->normalizeExposureCheckBox, ui->smoothExposureCheckBox,
                      ui->smoothWhiteBalanceCheckBox, ui->bakeIsoCheckBox,
                      ui->cfrConversionCheckBox, ui->cropEnableCheckBox,
                      ui->camModelOverrideCheckBox, ui->logTransformCheckBox,
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
    ui->vignetteCorrectionCheckBox->setChecked(checked(motioncam::RENDER_OPT_APPLY_VIGNETTE_CORRECTION));
    ui->vignetteOnlyColorCheckBox->setChecked(checked(motioncam::RENDER_OPT_VIGNETTE_ONLY_COLOR));
    ui->optimizeGainMapsCheckBox->setChecked(checked(motioncam::RENDER_OPT_OPTIMIZE_GAIN_MAPS));
    ui->scaleRawCheckBox->setChecked(checked(motioncam::RENDER_OPT_NORMALIZE_SHADING_MAP));
    ui->debugVignetteCheckBox->setChecked(checked(motioncam::RENDER_OPT_DEBUG_SHADING_MAP));
    ui->normalizeExposureCheckBox->setChecked(checked(motioncam::RENDER_OPT_NORMALIZE_EXPOSURE));
    ui->smoothExposureCheckBox->setChecked(checked(motioncam::RENDER_OPT_SMOOTH_EXPOSURE));
    ui->smoothWhiteBalanceCheckBox->setChecked(checked(motioncam::RENDER_OPT_SMOOTH_WHITE_BALANCE));
    ui->bakeIsoCheckBox->setChecked(checked(motioncam::RENDER_OPT_BAKE_ISO));
    ui->cfrConversionCheckBox->setChecked(checked(motioncam::RENDER_OPT_FRAMERATE_CONVERSION));
    ui->cropEnableCheckBox->setChecked(checked(motioncam::RENDER_OPT_CROPPING));
    ui->camModelOverrideCheckBox->setChecked(checked(motioncam::RENDER_OPT_CAMMODEL_OVERRIDE));
    ui->logTransformCheckBox->setChecked(checked(motioncam::RENDER_OPT_LOG_TRANSFORM));
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
    if (!isCameraNativeFormat(ui->dngCompressionModeComboBox->currentText())) {
        if (motioncam::isLossyJpegDct(settings.jxlDistance))
            ui->dngCompressionModeComboBox->setCurrentIndex(1);
        else if (settings.jxlDistance < 0.0f)
            ui->dngCompressionModeComboBox->setCurrentIndex(0);
        else if (settings.jxlDistance == 0.0f)
            ui->dngCompressionModeComboBox->setCurrentIndex(2);
        else if (std::abs(settings.jxlDistance - 0.3f) <= 0.0001f)
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
        markCheck(ui->vignetteCorrectionCheckBox, mixedFlag(motioncam::RENDER_OPT_APPLY_VIGNETTE_CORRECTION));
        markCheck(ui->vignetteOnlyColorCheckBox, mixedFlag(motioncam::RENDER_OPT_VIGNETTE_ONLY_COLOR));
        markCheck(ui->optimizeGainMapsCheckBox, mixedFlag(motioncam::RENDER_OPT_OPTIMIZE_GAIN_MAPS));
        markCheck(ui->scaleRawCheckBox, mixedFlag(motioncam::RENDER_OPT_NORMALIZE_SHADING_MAP));
        markCheck(ui->debugVignetteCheckBox, mixedFlag(motioncam::RENDER_OPT_DEBUG_SHADING_MAP));
        markCheck(ui->normalizeExposureCheckBox, mixedFlag(motioncam::RENDER_OPT_NORMALIZE_EXPOSURE));
        markCheck(ui->smoothExposureCheckBox, mixedFlag(motioncam::RENDER_OPT_SMOOTH_EXPOSURE));
        markCheck(ui->smoothWhiteBalanceCheckBox, mixedFlag(motioncam::RENDER_OPT_SMOOTH_WHITE_BALANCE));
        markCheck(ui->bakeIsoCheckBox, mixedFlag(motioncam::RENDER_OPT_BAKE_ISO));
        markCheck(ui->cfrConversionCheckBox, mixedFlag(motioncam::RENDER_OPT_FRAMERATE_CONVERSION));
        markCheck(ui->cropEnableCheckBox, mixedFlag(motioncam::RENDER_OPT_CROPPING));
        markCheck(ui->camModelOverrideCheckBox, mixedFlag(motioncam::RENDER_OPT_CAMMODEL_OVERRIDE));
        markCheck(ui->logTransformCheckBox, mixedFlag(motioncam::RENDER_OPT_LOG_TRANSFORM));
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
        markValue(ui->cfaPhaseComboBox, [](const auto& s) { return s.cfaPhase; });
    }
    updateUi();
}

void MainWindow::onApplySelected() {
    if (mSelectedMountIds.isEmpty()) return;
    const auto edited = buildRenderSettings();
    for (auto id : mSelectedMountIds) {
        auto settings = edited;
        const auto previous = mLocalSettings.value(id, mGlobalRenderSettings);
        auto preserveMixedFlag = [&](QCheckBox* box, motioncam::FileRenderOptions option) {
            if (box->checkState() != Qt::PartiallyChecked) return;
            if (previous.options & option) settings.options |= option;
            else settings.options = static_cast<motioncam::FileRenderOptions>(settings.options & ~option);
        };
        preserveMixedFlag(ui->draftModeCheckBox, motioncam::RENDER_OPT_DRAFT);
        preserveMixedFlag(ui->vignetteCorrectionCheckBox, motioncam::RENDER_OPT_APPLY_VIGNETTE_CORRECTION);
        preserveMixedFlag(ui->vignetteOnlyColorCheckBox, motioncam::RENDER_OPT_VIGNETTE_ONLY_COLOR);
        preserveMixedFlag(ui->optimizeGainMapsCheckBox, motioncam::RENDER_OPT_OPTIMIZE_GAIN_MAPS);
        preserveMixedFlag(ui->scaleRawCheckBox, motioncam::RENDER_OPT_NORMALIZE_SHADING_MAP);
        preserveMixedFlag(ui->debugVignetteCheckBox, motioncam::RENDER_OPT_DEBUG_SHADING_MAP);
        preserveMixedFlag(ui->normalizeExposureCheckBox, motioncam::RENDER_OPT_NORMALIZE_EXPOSURE);
        preserveMixedFlag(ui->smoothExposureCheckBox, motioncam::RENDER_OPT_SMOOTH_EXPOSURE);
        preserveMixedFlag(ui->smoothWhiteBalanceCheckBox, motioncam::RENDER_OPT_SMOOTH_WHITE_BALANCE);
        preserveMixedFlag(ui->bakeIsoCheckBox, motioncam::RENDER_OPT_BAKE_ISO);
        preserveMixedFlag(ui->cfrConversionCheckBox, motioncam::RENDER_OPT_FRAMERATE_CONVERSION);
        preserveMixedFlag(ui->cropEnableCheckBox, motioncam::RENDER_OPT_CROPPING);
        preserveMixedFlag(ui->camModelOverrideCheckBox, motioncam::RENDER_OPT_CAMMODEL_OVERRIDE);
        preserveMixedFlag(ui->logTransformCheckBox, motioncam::RENDER_OPT_LOG_TRANSFORM);
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
        if (ui->cfaPhaseComboBox->property("localOverride").toBool()) settings.cfaPhase = previous.cfaPhase;
        mLocalSettings.insert(id, settings);
        mFuseFilesystem->updateOptions(id, settings);
        updateLocalBadge(id);
    }
    updateFpsLabels();
    clearApplyFeedback();
    autoSaveSession();
}

void MainWindow::onApplyAll() {
    if (mMountedFiles.isEmpty()) return;
    mGlobalRenderSettings = buildRenderSettings();
    mRenderSettings = mGlobalRenderSettings;
    mLocalSettings.clear();
    for (const auto& file : mMountedFiles) {
        mFuseFilesystem->updateOptions(file.mountId, mGlobalRenderSettings);
        updateLocalBadge(file.mountId);
    }
    updateFpsLabels();
    clearApplyFeedback();
    autoSaveSession();
}

void MainWindow::updateThumbnail(motioncam::MountId mountId) {
    auto* card = fileWidgetForMount(mountId);
    auto* label = card ? card->findChild<QLabel*>("thumbnailLabel") : nullptr;
    if (!label) return;
    const QString path = QDir::temp().filePath(QStringLiteral("motioncam-fuse-%1.jpg").arg(mountId));
    label->setText(tr("Generating..."));
    QPointer<QLabel> guardedLabel(label);
    auto render = [this, mountId, path, guardedLabel] {
        const bool generated = mFuseFilesystem->generateThumbnail(
            mountId, path.toStdString(), 352, 224);
        QMetaObject::invokeMethod(this, [path, guardedLabel, generated] {
            if (!guardedLabel) return;
            const QPixmap image(path);
            if (generated && !image.isNull()) {
                const QPixmap preview = image.scaled(
                    QSize(176, 112), Qt::KeepAspectRatio, Qt::SmoothTransformation);
                guardedLabel->setFixedSize(preview.size());
                guardedLabel->setPixmap(preview);
            } else {
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
    SettingsDialog dialog(this);
    dialog.setCacheFolder(mCacheRootFolder);
    dialog.setPlayerPath(mPlayerPath);
    dialog.setAutoApplyClipSettings(mAutoApplyClipSettings);
    dialog.setUnmountOnFinalize(mUnmountOnFinalize);
#ifdef _WIN32
    dialog.setDeleteOnUnmount(mDeleteOnUnmount);
    dialog.setCachePolicyMode(mCachePolicy == motioncam::CachePolicy::Quota ? "quota" : "off");
    dialog.setCacheQuotaGb(static_cast<double>(mCacheQuotaBytes) / (1024.0 * 1024.0 * 1024.0));
    dialog.setCacheCleanupIntervalSeconds(mCacheCleanupIntervalSeconds);
#endif

    if (dialog.exec() != QDialog::Accepted)
        return;

    const bool wasAutoApply = mAutoApplyClipSettings;
    mCacheRootFolder = dialog.getCacheFolder();
    mPlayerPath = dialog.getPlayerPath();
    mAutoApplyClipSettings = dialog.getAutoApplyClipSettings();
    mUnmountOnFinalize = dialog.getUnmountOnFinalize();
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

void MainWindow::saveSessionToFile(const QString& path) {
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
        object["cfaPhase"] = QString::fromStdString(settings.cfaPhase);
        object["jxlDistance"] = settings.jxlDistance;
        return object;
    };
    QJsonObject root;
    root["version"] = 2;
    root["globalSettings"] = encode(mGlobalRenderSettings);
    root["cacheFolder"] = mCacheRootFolder;
    QJsonArray clips;
    for (const auto& file : mMountedFiles) {
        QJsonObject clip;
        clip["path"] = file.srcFile;
        if (mLocalSettings.contains(file.mountId))
            clip["localSettings"] = encode(mLocalSettings.value(file.mountId));
        clips.append(clip);
    }
    root["clips"] = clips;
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly) ||
        file.write(QJsonDocument(root).toJson()) < 0 || !file.commit()) {
        QMessageBox::warning(this, tr("Save Session"), tr("Could not save %1").arg(path));
        return;
    }
    if (QFileInfo(path).absoluteFilePath() != QFileInfo(autoSessionPath()).absoluteFilePath()) {
        mCurrentSessionFile = path;
        addRecentSession(path);
    }
}

void MainWindow::clearSession() {
    while (!mMountedFiles.isEmpty()) {
        auto* card = fileWidgetForMount(mMountedFiles.front().mountId);
        if (card) removeFile(card);
        else {
            mFuseFilesystem->unmount(mMountedFiles.front().mountId);
            mMountedFiles.removeFirst();
        }
    }
    mLocalSettings.clear();
    mSelectedMountIds.clear();
    updateSelectionUi();
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
        settings.cfaPhase = object["cfaPhase"].toString("Don't override CFA").toStdString();
        settings.jxlDistance = static_cast<float>(object["jxlDistance"].toDouble(-1.0));
        return settings;
    };
    const auto root = document.object();
    const auto clips = root["clips"].toArray();
    clearSession();
    mCacheRootFolder = root["cacheFolder"].toString();
    mGlobalRenderSettings = decode(root["globalSettings"].toObject());
    mRenderSettings = mGlobalRenderSettings;
    updateSelectionUi();
    for (const auto& value : clips) {
        const auto clip = value.toObject();
        const QString clipPath = clip["path"].toString();
        if (!QFileInfo::exists(clipPath)) continue;
        mountFile(clipPath);
        if (clip.contains("localSettings") && !mMountedFiles.isEmpty()) {
            const auto id = mMountedFiles.back().mountId;
            const auto local = decode(clip["localSettings"].toObject());
            mLocalSettings.insert(id, local);
            mFuseFilesystem->updateOptions(id, local);
            updateLocalBadge(id);
        }
    }
    if (QFileInfo(path).absoluteFilePath() == QFileInfo(autoSessionPath()).absoluteFilePath())
        mCurrentSessionFile.clear();
    else {
        mCurrentSessionFile = path;
        addRecentSession(path);
    }
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
    if (mMountedFiles.isEmpty()) {
        QFile::remove(autoSessionPath());
        return;
    }
    const QString current = mCurrentSessionFile;
    saveSessionToFile(autoSessionPath());
    mCurrentSessionFile = current;
}

void MainWindow::promptToResumeSession() {
    const QString path = autoSessionPath();
    if (!QFileInfo::exists(path)) return;
    QMessageBox prompt(this);
    prompt.setIcon(QMessageBox::Question);
    prompt.setWindowTitle(tr("Resume Session"));
    prompt.setText(tr("Resume the last session or start a new one?"));
    auto* resume = prompt.addButton(tr("Resume"), QMessageBox::AcceptRole);
    prompt.addButton(tr("New Session"), QMessageBox::RejectRole);
    prompt.setDefaultButton(resume);
    prompt.exec();
    if (prompt.clickedButton() == resume) loadSessionFromFile(path);
    else onNewSession();
}

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
    const QString normalized = QFileInfo(path).absoluteFilePath();
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
        auto* action = mRecentSessionsMenu->addAction(QFileInfo(path).completeBaseName());
        action->setToolTip(path);
        connect(action, &QAction::triggered, this, [this, path] {
            if (QFileInfo::exists(path)) loadSessionFromFile(path);
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
    clearSession();
    mCurrentSessionFile.clear();
    QFile::remove(autoSessionPath());
    onSetDefaultSettings(false);
    mGlobalRenderSettings = buildRenderSettings();
    mRenderSettings = mGlobalRenderSettings;
    clearApplyFeedback();
}
void MainWindow::onLoadSession() {
    const QString path = QFileDialog::getOpenFileName(this, tr("Load Session"), sessionDirectory(), tr("MotionCam Session (*.json)"));
    if (!path.isEmpty()) loadSessionFromFile(path);
}
void MainWindow::onSaveSession() {
    if (mCurrentSessionFile.isEmpty()) onSaveSessionAs();
    else saveSessionToFile(mCurrentSessionFile);
}
void MainWindow::onSaveSessionAs() {
    QString path = QFileDialog::getSaveFileName(this, tr("Save Session"), sessionDirectory(), tr("MotionCam Session (*.json)"));
    if (!path.isEmpty()) {
        if (!path.endsWith(".json", Qt::CaseInsensitive)) path += ".json";
        saveSessionToFile(path);
    }
}

void MainWindow::onSetDefaultSettings(bool checked) {
    ui->draftModeCheckBox->setCheckState(Qt::CheckState::Unchecked);
    ui->vignetteCorrectionCheckBox->setCheckState(Qt::CheckState::Checked);
    ui->scaleRawCheckBox->setCheckState(Qt::CheckState::Unchecked);
    ui->debugVignetteCheckBox->setCheckState(Qt::CheckState::Unchecked);
    ui->vignetteOnlyColorCheckBox->setCheckState(Qt::CheckState::Checked);
    ui->optimizeGainMapsCheckBox->setCheckState(Qt::CheckState::Unchecked);
    ui->normalizeExposureCheckBox->setCheckState(Qt::CheckState::Checked);
    ui->smoothExposureCheckBox->setCheckState(Qt::CheckState::Unchecked);
    ui->smoothWhiteBalanceCheckBox->setCheckState(Qt::CheckState::Unchecked);
    ui->bakeIsoCheckBox->setCheckState(Qt::CheckState::Unchecked);
    ui->cfrConversionCheckBox->setCheckState(Qt::CheckState::Checked);
    ui->detectDuplicateDngsCheckBox->setChecked(true);
    ui->cropEnableCheckBox->setCheckState(Qt::CheckState::Unchecked);
    ui->camModelOverrideCheckBox->setCheckState(Qt::CheckState::Checked);
    ui->logTransformCheckBox->setCheckState(Qt::CheckState::Checked);
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
    QString jsonPath = fileInfo.isDir()
        ? fileInfo.absoluteFilePath() + "/" + fileInfo.fileName() + ".json"
        : fileInfo.absolutePath() + "/" + fileInfo.completeBaseName() + ".json";

    // When exactly one clip with a valid sidecar is selected, the calibration
    // buttons on the other clips copy that sidecar instead of creating a new
    // one from the global template.
    for (auto* candidate : fileWidget->findChildren<QPushButton*>()) {
        if (!candidate->property("calibButton").toBool()) continue;
        const QString sourcePath = candidate->property("copyJsonSourcePath").toString();
        if (!sourcePath.isEmpty()) {
            if (!QFile::copy(sourcePath, jsonPath)) {
                QMessageBox::critical(this, tr("Error"),
                    tr("Failed to copy calibration file:\n%1\n\nto:\n%2")
                        .arg(sourcePath, jsonPath));
                return;
            }
            updateCalibrationButtonStates();
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

    // Write JSON file
    std::ofstream outFile(jsonPath.toStdString());
    if (!outFile.is_open()) {
        QMessageBox::critical(this, "Error", QString("Failed to create calibration file:\n%1").arg(jsonPath));
        return;
    }

    outFile << jsonContent;
    outFile.close();

    // Update button states
    updateCalibrationButtonStates();
    // Reload the newly-created sidecar for any already-mounted clip.
    reloadCalibration(fileWidget);
}

void MainWindow::reloadCalibration(QWidget* fileWidget) {
    bool ok = false;
    const auto mountId = fileWidget->property("mountId").toInt(&ok);
    if (!ok) return;

    // updateOptions also reloads the sidecar. Preserve whether this clip uses
    // global or local render settings; calibration file operations must not
    // create a local settings override for the currently selected clip.
    mFuseFilesystem->updateOptions(
        mountId, mLocalSettings.value(mountId, mGlobalRenderSettings));
    updateFpsLabels();
}

void MainWindow::updateCalibrationButtonStates() {
    auto* scrollContent = ui->dragAndDropScrollArea->widget();
    if (!scrollContent) return;

    auto fileWidgets = scrollContent->findChildren<QWidget*>(QString(), Qt::FindDirectChildrenOnly);

    QString selectedJsonPath;
    QWidget* selectedFileWidget = nullptr;
    if (mSelectedMountIds.size() == 1) {
        selectedFileWidget = fileWidgetForMount(*mSelectedMountIds.constBegin());
        if (selectedFileWidget) {
            const QFileInfo selectedInfo(selectedFileWidget->property("filePath").toString());
            const QString candidate = selectedInfo.isDir()
                ? selectedInfo.absoluteFilePath() + "/" + selectedInfo.fileName() + ".json"
                : selectedInfo.absolutePath() + "/" + selectedInfo.completeBaseName() + ".json";
            if (QFile::exists(candidate) &&
                motioncam::CalibrationData::loadFromFile(candidate.toStdString()).has_value())
                selectedJsonPath = candidate;
        }
    }

    for (auto* fileWidget : fileWidgets) {
        // Skip if not a file widget (e.g., separators)
        if (!fileWidget->property("filePath").isValid()) {
            continue;
        }

        auto filePath = fileWidget->property("filePath").toString();
        QFileInfo fileInfo(filePath);
        QString jsonPath = fileInfo.isDir()
            ? fileInfo.absoluteFilePath() + "/" + fileInfo.fileName() + ".json"
            : fileInfo.absolutePath() + "/" + fileInfo.completeBaseName() + ".json";

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

        const bool canCopySelectedJson = !selectedJsonPath.isEmpty() &&
                                         fileWidget != selectedFileWidget;
        actualCalibButton->setText(canCopySelectedJson ? tr("Copy JSON") : tr("Create JSON"));
        actualCalibButton->setToolTip(canCopySelectedJson
            ? tr("Copy the selected clip's calibration JSON to this clip")
            : QString());
        actualCalibButton->setProperty("copyJsonSourcePath",
            canCopySelectedJson ? selectedJsonPath : QString());

        // Check if JSON exists and is valid
        if (QFile::exists(jsonPath)) {
            auto calibData = motioncam::CalibrationData::loadFromFile(jsonPath.toStdString());

            if (calibData.has_value()) {
                // Valid calibration found
                actualCalibButton->setVisible(false);
                actualStatusLabel->setText("Calibration Loaded");
                actualStatusLabel->setStyleSheet("font-size: 9pt; font-weight: bold; color: #00AA00;");
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
                actualStatusLabel->setStyleSheet("font-size: 9pt; font-weight: bold; color: #AA0000;");
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
