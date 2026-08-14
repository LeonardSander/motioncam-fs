#include "mainwindow.h"
#include "ui_mainwindow.h"
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

using namespace motioncam;
#include <QMimeData>
#include <QPushButton>
#include <QFileInfo>
#include <QProcess>
#include <QMessageBox>
#include <QFileDialog>
#include <QSettings>
#include <QDir>
#include <QSignalBlocker>
#include <QLabel>
#include <QFrame>
#include <QProgressDialog>
#include <QThread>
#include <QUuid>
#include <QTemporaryDir>
#include <QStandardPaths>
#include <algorithm>
#include <QTimer>
#include <QtConcurrent>
#include <fstream>
#include <sstream>
#include <spdlog/spdlog.h>

#include <boost/filesystem.hpp>

#ifdef _WIN32
#include "win/FuseFileSystemImpl_Win.h"
#elif __APPLE__
#include "macos/FuseFileSystemImpl_MacOS.h"
#elif __linux__
#include "linux/FuseFileSystemImpl_Linux.h"
#endif

namespace {
    constexpr auto PACKAGE_NAME = "com.motioncam";
    constexpr auto APP_NAME = "MotionCam FS";
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
#elif __linux__
    mFuseFilesystem = std::make_unique<motioncam::FuseFileSystemImpl_Linux>();
#endif

    // Enable drag and drop on the scroll area
    ui->dragAndDropScrollArea->setAcceptDrops(true);
    ui->dragAndDropScrollArea->installEventFilter(this);

