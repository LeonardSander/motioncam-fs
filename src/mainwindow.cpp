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
#include <algorithm>

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

        if(ui.scaleRawCheckBox->checkState() == Qt::CheckState::Checked)
            options |= motioncam::RENDER_OPT_NORMALIZE_SHADING_MAP;

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
    connect(ui->draftQuality, &QComboBox::currentIndexChanged, this, &MainWindow::onDraftModeQualityChanged);

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
    settings.setValue("cachePath", mCacheRootFolder);
    settings.setValue("draftQuality", mDraftQuality);

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

    mCacheRootFolder = settings.value("cachePath").toString();    
    mDraftQuality = std::max(1, settings.value("draftQuality").toInt());

    if(mDraftQuality == 2)
        ui->draftQuality->setCurrentIndex(0);
    else if(mDraftQuality == 4)
        ui->draftQuality->setCurrentIndex(1);
    else if(mDraftQuality == 8)
        ui->draftQuality->setCurrentIndex(2);

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
            getRenderOptions(*ui), mDraftQuality, filePath.toStdString(), dstPath.toStdString());
    }
    catch(std::runtime_error& e) {
        QMessageBox::critical(this, "Error", QString("There was an error mounting the file. (error: %1)").arg(e.what()));
        return;
    }

    // Get the scroll area's content widget and its layout
    auto* scrollContent = ui->dragAndDropScrollArea->widget();
    auto* scrollLayout = qobject_cast<QVBoxLayout*>(scrollContent->layout());

    // Create a widget to hold a filename label and remove button
    auto* fileWidget = new QWidget(scrollContent);

    fileWidget->setFixedHeight(50);
    fileWidget->setProperty("filePath", filePath);
    fileWidget->setProperty("mountId", mountId);

    auto* fileLayout = new QHBoxLayout(fileWidget);
    fileLayout->setContentsMargins(5, 5, 5, 5);

    // Create and add the filename label
    auto* fileLabel = new QLabel(fileName, fileWidget);

    fileLabel->setToolTip(filePath); // Show full path on hover
    fileLayout->addWidget(fileLabel);

    // Add a spacer to push the button to the right
    fileLayout->addStretch();

    // Create and add the remove button
    auto* playButton = new QPushButton("Play", fileWidget);

    playButton->setMaximumWidth(100);
    playButton->setMaximumHeight(30);
    playButton->setIcon(QIcon(":/assets/play_btn.png"));

    fileLayout->addWidget(playButton);

    // Create and add the remove button
    auto* removeButton = new QPushButton("Remove", fileWidget);

    removeButton->setMaximumWidth(100);
    removeButton->setMaximumHeight(30);
    removeButton->setIcon(QIcon(":/assets/remove_btn.png"));

    fileLayout->addWidget(removeButton);

    // Add the file widget to the scroll area
    scrollLayout->insertWidget(0, fileWidget);

    // Hide the drag-drop label since we now have content
    ui->dragAndDropLabel->hide();

    // Connect buttons
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
    QStringList arguments;
    arguments << path;

    bool success = false;

#ifdef _WIN32
    success = QProcess::startDetached("MotionCam_Player.exe", arguments);
#elif __APPLE__
    success = QProcess::startDetached("/usr/bin/open", arguments);
#endif

    if (!success)
        QMessageBox::warning(this, "Error", QString("Failed to launch player with file: %1").arg(path));
}

void MainWindow::removeFile(QWidget* fileWidget) {
    auto* scrollContent = ui->dragAndDropScrollArea->widget();
    auto* scrollLayout = qobject_cast<QVBoxLayout*>(scrollContent->layout());

    scrollLayout->removeWidget(fileWidget);
    fileWidget->deleteLater();

    // If all files are removed, show the drag-drop label again
    if (scrollLayout->count() == 0) {
        ui->dragAndDropLabel->show();
    }

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
}

void MainWindow::updateUi() {
    // Draft quality only enabled when draft mode is on
    if(ui->draftModeCheckBox->checkState() == Qt::CheckState::Checked)
        ui->draftQuality->setEnabled(true);
    else
        ui->draftQuality->setEnabled(false);

    // Scale raw only enabled when vignette correction is on
    if(ui->vignetteCorrectionCheckBox->checkState() == Qt::CheckState::Checked)
        ui->scaleRawCheckBox->setEnabled(true);
    else
        ui->scaleRawCheckBox->setEnabled(false);

    ui->cacheFolderLabel->setText(mCacheRootFolder);
}

void MainWindow::onRenderSettingsChanged(const Qt::CheckState &checkState) {
    auto it = mMountedFiles.begin();
    auto renderOptions = getRenderOptions(*ui);

    updateUi();

    while(it != mMountedFiles.end()) {
        mFuseFilesystem->updateOptions(it->mountId, renderOptions, mDraftQuality);
        ++it;
    }
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

void MainWindow::onSetCacheFolder(bool checked) {
    Q_UNUSED(checked);  // Parameter not needed for folder selection

    auto folderPath = QFileDialog::getExistingDirectory(
        this,
        tr("Select Cache Root Folder"),
        QString(),  // Start from default location
        QFileDialog::ShowDirsOnly | QFileDialog::DontResolveSymlinks
    );

    mCacheRootFolder = folderPath;
    ui->cacheFolderLabel->setText(mCacheRootFolder);
}
