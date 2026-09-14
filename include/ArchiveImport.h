#pragma once

#include <QString>
#include <QStringList>
#include <stdexcept>

namespace motioncam {

class InsufficientArchiveSpace : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

struct ArchiveImportResult {
    QString sourcePath;
    QString temporaryRoot;
    QString sidecarPath;
    QString gyroflowSidecarPath;
};

// Extracts the first supported clip directory in a 7z archive. Files in the
// archive root take precedence; subdirectories are considered only when the
// root contains no MCRAW or DNG files.
ArchiveImportResult extractReviewArchive(const QString& archivePath);

}
