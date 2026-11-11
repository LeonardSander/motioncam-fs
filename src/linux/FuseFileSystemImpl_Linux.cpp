#include "DebugInstrumentation.h"
#define FUSE_USE_VERSION 31

#include "linux/FuseFileSystemImpl_Linux.h"
#include "linux/FuseSupport.h"

#include "CameraFrameMetadata.h"
#include "CameraMetadata.h"
#include "LRUCache.h"
#include "Utils.h"
#include "DNGDecoder.h"
#include "IVirtualFileSystem.h"
#include "VirtualFileSystemImpl_DNG.h"
#include "VirtualFileSystemImpl_NativeVideo.h"
#include "VirtualFileSystemImpl_MCRAW.h"

#include <BS_thread_pool.hpp>
#include <boost/algorithm/string/predicate.hpp>
#include <boost/filesystem.hpp>
#include <fuse3/fuse.h>
#include <motioncam/Decoder.hpp>
#include <QDir>
#include <QCoreApplication>
#include <QProcess>
#include <spdlog/sinks/rotating_file_sink.h>
#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cerrno>
#include <ctime>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <fcntl.h>
#include <iostream>
#include <limits>
#include <pwd.h>
#include <stdexcept>
#include <string>
#include <signal.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <unistd.h>
#include <thread>
#include <vector>

namespace fs = boost::filesystem;

namespace rawpreamp {
namespace {
constexpr auto CACHE_SIZE = 1024 * 1024 * 1024;
constexpr auto IO_THREADS = 4;
constexpr auto MAX_READ = 1024 * 1024;

int probeLegacyMount(const linux_fuse::MountRecord& mount) {
    if (mount.type != "fuse" || mount.source != "/dev/fuse") return -1;
    // Legacy versions did not label mounts. Only reclaim an endpoint that the
    // kernel explicitly reports as disconnected; a timeout is not proof of death.
    QProcess probe;
    probe.start(QCoreApplication::applicationFilePath(),
                {QStringLiteral("--probe-fuse-mount"), QString::fromStdString(mount.path)});
    if (!probe.waitForStarted(1000)) return -1;
    if (!probe.waitForFinished(2000)) {
        probe.kill();
        probe.waitForFinished(1000);
        return -1;
    }
    return probe.exitStatus() == QProcess::NormalExit ? probe.exitCode() : -1;
}

void recoverStaleMount(const std::string& path) {
    std::ifstream mountInfo("/proc/self/mountinfo");
    if (!mountInfo) throw std::runtime_error("Cannot inspect /proc/self/mountinfo");
    const auto expected = std::filesystem::absolute(path).lexically_normal().string();
    bool mounted = false;
    for (const auto& mount : linux_fuse::readMounts(mountInfo)) {
        if (mount.path != expected) continue;
        mounted = true;
        const auto pid = linux_fuse::ownerPid(mount);
        const bool deadProvider = pid && ::kill(*pid, 0) != 0 && errno == ESRCH;
        if (!linux_fuse::canRecoverMount(mount, deadProvider,
                                         deadProvider ? -1 : probeLegacyMount(mount)))
            throw std::runtime_error(
                "Mount destination is already in use: " + path +
                ". Unmount it explicitly if it belongs to a stopped application.");
    }
    if (!mounted) return;

    spdlog::warn("Recovering stale FUSE mount at {}", path);
    QProcess fusermount;
    fusermount.start(QStringLiteral("fusermount3"),
                     {QStringLiteral("-u"), QStringLiteral("-z"),
                      QString::fromStdString(path)});
    const bool finished = fusermount.waitForStarted(1000) && fusermount.waitForFinished(10000);
    if (!finished && fusermount.state() != QProcess::NotRunning) {
        fusermount.kill();
        fusermount.waitForFinished(1000);
    }
    if (!finished || fusermount.exitStatus() != QProcess::NormalExit || fusermount.exitCode() != 0) {
        throw std::runtime_error(
            "Failed to unmount stale FUSE mount " + path +
            " (fusermount3 exit code " + std::to_string(fusermount.exitCode()) + ")");
    }
    // A successful helper exit does not guarantee that the mount disappeared.
    // Never let mkpath or fuse_mount follow a still-mounted, possibly dead FUSE endpoint.
    std::ifstream remainingMounts("/proc/self/mountinfo");
    if (!remainingMounts) throw std::runtime_error("Cannot verify FUSE mount detachment");
    for (const auto& mount : linux_fuse::readMounts(remainingMounts)) {
        if (mount.path == expected)
            throw std::runtime_error("FUSE mount remains after detachment at " + path);
    }
}

std::string prepareMountPath(const std::string& source, const std::string& destination) {
    const auto absoluteSource = std::filesystem::absolute(source).lexically_normal();
    const auto absoluteDestination = std::filesystem::absolute(destination).lexically_normal();
    linux_fuse::checkSourcePath(absoluteSource, absoluteDestination);
    // Detach a known dead mount before canonicalization can stat its root.
    recoverStaleMount(absoluteDestination.string());
    const auto resolved = std::filesystem::weakly_canonical(absoluteDestination);
    linux_fuse::checkSourcePath(std::filesystem::weakly_canonical(absoluteSource), resolved);
    if (resolved != absoluteDestination) recoverStaleMount(resolved.string());
    return resolved.string();
}

void setupLogging() {
    try {
        const char* home = getenv("HOME");
        if (!home)
            home = getpwuid(getuid())->pw_dir;
        const std::string directory = std::string(home) + "/.local/state/rawpreamp";
        std::filesystem::create_directories(directory);
        std::vector<spdlog::sink_ptr> sinks;
        sinks.push_back(std::make_shared<spdlog::sinks::stdout_color_sink_mt>());
        sinks.push_back(std::make_shared<spdlog::sinks::rotating_file_sink_mt>(
            directory + "/fuse.log", 5 * 1024 * 1024, 3));
        spdlog::set_default_logger(std::make_shared<spdlog::logger>(
            "rawpreamp", sinks.begin(), sinks.end()));
#ifdef NDEBUG
        spdlog::set_level(spdlog::level::info);
#else
        spdlog::set_level(spdlog::level::debug);
#endif
        spdlog::flush_on(spdlog::level::info);
    } catch (const spdlog::spdlog_ex& error) {
        std::cerr << "Log initialization failed: " << error.what() << '\n';
    }
}

struct FuseContext {
    IVirtualFileSystem* fs;
    std::atomic_uint64_t nextFileHandle{0};
    std::atomic<std::time_t> mountTime;
    std::string backingDirectory;
};

FuseContext* context() {
    return static_cast<FuseContext*>(fuse_get_context()->private_data);
}
} // namespace

struct LinuxFuseSession {
    LinuxFuseSession(const std::string& source, const std::string& destination,
                     std::unique_ptr<IVirtualFileSystem> filesystem, bool projectFiles,
                     const RenderSettings& settings)
        : mSrcPath(source), mDstPath(destination), mFs(std::move(filesystem)), mSettings(settings) {
        if (projectFiles) start();
    }

