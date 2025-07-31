#include "mainwindow.h"
#include "ui_mainwindow.h"

#include <QDragEnterEvent>
#include <QDropEvent>
#include <QMimeData>
#include <QPushButton>
#include <QFileInfo>
#include <QProcess>
#include <QMessageBox>
#include <QFileDialog>
#include <QSettings>
#include <QDir>
#include <algorithm>
#include <QTimer>

#ifdef _WIN32
#include "win/FuseFileSystemImpl_Win.h"
#elif __APPLE__
#include "macos/FuseFileSystemImpl_MacOS.h"
#endif

namespace {
    constexpr auto PACKAGE_NAME = "com.motioncam";
    constexpr auto APP_NAME = "MotionCam FS";

    motioncam::FileRenderOptions getRenderOptions(Ui::MainWindow& ui) {
        motioncam::FileRenderOptions options = motioncam::RENDER_OPT_NONE;

        if(ui.draftModeCheckBox->checkState() == Qt::CheckState::Checked)
            options |= motioncam::RENDER_OPT_DRAFT;

        if(ui.vignetteCorrectionCheckBox->checkState() == Qt::CheckState::Checked)
            options |= motioncam::RENDER_OPT_APPLY_VIGNETTE_CORRECTION;

        if(ui.vignetteOnlyColorCheckBox->checkState() == Qt::CheckState::Checked)
            options |= motioncam::RENDER_OPT_VIGNETTE_ONLY_COLOR;

        if(ui.scaleRawCheckBox->checkState() == Qt::CheckState::Checked)
            options |= motioncam::RENDER_OPT_NORMALIZE_SHADING_MAP;

        if(ui.debugVignetteCheckBox->checkState() == Qt::CheckState::Checked)
            options |= motioncam::RENDER_OPT_DEBUG_SHADING_MAP;

        if(ui.normalizeExposureCheckBox->checkState() == Qt::CheckState::Checked)
            options |= motioncam::RENDER_OPT_NORMALIZE_EXPOSURE;

        if(ui.cfrConversionCheckBox->checkState() == Qt::CheckState::Checked)
            options |= motioncam::RENDER_OPT_FRAMERATE_CONVERSION;

        if(ui.cropEnableCheckBox->checkState() == Qt::CheckState::Checked)
            options |= motioncam::RENDER_OPT_CROPPING;

        return options;
    }
}

MainWindow::MainWindow(QWidget *parent)
    : QMainWindow(parent)
    , ui(new Ui::MainWindow)
    , mDraftQuality(1)
{
    ui->setupUi(this);

#ifdef _WIN32
    mFuseFilesystem = std::make_unique<motioncam::FuseFileSystemImpl_Win>();
#elif __APPLE__
    mFuseFilesystem = std::make_unique<motioncam::FuseFileSystemImpl_MacOs>();
#endif

    // Enable drag and drop on the scroll area
    ui->dragAndDropScrollArea->setAcceptDrops(true);
    ui->dragAndDropScrollArea->installEventFilter(this);

    restoreSettings();

    // Connect to widgets
    connect(ui->draftModeCheckBox, &QCheckBox::checkStateChanged, this, &MainWindow::onRenderSettingsChanged);
    connect(ui->vignetteCorrectionCheckBox, &QCheckBox::checkStateChanged, this, &MainWindow::onRenderSettingsChanged);
    connect(ui->scaleRawCheckBox, &QCheckBox::checkStateChanged, this, &MainWindow::onRenderSettingsChanged);
    connect(ui->vignetteOnlyColorCheckBox, &QCheckBox::checkStateChanged, this, &MainWindow::onRenderSettingsChanged);
    connect(ui->normalizeExposureCheckBox, &QCheckBox::checkStateChanged, this, &MainWindow::onRenderSettingsChanged);
    connect(ui->cfrConversionCheckBox, &QCheckBox::checkStateChanged, this, &MainWindow::onRenderSettingsChanged);
    connect(ui->cropEnableCheckBox, &QCheckBox::checkStateChanged, this, &MainWindow::onRenderSettingsChanged);
    
    connect(ui->draftQuality, &QComboBox::currentIndexChanged, this, &MainWindow::onDraftModeQualityChanged);
    connect(ui->cfrTarget, &QComboBox::currentTextChanged, this, [this](const QString& text) {
        onCFRTargetChanged(text.toStdString());
        QTimer::singleShot(100, this, &MainWindow::updateFpsLabels);
    });
    connect(ui->cropTargetComboBox, &QComboBox::currentTextChanged, this, [this](const QString& text) {
        onCropTargetChanged(text.toStdString());
    });

    connect(ui->changeCacheBtn, &QPushButton::clicked, this, &MainWindow::onSetCacheFolder);
}

