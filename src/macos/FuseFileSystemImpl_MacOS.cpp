#include "macos/FuseFileSystemImpl_MacOS.h"
#include "VirtualFileSystemImpl_MCRAW.h"
#include "LRUCache.h"

#include <boost/algorithm/string/predicate.hpp>
#include <boost/filesystem.hpp>

#include <iostream>
#include <pwd.h>
#include <unistd.h>

#include <BS_thread_pool.hpp>
#include <fuse_t/fuse_t.h>
#include <QDir>

// Logging
#include <spdlog/spdlog.h>
#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/sinks/rotating_file_sink.h>

namespace fs = boost::filesystem;

namespace motioncam {

constexpr auto CACHE_SIZE = 1024 * 1024 * 1024; // 1 GB cache size
constexpr auto IO_THREADS = 4;

namespace {

std::string getLogDirectory() {
    std::string logPath;

    const char* home = getenv("HOME");
    if (!home) {
        // Fallback to getpwuid if HOME is not set
        struct passwd* pw = getpwuid(getuid());
        home = pw->pw_dir;
    }

    logPath = std::string(home) + "/Library/Logs/MotionCam Tools";

    // Create directory if it doesn't exist
    std::filesystem::create_directories(logPath);

    return logPath;
}

void setupLogging() {
    try {
        std::string logDir = getLogDirectory();
        std::string logFile = logDir + "/fuse.txt";

        std::vector<spdlog::sink_ptr> sinks;

        // Console sink
        sinks.push_back(std::make_shared<spdlog::sinks::stdout_color_sink_mt>());

        // Rotating file sink: max 5MB per file, keep 3 files
        sinks.push_back(std::make_shared<spdlog::sinks::rotating_file_sink_mt>(
            logFile, 1024 * 1024 * 5, 3));

        auto logger = std::make_shared<spdlog::logger>("multi_sink", sinks.begin(), sinks.end());
        spdlog::set_default_logger(logger);

#ifdef NDEBUG
        spdlog::set_level(spdlog::level::info);
#else
        spdlog::set_level(spdlog::level::debug);
#endif

        spdlog::flush_on(spdlog::level::info);
    }
    catch (const spdlog::spdlog_ex& ex) {
        std::cerr << "Log initialization failed: " << ex.what() << std::endl;
    }
}

} // namespace

//

struct FuseContext {
    VirtualFileSystemImpl_MCRAW* fs;
    std::atomic_int nextFileHandle;
};

class Session {
public:
    Session(const std::string& srcFile, const std::string& dstPath, VirtualFileSystemImpl_MCRAW* fs);
    ~Session();

    void updateOptions(FileRenderOptions options, int draftScale);

private:
    void init(VirtualFileSystemImpl_MCRAW* fs);

    void fuseMain(struct fuse_chan* ch, struct fuse* fuse);

