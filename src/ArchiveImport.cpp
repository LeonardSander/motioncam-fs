#include "ArchiveImport.h"
#include "SourceInput.h"

#include <archive.h>
#include <archive_entry.h>

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSaveFile>
#ifndef __ANDROID__
#include <QStorageInfo>
#else
#include <filesystem>
#endif
#include <QUuid>

#include <algorithm>
#include <cstring>
#include <iterator>
#include <limits>
#include <memory>
#include <stdexcept>
#include <vector>

namespace rawpreamp {
namespace {
struct Entry {
    QString path;
    QString parent;
    bool mcraw = false;
    bool dng = false;
    int order = 0;
    qint64 size = 0;
};

struct ArchiveCloser {
    FILE* file = nullptr;
    void operator()(archive* input) const {
        archive_read_free(input);
        if (file) std::fclose(file);
    }
};
using ArchivePtr = std::unique_ptr<archive, ArchiveCloser>;

ArchivePtr openArchive(const QString& path) {
    ArchivePtr input(archive_read_new());
    if (!input) throw std::runtime_error("Could not allocate archive reader");
    archive_read_support_filter_all(input.get());
    archive_read_support_format_7zip(input.get());
    archive_read_support_format_zip(input.get());
#ifdef __ANDROID__
    input.get_deleter().file = openSourceFile(path.toStdString());
#elif defined(_WIN32)
    input.get_deleter().file = _wfopen(reinterpret_cast<const wchar_t*>(path.utf16()), L"rb");
#else
    input.get_deleter().file = std::fopen(QFile::encodeName(path).constData(), "rb");
#endif
    // The fd reader provides seeking, which the 7z reader requires.
    // archive_read_open_FILE does not install a seek callback.
    if (!input.get_deleter().file ||
        archive_read_open_fd(input.get(),
#ifdef _WIN32
            _fileno(input.get_deleter().file),
#else
            fileno(input.get_deleter().file),
#endif
            65536) != ARCHIVE_OK) {
        const char* detail = archive_error_string(input.get());
        throw std::runtime_error(detail ? detail : "Could not open archive");
    }
    return input;
}

void requireCleanArchiveStatus(archive* input, int status, const QString& path) {
    if (status == ARCHIVE_EOF) return;
    const char* detail = archive_error_string(input);
    throw std::runtime_error(path.toStdString() + ": " + (detail ? detail : "Could not read archive"));
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

// The ZIP end record is always present, including in ZIP64 archives. Read only
// its bounded comment rather than scanning gigabytes of image payloads.
QByteArray zipComment(const QString& path) {
    if (!path.endsWith(".zip", Qt::CaseInsensitive)) return {};
    SourceInputStream file(path.toStdString(), std::ios::binary);
    if (!file) return {};
    constexpr std::streamoff endSize = 22, maxComment = 65535;
    file.seekg(0, std::ios::end);
    const std::streamoff size = file.tellg();
    if (size < endSize) return {};
    file.seekg(std::max<std::streamoff>(0, size - endSize - maxComment));
    const std::string bytes(std::istreambuf_iterator<char>(file), {});
    const auto tail = QByteArray::fromStdString(bytes);
    for (qint64 pos = tail.size() - endSize; pos >= 0; --pos) {
        if (std::memcmp(tail.constData() + pos, "PK\x05\x06", 4) != 0) continue;
        const auto* p = reinterpret_cast<const unsigned char*>(tail.constData() + pos);
        const unsigned length = p[20] | unsigned(p[21]) << 8;
        if (pos + endSize + length == tail.size()) return tail.mid(pos + endSize, length);
    }
    return {};
}

QString externalSidecar(const QString& archivePath, const QString& suffix) {
    const QFileInfo archive(archivePath);
    return QDir(archive.absolutePath()).absoluteFilePath(archive.completeBaseName() + suffix);
}

}

ArchiveImportResult extractReviewArchive(const QString& archivePath) {
    std::vector<Entry> entries;
    std::vector<Entry> companions;
    const auto comment = zipComment(archivePath);
    const bool zip = archivePath.endsWith(".zip", Qt::CaseInsensitive);
    const auto zipIndex = zip ? indexZipSource(archivePath.toStdString()) : std::vector<ZipSourceEntry>{};
    const auto addEntry = [&](const QString& path, bool regular, qint64 size, int order) {
        const QFileInfo info(path);
        const bool mcraw = regular && info.suffix().compare("mcraw", Qt::CaseInsensitive) == 0;
        const bool dng = regular && info.suffix().compare("dng", Qt::CaseInsensitive) == 0;
        const QString parent = info.path() == "." ? QString() : info.path();
        if (!path.isEmpty() && (mcraw || dng)) entries.push_back({path, parent, mcraw, dng, order, size});
        const QString suffix = info.suffix().toLower();
        if (!path.isEmpty() && regular &&
            (suffix == "wav" || suffix == "flac" || suffix == "opus" ||
             suffix == "aac" || suffix == "csv" || suffix == "jsonl" || suffix == "gcsv"))
            companions.push_back({path, parent, false, false, order, size});
    };
    if (zip) {
        int order = 0;
        for (const auto& entry : zipIndex)
            addEntry(safeEntryPath(entry.name.c_str()), entry.regular, static_cast<qint64>(entry.size), order++);
    } else {
        auto input = openArchive(archivePath);
        archive_entry* item = nullptr;
        int order = 0;
        int status = ARCHIVE_OK;
        while ((status = archive_read_next_header(input.get(), &item)) == ARCHIVE_OK) {
            addEntry(safeEntryPath(archive_entry_pathname_utf8(item)),
                archive_entry_filetype(item) == AE_IFREG && !archive_entry_symlink(item) && !archive_entry_hardlink(item),
                archive_entry_size(item), order++);
            archive_read_data_skip(input.get());
        }
        requireCleanArchiveStatus(input.get(), status, archivePath);
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
    const bool dngSequence = firstMcraw == selected.end();
    if (!dngSequence) selected = {*firstMcraw};
    else {
        selected.erase(std::remove_if(selected.begin(), selected.end(), [](const Entry& e) { return !e.dng; }), selected.end());
        for (const auto& entry : companions)
            if (entry.parent == selectedParent) selected.push_back(entry);
    }
    std::sort(selected.begin(), selected.end(), [](const Entry& a, const Entry& b) { return a.path < b.path; });
    if (std::adjacent_find(selected.begin(), selected.end(), [](const Entry& a, const Entry& b) {
            return a.path == b.path;
        }) != selected.end()) throw std::runtime_error("Archive contains duplicate file paths");

    const bool directZip = dngSequence && zip;
    if (!directZip) {
        quint64 decompressedBytes = 0;
        for (const auto& entry : selected) {
            if (entry.size < 0 || static_cast<quint64>(entry.size) >
                    std::numeric_limits<quint64>::max() - decompressedBytes)
                throw std::runtime_error("Archive contains an invalid uncompressed size");
            decompressedBytes += static_cast<quint64>(entry.size);
        }
        constexpr quint64 reserveBytes = 5ULL * 1024 * 1024 * 1024;
#ifdef __ANDROID__
        // QStorageInfo queries Android's Java runtime for APK mount data.
        // Temporary staging is a native path, so query it without JNI.
        std::error_code spaceError;
        const auto space = std::filesystem::space(QDir::tempPath().toStdString(), spaceError);
        if (spaceError || space.available == static_cast<std::uintmax_t>(-1))
            throw std::runtime_error("Could not determine free space for the temporary directory");
        const quint64 availableBytes = space.available;
#else
        const QStorageInfo temporaryStorage(QDir::tempPath());
        if (!temporaryStorage.isValid() || !temporaryStorage.isReady())
            throw std::runtime_error("Could not determine free space for the temporary directory");
        const qint64 reportedAvailableBytes = temporaryStorage.bytesAvailable();
        if (reportedAvailableBytes < 0)
            throw std::runtime_error("Could not determine free space for the temporary directory");
        const quint64 availableBytes = static_cast<quint64>(reportedAvailableBytes);
#endif
        if (availableBytes < reserveBytes || decompressedBytes > availableBytes - reserveBytes) {
            throw InsufficientArchiveSpace(
                QString("Import needs %1 GiB of temporary space, but only %2 GiB can be used while leaving 5 GiB free")
                    .arg(decompressedBytes / 1073741824.0, 0, 'f', 2)
                    .arg(availableBytes > reserveBytes
                             ? (availableBytes - reserveBytes) / 1073741824.0 : 0.0,
                         0, 'f', 2).toStdString());
        }
    }

    const QString temporaryRoot = QDir::temp().absoluteFilePath(
        "rawpreamp-archive-" + QUuid::createUuid().toString(QUuid::WithoutBraces));
    if (!QDir().mkpath(temporaryRoot)) throw std::runtime_error("Could not create archive staging directory");
    try {
        if (directZip) {
            std::vector<ZipSourceEntry> members;
            for (const auto& entry : zipIndex) {
                if (!entry.regular) continue;
                const auto path = safeEntryPath(entry.name.c_str());
                const auto wanted = std::lower_bound(selected.begin(), selected.end(), path,
                    [](const Entry& candidate, const QString& name) { return candidate.path < name; });
                if (wanted == selected.end() || wanted->path != path) continue;
                if ((entry.flags & (1 | 64)) || (entry.method != 0 && entry.method != 8 && entry.method != 93))
                    throw std::runtime_error("ZIP media must be unencrypted and use Store, Deflate, or Zstandard compression");
                if (wanted->size < 0 || entry.size != static_cast<uint64_t>(wanted->size))
                    throw std::runtime_error("ZIP directory contains inconsistent entry sizes");
                const QString outputPath = QDir(temporaryRoot).absoluteFilePath(path);
                if (!QDir().mkpath(QFileInfo(outputPath).absolutePath()))
                    throw std::runtime_error("Could not create ZIP index directory");
                QFile placeholder(outputPath);
                if (!placeholder.open(QIODevice::WriteOnly | QIODevice::NewOnly))
                    throw std::runtime_error("Could not create ZIP index entry");
                members.push_back(entry);
            }
            if (members.size() != selected.size())
                throw std::runtime_error("ZIP directory is missing selected media files");
            registerZipSource(archivePath.toStdString(), temporaryRoot.toStdString(), members);
        } else {
            auto input = openArchive(archivePath);
            archive_entry* item = nullptr;
            size_t extractedCount = 0;
            int status = ARCHIVE_OK;
            // Reuse heap storage across entries; worker stacks can be small on Windows.
            std::vector<char> buffer(1024 * 1024);
            while ((status = archive_read_next_header(input.get(), &item)) == ARCHIVE_OK) {
                const QString path = safeEntryPath(archive_entry_pathname_utf8(item));
                const auto wanted = std::lower_bound(selected.begin(), selected.end(), path,
                    [](const Entry& entry, const QString& key) { return entry.path < key; });
                if (wanted == selected.end() || wanted->path != path) { archive_read_data_skip(input.get()); continue; }
                const QString outputPath = QDir(temporaryRoot).absoluteFilePath(path);
                if (!QDir().mkpath(QFileInfo(outputPath).absolutePath()))
                    throw std::runtime_error("Could not create archive output directory");
                QSaveFile output(outputPath);
                if (!output.open(QIODevice::WriteOnly)) throw std::runtime_error("Could not create extracted file");
                qint64 extractedBytes = 0;
                for (;;) {
                    const la_ssize_t count = archive_read_data(input.get(), buffer.data(), buffer.size());
                    if (count == 0) break;
                    if (count < 0) throw std::runtime_error(archive_error_string(input.get()));
                    if (count > wanted->size - extractedBytes)
                        throw std::runtime_error("Archive entry exceeds its declared size");
                    extractedBytes += count;
                    if (output.write(buffer.data(), count) != count) throw std::runtime_error("Could not write extracted file");
                }
                if (extractedBytes != wanted->size)
                    throw std::runtime_error("Archive entry is shorter than its declared size");
                if (!output.commit()) throw std::runtime_error("Could not finish extracted file");
                ++extractedCount;
            }
            requireCleanArchiveStatus(input.get(), status, archivePath);
            if (extractedCount != selected.size())
                throw std::runtime_error("Archive ended before all selected media files were extracted");
        }

        const QString sourceDirectory = selectedParent.isEmpty() ? temporaryRoot
            : QDir(temporaryRoot).absoluteFilePath(selectedParent);
        if (dngSequence && comment.contains("AppVersion:mcpro24fps")) {
            QSaveFile metadata(QDir(sourceDirectory).absoluteFilePath(".mcpro24fps.txt"));
            if (!metadata.open(QIODevice::WriteOnly) || metadata.write(comment) != comment.size() ||
                !metadata.commit()) throw std::runtime_error("Could not retain mcpro24fps archive metadata");
        }
        const QString sourcePath = dngSequence ? sourceDirectory
            : QDir(temporaryRoot).absoluteFilePath(selected.front().path);
        return {sourcePath, temporaryRoot,
                externalSidecar(archivePath, ".json"),
                externalSidecar(archivePath, "_gyroflow.json")};
    } catch (...) {
        removeReviewArchive(temporaryRoot);
        throw;
    }
}

bool removeReviewArchive(const QString& temporaryRoot) {
    if (temporaryRoot.isEmpty()) return true;
    unregisterZipSource(temporaryRoot.toStdString());
    QDir directory(temporaryRoot);
    return !directory.exists() || directory.removeRecursively();
}
}
