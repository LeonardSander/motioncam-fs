#include "SourceInput.h"
#include "SourceMedia.h"
#include <QScopeGuard>
#include <QThreadPool>
#include <QJniObject>
#include <QCoreApplication>
#include <cerrno>
#include <spdlog/spdlog.h>
#include <algorithm>
#include <mutex>
#include <vector>
#include <sys/stat.h>
#include <sys/resource.h>
#include <fcntl.h>
#include <unordered_map>
#include <unordered_set>
#include <unistd.h>

namespace motioncam {
namespace {
QString sourceUri(const std::string& path) {
    const QString name = QString::fromStdString(path);
    if (name.startsWith("content:")) return name;
    const auto activity = QNativeInterface::QAndroidApplication::context();
    return activity.callMethod<QString>("sourceUriForImport", name);
}
int readMedia(void* opaque, uint8_t* bytes, int size) {
    FILE* file = static_cast<FILE*>(opaque);
    const size_t count = std::fread(bytes, 1, size, file);
    return count ? int(count) : std::ferror(file) ? AVERROR(EIO) : AVERROR_EOF;
}
int64_t seekMedia(void* opaque, int64_t offset, int origin) {
    FILE* file = static_cast<FILE*>(opaque);
    origin &= ~AVSEEK_FORCE;
    if (origin == AVSEEK_SIZE) {
        struct stat info{};
        return ::fstat(::fileno(file), &info) ? AVERROR(errno) : info.st_size;
    }
    if (::fseeko(file, offset, origin)) return AVERROR(errno);
    return ::ftello(file);
}
}
namespace {
int openSourceDescriptor(const QString& uri) {
    const auto activity = QNativeInterface::QAndroidApplication::context();
    const int fd = activity.callMethod<jint>("openSourceUriFd", uri);
    if (fd < 0) { errno = EACCES; return -1; }
    if (::lseek(fd, 0, SEEK_SET) < 0) { ::close(fd); errno = ESPIPE; return -1; }
    return fd;
}
struct DescriptorEntry {
    int fd;
    std::string revision;
    uint64_t used;
};
struct DescriptorCache {
    std::mutex mutex;
    std::unordered_map<std::string, DescriptorEntry> entries;
    uint64_t clock = 0, generation = 0;
    std::unordered_set<std::string> pending;
    const size_t limit = [] {
        struct rlimit budget{};
        if (::getrlimit(RLIMIT_NOFILE, &budget)) return size_t{64};
        return size_t(std::min<rlim_t>(1024, budget.rlim_cur / 8));
    }();
    ~DescriptorCache() { for (const auto& [key, entry] : entries) ::close(entry.fd); }
};
DescriptorCache descriptorCache;
}
void releaseSourceReadDescriptors(const std::string& path) {
    std::error_code error;
    const std::filesystem::path source(path);
    const auto directory = std::filesystem::is_directory(source, error) ? source : source.parent_path();
    const std::string prefix = directory.string() + '/';
    std::lock_guard lock(descriptorCache.mutex);
    ++descriptorCache.generation;
    for (auto it = descriptorCache.entries.begin(); it != descriptorCache.entries.end();) {
        if (it->first.compare(0, prefix.size(), prefix) == 0) {
            ::close(it->second.fd);
            it = descriptorCache.entries.erase(it);
        } else ++it;
    }
}
int openSourceReadDescriptor(const std::string& path) {
    QString uri;
    std::string version;
    if (path.compare(0, 8, "content:") != 0) {
        const auto activity = QNativeInterface::QAndroidApplication::context();
        const QString info = activity.callMethod<QString>("sourceDescriptorInfoForImport",
                                                          QString::fromStdString(path));
        if (info.isEmpty()) return ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
        const auto sizeStart = info.indexOf(QChar('\n'));
        const auto revisionStart = info.indexOf(QChar('\n'), sizeStart + 1);
        if (sizeStart < 0 || revisionStart < 0) { errno = EIO; return -1; }
        uri = info.left(sizeStart);
        QString size = info.mid(sizeStart + 1, revisionStart - sizeStart - 1);
        bool knownSize = false;
        const auto indexedSize = size.toLongLong(&knownSize);
        if (!knownSize || indexedSize < 0) {
            std::error_code error;
            const auto bytes = sourceFileSize(path, error);
            size = error ? QStringLiteral("unknown") : QString::number(bytes);
        }
        version = (uri + ':' + info.mid(revisionStart + 1) + ':' + size).toStdString();
    } else {
        uri = sourceUri(path);
        if (uri.isEmpty()) return ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
        const std::string revision = sourceRevision(path);
        std::error_code error;
        const auto bytes = sourceFileSize(path, error);
        version = revision + ':' + (error ? "unknown" : std::to_string(bytes));
    }
    static std::once_flag logged;
    std::call_once(logged, [] {
        spdlog::info("URI source descriptor cache: capacity={}", descriptorCache.limit);
    });
    // Resolve URI, size and revision together on each open so source refreshes
    // still invalidate replaced files without retaining stale native metadata.
    const std::string& key = path;
    uint64_t generation;
    {
        std::lock_guard lock(descriptorCache.mutex);
        generation = descriptorCache.generation;
        auto found = descriptorCache.entries.find(key);
        if (found != descriptorCache.entries.end() && found->second.revision == version) {
            found->second.used = ++descriptorCache.clock;
            return ::fcntl(found->second.fd, F_DUPFD_CLOEXEC, 0);
        }
    }
    // Provider calls must stay outside the cache lock so imports can overlap.
    const int fd = openSourceDescriptor(uri);
    if (fd < 0) return -1;
    auto closeNewFd = qScopeGuard([fd] { ::close(fd); });
    std::lock_guard lock(descriptorCache.mutex);
    if (generation != descriptorCache.generation) {
        closeNewFd.dismiss();
        return fd;
    }
    auto found = descriptorCache.entries.find(key);
    if (found != descriptorCache.entries.end()) {
        if (found->second.revision == version) {
            found->second.used = ++descriptorCache.clock;
            return ::fcntl(found->second.fd, F_DUPFD_CLOEXEC, 0);
        }
        ::close(found->second.fd);
        descriptorCache.entries.erase(found);
    }
    if (!descriptorCache.limit) {
        closeNewFd.dismiss();
        return fd;
    }
    while (descriptorCache.entries.size() >= descriptorCache.limit) {
        auto oldest = std::min_element(descriptorCache.entries.begin(), descriptorCache.entries.end(),
            [](const auto& a, const auto& b) { return a.second.used < b.second.used; });
        ::close(oldest->second.fd);
        descriptorCache.entries.erase(oldest);
    }
    descriptorCache.entries.emplace(key, DescriptorEntry{fd, version, ++descriptorCache.clock});
    closeNewFd.dismiss();
    return ::fcntl(fd, F_DUPFD_CLOEXEC, 0);
}
void prefetchSourceReadDescriptor(const std::string& path) noexcept {
    try {
        uint64_t generation;
        {
            std::lock_guard lock(descriptorCache.mutex);
            if (!descriptorCache.limit || descriptorCache.pending.size() >= 16 ||
                descriptorCache.entries.count(path) || descriptorCache.pending.count(path)) return;
            descriptorCache.pending.insert(path);
            generation = descriptorCache.generation;
        }
        auto undoPending = qScopeGuard([&path] {
            std::lock_guard lock(descriptorCache.mutex);
            descriptorCache.pending.erase(path);
        });
        QThreadPool::globalInstance()->start([path, generation] {
            auto completed = qScopeGuard([&path] {
                std::lock_guard lock(descriptorCache.mutex);
                descriptorCache.pending.erase(path);
            });
            {
                std::lock_guard lock(descriptorCache.mutex);
                if (generation != descriptorCache.generation) return;
            }
            try {
                const int fd = openSourceReadDescriptor(path);
                if (fd >= 0) ::close(fd);
            } catch (...) { } // Optional lookahead must never fail a frame read.
        });
        undoPending.dismiss();
    } catch (...) { } // Allocation failure leaves synchronous reads available.
}
FILE* openSourceFile(const std::string& path) {
    const QString uri = sourceUri(path);
    if (uri.isEmpty()) return std::fopen(path.c_str(), "rb");
    const int fd = openSourceDescriptor(uri);
    if (fd < 0) return nullptr;
    FILE* file = ::fdopen(fd, "rb");
    if (!file) ::close(fd);
    return file;
}
uintmax_t sourceFileSize(const std::filesystem::path& path, std::error_code& error) {
    const auto activity = QNativeInterface::QAndroidApplication::context();
    const auto size = activity.callMethod<jlong>("sourceSizeForImport", QString::fromStdString(path.string()));
    if (size >= 0) { error.clear(); return uintmax_t(size); }
    if (sourceUri(path.string()).isEmpty()) return std::filesystem::file_size(path, error);
    FILE* file = openSourceFile(path.string());
    if (!file) { error = std::error_code(errno, std::generic_category()); return uintmax_t(-1); }
    const bool seeked = ::fseeko(file, 0, SEEK_END) == 0;
    const auto length = seeked ? ::ftello(file) : -1;
    const int failure = errno;
    std::fclose(file);
    if (length < 0) { error = std::error_code(failure, std::generic_category()); return uintmax_t(-1); }
    error.clear();
    return uintmax_t(length);
}
std::string sourceRevision(const std::string& path) {
    const auto activity = QNativeInterface::QAndroidApplication::context();
    return activity.callMethod<QString>("sourceRevisionForImport", QString::fromStdString(path)).toStdString();
}
int openSourceMedia(AVFormatContext** context, const std::string& path) {
    if (sourceUri(path).isEmpty())
        return avformat_open_input(context, path.c_str(), nullptr, nullptr);
    FILE* file = openSourceFile(path);
    if (!file) return AVERROR(errno);
    auto* buffer = static_cast<uint8_t*>(av_malloc(65536));
    AVIOContext* io = buffer ? avio_alloc_context(buffer, 65536, 0, file, readMedia, nullptr, seekMedia) : nullptr;
    AVFormatContext* format = io ? avformat_alloc_context() : nullptr;
    if (!format) {
        if (io) { av_freep(&io->buffer); avio_context_free(&io); } else av_free(buffer);
        std::fclose(file);
        return AVERROR(ENOMEM);
    }
    format->pb = io;
    format->flags |= AVFMT_FLAG_CUSTOM_IO;
    const int result = avformat_open_input(&format, path.c_str(), nullptr, nullptr);
    if (result < 0) {
        av_freep(&io->buffer); avio_context_free(&io); std::fclose(file);
        return result;
    }
    *context = format;
    return 0;
}
void closeSourceMedia(AVFormatContext** context) {
    AVIOContext* io = *context && ((*context)->flags & AVFMT_FLAG_CUSTOM_IO) ? (*context)->pb : nullptr;
    FILE* file = io ? static_cast<FILE*>(io->opaque) : nullptr;
    avformat_close_input(context);
    if (io) { av_freep(&io->buffer); avio_context_free(&io); }
    if (file) std::fclose(file);
}
}