    ~LinuxFuseSession() {
        stop();
        QDir().rmdir(QString::fromStdString(mDstPath));
    }

    void setProjectionEnabled(bool enabled) {
        if (enabled == (mFuse != nullptr)) return;
        if (!enabled) {
            stop();
            mSettings.projectedFileSizes = false;
            mFs->updateOptions(mSettings);
            if (!QDir().rmdir(QString::fromStdString(mDstPath)))
                spdlog::warn("Could not remove empty mount directory {}", mDstPath);
            return;
        }
        mDstPath = prepareMountPath(mSrcPath, mDstPath);
        if (!QDir().mkpath(QString::fromStdString(mDstPath)))
            throw std::runtime_error("Failed to create " + mDstPath);
        mSettings.projectedFileSizes = true;
        mFs->updateOptions(mSettings);
        start();
    }

    void stop() {
        if (mFuse) {
            fuse_exit(mFuse);
            fuse_unmount(mFuse);
        }
        if (mThread.joinable())
            mThread.join();
        if (mFuse)
            fuse_destroy(mFuse);
        mFuse = nullptr;
        mState = nullptr;
    }

    void updateOptions(const RenderSettings& settings) {
        mSettings = settings;
        mSettings.projectedFileSizes = mFuse != nullptr;
        mFs->updateOptions(mSettings);
        if (!mFuse) return;
        if (mState) {
            const auto previous = mState->mountTime.load();
            mState->mountTime.store(
                std::max(std::time(nullptr), previous + 1));
        }
        // Render settings alter file bytes and often their sizes. Invalidating
        // only the root leaves cached child attributes/pages behind.
        for (const auto& entry : mFs->listFiles("")) {
            const std::string path = "/" + entry.getFullPath().generic_string();
            fuse_invalidate_path(mFuse, path.c_str());
        }
        fuse_invalidate_path(mFuse, "/");
    }
    FileInfo getFileInfo() const { return mFs->getFileInfo(); }
    const std::string& sourcePath() const { return mSrcPath; }
    void finalize(const std::string& destination, bool jpegCompression,
                  const FinalizeOptions& options,
                  const std::function<bool(size_t, size_t, const std::string&)>& progress,
                  const std::function<void(const std::vector<uint8_t>&, Timestamp)>& fileReady,
                  bool writeFiles) {
        vfs::finalize(*mFs, destination, jpegCompression, options, progress, fileReady, writeFiles);
    }

private:
    static void* init(fuse_conn_info* connection, fuse_config* config) {
        // Resolve commonly probes a frame at random offsets, then reads it
        // sequentially.  Retain generated files in the kernel and allow 1 MiB
        // reads/readahead so the latter pattern stays out of userspace.
        config->kernel_cache = 1;
        config->auto_cache = 1;
        config->entry_timeout = 1;
        config->attr_timeout = 1;
        config->negative_timeout = 0;
        connection->max_write = MAX_READ;
        connection->max_readahead = MAX_READ;
        return fuse_get_context()->private_data;
    }
    static void destroy(void* privateData) {
        auto* state = static_cast<FuseContext*>(privateData);
        delete state;
    }
    static int getattr(const char* path, struct stat* statBuffer, fuse_file_info*) {
        const auto* state = context();
        const auto mountTime = state->mountTime.load();
        if (std::string(path) == "/") {
            linux_fuse::setReadOnlyAttributes(*statBuffer, true, 0, mountTime);
            return 0;
        }
        const auto entry = state->fs->findEntry(path);
        if (!entry) return -ENOENT;
        linux_fuse::setReadOnlyAttributes(
            *statBuffer, entry->type == DIRECTORY_ENTRY, entry->size, mountTime);
        return 0;
    }
    static int readdir(const char* path, void* buffer, fuse_fill_dir_t filler,
                       off_t, fuse_file_info*, fuse_readdir_flags) {
        if (std::string(path) != "/")
            return -ENOENT;
        // Without st_mode libfuse emits DT_UNKNOWN. Directory scanners may
        // use d_type directly instead of issuing a separate stat for each file.
        struct stat attributes{};
        attributes.st_mode = S_IFDIR | 0755;
        filler(buffer, ".", &attributes, 0, static_cast<fuse_fill_dir_flags>(0));
        filler(buffer, "..", &attributes, 0, static_cast<fuse_fill_dir_flags>(0));
        for (const auto& entry : context()->fs->listFiles("")) {
            attributes.st_mode = entry.type == DIRECTORY_ENTRY ? S_IFDIR | 0755 : S_IFREG | 0444;
            filler(buffer, entry.name.c_str(), &attributes, 0,
                   static_cast<fuse_fill_dir_flags>(0));
        }
        return 0;
    }
    static int statfs(const char*, struct statvfs* statistics) {
        // The generated files have no allocated volume of their own. Report
        // the containing filesystem rather than libfuse's zero-capacity default.
        // Query the parent, never the mount itself (which would recurse).
        if (::statvfs(context()->backingDirectory.c_str(), statistics) != 0)
            return -errno;
        statistics->f_flag |= ST_RDONLY;
        return 0;
    }
    static int open(const char* path, fuse_file_info* info) {
        if ((info->flags & O_ACCMODE) != O_RDONLY)
            return -EACCES;
        if (!context()->fs->findEntry(path))
            return -ENOENT;
        info->fh = ++context()->nextFileHandle;
        info->keep_cache = 1;
        return 0;
    }
    static int read(const char* path, char* buffer, size_t size, off_t offset,
                    fuse_file_info*) {
        const auto entry = context()->fs->findEntry(path);
        if (!entry)
            return -ENOENT;
        size_t bytesRead = 0;
        int readError = 0;
        context()->fs->readFile(
            *entry, static_cast<size_t>(offset), size, buffer,
            [&bytesRead, &readError](size_t count, int error) {
                bytesRead = count;
                readError = error;
            },
            false);
        if (readError != 0) {
            // The VFS uses -1 as a generic materialization failure, not EPERM.
            return readError == -1 || readError > 0 ? -EIO : readError;
        }
        if (bytesRead > static_cast<size_t>(std::numeric_limits<int>::max()))
            return -EOVERFLOW;
        return static_cast<int>(bytesRead);
    }
    void start() {
        fuse_operations operations{};
        operations.init = init;
        operations.destroy = destroy;
        operations.getattr = getattr;
        operations.readdir = readdir;
        operations.open = open;
        operations.read = read;
        operations.statfs = statfs;

        fuse_args args = FUSE_ARGS_INIT(0, nullptr);
        // fuse_parse_cmdline expects argv[0] even when the filesystem is
        // embedded in a GUI application.
        fuse_opt_add_arg(&args, "RawPreamp");
        fuse_opt_add_arg(&args, "-o");
        const std::string mountOptions =
            "ro,default_permissions,subtype=rawpreamp,fsname=RawPreamp@" +
            std::to_string(getpid());
        fuse_opt_add_arg(&args, mountOptions.c_str());
        auto* state = new FuseContext{
            mFs.get(), 0, std::time(nullptr),
            std::filesystem::absolute(mDstPath).parent_path().string()};
        mState = state;
        mFuse = fuse_new(&args, &operations, sizeof(operations), state);
        fuse_opt_free_args(&args);
        if (!mFuse) {
            mState = nullptr;
            delete state;
            throw std::runtime_error("Failed to create FUSE session");
        }
        if (fuse_mount(mFuse, mDstPath.c_str()) != 0) {
            fuse_destroy(mFuse);
            mFuse = nullptr;
            mState = nullptr;
            throw std::runtime_error("Failed to mount " + mDstPath);
        }
        mThread = std::thread([this] {
            // API 3.1 is available on all supported FUSE3 distributions;
            // clone_fd lets concurrent readers avoid a shared device FD.
            spdlog::info("FUSE loop exited with code {}", fuse_loop_mt(mFuse, 1));
        });
    }

