#include "ThumbnailDiskCache.h"
#include <QCoreApplication>
#include <QTemporaryDir>
#include <cassert>
#ifdef __ANDROID__
#include <QDir>
#include <QtPlugin>
#include <dlfcn.h>
#include <stdexcept>

namespace {
void registerConsoleJpegPlugin() {
    // Qt's Android dynamic loader queries Java's SDK version. Console tests
    // have no JVM, so load the real JPEG plugin through the native loader and
    // register its factory with Qt as a static plugin for this process.
    const QDir plugins(qEnvironmentVariable("QT_PLUGIN_PATH"));
    const auto files = plugins.entryList({"libplugins_imageformats_qjpeg_*.so"}, QDir::Files);
    if (files.size() != 1) throw std::runtime_error("Expected one Android JPEG plugin");
    void* handle = dlopen(plugins.filePath(files.front()).toUtf8().constData(), RTLD_NOW | RTLD_LOCAL);
    if (!handle) throw std::runtime_error(dlerror());
    auto instance = reinterpret_cast<QtPluginInstanceFunction>(dlsym(handle, "qt_plugin_instance"));
    auto metadata = reinterpret_cast<QtPluginMetaDataFunction>(dlsym(handle, "qt_plugin_query_metadata_v2"));
    if (!instance || !metadata) throw std::runtime_error("Invalid JPEG plugin exports");
    // Keep the handle loaded for Qt's static plugin lifetime.
    QCoreApplication::setLibraryPaths({});
    qRegisterStaticPluginFunction(QStaticPlugin(instance, metadata));
}
}
#endif

int main(int argc, char** argv) {
    // Android console tests have no JVM; QCoreApplication initializes JNI.
#ifndef __ANDROID__
    QCoreApplication app(argc, argv);
#else
    registerConsoleJpegPlugin();
#endif
    QTemporaryDir directory;
    assert(directory.isValid());
    const QString first = directory.filePath("first.jpg");
    const QString second = directory.filePath("second.jpg");
    QImage image(64, 32, QImage::Format_RGB32);
    image.fill(0xff3377aa);
    std::atomic<qint64> used{0};
    using Result = rawpreamp::ThumbnailCacheWrite;
    assert(rawpreamp::saveThumbnailCacheFile(first, image, &used) == Result::Saved);
    const qint64 bytes = QFileInfo(first).size();
    assert(bytes > 0 && used.load() == bytes);
    assert(QImage(first).size() == image.size());
    assert(rawpreamp::saveThumbnailCacheFile(first, image, &used) == Result::Saved);
    assert(used.load() == bytes);
    assert(rawpreamp::saveThumbnailCacheFile(second, image, &used, bytes) == Result::Full);
    assert(!QFileInfo::exists(second) && used.load() == bytes);
    assert(rawpreamp::saveThumbnailCacheFile(second, image, &used, bytes * 2) == Result::Saved);
    // Repeated invalidation must return space while preserving another clip's cache.
    for (int iteration = 0; iteration < 20; ++iteration) {
        assert(rawpreamp::removeThumbnailCacheFile(first, &used));
        assert(used.load() == bytes);
        assert(rawpreamp::saveThumbnailCacheFile(first, image, &used, bytes * 2) == Result::Saved);
        assert(used.load() == bytes * 2);
    }
    assert(rawpreamp::removeThumbnailCacheFile(first, &used));
    assert(rawpreamp::removeThumbnailCacheFile(second, &used));
    assert(used.load() == 0);
    assert(rawpreamp::saveThumbnailCacheFile(directory.filePath("missing/image.jpg"), image, &used)
           == Result::Failed);
    assert(used.load() == 0);
    assert(rawpreamp::saveThumbnailCacheFile(first, QImage(), &used) == Result::Failed);
    assert(used.load() == 0 && !QFileInfo::exists(first));
    assert(rawpreamp::saveThumbnailCacheFile(first, image) == Result::Saved);
    assert(rawpreamp::removeThumbnailCacheFile(first));
}