    static void* fuseInit(struct fuse_conn_info* conn);
    static void fuseDestroy(void* privateData);
    static int fuseRelease(const char* path, struct fuse_file_info* fi);
    static int fuseGetattr(const char* path, struct stat* stbuf);
    static int fuseReaddir(const char* path, void* buf, fuse_fill_dir_t filler, off_t offset, struct fuse_file_info* fi);
    static int fuseOpen(const char* path, struct fuse_file_info* fi);
    static int fuseRead(const char* path, char* buf, size_t size, off_t offset, struct fuse_file_info* fi);

private:
    std::string mSrcFile;
    std::string mDstPath;
    std::unique_ptr<std::thread> mThread;
    VirtualFileSystemImpl_MCRAW* mFs;
    struct fuse_chan* mFuseCh;
    struct fuse* mFuse;
};


Session::Session(const std::string& srcFile, const std::string& dstPath, VirtualFileSystemImpl_MCRAW* fs) :
    mSrcFile(srcFile),
    mDstPath(dstPath),
    mFs(fs),
    mFuseCh(nullptr),
    mFuse(nullptr)
{
    init(fs);
}

Session::~Session() {
    auto mountPoint = mDstPath;
    auto channel = mFuseCh;

    if(channel) {
        std::thread([mountPoint, channel]() {
            spdlog::debug("Unmounting {}", mountPoint);

            fuse_unmount(mountPoint.c_str(), channel);

            spdlog::debug("Umounted {}", mountPoint);
        }).detach();
    }

    mFuseCh = nullptr;
    mFuse = nullptr;

    if(mThread && mThread->joinable())
        mThread->join();

    QDir dst;

    if(!dst.rmdir(mDstPath.c_str()))
        spdlog::warn("Failed to remove {}", mDstPath);

    spdlog::debug("Exiting session for {}", mSrcFile);
}

void Session::init(VirtualFileSystemImpl_MCRAW* fs) {
    // FUSE operations structure
    struct fuse_operations ops = {};

    ops.init = fuseInit;
    ops.destroy = fuseDestroy;
    ops.release = fuseRelease;
    ops.getattr = fuseGetattr;
    ops.readdir = fuseReaddir;
    ops.open = fuseOpen;
    ops.read = fuseRead;

    struct fuse_args args = FUSE_ARGS_INIT(0, nullptr);

    // Read only
    fuse_opt_add_arg(&args, "-r");
    fuse_opt_add_arg(&args, "-o");
    fuse_opt_add_arg(&args, "nobrowse");
    fuse_opt_add_arg(&args, "-o");
    fuse_opt_add_arg(&args, "rwsize=262144");
    fuse_opt_add_arg(&args, "-o");
    fuse_opt_add_arg(&args, "nonamedattr");
    fuse_opt_add_arg(&args, "-o");
    fuse_opt_add_arg(&args, "nomtime");
    fuse_opt_add_arg(&args, "-o");
    fuse_opt_add_arg(&args, "noappledouble");
    fuse_opt_add_arg(&args, "-o");
    fuse_opt_add_arg(&args, "noapplexattr");

    auto* context = new FuseContext();

    context->fs = fs;
    context->nextFileHandle = 0;

    struct fuse_chan* ch = fuse_mount(mDstPath.c_str(), &args);
    struct fuse* fuse = fuse_new(ch, &args, &ops, sizeof(ops), context);

    // Clean up
    fuse_opt_free_args(&args);

    if (fuse == nullptr) {
        delete context;
        throw std::runtime_error("Failed to create mount point (path: " + mDstPath + ")");
    }

    mFuseCh = ch;
    mFuse = fuse;

    // Start fuse thread
    mThread = std::make_unique<std::thread>(&Session::fuseMain, this, ch, fuse);

}

void Session::updateOptions(FileRenderOptions options, int draftScale) {
    mFs->updateOptions(options, draftScale);

    fuse_invalidate_path(mFuse, mDstPath.c_str());

}

void Session::fuseMain(struct fuse_chan* ch, struct fuse* fuse) {
    int res = fuse_loop_mt(fuse);

    fuse_destroy(fuse);

    spdlog::info("Fuse has exited with code {}", res);
}

FuseContext* fuseGetContext() {
    auto context = fuse_get_context();

    return reinterpret_cast<FuseContext*>(context->private_data);
}

void* Session::fuseInit(struct fuse_conn_info* conn) {
    auto context = fuse_get_context();

    return context->private_data;
}

void Session::fuseDestroy(void* privateData) {
    spdlog::debug("fuseDestroy() entering");

    auto* context = reinterpret_cast<FuseContext*>(privateData);

    if(context->fs)
        delete context->fs;

    context->fs = nullptr;
    context->nextFileHandle = INT_MIN;

    delete context;

    spdlog::debug("fuseDestroy() exiting");
}

int Session::fuseGetattr(const char* path, struct stat* stbuf) {
    spdlog::debug("fuse_get_attr(path: {})", path);

    memset(stbuf, 0, sizeof(struct stat));

    auto* context = fuseGetContext();
    std::string pathStr(path);

    // Root directory
    if (pathStr == "/" || pathStr == "//") {
        stbuf->st_mode = S_IFDIR | 0755;
        stbuf->st_nlink = 2;

        return 0;
    }
    else {
        auto entry = context->fs->findEntry(pathStr);

        if(!entry.has_value())
            return -ENOENT;

        if(entry->type == EntryType::DIRECTORY_ENTRY) {
            stbuf->st_mode = S_IFDIR | 0755;
            stbuf->st_nlink = 2;
            stbuf->st_mtime = stbuf->st_ctime = time(NULL);
            stbuf->st_size = 4096;
        }
        else if(entry->type == EntryType::FILE_ENTRY) {
            stbuf->st_mode = S_IFREG | 0644;
            stbuf->st_nlink = 1;
            stbuf->st_size = entry->size;

            stbuf->st_mtime = stbuf->st_ctime = time(NULL);
            stbuf->st_uid = getuid();
            stbuf->st_gid = getgid();
        }

        return 0;
    }

    return -ENOENT;
}

int Session::fuseReaddir(const char* path, void* buf, fuse_fill_dir_t filler, off_t offset, struct fuse_file_info* fi) {
    spdlog::debug("fuse_read_dir(path: {})", path);

    auto* context = fuseGetContext();
    std::string pathStr(path);

    if(pathStr == "//" || pathStr == "/") {
        filler(buf, ".", nullptr, 0);
        filler(buf, "..", nullptr, 0);

        auto files = context->fs->listFiles("/");
        for(auto& entry : files) {
            filler(buf, entry.getFullPath().c_str(), nullptr, 0);
        }

        return 0;
    }

    return -ENOENT;
}

int Session::fuseOpen(const char* path, struct fuse_file_info* fi) {
    spdlog::debug("fuse_open(path: {})", path);

    auto* context = fuseGetContext();
    std::string pathStr(path);

    auto entry = context->fs->findEntry(pathStr);

    if(!entry.has_value())
        return -ENOENT;

    // Only allow read access
    if ((fi->flags & 3) != O_RDONLY)
        return -EACCES;


    // Set file handle
    fi->fh = ++context->nextFileHandle;

    return 0;
}

int Session::fuseRead(const char* path, char* buf, size_t size, off_t offset, struct fuse_file_info* fi) {
    spdlog::debug("fuse_read(path: {}, size: {}, offset: {})", path, size, offset);

    auto* context = fuseGetContext();
    std::string pathStr(path);

    auto entry = context->fs->findEntry(pathStr);

    if(!entry.has_value())
        return -ENOENT;

    return context->fs->readFile(
        entry.value(),
        offset,
        size,
        buf,
        [](auto a, auto b) {},
        false
        );
}

int Session::fuseRelease(const char* path, struct fuse_file_info* fi) {
    return 0;
}

//

FuseFileSystemImpl_MacOs::FuseFileSystemImpl_MacOs() :
    mNextMountId(0),
    mIoThreadPool(std::make_unique<BS::thread_pool>(IO_THREADS)),
    mProcessingThreadPool(std::make_unique<BS::thread_pool>()),
    mCache(std::make_unique<LRUCache>(CACHE_SIZE))
{
    setupLogging();
}

FuseFileSystemImpl_MacOs::~FuseFileSystemImpl_MacOs() {
    mMountedFiles.clear();

    // Wait for tasks to complete before we destroy ourselves
    mIoThreadPool->wait();

    mProcessingThreadPool->wait();

    spdlog::info("Destroying FuseFileSystemImpl_MacOs()");
}

MountId FuseFileSystemImpl_MacOs::mount(
    FileRenderOptions options, int draftScale, const std::string& srcFile, const std::string& dstPath)
{
    fs::path srcPath(srcFile);
    std::string extension = srcPath.extension().string();

    spdlog::debug("Mounting file {} to {}", srcFile, dstPath);

    QDir dst(dstPath.c_str());

    if(!dst.exists()) {
        spdlog::info("Creating path {}", dstPath);

        if(!dst.mkpath(dstPath.c_str())) {
            spdlog::error("Could not create path {}", dstPath);

            throw std::runtime_error("Failed to create " + dstPath);
        }
    }

    if(boost::iequals(extension, ".mcraw")) {
        auto mountId = mNextMountId++;

        void* stack_addr = nullptr;
        size_t stack_size = 0;

        try {
            auto* fs =
                new VirtualFileSystemImpl_MCRAW(
                    *mIoThreadPool,
                    *mProcessingThreadPool,
                    *mCache,
                    options,
                    draftScale,
                    srcFile);

            auto session = std::make_unique<Session>(srcFile, dstPath, fs);

            if(!session) {
                spdlog::error("Failed to mount {} to {}", srcFile, dstPath);

                throw std::runtime_error("Failed to session");
            }

            mMountedFiles[mountId] = std::move(session);
        }
        catch(std::runtime_error& e) {
            spdlog::error("Failed to mount {} to {} (error: {})", srcFile, dstPath, e.what());

            throw std::runtime_error(e.what());
        }

        return mountId;
    }

    spdlog::error("Failed to mount {} to {}, invalid file format", srcFile, dstPath);

    throw std::runtime_error("Invalid format");
}

void FuseFileSystemImpl_MacOs::unmount(MountId mountId) {
    auto it = mMountedFiles.find(mountId);
    if(it != mMountedFiles.end()) {
        mMountedFiles.erase(it);
    }
}

void FuseFileSystemImpl_MacOs::updateOptions(MountId mountId, FileRenderOptions options, int draftScale) {
    auto it = mMountedFiles.find(mountId);
    if(it != mMountedFiles.end()) {
        it->second->updateOptions(options, draftScale);
    }
}

} // namespace motioncam
