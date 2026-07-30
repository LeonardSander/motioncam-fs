#define FUSE_USE_VERSION 31

#include "linux/FuseFileSystemImpl_Linux.h"

#include "LRUCache.h"
#include "DNGDecoder.h"
#include "IVirtualFileSystem.h"
#include "VirtualFileSystemImpl_DNG.h"
#include "VirtualFileSystemImpl_DirectLog.h"
#include "VirtualFileSystemImpl_MCRAW.h"

#include <BS_thread_pool.hpp>
#include <boost/algorithm/string/predicate.hpp>
#include <boost/filesystem.hpp>
#include <fuse3/fuse.h>
#include <QDir>
#include <spdlog/sinks/rotating_file_sink.h>
#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <ctime>
#include <cstring>
#include <filesystem>
#include <fcntl.h>
#include <iostream>
#include <pwd.h>
#include <stdexcept>
#include <string>
#include <sys/stat.h>
#include <unistd.h>
#include <thread>
#include <vector>

namespace fs = boost::filesystem;

namespace motioncam {
namespace {
constexpr auto CACHE_SIZE = 1024 * 1024 * 1024;
constexpr auto IO_THREADS = 4;
constexpr auto MAX_READ = 1024 * 1024;

void setupLogging() {
    try {
        const char* home = getenv("HOME");
        if (!home)
            home = getpwuid(getuid())->pw_dir;
        const std::string directory = std::string(home) + "/.local/state/motioncam-fuse";
        std::filesystem::create_directories(directory);
        std::vector<spdlog::sink_ptr> sinks;
        sinks.push_back(std::make_shared<spdlog::sinks::stdout_color_sink_mt>());
        sinks.push_back(std::make_shared<spdlog::sinks::rotating_file_sink_mt>(
            directory + "/fuse.log", 5 * 1024 * 1024, 3));
        spdlog::set_default_logger(std::make_shared<spdlog::logger>(
            "motioncam-fuse", sinks.begin(), sinks.end()));
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
};

FuseContext* context() {
    return static_cast<FuseContext*>(fuse_get_context()->private_data);
}
} // namespace

struct LinuxFuseSession {
    LinuxFuseSession(const std::string& source, const std::string& destination,
                     std::unique_ptr<IVirtualFileSystem> filesystem)
        : mDstPath(destination), mFs(std::move(filesystem)) { start(); }

    ~LinuxFuseSession() {
        if (mFuse) {
            fuse_exit(mFuse);
            fuse_unmount(mFuse);
        }
        if (mThread.joinable())
            mThread.join();
        if (mFuse)
            fuse_destroy(mFuse);
        QDir().rmdir(QString::fromStdString(mDstPath));
    }

