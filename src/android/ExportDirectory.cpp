#include "android/ExportDirectory.h"

#include <QCoreApplication>
#include <QJniObject>
#include <QJniEnvironment>
#include <QFile>
#include <QFileInfo>
#include <QUrl>
#include <cerrno>
#include <fcntl.h>
#include <map>
#include <mutex>
#include <string>
#include <sys/stat.h>
#include <unistd.h>

namespace motioncam {
namespace {
std::mutex importDescriptorMutex;
std::map<QString, int> importDescriptors;

template<typename... Args>
void callActivity(const char* method, const Args&... args) {
    const auto activity = QNativeInterface::QAndroidApplication::context();
    if (activity.isValid()) activity.callMethod<void>(method, args...);
}

template<typename... Args>
QString activityString(const QString& fallback, const char* method, const Args&... args) {
    const auto activity = QNativeInterface::QAndroidApplication::context();
    return activity.isValid() ? activity.callMethod<QString>(method, args...) : fallback;
}
}

QString prepareFinalizeOutput(const QString& folderName, const QString& mode,
                              const QString& importPath) {
    return activityString(QStringLiteral("error:Android activity unavailable"),
                          "prepareFinalizeOutput", folderName, mode, importPath);
}

QString finalizeTreeSelectionStatus() {
    return activityString(QStringLiteral("error"), "finalizeTreeSelectionStatus");
}

QString finalizeDirectoryPath(const QString& uri) {
    return activityString(uri,"finalizeDirectoryPath",uri);
}

QString legacyCustomFinalizeDirectory() {
    return activityString(QString(), "legacyCustomFinalizeDirectory");
}

bool writeFinalizeOutputEntry(const QString& name,
                              const std::vector<uint8_t>& bytes) {
    const QJniObject activity = QNativeInterface::QAndroidApplication::context();
    if (!activity.isValid() || bytes.size() > static_cast<size_t>(INT32_MAX))
        return false;
    QJniEnvironment env;
    jobject payload = env->NewDirectByteBuffer(
        const_cast<uint8_t*>(bytes.data()), static_cast<jlong>(bytes.size()));
    if (!payload) {
        if (env->ExceptionCheck()) env->ExceptionClear();
        return false;
    }
    const auto entryName = QJniObject::fromString(name);
    // QJniObject::callMethod clears Java exceptions before returning. Use JNI
    // directly here so failed writes and renames reach the transaction caller.
    jclass type = env->GetObjectClass(activity.object());
    jmethodID method = type ? env->GetMethodID(type, "writeFinalizeOutputEntry",
        "(Ljava/lang/String;Ljava/nio/ByteBuffer;)V") : nullptr;
    if (method) env->CallVoidMethod(activity.object(), method,
                                    entryName.object<jstring>(), payload);
    const bool ok = method && !env->ExceptionCheck();
    if (env->ExceptionCheck()) env->ExceptionClear();
    if (type) env->DeleteLocalRef(type);
    env->DeleteLocalRef(payload);
    return ok;
}

void finishFinalizeOutput() {
    callActivity("finishFinalizeOutput");
}

void cancelFinalizeOutput() {
    callActivity("cancelFinalizeOutput");
}

void requestDngSequenceFolderImport() {
    callActivity("requestDngSequenceFolderImport");
}

QString dngSequenceFolderImportStatus() {
    return activityString(QStringLiteral("error:Android activity unavailable"), "dngSequenceFolderImportStatus");
}

QString documentDisplayName(const QString& uri) {
    return activityString({}, "displayNameForUri", uri);
}

QString writeDocument(const QString& uri,const QByteArray& contents) {
    const QJniObject activity=QNativeInterface::QAndroidApplication::context();
    if(!activity.isValid())return QStringLiteral("Android activity unavailable");
    const auto resolver=activity.callObjectMethod("getContentResolver","()Landroid/content/ContentResolver;");
    const auto document=QJniObject::callStaticObjectMethod("android/net/Uri","parse",
        "(Ljava/lang/String;)Landroid/net/Uri;",QJniObject::fromString(QUrl(uri).toString(QUrl::FullyEncoded)).object<jstring>());
    const auto mode=QJniObject::fromString("wt");
    auto descriptor=resolver.callObjectMethod("openFileDescriptor",
        "(Landroid/net/Uri;Ljava/lang/String;)Landroid/os/ParcelFileDescriptor;",
        document.object(),mode.object<jstring>());
    if(!descriptor.isValid())return QStringLiteral("Could not open the selected document for writing");
    const int fd=descriptor.callMethod<jint>("detachFd");
    if(fd<0)return QStringLiteral("Could not obtain the document descriptor");
    QFile file;
    if(!file.open(fd,QIODevice::WriteOnly,QFileDevice::AutoCloseHandle)){
        ::close(fd);return file.errorString();
    }
    if(file.write(contents)!=contents.size()||!file.flush())return file.errorString();
    file.close();return {};
}

void scanDirectoryForSidecars(const QString& importRoot,
                              const QString& targetImportPath) {
    callActivity("scanDirectoryForSidecars", importRoot, targetImportPath);
}

QString sidecarScanStatus() {
    return activityString(QStringLiteral("error:Android activity unavailable"), "sidecarScanStatus");
}

QString sourceDirectoryForUri(const QString& uri) {
    return activityString(QString(), "sourceDirectoryForUri", uri);
}

void registerImportedClip(const QString& importPath, const QString& sourceUri) {
    callActivity("registerImportedClip", importPath, sourceUri);
}

bool installSeekableSourceLink(const QString& importPath, int descriptor) {
    if (descriptor < 0) return false;
    if (::lseek(descriptor, 0, SEEK_SET) < 0 ||
        ::fcntl(descriptor, F_SETFD, FD_CLOEXEC) < 0) {
        ::close(descriptor);
        return false;
    }

    // Serialize publication with release so an old source cannot unlink a
    // newly installed descriptor. Keep the old link usable until rename.
    std::lock_guard lock(importDescriptorMutex);
    const QByteArray destination = QFile::encodeName(importPath);
    struct stat existing{};
    if (::lstat(destination.constData(), &existing) == 0) {
        if (!S_ISLNK(existing.st_mode)) {
            ::close(descriptor);
            return false;
        }
    }

    const QByteArray target = QByteArray("/proc/") +
        QByteArray::number(static_cast<qlonglong>(::getpid())) + "/fd/" +
        QByteArray::number(descriptor);
    const QByteArray temporary = destination + ".fd-" + QByteArray::number(descriptor);
    if (::symlink(target.constData(), temporary.constData()) != 0) {
        ::close(descriptor);
        return false;
    }
    if (::rename(temporary.constData(), destination.constData()) != 0) {
        ::unlink(temporary.constData());
        ::close(descriptor);
        return false;
    }

    const QString key = QFileInfo(importPath).absoluteFilePath();
    const auto existingFd = importDescriptors.find(key);
    if (existingFd != importDescriptors.end()) {
        ::close(existingFd->second);
        existingFd->second = descriptor;
    } else {
        importDescriptors.emplace(key, descriptor);
    }
    return true;
}

bool reopenSeekableSourceLink(const QString& importPath) {
    const QByteArray destination = QFile::encodeName(importPath);
    struct stat info{};
    const bool hasEntry = ::lstat(destination.constData(), &info) == 0;
    if (hasEntry && !S_ISLNK(info.st_mode)) return false;
    if (hasEntry) {
        const QString key = QFileInfo(importPath).absoluteFilePath();
        std::lock_guard lock(importDescriptorMutex);
        const auto retained = importDescriptors.find(key);
        if (retained != importDescriptors.end() && QFileInfo(importPath).exists())
            return true;
    }

    const auto activity = QNativeInterface::QAndroidApplication::context();
    if (!activity.isValid()) return false;
    const QString sourceUri = activity.callMethod<QString>(
        "sourceUriForImport", importPath);
    if (sourceUri.isEmpty()) return false;
    const int descriptor = activity.callMethod<jint>("openSourceUriFd", sourceUri);
    return installSeekableSourceLink(importPath, descriptor);
}

void releaseSeekableSourceLink(const QString& importPath) {
    const QString key = QFileInfo(importPath).absoluteFilePath();
    std::lock_guard lock(importDescriptorMutex);
    const auto retained = importDescriptors.find(key);
    if (retained == importDescriptors.end()) return;
    const int descriptor = retained->second;
    importDescriptors.erase(retained);
    const QByteArray destination = QFile::encodeName(importPath);
    struct stat info{};
    if (::lstat(destination.constData(), &info) == 0 && S_ISLNK(info.st_mode))
        ::unlink(destination.constData());
    ::close(descriptor);
}

QString sourceDirectoryForImport(const QString& importPath) {
    return activityString(QString(), "sourceDirectoryForImport", importPath);
}

QString writeSidecarToSource(const QString& importPath, const QString& name,
                             const QString& contents) {
    return activityString(QStringLiteral("Android activity unavailable"), "writeSidecarToSource", importPath, name, contents);
}

QString syncSidecarsForImport(const QString& importPath) {
    return activityString(QStringLiteral("Android activity unavailable"), "syncSidecarsForImport", importPath);
}

QString editSidecarInExternalApp(const QString& importPath,
                                 const QString& name) {
    return activityString(QStringLiteral("Android activity unavailable"), "editSidecarInExternalApp", importPath, name);
}
}
