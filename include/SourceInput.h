#pragma once

#include "SourceAnalysisCache.h"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#if defined(__ANDROID__) || defined(MOTIONCAM_SOURCE_INPUT_TEST)
#include <algorithm>
#include <cerrno>
#include <limits>
#include <unistd.h>
#endif

namespace motioncam {
#if defined(__ANDROID__) || defined(MOTIONCAM_SOURCE_INPUT_TEST)
inline constexpr bool sourceReadAheadEnabled = true;
void prefetchSourceReadDescriptor(const std::string& path) noexcept;
int openSourceReadDescriptor(const std::string& path);
void releaseSourceReadDescriptors(const std::string& path);
FILE* openSourceFile(const std::string& path);
uintmax_t sourceFileSize(const std::filesystem::path& path, std::error_code& error);
std::string sourceRevision(const std::string& path);

// Each stream owns a descriptor and uses pread so duplicated URI handles
// never share a mutable seek position.
class SourceInputStream : public std::istream {
    class Buffer : public std::streambuf {
    public:
        Buffer(int fd, const std::filesystem::path& path) : mFd(fd), mPath(path) {}
        ~Buffer() { detach(); }
        Buffer(const Buffer&) = delete;
        Buffer& operator=(const Buffer&) = delete;
        void attach(int fd) { mFd = fd; }
        bool isOpen() const { return mFd >= 0; }
        void detach() {
            if (mFd >= 0) ::close(mFd);
            mFd = -1;
            setg(nullptr, nullptr, nullptr);
        }
    protected:
        int_type underflow() override {
            if (gptr() && gptr() < egptr()) return traits_type::to_int_type(*gptr());
            const ssize_t count = readAt(mBytes, sizeof(mBytes));
            if (count <= 0) return traits_type::eof();
            setg(mBytes, mBytes, mBytes + count);
            return traits_type::to_int_type(*gptr());
        }
        std::streamsize xsgetn(char* output, std::streamsize size) override {
            std::streamsize copied = 0;
            while (copied < size) {
                const auto available = gptr() ? egptr() - gptr() : 0;
                if (available) {
                    const auto count = std::min<std::streamsize>(available, size - copied);
                    traits_type::copy(output + copied, gptr(), size_t(count));
                    gbump(int(count));
                    copied += count;
                } else if (size - copied >= std::streamsize(sizeof(mBytes))) {
                    // Match file streams' bulk-read path instead of issuing a
                    // separate provider read and memory copy for every 64 KiB.
                    const auto count = readAt(output + copied, size_t(std::min<std::streamsize>(
                        size - copied, std::numeric_limits<ssize_t>::max())));
                    if (count <= 0) break;
                    copied += count;
                } else if (traits_type::eq_int_type(underflow(), traits_type::eof())) break;
            }
            return copied;
        }
        pos_type seekoff(off_type offset, std::ios::seekdir direction,
                         std::ios::openmode) override {
            if (mFd < 0) return pos_type(off_type(-1));
            off_type base = 0;
            if (direction == std::ios::cur) base = mOffset - (gptr() ? egptr() - gptr() : 0);
            else if (direction == std::ios::end) {
                std::error_code error;
                const auto size = sourceFileSize(mPath, error);
                if (error || size > uintmax_t(std::numeric_limits<off_type>::max()))
                    return pos_type(off_type(-1));
                base = static_cast<off_type>(size);
            }
            if ((offset < 0 && offset < -base) ||
                (offset > 0 && base > std::numeric_limits<off_type>::max() - offset))
                return pos_type(off_type(-1));
            mOffset = base + offset;
            setg(nullptr, nullptr, nullptr);
            return pos_type(mOffset);
        }
        pos_type seekpos(pos_type position, std::ios::openmode mode) override {
            return seekoff(off_type(position), std::ios::beg, mode);
        }
    private:
        ssize_t readAt(char* output, size_t size) {
            ssize_t count;
            do { count = mFd < 0 ? 0 : ::pread(mFd, output, size, mOffset); }
            while (count < 0 && errno == EINTR);
            if (count > 0) mOffset += count;
            return count;
        }
        int mFd;
        std::filesystem::path mPath;
        off_type mOffset = 0;
        char mBytes[65536];
    };
public:
    explicit SourceInputStream(const std::filesystem::path& path,
                               std::ios::openmode mode = std::ios::in)
        : std::istream(nullptr), mBuffer(-1, path) {
        mBuffer.attach(openSourceReadDescriptor(path.string()));
        rdbuf(&mBuffer);
        if (!mBuffer.isOpen()) setstate(std::ios::failbit);
        else if (mode & std::ios::ate) seekg(0, std::ios::end);
    }
    ~SourceInputStream() override { close(); }
    void close() {
        mBuffer.detach();
    }
    bool is_open() const { return mBuffer.isOpen(); }
private:
    Buffer mBuffer;
};
inline FILE* decoderSource(const std::string& path) { return openSourceFile(path); }
#else
inline constexpr bool sourceReadAheadEnabled = false;
inline void prefetchSourceReadDescriptor(const std::string&) noexcept {}
using SourceInputStream = std::ifstream;
inline const std::string& decoderSource(const std::string& path) { return path; }
inline uintmax_t sourceFileSize(const std::filesystem::path& path, std::error_code& error) {
    return std::filesystem::file_size(path, error);
}
inline std::string sourceRevision(const std::string&) { return {}; }
#endif
}
