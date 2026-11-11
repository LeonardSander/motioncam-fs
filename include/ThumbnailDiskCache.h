#pragma once

#include <QBuffer>
#include <QFile>
#include <QFileInfo>
#include <QImage>
#include <QSaveFile>
#include <QScopeGuard>
#include <atomic>

namespace rawpreamp {
enum class ThumbnailCacheWrite { Saved, Full, Failed };

inline ThumbnailCacheWrite saveThumbnailCacheFile(
        const QString& path, const QImage& image, std::atomic<qint64>* used = nullptr,
        qint64 limit = 64LL * 1024 * 1024) {
    if (QFileInfo::exists(path)) return ThumbnailCacheWrite::Saved;
    QByteArray encoded;
    qint64 bytes = 0;
    if (used) {
        QBuffer buffer(&encoded);
        if (!buffer.open(QIODevice::WriteOnly) || !image.save(&buffer, "JPG", 80))
            return ThumbnailCacheWrite::Failed;
        bytes = encoded.size();
        if (used->fetch_add(bytes, std::memory_order_relaxed) > limit - bytes) {
            used->fetch_sub(bytes, std::memory_order_relaxed);
            return ThumbnailCacheWrite::Full;
        }
    }
    auto refund = qScopeGuard([used, bytes] {
        if (used) used->fetch_sub(bytes, std::memory_order_relaxed);
    });
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly) ||
        !(used ? file.write(encoded) == bytes : image.save(&file, "JPG", 80)) ||
        !file.commit()) return ThumbnailCacheWrite::Failed;
    refund.dismiss();
    return ThumbnailCacheWrite::Saved;
}

inline bool removeThumbnailCacheFile(const QString& path, std::atomic<qint64>* used = nullptr) {
    const qint64 bytes = used ? QFileInfo(path).size() : 0;
    if (!QFile::remove(path)) return false;
    if (used) used->fetch_sub(bytes, std::memory_order_relaxed);
    return true;
}
}
