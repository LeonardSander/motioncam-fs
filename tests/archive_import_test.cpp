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
                   const std::vector<std::pair<QString, QByteArray>>& files, const char* compression = nullptr) {
    archive* output = archive_write_new();
    if (path.endsWith(".zip")) {
        archive_write_set_format_zip(output);
        if (compression && archive_write_set_format_option(output, "zip", "compression", compression) != ARCHIVE_OK)
            throw std::runtime_error(archive_error_string(output));
    } else archive_write_set_format_7zip(output);
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

    for (const char* compression : {"store", "deflate", "zstd"}) {
        const QString zipPath = QDir(directory.path()).absoluteFilePath(QString(compression) + ".mcpro24fps.zip");
        const QByteArray csv = "name,sensor_timestamp_ns,slot\n"
            "clip-00000000.DNG,1708783772168520,0\n"
            "clip-00000002.DNG,1708783805495603,2\n";
        createArchive(zipPath, {{"clip-00000000.DNG", "one"}, {"clip-00000002.DNG", "two"},
                               {"clip-audio.wav", "audio"}, {"clip.frames.csv", csv},
                               {"clip-00000000.jsonl", "gyro metadata"},
                               {"nested/other.wav", "ignored"}, {"../escape.dng", "ignored"}}, compression);
        const QByteArray comment = "FrameRate:60.000000\nAudioOffsetUs:170922\nAppVersion:mcpro24fps 044";
        QFile zip(zipPath);
        if (!zip.open(QIODevice::ReadWrite) || !zip.seek(zip.size() - 2)) return 1;
        const char size[2] = {char(comment.size()), char(comment.size() >> 8)};
        if (zip.write(size, 2) != 2 || zip.write(comment) != comment.size()) return 1;
        zip.close();
        const auto imported = motioncam::extractReviewArchive(zipPath);
        const QDir media(imported.sourcePath);
        QFile retainedComment(media.filePath(".mcpro24fps.txt"));
        QFile retainedCsv(media.filePath("clip.frames.csv"));
        const bool valid = retainedComment.open(QIODevice::ReadOnly) && retainedComment.readAll() == comment &&
            retainedCsv.open(QIODevice::ReadOnly) && retainedCsv.readAll() == csv &&
            QFileInfo::exists(media.filePath("clip-audio.wav")) &&
            QFileInfo::exists(media.filePath("clip-00000000.jsonl")) &&
            !QFileInfo::exists(media.filePath("nested/other.wav"));
        QDir(imported.temporaryRoot).removeRecursively();
        if (!valid) { std::cerr << "ZIP companions or timing failed\n"; return 1; }
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
