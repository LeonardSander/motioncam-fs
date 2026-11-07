#include "ThumbnailDiskCache.h"
#include <QCoreApplication>
#include <QTemporaryDir>
#include <cassert>

int main(int argc, char** argv) {
    // Android console tests have no JVM; QCoreApplication initializes JNI.
#ifndef __ANDROID__
    QCoreApplication app(argc, argv);
#endif
    QTemporaryDir directory;
    assert(directory.isValid());
    const QString first = directory.filePath("first.jpg");
    const QString second = directory.filePath("second.jpg");
    QImage image(64, 32, QImage::Format_RGB32);
    image.fill(0xff3377aa);
    std::atomic<qint64> used{0};
    using Result = motioncam::ThumbnailCacheWrite;
    assert(motioncam::saveThumbnailCacheFile(first, image, &used) == Result::Saved);
    const qint64 bytes = QFileInfo(first).size();
    assert(bytes > 0 && used.load() == bytes);
    assert(QImage(first).size() == image.size());
    assert(motioncam::saveThumbnailCacheFile(first, image, &used) == Result::Saved);
    assert(used.load() == bytes);
    assert(motioncam::saveThumbnailCacheFile(second, image, &used, bytes) == Result::Full);
    assert(!QFileInfo::exists(second) && used.load() == bytes);
    assert(motioncam::saveThumbnailCacheFile(second, image, &used, bytes * 2) == Result::Saved);
    // Repeated invalidation must return space while preserving another clip's cache.
    for (int iteration = 0; iteration < 20; ++iteration) {
        assert(motioncam::removeThumbnailCacheFile(first, &used));
        assert(used.load() == bytes);
        assert(motioncam::saveThumbnailCacheFile(first, image, &used, bytes * 2) == Result::Saved);
        assert(used.load() == bytes * 2);
    }
    assert(motioncam::removeThumbnailCacheFile(first, &used));
    assert(motioncam::removeThumbnailCacheFile(second, &used));
    assert(used.load() == 0);
    assert(motioncam::saveThumbnailCacheFile(directory.filePath("missing/image.jpg"), image, &used)
           == Result::Failed);
    assert(used.load() == 0);
    assert(motioncam::saveThumbnailCacheFile(first, QImage(), &used) == Result::Failed);
    assert(used.load() == 0 && !QFileInfo::exists(first));
    assert(motioncam::saveThumbnailCacheFile(first, image) == Result::Saved);
    assert(motioncam::removeThumbnailCacheFile(first));
}
