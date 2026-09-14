#include "ArchiveImport.h"

#include <archive.h>
#include <archive_entry.h>

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QTemporaryDir>

#include <iostream>
#include <stdexcept>
#include <utility>
#include <vector>

namespace {
void createArchive(const QString& path,
                   const std::vector<std::pair<QString, QByteArray>>& files) {
    archive* output = archive_write_new();
    archive_write_set_format_7zip(output);
    if (archive_write_open_filename(output, QFile::encodeName(path).constData()) != ARCHIVE_OK)
        throw std::runtime_error(archive_error_string(output));
    for (const auto& [name, data] : files) {
        archive_entry* entry = archive_entry_new();
        archive_entry_set_pathname_utf8(entry, name.toUtf8().constData());
        archive_entry_set_filetype(entry, AE_IFREG);
        archive_entry_set_perm(entry, 0600);
        archive_entry_set_size(entry, data.size());
        archive_write_header(output, entry);
        archive_write_data(output, data.constData(), data.size());
        archive_entry_free(entry);
    }
    archive_write_close(output);
    archive_write_free(output);
}
}

int main() {
    QTemporaryDir directory;
    if (!directory.isValid()) return 1;
    const QString archivePath = QDir(directory.path()).absoluteFilePath("review.7z");
    createArchive(archivePath, {{"nested/ignored.mcraw", "nested"},
                                {"root-1.dng", "one"},
                                {"root-2.DNG", "two"},
                                {"notes.txt", "ignored"}});
    const auto result = motioncam::extractReviewArchive(archivePath);
    const QDir extracted(result.temporaryRoot);
    const bool ok = QFileInfo(result.sourcePath).isDir() &&
        QFileInfo::exists(extracted.absoluteFilePath("root-1.dng")) &&
        QFileInfo::exists(extracted.absoluteFilePath("root-2.DNG")) &&
        !QFileInfo::exists(extracted.absoluteFilePath("nested/ignored.mcraw")) &&
        result.sidecarPath == QDir(directory.path()).absoluteFilePath("review.json") &&
        result.gyroflowSidecarPath == QDir(directory.path()).absoluteFilePath("review_gyroflow.json");
    QDir(result.temporaryRoot).removeRecursively();
    if (!ok) {
        std::cerr << "archive selection or sidecar routing failed\n";
        return 1;
    }

    const QString corruptPath = QDir(directory.path()).absoluteFilePath("corrupt.7z");
    createArchive(corruptPath, {{"frame.dng", QByteArray(1024 * 1024, 'x')}});
    QFile corrupt(corruptPath);
    if (!corrupt.open(QIODevice::ReadWrite) || !corrupt.resize(corrupt.size() / 2)) return 1;
    corrupt.close();
    try {
        const auto unexpected = motioncam::extractReviewArchive(corruptPath);
        QDir(unexpected.temporaryRoot).removeRecursively();
        std::cerr << "truncated archive was accepted\n";
        return 1;
    } catch (const std::exception&) {
    }
    return 0;
}
