#pragma once

#include <QString>
#include <stdexcept>

namespace rawpreamp {

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

// Prepares the first supported clip directory in a 7z or ZIP archive. DNG ZIPs
// read media directly from the archive through a metadata-only directory index;
// 7z and MCRAW archives are extracted. Files in the archive root take
// precedence; subdirectories are considered only when the
// root contains no MCRAW or DNG files.
ArchiveImportResult extractReviewArchive(const QString& archivePath);

// Releases ZIP source bindings and removes the archive's discovery/extraction
// directory after its readers have been closed.
bool removeReviewArchive(const QString& temporaryRoot);

}
