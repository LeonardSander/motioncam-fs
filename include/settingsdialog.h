#ifndef SETTINGSDIALOG_H
#define SETTINGSDIALOG_H

#include <QDialog>
#include <QString>
#include <cstdint>

class QLineEdit;
class QPushButton;
class QComboBox;
class QLabel;
class QDialogButtonBox;
class QCheckBox;

class SettingsDialog : public QDialog
{
    Q_OBJECT

public:
    explicit SettingsDialog(QWidget *parent = nullptr);
    ~SettingsDialog();

    void setPlayerPath(const QString& path);
    QString getPlayerPath() const;

    void setCacheFolder(const QString& path);
    QString getCacheFolder() const;

    void setCachePolicyMode(const QString& mode);
    QString getCachePolicyMode() const;

    void setCacheQuotaGb(double gb);
    double getCacheQuotaGb() const;

    void setCacheCleanupIntervalSeconds(int seconds);
    int getCacheCleanupIntervalSeconds() const;

    void setCacheFolderWarning(const QString& message);

    void setDeleteOnUnmount(bool enabled);
    bool getDeleteOnUnmount() const;

    void setAutoApplyClipSettings(bool enabled);
    bool getAutoApplyClipSettings() const;
    void setUnmountOnFinalize(bool enabled);
    bool getUnmountOnFinalize() const;

    void setMatrixOverrideEnabled(bool enabled);
    bool getMatrixOverrideEnabled() const;
    void setMatrixProfile(const QString& profile);
    QString getMatrixProfile() const;
    void setMatrixProfiles(const QStringList& profiles);

private slots:
    void onBrowsePlayerPath();
    void onBrowseCacheFolder();
    void onCacheFolderTextChanged(const QString& text);
    void onCachePolicyChanged(int index);
    void onCacheQuotaChanged(int index);
    void onCleanupIntervalChanged(int index);
    void onResetPaths();

private:
    int mHelpFontSizePt;
    QLineEdit* mPlayerPathEdit;
    QPushButton* mPlayerBrowseButton;

    QLineEdit* mCacheFolderEdit;
    QPushButton* mCacheBrowseButton;
    QPushButton* mResetPathsButton;

    QLabel* mCacheFolderWarningLabel;
    QDialogButtonBox* mButtonBox;
    QCheckBox* mDeleteOnUnmountCheckBox;
    QCheckBox* mAutoApplyClipSettingsCheckBox;
    QCheckBox* mUnmountOnFinalizeCheckBox;
    QCheckBox* mMatrixOverrideCheckBox;
    QComboBox* mMatrixProfileComboBox;

    QComboBox* mCachePolicyComboBox;
    QComboBox* mCacheQuotaComboBox;
    QLineEdit* mCacheQuotaCustomEdit;
    QComboBox* mCacheCleanupIntervalComboBox;
    QLineEdit* mCacheCleanupCustomEdit;
};

#endif // SETTINGSDIALOG_H
