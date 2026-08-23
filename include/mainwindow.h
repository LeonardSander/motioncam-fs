#ifndef MAINWINDOW_H
#define MAINWINDOW_H

#include "IFuseFileSystem.h"
#include "CalibrationData.h"

#include <QMainWindow>
#include <QList>
#include <QHash>
#include <QSet>
#include <QString>
#include <QStringList>
#include <QFutureWatcher>
#include <QFutureSynchronizer>
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
class QLabel;
class QMenu;
class QPushButton;
class QKeyEvent;
class QTimer;
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
    void promptToResumeSession();

protected:
    bool eventFilter(QObject *watched, QEvent *event) override;
    void keyPressEvent(QKeyEvent *event) override;

private slots:
    void onProcessingStarted();
    void onProcessingFinished();
    void onProcessingProgress(int current, int total);

private slots:
    void onRenderSettingsChanged(Qt::CheckState state);
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
    void onOpenPreferences();
    void onApplySelected();
    void onApplyAll();
    void onNewSession();
    void onLoadSession();
    void onSaveSession();
    void onSaveSessionAs();
    void onClearRecentSessions();

    void playFile(const QString& path);
    void openMountedDirectory(QWidget* fileWidget);
    void removeFile(QWidget* fileWidget);
#ifdef _WIN32
    void discardFile(QWidget* fileWidget);
#endif
    void finalizeFile(QWidget* fileWidget);
    void finalizeCameraNative(QWidget* fileWidget, bool av1, bool hdrNoise);
    void createCalibrationJson(QWidget* fileWidget);
    void updateCalibrationButtonStates();

private:
    void saveSettings();
    void restoreSettings();
    void updateUi();
    void updateFpsLabels();
    void scheduleOptionsUpdate();
    void updateSelectionUi();
    void updateClipIndices();
    void updateLocalBadge(motioncam::MountId mountId);
    void updateThumbnail(motioncam::MountId mountId);
    QWidget* fileWidgetForMount(motioncam::MountId mountId) const;
    void saveSessionToFile(const QString& path);
    void loadSessionFromFile(const QString& path);
    void clearSession();
    void updateRecentSessionsMenu();
    void addRecentSession(const QString& path);
    QString sessionDirectory() const;
    QString autoSessionPath() const;
    void autoSaveSession();
    void markSettingsDirty();
    void clearApplyFeedback();
#ifdef __APPLE__
    void cleanupStaleMacFuseMounts();
    void forceUnmountAllMacFuseMounts();
#endif
    std::optional<QString> ensureRifeRuntime();

private:
    motioncam::RenderSettings buildRenderSettings() const;

private:
    Ui::MainWindow *ui;
    std::unique_ptr<motioncam::IFuseFileSystem> mFuseFilesystem;
    QList<motioncam::MountedFile> mMountedFiles;
    QString mCacheRootFolder;
    motioncam::RenderSettings mRenderSettings;
    motioncam::RenderSettings mGlobalRenderSettings;
    std::optional<motioncam::CalibrationData> mGlobalCalibration;
    
    QFutureWatcher<void>* mProcessingWatcher;
    bool mProcessingInProgress;
    bool mOptionsUpdatePending;
    bool mMountInProgress = false;
    bool mDeleteOnUnmount = false;
    motioncam::CachePolicy mCachePolicy = motioncam::CachePolicy::Quota;
    std::uint64_t mCacheQuotaBytes = 30ULL * 1024 * 1024 * 1024;
    int mCacheCleanupIntervalSeconds = 30;
    QTimer* mCacheCleanupTimer = nullptr;
    QHash<motioncam::MountId, motioncam::RenderSettings> mLocalSettings;
    QSet<motioncam::MountId> mSelectedMountIds;
    QPushButton* mApplySelectedButton = nullptr;
    QPushButton* mApplyAllButton = nullptr;
    QLabel* mSelectedFilesLabel = nullptr;
    QString mCurrentSessionFile;
    QString mPlayerPath;
    QStringList mRecentSessions;
    QMenu* mRecentSessionsMenu = nullptr;
    QFutureSynchronizer<void> mThumbnailTasks;
    QTimer* mAutoApplyTimer = nullptr;
    bool mSettingsDirty = false;
    QString mApplySelectedButtonBaseStyle;
    QString mApplyAllButtonBaseStyle;
    
#ifdef _WIN32
    ITaskbarList3* mTaskbarList;
#endif
};

#endif // MAINWINDOW_H