    restoreSettings();

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
    connect(ui->normalizeExposureCheckBox, &QCheckBox::checkStateChanged, this, &MainWindow::onRenderSettingsChanged);
    connect(ui->smoothExposureCheckBox, &QCheckBox::checkStateChanged, this, &MainWindow::onRenderSettingsChanged);
    connect(ui->smoothWhiteBalanceCheckBox, &QCheckBox::checkStateChanged, this, &MainWindow::onRenderSettingsChanged);
    connect(ui->cfrConversionCheckBox, &QCheckBox::checkStateChanged, this, &MainWindow::onRenderSettingsChanged);
    connect(ui->cropEnableCheckBox, &QCheckBox::checkStateChanged, this, &MainWindow::onRenderSettingsChanged);
    connect(ui->camModelOverrideCheckBox, &QCheckBox::checkStateChanged, this, &MainWindow::onRenderSettingsChanged);
    connect(ui->logTransformCheckBox, &QCheckBox::checkStateChanged, this, &MainWindow::onRenderSettingsChanged);
    connect(ui->remosaicCheckBox, &QCheckBox::checkStateChanged, this, &MainWindow::onRenderSettingsChanged);
    connect(ui->higherCfaHqCheckBox, &QCheckBox::checkStateChanged, this, &MainWindow::onRenderSettingsChanged);
    connect(ui->dngCompressionCheckBox, &QCheckBox::checkStateChanged, this, &MainWindow::onRenderSettingsChanged);
    connect(ui->dngCompressionModeComboBox, &QComboBox::currentIndexChanged, this, [this](int index) {
        static constexpr float modes[] = {-1.0f, 0.0f, 0.1f, 0.3f, 0.5f, 1.0f};
        if (index < 6)
            mRenderSettings.jxlDistance = modes[std::clamp(index, 0, 5)];
        onRenderSettingsChanged(Qt::CheckState::Unchecked);
    });
    connect(ui->draftQuality, &QComboBox::currentIndexChanged, this, &MainWindow::onDraftModeQualityChanged);
    connect(ui->cfrTarget, &QComboBox::currentTextChanged, this, [this](const QString& text) {
        onCFRTargetChanged(text.toStdString());
        QTimer::singleShot(100, this, &MainWindow::updateFpsLabels);
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
    connect(ui->cfaPhaseComboBox, &QComboBox::currentTextChanged, this, [this](const QString& text) {
        onCfaPhaseChanged(text.toStdString());
    });

    connect(ui->changeCacheBtn, &QPushButton::clicked, this, &MainWindow::onSetCacheFolder);
    connect(ui->defaultBtn, &QPushButton::clicked, this, &MainWindow::onSetDefaultSettings);
    
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
    saveSettings();
    
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
    settings.setValue("normalizeExposure", ui->normalizeExposureCheckBox->checkState() == Qt::CheckState::Checked);
    settings.setValue("smoothExposure", ui->smoothExposureCheckBox->checkState() == Qt::CheckState::Checked);
    settings.setValue("smoothWhiteBalance", ui->smoothWhiteBalanceCheckBox->checkState() == Qt::CheckState::Checked);
    settings.setValue("cfrConversion", ui->cfrConversionCheckBox->checkState() == Qt::CheckState::Checked);
    settings.setValue("cropEnabled", ui->cropEnableCheckBox->checkState() == Qt::CheckState::Checked);
    settings.setValue("camModelOverrideEnabled", ui->camModelOverrideCheckBox->checkState() == Qt::CheckState::Checked);
    settings.setValue("logTransformEnabled", ui->logTransformCheckBox->checkState() == Qt::CheckState::Checked);
    settings.setValue("jpegCompression", ui->dngCompressionCheckBox->checkState() == Qt::CheckState::Checked);
    settings.setValue("jxlDistance", mRenderSettings.jxlDistance);
    settings.setValue("cameraNativeFinalization",
        ui->dngCompressionModeComboBox->currentText() == "Camera Native");
    settings.setValue("higherCfaHq", ui->higherCfaHqCheckBox->isChecked());
    settings.setValue("cachePath", mCacheRootFolder);
    settings.setValue("draftQuality", mRenderSettings.draftScale);
    settings.setValue("cfrTarget", QString::fromStdString(cfrTargetToString(mRenderSettings.cfrTarget)));
    settings.setValue("cropTarget", QString::fromStdString(mRenderSettings.cropTarget));
    settings.setValue("exposureCompensation", QString::fromStdString(mRenderSettings.exposureCompensation));
    settings.setValue("camModelOverride", QString::fromStdString(mRenderSettings.cameraModel));
    settings.setValue("levels", QString::fromStdString(mRenderSettings.levels));
    settings.setValue("logTransform", QString::fromStdString(logTransformModeToString(mRenderSettings.logTransform)));
    settings.setValue("quadBayerOption", QString::fromStdString(quadBayerModeToString(mRenderSettings.quadBayerOption)));
    settings.setValue("cfaPhase", QString::fromStdString(mRenderSettings.cfaPhase));
    // Save mounted files
    settings.beginWriteArray("mountedFiles");

    for (auto i = 0; i < mMountedFiles.size(); ++i) {
        settings.setArrayIndex(i);
        settings.setValue("srcFile", mMountedFiles[i].srcFile);
    }

    settings.endArray();
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

    ui->normalizeExposureCheckBox->setCheckState(
        !settings.contains("normalizeExposure") ? Qt::CheckState::Checked :
        (settings.value("normalizeExposure").toBool() ? Qt::CheckState::Checked : Qt::CheckState::Unchecked));

    ui->smoothExposureCheckBox->setCheckState(
        settings.value("smoothExposure").toBool() ? Qt::CheckState::Checked : Qt::CheckState::Unchecked);
    ui->smoothWhiteBalanceCheckBox->setCheckState(
        settings.value("smoothWhiteBalance").toBool() ? Qt::CheckState::Checked : Qt::CheckState::Unchecked);

    ui->cfrConversionCheckBox->setCheckState(
        !settings.contains("cfrConversion") ? Qt::CheckState::Checked :
        (settings.value("cfrConversion").toBool() ? Qt::CheckState::Checked : Qt::CheckState::Unchecked));

    ui->cropEnableCheckBox->setCheckState(
        settings.value("cropEnabled").toBool() ? Qt::CheckState::Checked : Qt::CheckState::Unchecked);

    ui->camModelOverrideCheckBox->setCheckState(
        !settings.contains("camModelOverrideEnabled") ? Qt::CheckState::Checked :
        (settings.value("camModelOverrideEnabled").toBool() ? Qt::CheckState::Checked : Qt::CheckState::Unchecked));

    ui->logTransformCheckBox->setCheckState(
        !settings.contains("logTransformEnabled") ? Qt::CheckState::Checked :
        (settings.value("logTransformEnabled").toBool() ? Qt::CheckState::Checked : Qt::CheckState::Unchecked));

    ui->dngCompressionCheckBox->setCheckState(
        settings.value("jpegCompression").toBool() ? Qt::CheckState::Checked : Qt::CheckState::Unchecked);
    mRenderSettings.jxlDistance = settings.value("jxlDistance", -1.0).toFloat();
    const std::array<float, 6> jxlDistances = {-1.0f, 0.0f, 0.1f, 0.3f, 0.5f, 1.0f};
    auto nearestJxl = std::min_element(jxlDistances.begin(), jxlDistances.end(), [this](float a, float b) {
        return std::abs(a - mRenderSettings.jxlDistance) < std::abs(b - mRenderSettings.jxlDistance);
    });
    ui->dngCompressionModeComboBox->setCurrentIndex(
        settings.value("cameraNativeFinalization", false).toBool()
            ? 6 : static_cast<int>(nearestJxl - jxlDistances.begin()));
    ui->higherCfaHqCheckBox->setChecked(
        !settings.contains("higherCfaHq") || settings.value("higherCfaHq").toBool());

    mCacheRootFolder = settings.value("cachePath").toString();    
    mRenderSettings.draftScale = std::max(1, settings.value("draftQuality").toInt());
    mRenderSettings.cfrTarget = stringToCFRTarget(!settings.contains("cfrTarget") ? "Prefer Drop Frame" : settings.value("cfrTarget").toString().toStdString());
    mRenderSettings.exposureCompensation = (!settings.contains("exposureCompensation") ? "" : settings.value("exposureCompensation").toString().toStdString());
    mRenderSettings.quadBayerOption = stringToQuadBayerMode(!settings.contains("quadBayerOption") ? "Demosaic" : settings.value("quadBayerOption").toString().toStdString());
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
    ui->cfaPhaseComboBox->setCurrentText(QString::fromStdString(mRenderSettings.cfaPhase));
    ui->cropTargetComboBox->setCurrentText(QString::fromStdString(mRenderSettings.cropTarget));    
    ui->camModelOverrideComboBox->setCurrentText(QString::fromStdString(mRenderSettings.cameraModel));
    ui->levelsComboBox->setCurrentText(QString::fromStdString(mRenderSettings.levels));  
    ui->logTransformComboBox->setCurrentText(QString::fromStdString(logTransformModeToString(mRenderSettings.logTransform)));  
  
    // Restore mounted files
    auto size = settings.beginReadArray("mountedFiles");
    for (int i = 0; i < size; ++i) {
        settings.setArrayIndex(i);

        auto srcFile = settings.value("srcFile").toString();
        if(QFile::exists(srcFile)) // Mount files that exist
            mountFile(srcFile);
    }
    settings.endArray();

    updateUi();
}

bool MainWindow::eventFilter(QObject *watched, QEvent *event) {
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
                          filePath.endsWith(".mp4", Qt::CaseInsensitive))) ||
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
                          filePath.endsWith(".mp4", Qt::CaseInsensitive))) ||
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
    // Extract just the filename from the path
    QFileInfo fileInfo(filePath);
    auto fileName = fileInfo.fileName();
    const QString destinationRoot = mCacheRootFolder.isEmpty() ? fileInfo.path() : mCacheRootFolder;
    // A sequence directory cannot be mounted onto itself: doing so hides the
    // source DNGs and makes every projected read recursively enter FUSE.
    const QString mountName = fileInfo.isDir()
        ? fileInfo.fileName() + "-mounted"
        : fileInfo.baseName();
    auto dstPath = destinationRoot + "/" + mountName;
    motioncam::MountId mountId;

    try {
        auto settings = buildRenderSettings();
        mountId = mFuseFilesystem->mount(settings, filePath.toStdString(), dstPath.toStdString());
    }
    catch(std::runtime_error& e) {
        QMessageBox::critical(this, "Error", QString("There was an error mounting the file. (error: %1)").arg(e.what()));
        return;
    }

    // Get the scroll area's content widget and its layout
    auto* scrollContent = ui->dragAndDropScrollArea->widget();
    auto* scrollLayout = qobject_cast<QVBoxLayout*>(scrollContent->layout());

    // Create a widget to hold a filename label and buttons
    auto* fileWidget = new QWidget(scrollContent);

    fileWidget->setFixedHeight(168);        // Increased height for 2 lines of metrics
    fileWidget->setProperty("filePath", filePath);
    fileWidget->setProperty("mountId", mountId);
    fileWidget->setProperty("mountPath", dstPath);

    auto* fileLayout = new QVBoxLayout(fileWidget);
    fileLayout->setContentsMargins(16, 12, 16, 20);
    fileLayout->setSpacing(4);

    // Create and add the filename label
    auto* fileLabel = new QLabel(fileInfo.baseName(), fileWidget);
    fileLabel->setToolTip(filePath); // Show full path on hover
    fileLabel->setStyleSheet("font-weight: bold; font-size: 12pt;");
    fileLayout->addWidget(fileLabel);

    // Get file information from the FUSE filesystem
    auto fileInfoOpt = mFuseFilesystem->getFileInfo(mountId);
    if (fileInfoOpt.has_value()) {
        auto info = fileInfoOpt.value();

        // Format runtime as MM:SS
        int minutes = static_cast<int>(info.runtimeSeconds) / 60;
        int seconds = static_cast<int>(info.runtimeSeconds) % 60;
        QString runtimeStr = QString("%1:%2").arg(minutes).arg(seconds, 2, 10, QChar('0'));
        
        // Extract RAW bits and log indicator from levelsInfo for styling
        QString levelsStr = QString::fromStdString(info.levelsInfo);
        QString rawPart;
        int rawIdx = levelsStr.indexOf("RAW");
        if (rawIdx != -1) {
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
        infoLabel1->setProperty("infoLabel1", true);
        infoLabel1->setProperty("mountId", QVariant(mountId));
        fileLayout->addWidget(infoLabel1);
        
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
        infoLabel2->setProperty("infoLabel2", true);
        infoLabel2->setProperty("mountId", QVariant(mountId));
        fileLayout->addWidget(infoLabel2);
    }

    // Create and add the source folder label
    auto* sourceLabel = new QLabel(QString("Source: %1").arg(fileInfo.path()), fileWidget);
    sourceLabel->setStyleSheet("font-size: 9pt; color: #666666;");
    fileLayout->addWidget(sourceLabel);

    // Add spacer to maintain button position
    fileLayout->addSpacing(12);

    // Create horizontal layout for buttons
    auto* buttonLayout = new QHBoxLayout();
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
    
#ifdef _WIN32
    // ProjFS leaves hydrated files on disk after unmounting, so Windows needs
    // an explicit way to remove them.
    auto* discardButton = new QPushButton("Discard", fileWidget);
    discardButton->setFixedSize(buttonWidth, buttonHeight);
    discardButton->setToolTip("Unmount and delete all written DNG files");
    buttonLayout->addWidget(discardButton);
#endif
    
    // Create and add the finalize button
    auto* finalizeButton = new QPushButton("Finalize", fileWidget);
    finalizeButton->setFixedSize(buttonWidth, buttonHeight);
    finalizeButton->setToolTip("Render all frames to disk with compression (if enabled), then unmount");
    finalizeButton->setEnabled(true);
    buttonLayout->addWidget(finalizeButton);

    // Add stretch to push buttons to the left
    buttonLayout->addStretch();
    
    // Create calibration button (right-aligned)
    auto* calibButton = new QPushButton("Create JSON", fileWidget);
    calibButton->setFixedSize(buttonWidth, buttonHeight);
    calibButton->setProperty("calibButton", true);
    buttonLayout->addWidget(calibButton);
    
    // Create a container widget for status label and refresh button overlay
    auto* statusContainer = new QWidget(fileWidget);
    statusContainer->setProperty("statusContainer", true);
    statusContainer->setFixedHeight(buttonHeight);
    
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
    
#ifdef _WIN32
    connect(discardButton, &QPushButton::clicked, this, [this, fileWidget] {
        discardFile(fileWidget);
    });
#endif
    
    connect(finalizeButton, &QPushButton::clicked, this, [this, fileWidget] {
        finalizeFile(fileWidget);
    });
    
    connect(calibButton, &QPushButton::clicked, this, [this, fileWidget] {
        createCalibrationJson(fileWidget);
    });

    mMountedFiles.append(
        motioncam::MountedFile(mountId, filePath));
    
    // Update calibration button state
    updateCalibrationButtonStates();
}

