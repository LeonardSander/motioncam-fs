#include "ArchiveImport.h"
#include "SourceInput.h"
#include <array>
#include <cstring>
#include <clocale>
#include <future>
#include <zlib.h>

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
int failedCheck(int line) {
    std::cerr << "Archive import regression at line " << line << "\n";
    return 1;
}

void createArchive(const QString& path,
                   const std::vector<std::pair<QString, QByteArray>>& files, const char* compression = nullptr, bool zip64 = false) {
    archive* output = archive_write_new();
    const auto check = [&](int status) {
        if (status != ARCHIVE_OK) {
            const auto* error = archive_error_string(output);
            throw std::runtime_error(path.toStdString() + ": " + (error ? error : "archive writer failed"));
        }
    };
    if (path.endsWith(".zip")) {
        check(archive_write_set_format_zip(output));
        check(archive_write_set_format_option(output, "zip", "hdrcharset", "UTF-8"));
        if (zip64 && archive_write_set_format_option(output, "zip", "zip64", "1") != ARCHIVE_OK)
            throw std::runtime_error(archive_error_string(output));
        if (compression && archive_write_set_format_option(output, "zip", "compression", compression) != ARCHIVE_OK)
            throw std::runtime_error(archive_error_string(output));
    } else check(archive_write_set_format_7zip(output));
    if (archive_write_open_filename(output, QFile::encodeName(path).constData()) != ARCHIVE_OK)
        throw std::runtime_error(archive_error_string(output));
    for (const auto& [name, data] : files) {
        archive_entry* entry = archive_entry_new();
        archive_entry_set_pathname_utf8(entry, name.toUtf8().constData());
        archive_entry_set_filetype(entry, AE_IFREG);
        archive_entry_set_perm(entry, 0600);
        archive_entry_set_size(entry, data.size());
        check(archive_write_header(output, entry));
        if (archive_write_data(output, data.constData(), data.size()) != data.size())
            throw std::runtime_error(path.toStdString() + ": could not write fixture data");
        archive_entry_free(entry);
    }
    check(archive_write_close(output));
    archive_write_free(output);
}

// Construct names independently of libarchive's locale-dependent writer.
void createNamedZip(const QString& path, const QByteArray& rawName,
                    const QByteArray& unicodeName = {}, bool validUnicodeCrc = true) {
    QByteArray bytes;
    const auto word = [&](uint16_t value) {
        bytes.append(char(value)); bytes.append(char(value >> 8));
    };
    const auto dword = [&](uint32_t value) {
        word(uint16_t(value)); word(uint16_t(value >> 16));
    };
    const QByteArray payload("frame");
    const auto crc = crc32(0, reinterpret_cast<const Bytef*>(payload.constData()), payload.size());
    dword(0x04034b50); word(20); word(0); word(0); word(0); word(0);
    dword(crc); dword(payload.size()); dword(payload.size()); word(rawName.size()); word(0);
    bytes += rawName; bytes += payload;
    const auto directory = bytes.size();
    dword(0x02014b50); word(20); word(20); word(0); word(0); word(0); word(0);
    dword(crc); dword(payload.size()); dword(payload.size()); word(rawName.size());
    word(unicodeName.isEmpty() ? 0 : 9 + unicodeName.size());
    word(0); word(0); word(0); dword(0); dword(0);
    bytes += rawName;
    if (!unicodeName.isEmpty()) {
        word(0x7075); word(5 + unicodeName.size()); bytes.append(char(1));
        dword(crc32(0, reinterpret_cast<const Bytef*>(rawName.constData()), rawName.size()) ^ (validUnicodeCrc ? 0 : 1));
        bytes += unicodeName;
    }
    const auto directorySize = bytes.size() - directory;
    dword(0x06054b50); word(0); word(0); word(1); word(1);
    dword(directorySize); dword(directory); word(0);
    QFile output(path);
    if (!output.open(QIODevice::WriteOnly) || output.write(bytes) != bytes.size())
        throw std::runtime_error("Could not write named ZIP fixture");
}

