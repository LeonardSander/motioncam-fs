#ifndef MAINWINDOW_H
#define MAINWINDOW_H

#include "IFuseFileSystem.h"

#include <QMainWindow>
#include <QList>
#include <QString>
#include <QStringList>

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

class QTimer;
class QFileInfo;
class QMenu;
class QAction;

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
    void onCacheCleanup();
    void onSetDefaultSettings(bool checked);
    void onNewSession();
    void onLoadSession();
    void onSaveSession();
    void onSaveSessionAs();
    void onClearRecentSessions();

    void playFile(const QString& path);
    void openMountedDirectory(QWidget* fileWidget);
    void removeFile(QWidget* fileWidget);

private:
    void saveSettings();
    void restoreSettings();
    void updateUi();
    void updateFpsLabels();
    QString mountDestinationPath(const QFileInfo& fileInfo) const;
    void clearMountedFiles();
    void saveSessionToFile(const QString& filePath);
    bool loadSessionFromFile(const QString& filePath);
    void initSessionMenus();
    void updateRecentSessionsMenu();
    void loadRecentSessions();
    void saveRecentSessions() const;
    void addRecentSession(const QString& filePath);

private:
    Ui::MainWindow *ui;
    std::unique_ptr<motioncam::IFuseFileSystem> mFuseFilesystem;
    QList<motioncam::MountedFile> mMountedFiles;
    QString mCacheRootFolder;
    int mDraftQuality;
    std::string mCFRTarget;
    std::string mCropTarget;
    std::string mCameraModel;
    std::string mLevels;
    std::string mLogTransform;
    std::string mExposureCompensation;
    std::string mQuadBayerOption;
    QString mPlayerPath;
    QString mCurrentSessionFile;
    QStringList mRecentSessions;
    QMenu* mRecentSessionsMenu{nullptr};
    QAction* mClearRecentSessionsAction{nullptr};
};

#endif // MAINWINDOW_H
