#pragma once

#include <QString>
#include <QByteArray>
#include <cstdint>
#include <vector>

namespace motioncam {
QString prepareFinalizeOutput(const QString& folderName, const QString& mode,
                              const QString& importPath);
QString finalizeTreeSelectionStatus();
QString legacyCustomFinalizeDirectory();
QString finalizeDirectoryPath(const QString& uri);
bool writeFinalizeOutputEntry(const QString& name,
                              const std::vector<uint8_t>& bytes);
void finishFinalizeOutput();
void cancelFinalizeOutput();
void requestDngSequenceFolderImport();
QString dngSequenceFolderImportStatus() ;
QString documentDisplayName(const QString& uri);
QString writeDocument(const QString& uri,const QByteArray& contents);
void scanDirectoryForSidecars(const QString& importRoot,
                              const QString& targetImportPath = {});
QString sidecarScanStatus();
QString sourceDirectoryForUri(const QString& uri);
void registerImportedClip(const QString& importPath, const QString& sourceUri);
bool installSeekableSourceLink(const QString& importPath, int descriptor);
bool reopenSeekableSourceLink(const QString& importPath);
void releaseSeekableSourceLink(const QString& importPath);
QString sourceDirectoryForImport(const QString& importPath);
QString writeSidecarToSource(const QString& importPath, const QString& name,
                             const QString& contents);
QString syncSidecarsForImport(const QString& importPath);
QString editSidecarInExternalApp(const QString& importPath,
                                 const QString& name);
}