void MainWindow::playFile(const QString& path) {
    bool success = false;

#ifdef _WIN32
    QString appDir = QCoreApplication::applicationDirPath();
    QString playerPath = QDir(appDir).absoluteFilePath("../Player/MotionCamPlayer.exe");

    success = QProcess::startDetached(QDir::cleanPath(playerPath), QStringList() << path);
#elif __APPLE__
    success = QProcess::startDetached("/usr/bin/open", QStringList() << "-a" << "MotionCam Player" << path);
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
        mFuseFilesystem->unmount(mountId);

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

void MainWindow::finalizeCameraNative(QWidget* fileWidget) {
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
    // Inspect the source CFA metadata instead so 4x4 demosaic is accepted while
    // confirmed 2x2 CFA is rejected before rendering.
    bool unsupportedBayer = false;
    const bool demosaicHigherCfa =
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
                unsupportedBayer = cfaSize <= 2 || !demosaicHigherCfa;
            }
        } else if (DNGDecoder::isDNGSequence(srcFile.toStdString())) {
            DNGDecoder decoder(srcFile.toStdString());
            int cfaSize = 0;
            std::array<uint8_t, 4> cfaPhase{};
            const bool hasCfa = decoder.getCFAMetadata(0, cfaSize, cfaPhase);
            if (cfaSizeOverride > 0) cfaSize = cfaSizeOverride;
            if (hasCfa || cfaSizeOverride > 0)
                unsupportedBayer = cfaSize <= 2 || !demosaicHigherCfa;
        }
    } catch (const std::exception& e) {
        spdlog::warn("Could not preflight Camera Native CFA metadata: {}", e.what());
    }
    if (unsupportedBayer) {
        mFuseFilesystem->updateOptions(mountId, buildRenderSettings());
        QMessageBox::warning(this, "Camera Native finalization",
            "Camera Native currently requires an RGB sequence. Enable demosaic for higher-CFA footage. "
            "2x2 Bayer demosaic will be added later.");
        return;
    }

    const QFileInfo sourceInfo(srcFile);
    const QString outputBase = sourceInfo.completeBaseName() + "_LOG60_NATIVE";
    const QDir outputDir(QFileInfo(mountPath).absolutePath());
    const QString outputPath = outputDir.absoluteFilePath(outputBase + ".mov");
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

    QProgressDialog progress("Rendering linear RGB sequence...", "Cancel", 0,
        std::max(1, info->totalFrames - info->droppedFrames + info->duplicatedFrames), this);
    progress.setWindowModality(Qt::WindowModal);
    progress.setMinimumDuration(0);

    try {
        mFuseFilesystem->finalize(mountId, staging.path().toStdString(), false,
            [&](size_t completed, size_t count, const std::string& name) {
                progress.setMaximum(static_cast<int>(count));
                progress.setValue(static_cast<int>(completed));
                if (!name.empty())
                    progress.setLabelText(QString("Rendering RGB %1 of %2: %3")
                        .arg(completed + 1).arg(count).arg(QString::fromStdString(name)));
                QApplication::processEvents();
                return !progress.wasCanceled();
            });

        QDir stageDir(staging.path());
        const QStringList dngs = stageDir.entryList({"*.dng", "*.DNG"}, QDir::Files, QDir::Name);
        if (dngs.isEmpty())
            throw std::runtime_error("The rendered sequence contains no DNG frames");

        // Feed our parser's RGB output directly to FFmpeg. This avoids both
        // FFmpeg's unsupported 16-bit RGB DNG path and a sequence-sized raw
        // intermediate file.
        const QString partialPath = stageDir.absoluteFilePath(outputBase + ".mov");
        const QString partialJsonPath = stageDir.absoluteFilePath(outputBase + ".json");
        QStringList args{
            "-hide_banner", "-y", "-f", "rawvideo", "-pixel_format", "rgb48le",
            "-video_size", QString("%1x%2").arg(info->width).arg(info->height),
            "-framerate", QString::number(info->fps, 'g', 9), "-i", "pipe:0"
        };
        const QString audioPath = stageDir.absoluteFilePath("audio.wav");
        if (QFile::exists(audioPath))
            args << "-i" << audioPath;
        const QString log60 =
            "lutrgb=r=log(1+60*val/maxval)/log(61)*maxval:"
            "g=log(1+60*val/maxval)/log(61)*maxval:"
            "b=log(1+60*val/maxval)/log(61)*maxval,"
            "zscale=matrixin=gbr:matrix=2020_ncl:rangein=full:range=full:dither=none,"
            "format=yuv420p10le";
        args << "-map" << "0:v:0";
        if (QFile::exists(audioPath))
            args << "-map" << "1:a:0" << "-c:a" << "copy";
        args << "-vf" << log60 << "-c:v" << "libx265" << "-preset" << "slow"
             << "-crf" << "14" << "-pix_fmt" << "yuv420p10le"
             << "-color_range" << "pc" << "-colorspace" << "bt2020nc"
             << "-color_primaries" << "bt2020" << "-x265-params"
             << "range=full:colorprim=bt2020:colormatrix=bt2020nc"
             << "-fps_mode" << "cfr" << "-r" << QString::number(info->fps, 'g', 9)
             << "-movflags" << "+write_colr" << "-frames:v" << QString::number(dngs.size())
             << partialPath;

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

        progress.setRange(0, dngs.size());
        progress.setValue(0);
        progress.setLabelText("Encoding LOG60 Camera Native frame 1...");
        QProcess encoder;
        encoder.setProcessChannelMode(QProcess::MergedChannels);
        encoder.start(ffmpeg, args, QIODevice::ReadWrite);
        if (!encoder.waitForStarted())
            throw std::runtime_error("Could not start FFmpeg: " + encoder.errorString().toStdString());

        QByteArray diagnostic;
        DNGFrameMetadata colorMetadata;
        for (int frameIndex = 0; frameIndex < dngs.size(); ++frameIndex) {
            const QString& dngName = dngs[frameIndex];
            QFile dngFile(stageDir.absoluteFilePath(dngName));
            if (!dngFile.open(QIODevice::ReadOnly))
                throw std::runtime_error("Could not open staged frame " + dngName.toStdString());
            const QByteArray dngArray = dngFile.readAll();
            std::vector<uint8_t> dngBytes(dngArray.begin(), dngArray.end());
            if (frameIndex == 0 && !DNGDecoder::getColorMetadata(dngBytes, colorMetadata))
                throw std::runtime_error("Could not read color metadata from the staged DNG");
            std::vector<uint8_t> rgbBytes;
            uint32_t frameWidth = 0, frameHeight = 0;
            if (!DNGDecoder::extractUncompressedRGB16(
                    dngBytes, rgbBytes, frameWidth, frameHeight) ||
                frameWidth != static_cast<uint32_t>(info->width) ||
                frameHeight != static_cast<uint32_t>(info->height))
                throw std::runtime_error("Could not extract RGB16 from staged frame " + dngName.toStdString());

            constexpr qint64 chunkSize = 1024 * 1024;
            qint64 offset = 0;
            while (offset < static_cast<qint64>(rgbBytes.size())) {
                if (progress.wasCanceled()) {
                    encoder.kill();
                    encoder.waitForFinished();
                    throw std::runtime_error("Finalization cancelled");
                }
                if (encoder.state() == QProcess::NotRunning) {
                    diagnostic += encoder.readAll();
                    throw std::runtime_error(("FFmpeg stopped while receiving frame data:\n" +
                        QString::fromUtf8(diagnostic.right(4000))).toStdString());
                }
                const qint64 count = std::min(chunkSize,
                    static_cast<qint64>(rgbBytes.size()) - offset);
                const qint64 written = encoder.write(
                    reinterpret_cast<const char*>(rgbBytes.data()) + offset, count);
                if (written < 0)
                    throw std::runtime_error("Could not stream RGB frame data to FFmpeg");
                offset += written;
                while (encoder.bytesToWrite() > 2 * chunkSize) {
                    encoder.waitForBytesWritten(100);
                    QApplication::processEvents();
                    diagnostic += encoder.readAll();
                    if (diagnostic.size() > 8000) diagnostic = diagnostic.right(4000);
                    if (progress.wasCanceled()) {
                        encoder.kill();
                        encoder.waitForFinished();
                        throw std::runtime_error("Finalization cancelled");
                    }
                    if (encoder.state() == QProcess::NotRunning)
                        throw std::runtime_error(("FFmpeg stopped while receiving frame data:\n" +
                            QString::fromUtf8(diagnostic.right(4000))).toStdString());
                }
            }
            progress.setValue(frameIndex + 1);
            if (frameIndex + 1 < dngs.size())
                progress.setLabelText(QString("Encoding LOG60 Camera Native frame %1 of %2...")
                    .arg(frameIndex + 2).arg(dngs.size()));
            QApplication::processEvents();
        }
        encoder.closeWriteChannel();
        progress.setLabelText("Finishing LOG60 Camera Native MOV...");
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
        if (colorMetadata.hasColorMatrix1) sidecar["colorMatrix1"] = colorMetadata.colorMatrix1;
        if (colorMetadata.hasColorMatrix2) sidecar["colorMatrix2"] = colorMetadata.colorMatrix2;
        if (colorMetadata.hasForwardMatrix1) sidecar["forwardMatrix1"] = colorMetadata.forwardMatrix1;
        if (colorMetadata.hasForwardMatrix2) sidecar["forwardMatrix2"] = colorMetadata.forwardMatrix2;
        if (colorMetadata.hasAsShotNeutral) sidecar["asShotNeutral"] = colorMetadata.asShotNeutral;
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
        const QString movBackup = outputPath + backupId;
        const QString jsonBackup = jsonPath + backupId;
        const bool hadMov = QFile::exists(outputPath);
        const bool hadJson = QFile::exists(jsonPath);
        if ((hadMov && !QFile::rename(outputPath, movBackup)) ||
            (hadJson && !QFile::rename(jsonPath, jsonBackup))) {
            if (QFile::exists(movBackup)) QFile::rename(movBackup, outputPath);
            throw std::runtime_error("Could not preserve the existing Camera Native output");
        }
        const bool movCommitted = QFile::rename(partialPath, outputPath);
        const bool jsonCommitted = movCommitted && QFile::rename(partialJsonPath, jsonPath);
        if (!jsonCommitted) {
            if (movCommitted) QFile::remove(outputPath);
            if (QFile::exists(movBackup)) QFile::rename(movBackup, outputPath);
            if (QFile::exists(jsonBackup)) QFile::rename(jsonBackup, jsonPath);
            throw std::runtime_error("Could not commit the completed Camera Native MOV and JSON");
        }
        QFile::remove(movBackup);
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
    if (ui->dngCompressionCheckBox->isChecked() &&
        ui->dngCompressionModeComboBox->currentText() == "Camera Native") {
        finalizeCameraNative(fileWidget);
        return;
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
    QDir tempDir(tempPath);
    
    // Create temp directory
    if (!tempDir.mkpath(".")) {
        spdlog::error("Failed to create temp directory: {}", tempPath.toStdString());
        QMessageBox::critical(this, "Finalize failed", "Could not create the temporary render directory.");
        return;
    }
    
    spdlog::info("Using temp directory: {}", tempPath.toStdString());
    
    // Create progress dialog
    QProgressDialog progress("Rendering DNG sequence...", "Cancel", 0, totalFrames, this);
    progress.setWindowModality(Qt::WindowModal);
    progress.setMinimumDuration(0);
    progress.setValue(0);
    
    // Get current render config
    auto settings = buildRenderSettings();
    bool enableCompression = settings.options & motioncam::RENDER_OPT_JPEG_COMPRESSION;

    spdlog::info("Starting finalize for {} ({} frames from fileInfo, compression: {})",
                 srcFile.toStdString(), totalFrames, enableCompression ? "enabled" : "disabled");
    spdlog::info("Compression checkbox state: {}", enableCompression);
    
    
    // Render all frames directly to disk (bypassing ProjectedFS)
    // This allows compression to work correctly with variable file sizes
    bool mountReleased = false;
    try {
        int outputFrameCount = 0;
        mFuseFilesystem->finalize(
            mountId,
            tempPath.toStdString(),
            enableCompression,
            [&](size_t completed, size_t count, const std::string& name) {
                outputFrameCount = static_cast<int>(completed);
                progress.setMaximum(static_cast<int>(count));
                progress.setValue(static_cast<int>(completed));
                if (!name.empty()) {
                    progress.setLabelText(
                        QString("Rendering %1 of %2: %3")
                            .arg(completed + 1).arg(count)
                            .arg(QString::fromStdString(name)));
                }
                QApplication::processEvents();
                return !progress.wasCanceled();
            });
        
        if (!progress.wasCanceled()) {
            spdlog::info("Rendered {} frames to temp directory", outputFrameCount);
            
            // Now move files from temp to final location
            progress.setLabelText("Moving files to final location...");
            QApplication::processEvents();
            
            // Keep the active mount intact until rendering has fully succeeded.
            mFuseFilesystem->unmount(mountId);
            mountReleased = true;
#ifdef _WIN32
            QThread::msleep(150);
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
                         outputFrameCount, mountPath.toStdString());
        } else {
            spdlog::info("Finalize cancelled by user at frame {} of {}", outputFrameCount, totalFrames);
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

    // Scale raw only enabled when vignette correction is on
    if(ui->vignetteCorrectionCheckBox->checkState() == Qt::CheckState::Checked) {
        ui->scaleRawCheckBox->setEnabled(true);
        if(ui->scaleRawCheckBox->checkState() == Qt::CheckState::Checked) {
            ui->debugVignetteCheckBox->setEnabled(false);
            ui->debugVignetteCheckBox->setChecked(false);
        } else {
            ui->debugVignetteCheckBox->setEnabled(true);            
        }
        ui->vignetteOnlyColorCheckBox->setEnabled(true);
    } else {
        ui->scaleRawCheckBox->setEnabled(false);
        ui->scaleRawCheckBox->setChecked(false);
        ui->debugVignetteCheckBox->setEnabled(false);
        ui->debugVignetteCheckBox->setChecked(false);
        ui->vignetteOnlyColorCheckBox->setEnabled(false);
        ui->vignetteOnlyColorCheckBox->setChecked(false);
    }

    if (mCacheRootFolder.isEmpty()) {
        ui->cacheFolderLabel->setText("<i>Same as source file</i>");
        ui->cacheFolderLabel->setStyleSheet("color: white; font-weight: bold; font-style: italic;");
    }
    else {
        ui->cacheFolderLabel->setText(mCacheRootFolder);
        ui->cacheFolderLabel->setStyleSheet("color: white; font-weight: bold; font-family: monospace;");
    }
    
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
        
        auto info = fileInfoOpt.value();
        
        // Update first info label (runtime, resolution, data type, levels)
        if (label->property("infoLabel1").toBool()) {
            // Format runtime as MM:SS
            int minutes = static_cast<int>(info.runtimeSeconds) / 60;
            int seconds = static_cast<int>(info.runtimeSeconds) % 60;
            QString runtimeStr = QString("%1:%2").arg(minutes).arg(seconds, 2, 10, QChar('0'));
            
            // Extract RAW bits and log indicator from levelsInfo for styling
            QString levelsStr = QString::fromStdString(info.levelsInfo);
            QString rawPart;
            int rawIdx = levelsStr.indexOf("RAW");
            if (rawIdx != -1) {
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
}

void MainWindow::onRenderSettingsChanged(Qt::CheckState checkState) {
    updateUi();

    scheduleOptionsUpdate();
}

void MainWindow::scheduleOptionsUpdate() {
    // Coalesce edits made while a rebuild is active, then apply the newest
    // settings immediately after it completes instead of silently dropping it.
    if (mProcessingInProgress || (mProcessingWatcher && mProcessingWatcher->isRunning())) {
        mOptionsUpdatePending = true;
        return;
    }
    
    // If no files mounted, nothing to do
    if (mMountedFiles.isEmpty()) {
        return;
    }
    
    mProcessingInProgress = true;
    mOptionsUpdatePending = false;
    
    // Capture current settings
    auto settings = buildRenderSettings();
    auto mountedFiles = mMountedFiles;
    auto filesystem = mFuseFilesystem.get();
    
    // Show progress
    onProcessingStarted();
    
    // Run processing in background thread using QtConcurrent
    QFuture<void> future = QtConcurrent::run([this, filesystem, settings, mountedFiles]() {
        int current = 0;
        int total = mountedFiles.size();
        
        for (const auto& file : mountedFiles) {
            filesystem->updateOptions(file.mountId, settings);
            current++;
            
            // Update progress on main thread
            QMetaObject::invokeMethod(this, "onProcessingProgress", Qt::QueuedConnection,
                                     Q_ARG(int, current), Q_ARG(int, total));
        }
    });
    
    mProcessingWatcher->setFuture(future);
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

void MainWindow::onSetCacheFolder(bool checked) {
    Q_UNUSED(checked);  // Parameter not needed for folder selection

    auto folderPath = QFileDialog::getExistingDirectory(
        this,
        tr("Select Cache Root Folder"),
        QString(),  // Start from default location
        QFileDialog::ShowDirsOnly | QFileDialog::DontResolveSymlinks
    );

    mCacheRootFolder = folderPath;
    if (mCacheRootFolder.isEmpty()) {
        ui->cacheFolderLabel->setText("<i>Same as source file</i>");
        ui->cacheFolderLabel->setStyleSheet("color: white; font-weight: bold; font-style: italic;");
    }
    else {
        ui->cacheFolderLabel->setText(mCacheRootFolder);
        ui->cacheFolderLabel->setStyleSheet("color: white; font-weight: bold; font-family: monospace;");
    }
}

void MainWindow::onSetDefaultSettings(bool checked) {
    ui->draftModeCheckBox->setCheckState(Qt::CheckState::Unchecked);
    ui->vignetteCorrectionCheckBox->setCheckState(Qt::CheckState::Checked);
    ui->scaleRawCheckBox->setCheckState(Qt::CheckState::Unchecked);
    ui->debugVignetteCheckBox->setCheckState(Qt::CheckState::Unchecked);
    ui->vignetteOnlyColorCheckBox->setCheckState(Qt::CheckState::Checked);
    ui->normalizeExposureCheckBox->setCheckState(Qt::CheckState::Checked);
    ui->smoothExposureCheckBox->setCheckState(Qt::CheckState::Unchecked);
    ui->smoothWhiteBalanceCheckBox->setCheckState(Qt::CheckState::Unchecked);
    ui->cfrConversionCheckBox->setCheckState(Qt::CheckState::Checked);
    ui->cropEnableCheckBox->setCheckState(Qt::CheckState::Unchecked);
    ui->camModelOverrideCheckBox->setCheckState(Qt::CheckState::Checked);
    ui->logTransformCheckBox->setCheckState(Qt::CheckState::Checked);
    ui->higherCfaHqCheckBox->setChecked(true);

    mRenderSettings.draftScale = 1;
    mRenderSettings.cfrTarget = stringToCFRTarget("Prefer Drop Frame");
    mRenderSettings.exposureCompensation.clear();
    mRenderSettings.cameraModel = "Panasonic";
    mRenderSettings.levels = "Dynamic";
    mRenderSettings.logTransform = stringToLogTransformMode("Keep Input");
    mRenderSettings.quadBayerOption = stringToQuadBayerMode("Demosaic");
    mRenderSettings.cfaPhase = "Don't override CFA";

    ui->cfrTarget->setCurrentText(QString::fromStdString(cfrTargetToString(mRenderSettings.cfrTarget)));
    ui->exposureCompensationLineEdit->setText(QString::fromStdString(mRenderSettings.exposureCompensation));
    ui->camModelOverrideComboBox->setCurrentText(QString::fromStdString(mRenderSettings.cameraModel));    
    ui->levelsComboBox->setCurrentText(QString::fromStdString(mRenderSettings.levels)); 
    ui->cropTargetComboBox->setCurrentText(QString::fromStdString(mRenderSettings.cropTarget));    
    ui->logTransformComboBox->setCurrentText(QString::fromStdString(logTransformModeToString(mRenderSettings.logTransform)));  
    ui->quadBayerComboBox->setCurrentText(QString::fromStdString(quadBayerModeToString(mRenderSettings.quadBayerOption)));
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
}

void MainWindow::updateCalibrationButtonStates() {
    auto* scrollContent = ui->dragAndDropScrollArea->widget();
    if (!scrollContent) return;
    
    auto fileWidgets = scrollContent->findChildren<QWidget*>(QString(), Qt::FindDirectChildrenOnly);
    
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
