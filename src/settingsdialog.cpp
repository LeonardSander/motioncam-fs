#include "settingsdialog.h"

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QLineEdit>
#include <QPushButton>
#include <QFileDialog>
#include <QLabel>
#include <QGroupBox>
#include <QDialogButtonBox>
#include <QComboBox>
#include <QDoubleValidator>
#include <QIntValidator>
#include <QStorageInfo>
#include <QCheckBox>

SettingsDialog::SettingsDialog(QWidget *parent)
    : QDialog(parent)
{
    setWindowTitle("Preferences");
    setMinimumWidth(600);
    setMinimumHeight(200);

    // Main layout
    auto* mainLayout = new QVBoxLayout(this);

    // Cache folder group
    auto* cacheGroup = new QGroupBox("DNG Output Folder", this);
    auto* cacheLayout = new QVBoxLayout(cacheGroup);

    auto* cachePathLayout = new QHBoxLayout();
    auto* cacheLabel = new QLabel("Folder Path:", this);
    mCacheFolderEdit = new QLineEdit(this);
    mCacheFolderEdit->setPlaceholderText("Leave empty for same folder as source file...");

    mCacheBrowseButton = new QPushButton("Browse...", this);
    mCacheBrowseButton->setMaximumWidth(100);

    cachePathLayout->addWidget(cacheLabel);
    cachePathLayout->addWidget(mCacheFolderEdit, 1);
    cachePathLayout->addWidget(mCacheBrowseButton);
    cacheLayout->addLayout(cachePathLayout);

    auto* cacheHelpLabel = new QLabel(
        "<span style='color: #888888; font-size: 9pt;'>"
        "Sets the location where DNG file sequences will appear after MCRAWs are loaded. "
        "Source files must be on NTFS unless you set an NTFS DNG output folder."
        "</span>", this);
    cacheHelpLabel->setWordWrap(true);
    cacheLayout->addWidget(cacheHelpLabel);

    mCacheFolderWarningLabel = new QLabel(this);
    mCacheFolderWarningLabel->setWordWrap(true);
    mCacheFolderWarningLabel->setVisible(false);
    cacheLayout->addWidget(mCacheFolderWarningLabel);

    mainLayout->addWidget(cacheGroup);

    // Cache management group
    auto* cacheManagementGroup = new QGroupBox("Cache Management", this);
    auto* cacheManagementLayout = new QVBoxLayout(cacheManagementGroup);

    auto* policyLayout = new QHBoxLayout();
    auto* policyLabel = new QLabel("Mode:", this);
    mCachePolicyComboBox = new QComboBox(this);
    mCachePolicyComboBox->addItem("Off", "off");
    mCachePolicyComboBox->addItem("Disk quota", "quota");
    policyLayout->addWidget(policyLabel);
    policyLayout->addWidget(mCachePolicyComboBox, 1);
    cacheManagementLayout->addLayout(policyLayout);

    auto* quotaLayout = new QHBoxLayout();
    auto* quotaLabel = new QLabel("Quota:", this);
    mCacheQuotaComboBox = new QComboBox(this);
    mCacheQuotaComboBox->addItem("30 GB", 30.0);
    mCacheQuotaComboBox->addItem("50 GB", 50.0);
    mCacheQuotaComboBox->addItem("100 GB", 100.0);
    mCacheQuotaComboBox->addItem("Custom", -1.0);
    mCacheQuotaCustomEdit = new QLineEdit(this);
    mCacheQuotaCustomEdit->setPlaceholderText("GB");
    mCacheQuotaCustomEdit->setMaximumWidth(80);
    mCacheQuotaCustomEdit->setValidator(new QDoubleValidator(1.0, 10240.0, 2, mCacheQuotaCustomEdit));
    quotaLayout->addWidget(quotaLabel);
    quotaLayout->addWidget(mCacheQuotaComboBox, 1);
    quotaLayout->addWidget(mCacheQuotaCustomEdit);
    cacheManagementLayout->addLayout(quotaLayout);

    auto* intervalLayout = new QHBoxLayout();
    auto* intervalLabel = new QLabel("Cleanup interval:", this);
    mCacheCleanupIntervalComboBox = new QComboBox(this);
    mCacheCleanupIntervalComboBox->addItem("5 seconds", 5);
    mCacheCleanupIntervalComboBox->addItem("10 seconds", 10);
    mCacheCleanupIntervalComboBox->addItem("30 seconds", 30);
    mCacheCleanupIntervalComboBox->addItem("1 minute", 60);
    mCacheCleanupIntervalComboBox->addItem("2 minutes", 120);
    mCacheCleanupIntervalComboBox->addItem("Custom", -1);
    mCacheCleanupCustomEdit = new QLineEdit(this);
    mCacheCleanupCustomEdit->setPlaceholderText("Seconds");
    mCacheCleanupCustomEdit->setMaximumWidth(80);
    mCacheCleanupCustomEdit->setValidator(new QIntValidator(1, 3600, mCacheCleanupCustomEdit));
    intervalLayout->addWidget(intervalLabel);
    intervalLayout->addWidget(mCacheCleanupIntervalComboBox, 1);
    intervalLayout->addWidget(mCacheCleanupCustomEdit);
    cacheManagementLayout->addLayout(intervalLayout);

    mDeleteOnUnmountCheckBox = new QCheckBox("Delete local DNG output when unmounted", this);
    cacheManagementLayout->addWidget(mDeleteOnUnmountCheckBox);

    auto* deleteOnUnmountHelpLabel = new QLabel(
        "<span style='color: #888888; font-size: 9pt;'>"
        "Also removes the materialized DNG folder when you unmount/clear files."
        "</span>", this);
    deleteOnUnmountHelpLabel->setWordWrap(true);
    cacheManagementLayout->addWidget(deleteOnUnmountHelpLabel);

    auto* cacheManagementHelpLabel = new QLabel(
        "<span style='color: #888888; font-size: 9pt;'>"
        "Disk quota limits total materialized size by evicting oldest files first."
        "</span>", this);
    cacheManagementHelpLabel->setWordWrap(true);
    cacheManagementLayout->addWidget(cacheManagementHelpLabel);

    mainLayout->addWidget(cacheManagementGroup);

    // Player settings group
    auto* playerGroup = new QGroupBox("Video Player", this);
    auto* playerLayout = new QVBoxLayout(playerGroup);

    auto* pathLayout = new QHBoxLayout();
    auto* pathLabel = new QLabel("Player Executable:", this);
    mPlayerPathEdit = new QLineEdit(this);
    mPlayerPathEdit->setPlaceholderText("Path to MotionCamPlayer.exe...");

    mPlayerBrowseButton = new QPushButton("Browse...", this);
    mPlayerBrowseButton->setMaximumWidth(100);

    pathLayout->addWidget(pathLabel);
    pathLayout->addWidget(mPlayerPathEdit, 1);
    pathLayout->addWidget(mPlayerBrowseButton);
    playerLayout->addLayout(pathLayout);

    auto* playerHelpLabel = new QLabel(
        "<span style='color: #888888; font-size: 9pt;'>"
        "Path to MotionCamPlayer.exe for the Play button."
        "</span>", this);
    playerHelpLabel->setWordWrap(true);
    playerLayout->addWidget(playerHelpLabel);

    mainLayout->addWidget(playerGroup);

    // Camera model group
    auto* cameraGroup = new QGroupBox("Unique Camera Model", this);
    auto* cameraLayout = new QVBoxLayout(cameraGroup);

    auto* modelLayout = new QHBoxLayout();
    auto* modelLabel = new QLabel("Camera Model:", this);
    mCameraModelComboBox = new QComboBox(this);
    mCameraModelComboBox->setEditable(true);
    mCameraModelComboBox->addItem("Panasonic");
    mCameraModelComboBox->addItem("Blackmagic");
    mCameraModelComboBox->addItem("Fujifilm");

    modelLayout->addWidget(modelLabel);
    modelLayout->addWidget(mCameraModelComboBox, 1);
    cameraLayout->addLayout(modelLayout);

    auto* modelHelpLabel = new QLabel(
        "<span style='color: #888888; font-size: 9pt;'>"
        "Override the camera model name in DNG metadata."
        "</span>", this);
    modelHelpLabel->setWordWrap(true);
    cameraLayout->addWidget(modelHelpLabel);

    mainLayout->addWidget(cameraGroup);

    // Add spacer
    mainLayout->addStretch();

    // Button box
    mButtonBox = new QDialogButtonBox(
        QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    mainLayout->addWidget(mButtonBox);

    // Connect signals
    connect(mCacheBrowseButton, &QPushButton::clicked, this, &SettingsDialog::onBrowseCacheFolder);
    connect(mPlayerBrowseButton, &QPushButton::clicked, this, &SettingsDialog::onBrowsePlayerPath);
    connect(mButtonBox, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(mButtonBox, &QDialogButtonBox::rejected, this, &QDialog::reject);
    connect(mCacheFolderEdit, &QLineEdit::textChanged, this, &SettingsDialog::onCacheFolderTextChanged);
    connect(mCachePolicyComboBox, QOverload<int>::of(&QComboBox::currentIndexChanged), this, &SettingsDialog::onCachePolicyChanged);
    connect(mCacheQuotaComboBox, QOverload<int>::of(&QComboBox::currentIndexChanged), this, &SettingsDialog::onCacheQuotaChanged);
    connect(mCacheCleanupIntervalComboBox, QOverload<int>::of(&QComboBox::currentIndexChanged), this, &SettingsDialog::onCleanupIntervalChanged);

    onCachePolicyChanged(mCachePolicyComboBox->currentIndex());
    onCacheQuotaChanged(mCacheQuotaComboBox->currentIndex());
    onCleanupIntervalChanged(mCacheCleanupIntervalComboBox->currentIndex());

    setLayout(mainLayout);
}

SettingsDialog::~SettingsDialog()
{
}

void SettingsDialog::setPlayerPath(const QString& path)
{
    mPlayerPathEdit->setText(path);
}

QString SettingsDialog::getPlayerPath() const
{
    return mPlayerPathEdit->text();
}

void SettingsDialog::setCacheFolder(const QString& path)
{
    mCacheFolderEdit->setText(path);
}

QString SettingsDialog::getCacheFolder() const
{
    return mCacheFolderEdit->text();
}

void SettingsDialog::setCameraModel(const QString& model)
{
    mCameraModelComboBox->setCurrentText(model);
}

QString SettingsDialog::getCameraModel() const
{
    return mCameraModelComboBox->currentText();
}

void SettingsDialog::setCachePolicyMode(const QString& mode)
{
    const int index = mCachePolicyComboBox->findData(mode);
    if (index >= 0) {
        mCachePolicyComboBox->setCurrentIndex(index);
    }
    onCachePolicyChanged(mCachePolicyComboBox->currentIndex());
}

QString SettingsDialog::getCachePolicyMode() const
{
    return mCachePolicyComboBox->currentData().toString();
}

void SettingsDialog::setCacheQuotaGb(double gb)
{
    const int index = mCacheQuotaComboBox->findData(gb);
    if (index >= 0) {
        mCacheQuotaComboBox->setCurrentIndex(index);
    } else {
        const int customIndex = mCacheQuotaComboBox->findData(-1.0);
        if (customIndex >= 0) {
            mCacheQuotaComboBox->setCurrentIndex(customIndex);
        }
        mCacheQuotaCustomEdit->setText(QString::number(gb, 'f', 2));
    }
    onCacheQuotaChanged(mCacheQuotaComboBox->currentIndex());
}

double SettingsDialog::getCacheQuotaGb() const
{
    const double preset = mCacheQuotaComboBox->currentData().toDouble();
    if (preset > 0.0) {
        return preset;
    }
    bool ok = false;
    const double custom = mCacheQuotaCustomEdit->text().toDouble(&ok);
    return ok && custom > 0.0 ? custom : 50.0;
}

void SettingsDialog::setCacheCleanupIntervalSeconds(int seconds)
{
    const int index = mCacheCleanupIntervalComboBox->findData(seconds);
    if (index >= 0) {
        mCacheCleanupIntervalComboBox->setCurrentIndex(index);
    } else {
        const int customIndex = mCacheCleanupIntervalComboBox->findData(-1);
        if (customIndex >= 0) {
            mCacheCleanupIntervalComboBox->setCurrentIndex(customIndex);
        }
        mCacheCleanupCustomEdit->setText(QString::number(seconds));
    }
    onCleanupIntervalChanged(mCacheCleanupIntervalComboBox->currentIndex());
}

int SettingsDialog::getCacheCleanupIntervalSeconds() const
{
    const int preset = mCacheCleanupIntervalComboBox->currentData().toInt();
    if (preset > 0) {
        return preset;
    }
    bool ok = false;
    const int custom = mCacheCleanupCustomEdit->text().toInt(&ok);
    return ok && custom > 0 ? custom : 10;
}

void SettingsDialog::setCacheFolderWarning(const QString& message)
{
    if (message.isEmpty()) {
        mCacheFolderWarningLabel->setText("");
        mCacheFolderWarningLabel->setVisible(false);
        if (mButtonBox) {
            mButtonBox->button(QDialogButtonBox::Ok)->setEnabled(true);
        }
    } else {
        mCacheFolderWarningLabel->setText(
            QString("<span style='color: #cc8b2c; font-size: 9pt;'>%1</span>").arg(message));
        mCacheFolderWarningLabel->setVisible(true);
        if (mButtonBox) {
            mButtonBox->button(QDialogButtonBox::Ok)->setEnabled(false);
        }
    }
}

void SettingsDialog::setDeleteOnUnmount(bool enabled)
{
    mDeleteOnUnmountCheckBox->setChecked(enabled);
}

bool SettingsDialog::getDeleteOnUnmount() const
{
    return mDeleteOnUnmountCheckBox->isChecked();
}

void SettingsDialog::onCacheFolderTextChanged(const QString& text)
{
    if (text.trimmed().isEmpty()) {
        setCacheFolderWarning("");
        return;
    }

    QStorageInfo storage(text);
    const QString fsType = QString::fromLatin1(storage.fileSystemType());
    if (!storage.isValid() || !storage.isReady()) {
        setCacheFolderWarning("Selected folder is not on a ready volume.");
        return;
    }
    if (fsType.compare("NTFS", Qt::CaseInsensitive) != 0) {
        setCacheFolderWarning(QString("Selected folder is on %1. DNG output must be on NTFS.")
                                  .arg(fsType.isEmpty() ? "unknown" : fsType));
        return;
    }

    setCacheFolderWarning("");
}

void SettingsDialog::onCachePolicyChanged(int index)
{
    Q_UNUSED(index)
    const QString mode = mCachePolicyComboBox->currentData().toString();
    const bool isQuota = mode == "quota";
    const bool isOff = mode == "off";
    mCacheQuotaComboBox->setEnabled(isQuota);
    mCacheQuotaCustomEdit->setEnabled(isQuota && mCacheQuotaComboBox->currentData().toDouble() < 0.0);
    mCacheCleanupIntervalComboBox->setEnabled(!isOff);
    mCacheCleanupCustomEdit->setEnabled(!isOff && mCacheCleanupIntervalComboBox->currentData().toInt() < 0);
}

void SettingsDialog::onCacheQuotaChanged(int index)
{
    Q_UNUSED(index)
    const bool isQuota = mCachePolicyComboBox->currentData().toString() == "quota";
    if (!isQuota) {
        mCacheQuotaCustomEdit->setEnabled(false);
        return;
    }
    const bool custom = mCacheQuotaComboBox->currentData().toDouble() < 0.0;
    mCacheQuotaCustomEdit->setEnabled(custom);
    if (!custom) {
        mCacheQuotaCustomEdit->setText(QString::number(mCacheQuotaComboBox->currentData().toDouble(), 'f', 2));
    }
}

void SettingsDialog::onCleanupIntervalChanged(int index)
{
    Q_UNUSED(index)
    const bool isOff = mCachePolicyComboBox->currentData().toString() == "off";
    if (isOff) {
        mCacheCleanupCustomEdit->setEnabled(false);
        return;
    }
    const bool custom = mCacheCleanupIntervalComboBox->currentData().toInt() < 0;
    mCacheCleanupCustomEdit->setEnabled(custom);
    if (!custom) {
        mCacheCleanupCustomEdit->setText(QString::number(mCacheCleanupIntervalComboBox->currentData().toInt()));
    }
}

void SettingsDialog::onBrowseCacheFolder()
{
    QString folderPath = QFileDialog::getExistingDirectory(
        this,
        tr("Select DNG Output Folder"),
        mCacheFolderEdit->text(),
        QFileDialog::ShowDirsOnly | QFileDialog::DontResolveSymlinks);

    if (!folderPath.isEmpty()) {
        QStorageInfo storage(folderPath);
        const QString fsType = QString::fromLatin1(storage.fileSystemType());
        if (!storage.isValid() || !storage.isReady()) {
            setCacheFolderWarning("Selected folder is not on a ready volume.");
            return;
        }
        if (fsType.compare("NTFS", Qt::CaseInsensitive) != 0) {
            setCacheFolderWarning(QString("Selected folder is on %1. DNG output must be on NTFS.")
                                      .arg(fsType.isEmpty() ? "unknown" : fsType));
            return;
        }

        mCacheFolderEdit->setText(folderPath);
        setCacheFolderWarning("");
    }
}

void SettingsDialog::onBrowsePlayerPath()
{
    QString playerPath = QFileDialog::getOpenFileName(
        this,
        tr("Select MotionCamPlayer.exe"),
        mPlayerPathEdit->text(),
        tr("MotionCamPlayer (MotionCamPlayer.exe);;Executable Files (*.exe)"));

    if (!playerPath.isEmpty()) {
        mPlayerPathEdit->setText(playerPath);
    }
}
