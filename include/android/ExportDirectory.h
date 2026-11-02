#pragma once

#include <QString>
#include <QByteArray>
#include <cstdint>
#include <vector>

namespace motioncam {
// Folder exports return the created folder name; direct exports return a
// collision-free basename shared by the video and sidecar files.
QString prepareFinalizeOutput(const QString& folderName, const QString& mode,
                              const QString& importPath, bool createSubfolder = true);
QString finalizeTreeSelectionStatus();
QString legacyCustomFinalizeDirectory();
QString finalizeDirectoryPath(const QString& uri);
bool writeFinalizeOutputEntry(const QString& name,
                              const std::vector<uint8_t>& bytes);
bool writeFinalizeOutputFile(const QString& name, const QString& path);
void finishFinalizeOutput();
void cancelFinalizeOutput();
void requestDngSequenceFolderImport();
QString dngSequenceFolderImportStatus() ;
QString documentDisplayName(const QString& uri);
QString writeDocument(const QString& uri,const QByteArray& contents);
void scanDirectoryForSidecars(const QString& importRoot,
                              const QString& targetImportPath = {});
QString sidecarScanStatus();
QString registerImportedClip(const QString& importPath, const QString& sourceUri);
QString refreshImportedSource(const QString& importPath);
bool sameImportedSource(const QString& first, const QString& second);
QString importDisplayName(const QString& importPath);
QString importSourceLocation(const QString& importPath);
QString sourceDirectoryForImport(const QString& importPath);
QString writeSidecarToSource(const QString& importPath, const QString& name,
                             const QString& contents);
QString syncSidecarsForImport(const QString& importPath);
QString editSidecarInExternalApp(const QString& importPath,
                                 const QString& name);
}