    std::string mSrcPath;
    std::string mDstPath;
    std::unique_ptr<IVirtualFileSystem> mFs;
    RenderSettings mSettings;
    FuseContext* mState = nullptr;
    fuse* mFuse = nullptr;
    std::thread mThread;
};

FuseFileSystemImpl_Linux::FuseFileSystemImpl_Linux()
    : mNextMountId(0), mIoThreadPool(std::make_unique<BS::thread_pool>(IO_THREADS)),
      mProcessingThreadPool(std::make_unique<BS::thread_pool>()),
      mCache(std::make_unique<LRUCache>(CACHE_SIZE)) { setupLogging(); }

FuseFileSystemImpl_Linux::~FuseFileSystemImpl_Linux() {
    mMountedFiles.clear();
    mIoThreadPool->wait();
    mProcessingThreadPool->wait();
}

MountId FuseFileSystemImpl_Linux::mount(const RenderSettings& settings,
                                        const std::string& srcFile,
                                        const std::string& dstPath,
                                        bool projectFiles) {
    const auto mountStarted = rawpreamp::diagnosticNow();
    auto stageStarted = mountStarted;
    auto logStage = [&](const char* stage) {
        const auto now = rawpreamp::diagnosticNow();
        RAWPREAMP_DIAGNOSTIC_INFO("Mount timing [{}]: {} {:.1f} ms (total {:.1f} ms)", srcFile, stage,
                     std::chrono::duration<double, std::milli>(now - stageStarted).count(),
                     std::chrono::duration<double, std::milli>(now - mountStarted).count());
        stageStarted = now;
    };
    const fs::path sourcePath(srcFile);
    const std::string extension = sourcePath.extension().string();
    RenderSettings sourceSettings = settings;
    sourceSettings.projectedFileSizes = projectFiles;
    const std::string filename = sourcePath.filename().string();

    const std::string mountPath = projectFiles ? prepareMountPath(srcFile, dstPath) : dstPath;

    if (projectFiles) {
        logStage("backend stale-mount recovery");
        if (!QDir().mkpath(QString::fromStdString(mountPath)))
            throw std::runtime_error("Failed to create " + mountPath);
        logStage("mount directory creation");
    }

    const std::string baseName = fs::path(dstPath).filename().string();
    std::unique_ptr<IVirtualFileSystem> filesystem;
    if (boost::iequals(extension, ".mcraw") ||
        boost::iequals(extension, ".unspektra") ||
        boost::iequals(extension, ".mcpro")) {
        filesystem = std::make_unique<VirtualFileSystemImpl_MCRAW>(
            *mIoThreadPool, *mProcessingThreadPool, *mCache, sourceSettings,
            srcFile, baseName);
    } else if ((boost::iequals(extension, ".mov") ||
                boost::iequals(extension, ".mp4") ||
                boost::iequals(extension, ".mkv")) &&
               boost::icontains(filename, "NATIVE")) {
        filesystem = std::make_unique<VirtualFileSystemImpl_NativeVideo>(
            *mIoThreadPool, *mProcessingThreadPool, *mCache, sourceSettings,
            srcFile, baseName);
    } else if (boost::iequals(extension, ".dng") ||
               DNGDecoder::isDNGSequence(srcFile)) {
        filesystem = std::make_unique<VirtualFileSystemImpl_DNG>(
            *mIoThreadPool, *mProcessingThreadPool, *mCache, sourceSettings,
            srcFile, baseName);
    } else {
        throw std::runtime_error("Invalid format");
    }
    logStage("virtual filesystem construction");

    const MountId mountId = mNextMountId++;
    auto session = std::make_shared<LinuxFuseSession>(
        srcFile, mountPath, std::move(filesystem), projectFiles, sourceSettings);
    {
        std::lock_guard<std::mutex> lock(mMountedFilesMutex);
        mMountedFiles.emplace(mountId, std::move(session));
    }
    logStage(projectFiles ? "FUSE session creation/start" : "processing session creation");
    return mountId;
}
void FuseFileSystemImpl_Linux::unmount(MountId mountId) {
    std::shared_ptr<LinuxFuseSession> session;
    {
        std::lock_guard<std::mutex> lock(mMountedFilesMutex);
        const auto it = mMountedFiles.find(mountId);
        if (it == mMountedFiles.end()) return;
        session = std::move(it->second);
        mMountedFiles.erase(it);
        mPreviewRenderers.erase(mountId);
    }
    // Session shutdown may wait for FUSE or an in-flight user. Never hold the
    // global mount registry lock while doing that work.
    session.reset();
}
void FuseFileSystemImpl_Linux::setProjectionEnabled(MountId mountId, bool enabled) {
    std::shared_ptr<LinuxFuseSession> session;
    {
        std::lock_guard<std::mutex> lock(mMountedFilesMutex);
        if (const auto it = mMountedFiles.find(mountId); it != mMountedFiles.end())
            session = it->second;
    }
    if (session) session->setProjectionEnabled(enabled);
}
void FuseFileSystemImpl_Linux::updateOptions(MountId mountId, const RenderSettings& settings) {
    std::shared_ptr<LinuxFuseSession> session;
    {
        std::lock_guard<std::mutex> lock(mMountedFilesMutex);
        if (const auto it = mMountedFiles.find(mountId); it != mMountedFiles.end()) {
            session = it->second;
            mPreviewRenderers.erase(mountId);
        }
    }
    if (session) session->updateOptions(settings);
}
std::optional<FileInfo> FuseFileSystemImpl_Linux::getFileInfo(MountId mountId) {
    std::shared_ptr<LinuxFuseSession> session;
    {
        std::lock_guard<std::mutex> lock(mMountedFilesMutex);
        if (const auto it = mMountedFiles.find(mountId); it != mMountedFiles.end())
            session = it->second;
    }
    if (session) return session->getFileInfo();
    return std::nullopt;
}
void FuseFileSystemImpl_Linux::finalize(
    MountId mountId, const std::string& destination, bool jpegCompression,
    const FinalizeOptions& options,
    const std::function<bool(size_t, size_t, const std::string&)>& progress,
    const std::function<void(const std::vector<uint8_t>&, Timestamp)>& fileReady,
    bool writeFiles) {
    std::shared_ptr<LinuxFuseSession> session;
    {
        std::lock_guard<std::mutex> lock(mMountedFilesMutex);
        const auto it = mMountedFiles.find(mountId);
        if (it == mMountedFiles.end()) throw std::runtime_error("Mount not found");
        session = it->second;
    }
    session->finalize(destination, jpegCompression, options, progress, fileReady, writeFiles);
}
void FuseFileSystemImpl_Linux::renderPreview(
    MountId mountId, const RenderSettings& settings, const PreviewOptions& options,
    const std::function<bool(size_t, size_t, const std::string&)>& progress,
    const std::function<void(PreviewFrame&&)>& frameReady) {
    std::shared_ptr<PreviewRenderer> renderer;
    {
        std::lock_guard<std::mutex> lock(mMountedFilesMutex);
        const auto it = mMountedFiles.find(mountId);
        if (it == mMountedFiles.end()) throw std::runtime_error("Mount not found");
        auto& cached = mPreviewRenderers[mountId];
        if (!cached) {
            cached = std::make_shared<PreviewRenderer>(
                *mIoThreadPool, *mProcessingThreadPool, it->second->sourcePath(),
                "gallery-preview-" + std::to_string(mountId));
        }
        renderer = cached;
    }
    renderer->render(settings, options, progress, frameReady);
}

void FuseFileSystemImpl_Linux::releasePreviewResources(MountId mountId) {
    std::lock_guard<std::mutex> lock(mMountedFilesMutex);
    mPreviewRenderers.erase(mountId);
}
} // namespace rawpreamp
