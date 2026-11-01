#include "android/ExportDirectory.h"
#include "SourceInput.h"

#include <QCoreApplication>
#include <QJniObject>
#include <QJniEnvironment>
#include <QFile>
#include <QFileInfo>
#include <QUrl>
#include <cerrno>
#include <fcntl.h>
#include <string>
#include <sys/stat.h>
#include <unistd.h>

namespace motioncam {
namespace {

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

QString registerImportedClip(const QString& importPath, const QString& sourceUri) {
    return activityString(QStringLiteral("Android activity unavailable"),
                          "registerImportedClip", importPath, sourceUri);
}

QString refreshImportedSource(const QString& importPath) {
    releaseSourceReadDescriptors(importPath.toStdString());
    return activityString(QStringLiteral("Android activity unavailable"),
                          "refreshImportedSource", importPath);
}

bool sameImportedSource(const QString& first, const QString& second) {
    const auto activity = QNativeInterface::QAndroidApplication::context();
    return activity.isValid() && activity.callMethod<jboolean>("sameImportedSource", first, second);
}

QString importDisplayName(const QString& importPath) {
    return activityString(QString(), "importDisplayName", importPath);
}

QString importSourceLocation(const QString& importPath) {
    return activityString(QStringLiteral("Source document"), "importSourceLocation", importPath);
}

QString sourceDirectoryForImport(const QString& importPath) {
    return activityString(QString(), "sourceDirectoryForImport", importPath);
}

QString writeSidecarToSource(const QString& importPath, const QString& name,
                             const QString& contents) {
    return activityString(QStringLiteral("Android activity unavailable"), "writeSidecarToSource", importPath, name, contents);
}

QString syncSidecarsForImport(const QString& importPath) {
    const QString result = activityString(QStringLiteral("Android activity unavailable"),
                                          "syncSidecarsForImport", importPath);
    // An explicit refresh must reopen replaced files even if the provider
    // omits timestamps or reports the same size and modification time.
    releaseSourceReadDescriptors(importPath.toStdString());
    return result;
}

QString editSidecarInExternalApp(const QString& importPath,
                                 const QString& name) {
    return activityString(QStringLiteral("Android activity unavailable"), "editSidecarInExternalApp", importPath, name);
}
}