std::string readSource(const QString& path) {
    motioncam::SourceInputStream input(path.toStdString(), std::ios::binary);
    if (!input) throw std::runtime_error("Could not open source: " + path.toStdString());
    const std::string bytes(std::istreambuf_iterator<char>(input), {});
    if (input.bad()) throw std::runtime_error("Could not read source");
    return bytes;
}

} // namespace

int main() {
    // This console test has no QCoreApplication to initialize the locale.
    // libarchive's Unicode ZIP fixtures require a UTF-8 native character set.
    if (!std::setlocale(LC_CTYPE, "C.UTF-8") &&
        !std::setlocale(LC_CTYPE, "en_US.UTF-8") &&
        !std::setlocale(LC_CTYPE, ".UTF-8"))
        throw std::runtime_error("Could not initialize a UTF-8 locale for archive fixtures");
    QTemporaryDir directory;
    if (!directory.isValid()) return failedCheck(__LINE__);
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
    motioncam::removeReviewArchive(result.temporaryRoot);
    if (!ok) {
        std::cerr << "archive selection or sidecar routing failed\n";
        return failedCheck(__LINE__);
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
        if (!zip.open(QIODevice::ReadWrite) || !zip.seek(zip.size() - 2)) return failedCheck(__LINE__);
        const char size[2] = {char(comment.size()), char(comment.size() >> 8)};
        if (zip.write(size, 2) != 2 || zip.write(comment) != comment.size()) return failedCheck(__LINE__);
        zip.close();
        const auto imported = motioncam::extractReviewArchive(zipPath);
        const QDir media(imported.sourcePath);
        QFile retainedComment(media.filePath(".mcpro24fps.txt"));
        const bool valid = retainedComment.open(QIODevice::ReadOnly) && retainedComment.readAll() == comment &&
            readSource(media.filePath("clip.frames.csv")) == csv.toStdString() &&
            readSource(media.filePath("clip-audio.wav")) == "audio" &&
            readSource(media.filePath("clip-00000000.DNG")) == "one" &&
            QFileInfo(media.filePath("clip-00000000.DNG")).size() == 0 &&
            QFileInfo::exists(media.filePath("clip-audio.wav")) &&
            QFileInfo::exists(media.filePath("clip-00000000.jsonl")) &&
            !QFileInfo::exists(media.filePath("nested/other.wav"));
        motioncam::removeReviewArchive(imported.temporaryRoot);
        if (!valid) { std::cerr << "ZIP companions or timing failed\n"; return failedCheck(__LINE__); }
    }

    for (bool unicodeExtra : {false, true}) {
        const auto zipPath = QDir(directory.path()).filePath(unicodeExtra ? "unicode-extra.zip" : "cp437.zip");
        const auto rawName = QByteArray("caf") + char(0x82) + "/frame.dng";
        const auto decoded = QString::fromUtf8(unicodeExtra ? "映像/frame.dng" : "café/frame.dng");
        createNamedZip(zipPath, rawName, unicodeExtra ? decoded.toUtf8() : QByteArray{});
        const auto imported = motioncam::extractReviewArchive(zipPath);
        const auto frame = QDir(imported.temporaryRoot).filePath(decoded);
        if (!motioncam::isZipSource(frame.toStdString()) || readSource(frame) != "frame") return failedCheck(__LINE__);
        motioncam::removeReviewArchive(imported.temporaryRoot);
        if (unicodeExtra) {
            createNamedZip(zipPath, rawName, decoded.toUtf8(), false);
            const auto fallback = motioncam::extractReviewArchive(zipPath);
            if (readSource(QDir(fallback.sourcePath).filePath("frame.dng")) != "frame" ||
                !fallback.sourcePath.normalized(QString::NormalizationForm_C).endsWith(QString::fromUtf8("café"))) return failedCheck(__LINE__);
            motioncam::removeReviewArchive(fallback.temporaryRoot);
        }
    }

    // Legacy names must also work through the compressed payload readers.
    for (const char* compression : {"store", "deflate", "zstd"}) {
        const auto zipPath = QDir(directory.path()).filePath(QString(compression) + "-legacy.zip");
        const QByteArray asciiName("cafe/frame.dng");
        const auto rawName = QByteArray("caf") + char(0x82) + "/frame.dng";
        createArchive(zipPath, {{QString::fromLatin1(asciiName), "frame"}, {"empty.dng", {}}}, compression);
        QFile archive(zipPath);
        if (!archive.open(QIODevice::ReadWrite)) return failedCheck(__LINE__);
        auto bytes = archive.readAll();
        bytes.replace(asciiName, rawName);
        if (!archive.seek(0) || archive.write(bytes) != bytes.size()) return failedCheck(__LINE__);
        archive.close();
        const auto imported = motioncam::extractReviewArchive(zipPath);
        // Root files take precedence over nested files, so test the nested entry
        // through registration independently of clip selection.
        const auto entries = motioncam::indexZipSource(zipPath.toStdString());
        QTemporaryDir root;
        motioncam::registerZipSource(zipPath.toStdString(), root.path().toStdString(), entries);
        if (readSource(QDir(root.path()).filePath(QString::fromUtf8("café/frame.dng"))) != "frame" ||
            readSource(QDir(imported.sourcePath).filePath("empty.dng")) != "") return failedCheck(__LINE__);
        motioncam::unregisterZipSource(root.path().toStdString());
        motioncam::removeReviewArchive(imported.temporaryRoot);
    }

    QByteArray payload(200000, Qt::Uninitialized);
    for (qsizetype i = 0; i < payload.size(); ++i) payload[i] = char(i * 37);
    for (const char* compression : {"store", "deflate", "zstd"}) {
        const auto zipPath = QDir(directory.path()).filePath(QString(compression) + "-seek.zip");
        createArchive(zipPath, {{"nested/clip-00000000.dng", payload}, {"nested\\clip-00000001.dng", payload},
                                {"nested/clip.frames.csv", "timing"}}, compression);
        const auto imported = motioncam::extractReviewArchive(zipPath);
        const auto frame = QDir(imported.sourcePath).filePath("clip-00000000.dng").toStdString();
        const auto check = [&] {
            motioncam::SourceInputStream input(frame, std::ios::binary | std::ios::ate);
            if (!input || input.tellg() != payload.size()) return false;
            for (const auto position : {65530, 100, 180000, 0}) {
                input.seekg(position);
                std::array<char, 101> part;
                input.read(part.data(), part.size());
                if (!input || input.tellg() != position + 101 ||
                    std::memcmp(part.data(), payload.constData() + position, part.size())) return false;
                input.seekg(-40, std::ios::cur);
                if (input.tellg() != position + 61) return false;
            }
            input.seekg(-8, std::ios::end);
            std::array<char, 9> last{};
            input.read(last.data(), last.size());
            return input.eof() && input.gcount() == 8 && !std::memcmp(last.data(), payload.constData() + payload.size() - 8, 8);
        };
        std::vector<std::future<bool>> workers;
        for (int i = 0; i < 4; ++i) workers.push_back(std::async(std::launch::async, check));
        for (auto& worker : workers) if (!worker.get()) { std::cerr << "ZIP seek/concurrency failed\n"; return failedCheck(__LINE__); }
        if (readSource(QString::fromStdString(frame)) != payload.toStdString() ||
            readSource(QDir(imported.sourcePath).filePath("clip-00000001.dng")) != payload.toStdString()) return failedCheck(__LINE__);
        // Changing the archive invalidates existing sources, including cached frames.
        QFile changed(zipPath);
        if (!changed.open(QIODevice::Append) || changed.write("x", 1) != 1) return failedCheck(__LINE__);
        changed.close();
        bool rejected = false;
        try { motioncam::SourceInputStream stale(frame); }
        catch (const std::runtime_error&) { rejected = true; }
        motioncam::removeReviewArchive(imported.temporaryRoot);
        if (!rejected) { std::cerr << "Modified ZIP source was accepted\n"; return failedCheck(__LINE__); }
    }

    // Metadata reads must not inflate the tail or allocate a full decoded
    // frame. A corrupt tail remains unread until a seek/read reaches it.
    for (const char* compression : {"deflate", "zstd"}) {
        const auto zipPath = QDir(directory.path()).filePath(QString(compression) + "-lazy.zip");
        const QByteArray large(16 * 1024 * 1024, 'x');
        createArchive(zipPath, {{"frame.dng", large}}, compression);
        const auto entry = motioncam::indexZipSource(zipPath.toStdString()).front();
        QFile archive(zipPath);
        if (!archive.open(QIODevice::ReadWrite) || !archive.seek(entry.localOffset + 26)) return failedCheck(__LINE__);
        const auto lengths = archive.read(4);
        if (lengths.size() != 4) return failedCheck(__LINE__);
        const auto* sizes = reinterpret_cast<const unsigned char*>(lengths.constData());
        const auto start = entry.localOffset + 30 + sizes[0] + (unsigned(sizes[1]) << 8) +
            sizes[2] + (unsigned(sizes[3]) << 8);
        const auto tail = start + entry.compressedSize - 1;
        if (!archive.seek(tail)) return failedCheck(__LINE__);
        const auto last = archive.read(1);
        if (last.size() != 1 || !archive.seek(tail) || archive.write(QByteArray(1, last[0] ^ 0x40)) != 1) return failedCheck(__LINE__);
        archive.close();
        const auto imported = motioncam::extractReviewArchive(zipPath);
        motioncam::SourceInputStream input(QDir(imported.sourcePath).filePath("frame.dng").toStdString(),
                                           std::ios::binary | std::ios::ate);
        if (!input || input.tellg() != large.size()) return failedCheck(__LINE__);
        input.seekg(0);
        std::array<char, 32> metadata{};
        input.read(metadata.data(), metadata.size());
        if (!input || std::memcmp(metadata.data(), large.constData(), metadata.size())) return failedCheck(__LINE__);
        // Exercise eviction and a backward seek that restarts the inflater.
        for (const auto position : {5 * 65536, 7 * 65536, 9 * 65536, 11 * 65536, 0}) {
            input.seekg(position);
            input.read(metadata.data(), metadata.size());
            if (!input || std::memcmp(metadata.data(), large.constData() + position, metadata.size())) return failedCheck(__LINE__);
        }
        input.seekg(-32, std::ios::end);
        input.read(metadata.data(), metadata.size());
        if (!input.bad()) { std::cerr << "Lazy ZIP reader accepted a corrupt tail\n"; return failedCheck(__LINE__); }
        motioncam::removeReviewArchive(imported.temporaryRoot);
    }

    {
        const auto zipPath = QDir(directory.path()).filePath("zip64.zip");
        createArchive(zipPath, {{"nested/café-00000000.dng", payload}}, "deflate", true);
        const auto imported = motioncam::extractReviewArchive(zipPath);
        const auto path = QDir(imported.sourcePath).filePath(QString::fromUtf8("café-00000000.dng"));
        if (readSource(path) != payload.toStdString() || QFileInfo(path).size() != 0) return failedCheck(__LINE__);
        motioncam::removeReviewArchive(imported.temporaryRoot);
    }
    for (const char* compression : {"store", "deflate", "zstd"}) {
        const auto zipPath = QDir(directory.path()).filePath(QString(compression) + "-bad-crc.zip");
        createArchive(zipPath, {{"frame.dng", payload}}, compression);
        const auto entry = motioncam::indexZipSource(zipPath.toStdString()).front();
        QFile file(zipPath);
        if (!file.open(QIODevice::ReadWrite) || !file.seek(entry.localOffset + 26)) return failedCheck(__LINE__);
        const auto lengths = file.read(4);
        if (lengths.size() != 4) return failedCheck(__LINE__);
        const auto* sizes = reinterpret_cast<const unsigned char*>(lengths.constData());
        const auto data = entry.localOffset + 30 + sizes[0] + (unsigned(sizes[1]) << 8) +
            sizes[2] + (unsigned(sizes[3]) << 8);
        const auto position = data + entry.compressedSize / 2;
        if (!file.seek(position)) return failedCheck(__LINE__);
        const auto byte = file.read(1);
        if (byte.size() != 1 || !file.seek(position) || file.write(QByteArray(1, byte[0] ^ 0x40)) != 1) return failedCheck(__LINE__);
        file.close();
        const auto imported = motioncam::extractReviewArchive(zipPath);
        bool rejected = false;
        try { readSource(QDir(imported.sourcePath).filePath("frame.dng")); }
        catch (const std::runtime_error&) { rejected = true; }
        motioncam::removeReviewArchive(imported.temporaryRoot);
        if (!rejected) { std::cerr << "Corrupt ZIP payload was accepted\n"; return failedCheck(__LINE__); }
    }

    {
        const auto zipPath = QDir(directory.path()).filePath("lifecycle.zip");
        createArchive(zipPath, {{"frame.dng", "frame"}}, "deflate");
        const auto first = motioncam::extractReviewArchive(zipPath);
        const auto second = motioncam::extractReviewArchive(zipPath);
        const auto firstFrame = QDir(first.sourcePath).filePath("frame.dng");
        const auto secondFrame = QDir(second.sourcePath).filePath("frame.dng");
        motioncam::unregisterZipSource(first.temporaryRoot.toStdString());
        if (motioncam::isZipSource(firstFrame.toStdString()) || readSource(secondFrame) != "frame") return failedCheck(__LINE__);
        if (!motioncam::removeReviewArchive(first.temporaryRoot) ||
            !motioncam::removeReviewArchive(first.temporaryRoot)) return failedCheck(__LINE__);
        if (readSource(secondFrame) != "frame") return failedCheck(__LINE__);
        motioncam::removeReviewArchive(second.temporaryRoot);

        // Invalid registrations must not publish any of their entries.
        QTemporaryDir invalidRoot;
        auto entries = motioncam::indexZipSource(zipPath.toStdString());
        auto unsafe = entries.front();
        unsafe.name = "../escape.dng";
        entries.push_back(unsafe);
        bool rejected = false;
        try { motioncam::registerZipSource(zipPath.toStdString(), invalidRoot.path().toStdString(), entries); }
        catch (const std::runtime_error&) { rejected = true; }
        if (!rejected || motioncam::isZipSource(QDir(invalidRoot.path()).filePath("frame.dng").toStdString())) return failedCheck(__LINE__);
    }
    {
        const auto zipPath = QDir(directory.path()).filePath("mismatched-header.zip");
        createArchive(zipPath, {{"frame.dng", payload}}, "store");
        const auto entry = motioncam::indexZipSource(zipPath.toStdString()).front();
        QFile file(zipPath);
        if (!file.open(QIODevice::ReadWrite) || !file.seek(entry.localOffset + 30) || file.write("x", 1) != 1) return failedCheck(__LINE__);
        file.close();
        const auto imported = motioncam::extractReviewArchive(zipPath);
        bool rejected = false;
        try { readSource(QDir(imported.sourcePath).filePath("frame.dng")); }
        catch (const std::runtime_error&) { rejected = true; }
        motioncam::removeReviewArchive(imported.temporaryRoot);
        if (!rejected) { std::cerr << "Mismatched ZIP local header was accepted\n"; return failedCheck(__LINE__); }
    }

    const QString corruptPath = QDir(directory.path()).absoluteFilePath("corrupt.7z");
    createArchive(corruptPath, {{"frame.dng", QByteArray(1024 * 1024, 'x')}});
    QFile corrupt(corruptPath);
    if (!corrupt.open(QIODevice::ReadWrite) || !corrupt.resize(corrupt.size() / 2)) return failedCheck(__LINE__);
    corrupt.close();
    try {
        const auto unexpected = motioncam::extractReviewArchive(corruptPath);
        motioncam::removeReviewArchive(unexpected.temporaryRoot);
        std::cerr << "truncated archive was accepted\n";
        return failedCheck(__LINE__);
    } catch (const std::exception&) {
    }
    return 0;
}