    void updateOptions(const RenderSettings& settings) {
        mFs->updateOptions(settings);
        if (mState) {
            const auto previous = mState->mountTime.load();
            mState->mountTime.store(
                std::max(std::time(nullptr), previous + 1));
        }
        // Render settings alter file bytes, so discard old kernel page cache.
        fuse_invalidate_path(mFuse, "/");
    }
    FileInfo getFileInfo() const { return mFs->getFileInfo(); }

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
        std::memset(statBuffer, 0, sizeof(*statBuffer));
        const auto* state = context();
        const auto mountTime = state->mountTime.load();
        statBuffer->st_atime = mountTime;
        statBuffer->st_mtime = mountTime;
        statBuffer->st_ctime = mountTime;
        if (std::string(path) == "/") {
            statBuffer->st_mode = S_IFDIR | 0755;
            statBuffer->st_nlink = 2;
            return 0;
        }
        const auto entry = state->fs->findEntry(path);
        if (!entry)
            return -ENOENT;
        statBuffer->st_mode = entry->type == DIRECTORY_ENTRY ? S_IFDIR | 0755 : S_IFREG | 0444;
        statBuffer->st_nlink = entry->type == DIRECTORY_ENTRY ? 2 : 1;
        statBuffer->st_size = entry->type == DIRECTORY_ENTRY ? 4096 : entry->size;
        statBuffer->st_uid = getuid();
        statBuffer->st_gid = getgid();
        return 0;
    }
    static int readdir(const char* path, void* buffer, fuse_fill_dir_t filler,
                       off_t, fuse_file_info*, fuse_readdir_flags) {
        if (std::string(path) != "/")
            return -ENOENT;
        filler(buffer, ".", nullptr, 0, static_cast<fuse_fill_dir_flags>(0));
        filler(buffer, "..", nullptr, 0, static_cast<fuse_fill_dir_flags>(0));
        for (const auto& entry : context()->fs->listFiles(""))
            filler(buffer, entry.name.c_str(), nullptr, 0,
                   static_cast<fuse_fill_dir_flags>(0));
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
        return context()->fs->readFile(*entry, static_cast<size_t>(offset), size,
                                       buffer, [](auto, auto) {}, false);
    }
    void start() {
        fuse_operations operations{};
        operations.init = init;
        operations.destroy = destroy;
        operations.getattr = getattr;
        operations.readdir = readdir;
        operations.open = open;
        operations.read = read;

        fuse_args args = FUSE_ARGS_INIT(0, nullptr);
        // fuse_parse_cmdline expects argv[0] even when the filesystem is
        // embedded in a GUI application.
        fuse_opt_add_arg(&args, "MotionCamFuse");
        auto* state = new FuseContext{mFs.get(), 0, std::time(nullptr)};
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
            throw std::runtime_error("Failed to mount " + mDstPath);
        }
        mThread = std::thread([this] {
            // API 3.1 is available on all supported FUSE3 distributions;
            // clone_fd lets concurrent readers avoid a shared device FD.
            spdlog::info("FUSE loop exited with code {}", fuse_loop_mt(mFuse, 1));
        });
    }

    std::string mDstPath;
    std::unique_ptr<IVirtualFileSystem> mFs;
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
                                        const std::string& dstPath) {
    const fs::path sourcePath(srcFile);
    const std::string extension = sourcePath.extension().string();
    const std::string filename = sourcePath.filename().string();

    if (!QDir().mkpath(QString::fromStdString(dstPath)))
        throw std::runtime_error("Failed to create " + dstPath);

    const std::string baseName = fs::path(dstPath).filename().string();
    std::unique_ptr<IVirtualFileSystem> filesystem;
    if (boost::iequals(extension, ".mcraw")) {
        filesystem = std::make_unique<VirtualFileSystemImpl_MCRAW>(
            *mIoThreadPool, *mProcessingThreadPool, *mCache, settings,
            srcFile, baseName);
    } else if ((boost::iequals(extension, ".mov") ||
                boost::iequals(extension, ".mp4")) &&
               boost::icontains(filename, "NATIVE")) {
        filesystem = std::make_unique<VirtualFileSystemImpl_DirectLog>(
            *mIoThreadPool, *mProcessingThreadPool, *mCache, settings,
            srcFile, baseName);
    } else if (boost::iequals(extension, ".dng") ||
               DNGDecoder::isDNGSequence(srcFile)) {
        filesystem = std::make_unique<VirtualFileSystemImpl_DNG>(
            *mIoThreadPool, *mProcessingThreadPool, *mCache, settings,
            srcFile, baseName);
    } else {
        throw std::runtime_error("Invalid format");
    }

    const MountId mountId = mNextMountId++;
    mMountedFiles.emplace(
        mountId,
        std::make_unique<LinuxFuseSession>(
            srcFile, dstPath, std::move(filesystem)));
    return mountId;
}
void FuseFileSystemImpl_Linux::unmount(MountId mountId) { mMountedFiles.erase(mountId); }
void FuseFileSystemImpl_Linux::updateOptions(MountId mountId, const RenderSettings& settings) {
    if (const auto it = mMountedFiles.find(mountId); it != mMountedFiles.end())
        it->second->updateOptions(settings);
}
std::optional<FileInfo> FuseFileSystemImpl_Linux::getFileInfo(MountId mountId) {
    if (const auto it = mMountedFiles.find(mountId); it != mMountedFiles.end())
        return it->second->getFileInfo();
    return std::nullopt;
}
} // namespace motioncam