MainWindow::~MainWindow() {
    saveSettings();

    delete ui;
}

void MainWindow::saveSettings() {
    QSettings settings(PACKAGE_NAME, APP_NAME);

    settings.setValue("draftMode", ui->draftModeCheckBox->checkState() == Qt::CheckState::Checked);
    settings.setValue("applyVignetteCorrection", ui->vignetteCorrectionCheckBox->checkState() == Qt::CheckState::Checked);
    settings.setValue("scaleRaw", ui->scaleRawCheckBox->checkState() == Qt::CheckState::Checked);
    settings.setValue("vignetteOnlyColor", ui->vignetteOnlyColorCheckBox->checkState() == Qt::CheckState::Checked);
    settings.setValue("normalizeExposure", ui->normalizeExposureCheckBox->checkState() == Qt::CheckState::Checked);
    settings.setValue("cfrConversion", ui->cfrConversionCheckBox->checkState() == Qt::CheckState::Checked);
    settings.setValue("cropEnabled", ui->cropEnableCheckBox->checkState() == Qt::CheckState::Checked);
    settings.setValue("cachePath", mCacheRootFolder);
    settings.setValue("draftQuality", mDraftQuality);
    settings.setValue("cfrTarget", ui->cfrTarget->currentText());
    settings.setValue("cropTarget", ui->cropTargetComboBox->currentText());

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
        settings.value("applyVignetteCorrection").toBool() ? Qt::CheckState::Checked : Qt::CheckState::Unchecked);

    ui->scaleRawCheckBox->setCheckState(
        settings.value("scaleRaw").toBool() ? Qt::CheckState::Checked : Qt::CheckState::Unchecked);

    ui->vignetteOnlyColorCheckBox->setCheckState(
        settings.value("vignetteOnlyColor").toBool() ? Qt::CheckState::Checked : Qt::CheckState::Unchecked);

    ui->normalizeExposureCheckBox->setCheckState(
        settings.value("normalizeExposure").toBool() ? Qt::CheckState::Checked : Qt::CheckState::Unchecked);

    ui->cfrConversionCheckBox->setCheckState(
        settings.value("cfrConversion").toBool() ? Qt::CheckState::Checked : Qt::CheckState::Unchecked);

    ui->cropEnableCheckBox->setCheckState(
        settings.value("cropEnabled").toBool() ? Qt::CheckState::Checked : Qt::CheckState::Unchecked);

    mCacheRootFolder = settings.value("cachePath").toString();    
    mDraftQuality = std::max(1, settings.value("draftQuality").toInt());
    mCFRTarget = settings.value("cfrTarget").toString().toStdString();
    mCropTarget = settings.value("cropTarget").toString().toStdString();

    if (!mCropTarget.empty())
        ui->cropTargetComboBox->setCurrentText(QString::fromStdString(mCropTarget));
    
    // Set default CFR target if none is restored
    if (mCFRTarget.empty()) {
        mCFRTarget = "Prefer Drop Frame";
    }

    if(mDraftQuality == 2)
        ui->draftQuality->setCurrentIndex(0);
    else if(mDraftQuality == 4)
        ui->draftQuality->setCurrentIndex(1);
    else if(mDraftQuality == 8)
        ui->draftQuality->setCurrentIndex(2);

    // Set CFR target ComboBox to match restored value
    int cfrIndex = ui->cfrTarget->findText(QString::fromStdString(mCFRTarget));
    if (cfrIndex != -1) {
        ui->cfrTarget->setCurrentIndex(cfrIndex);
    } else if (!mCFRTarget.empty()) {
        ui->cfrTarget->setEditText(QString::fromStdString(mCFRTarget));
    }

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

                    // Replace ".txt" with your desired file extension
                    if (filePath.endsWith(".mcraw", Qt::CaseInsensitive)) {
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
                    if (filePath.endsWith(".mcraw", Qt::CaseInsensitive)) {
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
    auto dstPath = (mCacheRootFolder.isEmpty() ? fileInfo.path() : mCacheRootFolder) + "/" + fileInfo.baseName();
    motioncam::MountId mountId;

    try {
        mountId = mFuseFilesystem->mount(
            getRenderOptions(*ui), mDraftQuality, mCFRTarget, mCropTarget, filePath.toStdString(), dstPath.toStdString());
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

    fileWidget->setFixedHeight(140);
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

        // Create info label with FPS, Total Frames/Dropped, and Resolution 
        auto infoText = QString("Median / Average / Target FPS: %1 / %2 -> %3 | Framecount: %4 | Dropped: -%5 | Duplicated: +%6 | Resolution: %7x%8")
                                .arg(QString::number(info.medFps, 'f', 2))
                                .arg(QString::number(info.avgFps, 'f', 2))
                                .arg(QString::number(info.fps, 'f', 2))
                                .arg(info.totalFrames)
                                .arg(info.droppedFrames)
                                .arg(info.duplicatedFrames)
                                .arg(info.width)
                                .arg(info.height);

        auto* infoLabel = new QLabel(infoText, fileWidget);
        infoLabel->setStyleSheet("font-size: 9pt; color: #888888;");
        infoLabel->setProperty("infoLabel", true); // <-- Add this line
        infoLabel->setProperty("mountId", QVariant(mountId)); // <-- Add this line for consistency
        fileLayout->addWidget(infoLabel);
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

    // Add stretch to push buttons to the left
    buttonLayout->addStretch();

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

    mMountedFiles.append(
        motioncam::MountedFile(mountId, filePath));
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

void MainWindow::updateUi() {
    // Draft quality only enabled when draft mode is on
    if(ui->draftModeCheckBox->checkState() == Qt::CheckState::Checked)
        ui->draftQuality->setEnabled(true);
    else
        ui->draftQuality->setEnabled(false);

    if(ui->cropEnableCheckBox->checkState() == Qt::CheckState::Checked)
        ui->cropTargetComboBox->setEnabled(true);
    else
        ui->cropTargetComboBox->setEnabled(false);

    // Scale raw only enabled when vignette correction is on
    if(ui->vignetteCorrectionCheckBox->checkState() == Qt::CheckState::Checked) {
        ui->scaleRawCheckBox->setEnabled(true);
        if(ui->scaleRawCheckBox->checkState() == Qt::CheckState::Checked) 
            ui->debugVignetteCheckBox->setEnabled(true);
        else {
            ui->debugVignetteCheckBox->setEnabled(false);
            ui->debugVignetteCheckBox->setChecked(false);
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
}

void MainWindow::updateFpsLabels() {
    // Get the scroll area's content widget
    auto* scrollContent = ui->dragAndDropScrollArea->widget();
    if (!scrollContent) {
        return;
    }
    
    // Force recalculation of fps values by calling updateOptions for all mounted files
    auto renderOptions = getRenderOptions(*ui);
    
    for (const auto& mountedFile : mMountedFiles) {
        mFuseFilesystem->updateOptions(mountedFile.mountId, renderOptions, mDraftQuality, mCFRTarget, mCropTarget);
    }
    
    // Find all fps labels in the scroll area
    auto fpsLabels = scrollContent->findChildren<QLabel*>();
    
    for (auto* label : fpsLabels) {
        // Check if this is an fps label by looking for the isFpsLabel property
        if (label->property("infoLabel").toBool()) {
            bool ok = false;
            auto mountId = label->property("mountId").toInt(&ok);
            
            if (ok && mountId >= 0) {
                // Get the updated fps value
                auto fileInfoOpt = mFuseFilesystem->getFileInfo(mountId);
                if (fileInfoOpt.has_value()) {
                    auto info = fileInfoOpt.value();
                    auto infoText = QString("Median / Average / Target FPS: %1 / %2 -> %3 | Framecount: %4 | Dropped: -%5 | Duplicated: +%6 | Resolution: %7x%8")
                                .arg(QString::number(info.medFps, 'f', 2))
                                .arg(QString::number(info.avgFps, 'f', 2))
                                .arg(QString::number(info.fps, 'f', 2))
                                .arg(info.totalFrames)
                                .arg(info.droppedFrames)
                                .arg(info.duplicatedFrames)
                                .arg(info.width)
                                .arg(info.height);
                    label->setText(infoText);
                }
            }
        }
    }
}

void MainWindow::onRenderSettingsChanged(const Qt::CheckState &checkState) {
    auto it = mMountedFiles.begin();
    auto renderOptions = getRenderOptions(*ui);

    updateUi();

    while(it != mMountedFiles.end()) {        
        mFuseFilesystem->updateOptions(it->mountId, renderOptions, mDraftQuality, mCFRTarget, mCropTarget);
        ++it;
    }
    
    // Update fps labels after a short delay to ensure updateOptions has completed
    QTimer::singleShot(100, this, &MainWindow::updateFpsLabels);
}

void MainWindow::onDraftModeQualityChanged(int index) {
    if(index == 0)
        mDraftQuality = 2;
    else if(index == 1)
        mDraftQuality = 4;
    else if(index == 2)
        mDraftQuality = 8;

    onRenderSettingsChanged(Qt::CheckState::Checked);
}

void MainWindow::onCFRTargetChanged(std::string input) {
    mCFRTarget = input;
    onRenderSettingsChanged(Qt::CheckState::Checked);
}

void MainWindow::onCropTargetChanged(std::string input) {
    mCropTarget = input;
    onRenderSettingsChanged(Qt::CheckState::Checked);
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
