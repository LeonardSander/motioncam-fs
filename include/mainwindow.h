#ifndef MAINWINDOW_H
#define MAINWINDOW_H

#include "IFuseFileSystem.h"
#include "CalibrationData.h"
#include "RenderConfig.h"

#include <QMainWindow>
#include <QList>
#include <QString>
#include <QFutureWatcher>
#include <optional>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <shobjidl.h>
#endif

namespace motioncam {
    struct MountedFile {
        MountedFile(MountId mountId, QString srcFile) :
            mountId(mountId), srcFile(srcFile)
        {}

        // Copy constructor
        MountedFile(const MountedFile& other) :
            mountId(other.mountId), srcFile(other.srcFile)
        {}

        const MountId mountId;
        const QString srcFile;

        // Copy assignment operator
        MountedFile& operator=(const MountedFile& other) {
            if (this != &other) {
                // Use const_cast to modify const members
                const_cast<MountId&>(mountId) = other.mountId;
                const_cast<QString&>(srcFile) = other.srcFile;
            }
            return *this;
        }

        // Move assignment operator
        MountedFile& operator=(MountedFile&& other) noexcept {
            if (this != &other) {
                // Use const_cast to modify const members
                const_cast<MountId&>(mountId) = std::move(other.mountId);
                const_cast<QString&>(srcFile) = std::move(other.srcFile);
            }
            return *this;
        }
    };
}

QT_BEGIN_NAMESPACE
namespace Ui {
class MainWindow;
}
QT_END_NAMESPACE

class MainWindow : public QMainWindow
{
    Q_OBJECT

public:
    MainWindow(QWidget *parent = nullptr);
    ~MainWindow();

    void mountFile(const QString& filePath);

protected:
    bool eventFilter(QObject *watched, QEvent *event) override;

private slots:
    void onProcessingStarted();
    void onProcessingFinished();
    void onProcessingProgress(int current, int total);

private slots:
    void onRenderSettingsChanged(const Qt::CheckState &state);
    void onDraftModeQualityChanged(int index);
    void onSetCacheFolder(bool checked);
    void onCFRTargetChanged(std::string input);
    void onCamModelOverrideChanged(std::string input);
    void onLevelsChanged(std::string input);
    void onCropTargetChanged(std::string input);
    void onLogTransformChanged(std::string input);
    void onExposureCompensationChanged(std::string input);
    void onQuadBayerChanged(std::string input);
    void onCfaPhaseChanged(std::string input);
    void onSetDefaultSettings(bool checked);

    void playFile(const QString& path);
    void openMountedDirectory(QWidget* fileWidget);
    void removeFile(QWidget* fileWidget);
    void createCalibrationJson(QWidget* fileWidget);
    void updateCalibrationButtonStates();

private:
    void saveSettings();
    void restoreSettings();
    void updateUi();
    void updateFpsLabels();
    void scheduleOptionsUpdate();

private:
    motioncam::RenderConfig buildRenderConfig() const;

private:
    Ui::MainWindow *ui;
    std::unique_ptr<motioncam::IFuseFileSystem> mFuseFilesystem;
    QList<motioncam::MountedFile> mMountedFiles;
    QString mCacheRootFolder;
    motioncam::RenderConfig mRenderConfig;
    std::optional<motioncam::CalibrationData> mGlobalCalibration;
    
    QFutureWatcher<void>* mProcessingWatcher;
    bool mProcessingInProgress;
    
#ifdef _WIN32
    ITaskbarList3* mTaskbarList;
#endif
};

#endif // MAINWINDOW_H
