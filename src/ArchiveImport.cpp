#include "ArchiveImport.h"

#include <archive.h>
#include <archive_entry.h>

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSaveFile>
#include <QStorageInfo>
#include <QUuid>

#include <algorithm>
#include <memory>
#include <limits>
#include <stdexcept>
#include <vector>

namespace motioncam {
namespace {
struct Entry {
    QString path;
    QString parent;
    bool mcraw = false;
    bool dng = false;
    int order = 0;
    qint64 size = 0;
};

using ArchivePtr = std::unique_ptr<archive, decltype(&archive_read_free)>;

ArchivePtr openArchive(const QString& path) {
    ArchivePtr input(archive_read_new(), archive_read_free);
    if (!input) throw std::runtime_error("Could not allocate 7z archive reader");
    archive_read_support_filter_all(input.get());
    archive_read_support_format_7zip(input.get());
    if (archive_read_open_filename(input.get(), QFile::encodeName(path).constData(), 1024 * 1024) != ARCHIVE_OK) {
        const char* detail = archive_error_string(input.get());
        throw std::runtime_error(detail ? detail : "Could not open 7z archive");
    }
    return input;
}

void requireCleanArchiveStatus(archive* input, int status) {
    if (status == ARCHIVE_EOF) return;
    const char* detail = archive_error_string(input);
    throw std::runtime_error(detail ? detail : "Could not read 7z archive");
}

QString safeEntryPath(const char* raw) {
    QString path = QString::fromUtf8(raw ? raw : "").replace('\\', '/');
    while (path.startsWith("./")) path.remove(0, 2);
    path = QDir::cleanPath(path);
    if (path.isEmpty() || path == "." || path.startsWith('/') ||
        path == ".." || path.startsWith("../") ||
        (path.size() >= 2 && path.at(1) == ':')) return {};
    return path;
}

QString externalSidecar(const QString& archivePath, const QString& suffix) {
    const QFileInfo archive(archivePath);
    return QDir(archive.absolutePath()).absoluteFilePath(archive.completeBaseName() + suffix);
}

}

ArchiveImportResult extractReviewArchive(const QString& archivePath) {
    std::vector<Entry> entries;
    {
        auto input = openArchive(archivePath);
        archive_entry* item = nullptr;
        int order = 0;
        int status = ARCHIVE_OK;
        while ((status = archive_read_next_header(input.get(), &item)) == ARCHIVE_OK) {
            const QString path = safeEntryPath(archive_entry_pathname_utf8(item));
            const QFileInfo info(path);
            const bool regular = archive_entry_filetype(item) == AE_IFREG;
            const bool mcraw = regular && info.suffix().compare("mcraw", Qt::CaseInsensitive) == 0;
            const bool dng = regular && info.suffix().compare("dng", Qt::CaseInsensitive) == 0;
            if (!path.isEmpty() && (mcraw || dng))
                entries.push_back({path, info.path() == "." ? QString() : info.path(),
                                   mcraw, dng, order, archive_entry_size(item)});
            ++order;
            archive_read_data_skip(input.get());
        }
        requireCleanArchiveStatus(input.get(), status);
    }
    if (entries.empty()) throw std::runtime_error("The archive contains no MCRAW or DNG files");

    QString selectedParent;
    auto root = std::find_if(entries.begin(), entries.end(), [](const Entry& e) { return e.parent.isEmpty(); });
    if (root == entries.end()) {
        auto selected = std::min_element(entries.begin(), entries.end(), [](const Entry& a, const Entry& b) {
            const int aDepth = a.parent.count('/') + 1;
            const int bDepth = b.parent.count('/') + 1;
            return aDepth != bDepth ? aDepth < bDepth : a.order < b.order;
        });
        selectedParent = selected->parent;
    }

    std::vector<Entry> selected;
    for (const auto& entry : entries)
        if (entry.parent == selectedParent) selected.push_back(entry);
    auto firstMcraw = std::find_if(selected.begin(), selected.end(), [](const Entry& e) { return e.mcraw; });
    if (firstMcraw != selected.end()) selected = {*firstMcraw};
    else selected.erase(std::remove_if(selected.begin(), selected.end(), [](const Entry& e) { return !e.dng; }), selected.end());

    quint64 decompressedBytes = 0;
    for (const auto& entry : selected) {
        if (entry.size < 0 || static_cast<quint64>(entry.size) >
                std::numeric_limits<quint64>::max() - decompressedBytes)
            throw std::runtime_error("Archive contains an invalid uncompressed size");
        decompressedBytes += static_cast<quint64>(entry.size);
    }
    constexpr quint64 reserveBytes = 5ULL * 1024 * 1024 * 1024;
    const QStorageInfo temporaryStorage(QDir::tempPath());
    if (!temporaryStorage.isValid() || !temporaryStorage.isReady())
        throw std::runtime_error("Could not determine free space for the temporary directory");
    const qint64 reportedAvailableBytes = temporaryStorage.bytesAvailable();
    if (reportedAvailableBytes < 0)
        throw std::runtime_error("Could not determine free space for the temporary directory");
    const quint64 availableBytes = static_cast<quint64>(reportedAvailableBytes);
    if (availableBytes < reserveBytes || decompressedBytes > availableBytes - reserveBytes) {
        throw InsufficientArchiveSpace(
            QString("Import needs %1 GiB of temporary space, but only %2 GiB can be used while leaving 5 GiB free")
                .arg(decompressedBytes / 1073741824.0, 0, 'f', 2)
                .arg(availableBytes > reserveBytes
                         ? (availableBytes - reserveBytes) / 1073741824.0 : 0.0,
                     0, 'f', 2).toStdString());
    }

    const QString temporaryRoot = QDir::temp().absoluteFilePath(
        "motioncam-fuse-7z-" + QUuid::createUuid().toString(QUuid::WithoutBraces));
    if (!QDir().mkpath(temporaryRoot)) throw std::runtime_error("Could not create archive staging directory");
    try {
        auto input = openArchive(archivePath);
        archive_entry* item = nullptr;
        size_t extractedCount = 0;
        int status = ARCHIVE_OK;
        while ((status = archive_read_next_header(input.get(), &item)) == ARCHIVE_OK) {
            const QString path = safeEntryPath(archive_entry_pathname_utf8(item));
            const auto wanted = std::find_if(selected.begin(), selected.end(), [&](const Entry& e) { return e.path == path; });
            if (wanted == selected.end()) { archive_read_data_skip(input.get()); continue; }
            const QString outputPath = QDir(temporaryRoot).absoluteFilePath(path);
            if (!QDir().mkpath(QFileInfo(outputPath).absolutePath()))
                throw std::runtime_error("Could not create archive output directory");
            QSaveFile output(outputPath);
            if (!output.open(QIODevice::WriteOnly)) throw std::runtime_error("Could not create extracted file");
            char buffer[1024 * 1024];
            for (;;) {
                const la_ssize_t count = archive_read_data(input.get(), buffer, sizeof(buffer));
                if (count == 0) break;
                if (count < 0) throw std::runtime_error(archive_error_string(input.get()));
                if (output.write(buffer, count) != count) throw std::runtime_error("Could not write extracted file");
            }
            if (!output.commit()) throw std::runtime_error("Could not finish extracted file");
            ++extractedCount;
        }
        requireCleanArchiveStatus(input.get(), status);
        if (extractedCount != selected.size())
            throw std::runtime_error("Archive ended before all selected media files were extracted");

        QString sourcePath;
        if (selected.size() == 1)
            sourcePath = QDir(temporaryRoot).absoluteFilePath(selected.front().path);
        else
            sourcePath = selectedParent.isEmpty() ? temporaryRoot
                                                  : QDir(temporaryRoot).absoluteFilePath(selectedParent);
        return {sourcePath, temporaryRoot,
                externalSidecar(archivePath, ".json"),
                externalSidecar(archivePath, "_gyroflow.json")};
    } catch (...) {
        QDir(temporaryRoot).removeRecursively();
        throw;
    }
}
}
