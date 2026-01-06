#ifndef MAINWINDOW_H
#define MAINWINDOW_H

#include "IFuseFileSystem.h"

#include <QMainWindow>
#include <QList>
#include <QSet>
#include <QHash>
#include <QString>

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
    void onSetDefaultSettings(bool checked);
    void onApplySelected();
    void onApplyAll();

    void playFile(const QString& path);
    void openMountedDirectory(QWidget* fileWidget);
    void removeFile(QWidget* fileWidget);

private:
    struct RenderSettings {
        motioncam::FileRenderOptions renderOptions = motioncam::RENDER_OPT_NONE;
        int draftQuality = 1;
        std::string cfrTarget;
        std::string cropTarget;
        std::string cameraModel;
        std::string levels;
        std::string logTransform;
        std::string exposureCompensation;
        std::string quadBayerOption;
    };

    void saveSettings();
    void restoreSettings();
    void updateUi();
    void updateFpsLabels();
    RenderSettings captureSettingsFromUi() const;
    motioncam::RenderSettings toMotionCamSettings(const RenderSettings& settings) const;
    void applySettingsToMount(motioncam::MountId mountId, const RenderSettings& settings);
    void applySettingsToMounts(const QSet<motioncam::MountId>& mounts, const RenderSettings& settings);
    void updateLocalBadgeForMount(motioncam::MountId mountId);
    void updateThumbnailForMount(motioncam::MountId mountId);
    void updateSelectionUi();
    void setFileWidgetSelected(QWidget* fileWidget, bool selected);
    QWidget* findFileWidgetForMountId(motioncam::MountId mountId) const;
    QWidget* findMountWidget(QObject* obj) const;
    void clearSelection();

private:
    Ui::MainWindow *ui;
    std::unique_ptr<motioncam::IFuseFileSystem> mFuseFilesystem;
    QList<motioncam::MountedFile> mMountedFiles;
    QHash<motioncam::MountId, RenderSettings> mLocalSettings;
    QSet<motioncam::MountId> mSelectedMountIds;
    QString mCacheRootFolder;
    int mDraftQuality;
    std::string mCFRTarget;
    std::string mCropTarget;
    std::string mCameraModel;
    std::string mLevels;
    std::string mLogTransform;
    std::string mExposureCompensation;
    std::string mQuadBayerOption;
    motioncam::MountId mLastSelectedMountId;
};

#endif // MAINWINDOW_H
