// libarchive can include Windows headers; keep their min/max macros out.
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include "ZipSource.h"
#include "SourceInput.h"

#include <archive.h>
#include <archive_entry.h>
#include <zlib.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstring>
#include <cerrno>
#include <limits>
#include <list>
#include <mutex>
#include <stdexcept>
#include <unordered_map>

namespace motioncam {
namespace {
uint16_t u16(const unsigned char* p) { return p[0] | uint16_t(p[1]) << 8; }
uint32_t u32(const unsigned char* p) { return u16(p) | uint32_t(u16(p + 2)) << 16; }
uint64_t u64(const unsigned char* p) { return u32(p) | uint64_t(u32(p + 4)) << 32; }
void readAt(std::istream& file, uint64_t offset, void* bytes, size_t size) {
    if (offset > uint64_t(std::numeric_limits<std::streamoff>::max()) ||
        size > size_t(std::numeric_limits<std::streamsize>::max()))
        throw std::runtime_error("ZIP offset or size is too large");
    if (!size) return;
    file.clear();
    if (file.tellg() != static_cast<std::streamoff>(offset))
        file.seekg(static_cast<std::streamoff>(offset));
    file.read(static_cast<char*>(bytes), static_cast<std::streamsize>(size));
    if (file.gcount() != static_cast<std::streamsize>(size))
        throw std::runtime_error("Truncated ZIP archive");
}
std::string key(const std::filesystem::path& path) {
    return std::filesystem::absolute(path).lexically_normal().string();
}
std::filesystem::path entryPath(std::string name) {
    std::replace(name.begin(), name.end(), '\\', '/');
    return std::filesystem::u8path(name).lexically_normal();
}
void appendUtf8(std::string& output, uint32_t value) {
    if (value < 0x80) output += static_cast<char>(value);
    else if (value < 0x800) {
        output += static_cast<char>(0xc0 | (value >> 6));
        output += static_cast<char>(0x80 | (value & 63));
    } else {
        output += static_cast<char>(0xe0 | (value >> 12));
        output += static_cast<char>(0x80 | ((value >> 6) & 63));
        output += static_cast<char>(0x80 | (value & 63));
    }
}
// Some producers write UTF-8 without bit 11. Preserve those paths while
// decoding non-UTF-8 legacy names using ZIP's CP437 default.
bool validUtf8(const std::string& value) {
    for (size_t i = 0; i < value.size();) {
        const auto first = static_cast<unsigned char>(value[i++]);
        if (first < 0x80) continue;
        const size_t count = first >= 0xc2 && first <= 0xdf ? 1 :
                             first >= 0xe0 && first <= 0xef ? 2 :
                             first >= 0xf0 && first <= 0xf4 ? 3 : 0;
        if (!count || count > value.size() - i) return false;
        uint32_t code = first & (0x7f >> (count + 1));
        for (size_t j = 0; j < count; ++j) {
            const auto next = static_cast<unsigned char>(value[i++]);
            if ((next & 0xc0) != 0x80) return false;
            code = (code << 6) | (next & 63);
        }
        if (code < (count == 1 ? 0x80u : count == 2 ? 0x800u : 0x10000u) ||
            code > 0x10ffff || (code >= 0xd800 && code <= 0xdfff)) return false;
    }
    return true;
}
std::string legacyZipName(const std::string& raw) {
    static constexpr uint16_t cp437[] = {
        0xc7, 0xfc, 0xe9, 0xe2, 0xe4, 0xe0, 0xe5, 0xe7,
        0xea, 0xeb, 0xe8, 0xef, 0xee, 0xec, 0xc4, 0xc5,
        0xc9, 0xe6, 0xc6, 0xf4, 0xf6, 0xf2, 0xfb, 0xf9,
        0xff, 0xd6, 0xdc, 0xa2, 0xa3, 0xa5, 0x20a7, 0x192,
        0xe1, 0xed, 0xf3, 0xfa, 0xf1, 0xd1, 0xaa, 0xba,
        0xbf, 0x2310, 0xac, 0xbd, 0xbc, 0xa1, 0xab, 0xbb,
        0x2591, 0x2592, 0x2593, 0x2502, 0x2524, 0x2561, 0x2562, 0x2556,
        0x2555, 0x2563, 0x2551, 0x2557, 0x255d, 0x255c, 0x255b, 0x2510,
        0x2514, 0x2534, 0x252c, 0x251c, 0x2500, 0x253c, 0x255e, 0x255f,
        0x255a, 0x2554, 0x2569, 0x2566, 0x2560, 0x2550, 0x256c, 0x2567,
        0x2568, 0x2564, 0x2565, 0x2559, 0x2558, 0x2552, 0x2553, 0x256b,
        0x256a, 0x2518, 0x250c, 0x2588, 0x2584, 0x258c, 0x2590, 0x2580,
        0x3b1, 0xdf, 0x393, 0x3c0, 0x3a3, 0x3c3, 0xb5, 0x3c4,
        0x3a6, 0x398, 0x3a9, 0x3b4, 0x221e, 0x3c6, 0x3b5, 0x2229,
        0x2261, 0xb1, 0x2265, 0x2264, 0x2320, 0x2321, 0xf7, 0x2248,
        0xb0, 0x2219, 0xb7, 0x221a, 0x207f, 0xb2, 0x25a0, 0xa0,
    };
    std::string output;
    for (unsigned char c : raw) appendUtf8(output, c < 128 ? c : cp437[c - 128]);
    return output;
}
std::string revision(const std::string& path) {
    std::error_code error;
    const auto size = nativeSourceFileSize(path, error);
    if (error) throw std::runtime_error("ZIP source is unavailable: " + path);
    auto version = nativeSourceRevision(path);
    if (version.empty()) {
        const auto time = std::filesystem::last_write_time(path, error);
        if (error) throw std::runtime_error("ZIP source is unavailable: " + path);
        // libc++ uses a 128-bit file clock representation, which has no
        // std::to_string overload. Normalize to a standard nanosecond count.
        version = std::to_string(std::chrono::duration_cast<std::chrono::nanoseconds>(
            time.time_since_epoch()).count());
    }
    return path + ':' + std::to_string(size) + ':' + version;
}
struct ArchiveSource { std::string path, root, version; };
struct Binding { std::shared_ptr<const ArchiveSource> archive; ZipSourceEntry entry; };
std::mutex sourceMutex;
std::unordered_map<std::string, Binding> sources;
std::optional<Binding> lookup(const std::filesystem::path& path) {
    std::lock_guard lock(sourceMutex);
    const auto found = sources.find(key(path));
    if (found == sources.end()) return {};
    std::error_code error;
    if (!std::filesystem::exists(found->second.archive->root, error)) return {};
    return found->second;
}
void validate(const Binding& binding) {
    if (revision(binding.archive->path) != binding.archive->version)
        throw std::runtime_error("ZIP archive changed; import it again: " + binding.archive->path);
}
// Each reader retains at most four decoded blocks. Forward seeks discard
// intervening output; backward seeks outside those blocks restart decoding.
// ZIP compression is sequential, so metadata after the pixels still requires
// decoding the preceding bytes, but never an image-sized allocation.
class CompressedEntry {
public:
    explicit CompressedEntry(const Binding& binding)
        : mEntry(binding.entry), mFile(binding.archive->path, std::ios::binary) {
        std::array<unsigned char, 30> header{};
        readAt(mFile, mEntry.localOffset, header.data(), header.size());
        if (u32(header.data()) != 0x04034b50 || u16(header.data() + 8) != mEntry.method ||
            (u16(header.data() + 6) & (1 | 64)))
            throw std::runtime_error("Invalid compressed ZIP local header");
        std::string name(u16(header.data() + 26), '\0');
        readAt(mFile, mEntry.localOffset + header.size(), name.data(), name.size());
        if (name != mEntry.rawName) throw std::runtime_error("ZIP local and central directory names differ");
        mStart = mEntry.localOffset + header.size() + name.size() + u16(header.data() + 28);
        std::error_code error;
        const auto archiveSize = nativeSourceFileSize(binding.archive->path, error);
        if (error || mStart > archiveSize || mEntry.compressedSize > archiveSize - mStart)
            throw std::runtime_error("Compressed ZIP entry exceeds archive bounds");
        if (mEntry.method != 8 && mEntry.method != 93)
            throw std::runtime_error("Unsupported ZIP compression method");
    }
    ~CompressedEntry() { release(); }

    void read(uint64_t offset, char* output, size_t count) {
        if (offset > mEntry.size || count > mEntry.size - offset)
            throw std::runtime_error("ZIP read exceeds entry bounds");
        if (!mEntry.size) { initialize(); finish(); }
        while (count) {
            const auto start = offset / blockSize * blockSize;
            auto found = std::find_if(mBlocks.begin(), mBlocks.end(),
                [&](const Block& block) { return block.start == start; });
            if (found == mBlocks.end()) {
                initialize();
                if (mPosition > start) { release(); initialize(); }
                std::array<char, blockSize> discard;
                while (mPosition < start)
                    decode(discard.data(), static_cast<size_t>(std::min<uint64_t>(discard.size(), start - mPosition)));
                Block block{start, std::vector<char>(static_cast<size_t>(std::min<uint64_t>(blockSize, mEntry.size - start)))};
                decode(block.bytes.data(), block.bytes.size());
                if (mPosition == mEntry.size) finish();
                if (mBlocks.size() == 4) mBlocks.pop_back();
                mBlocks.push_front(std::move(block));
            } else mBlocks.splice(mBlocks.begin(), mBlocks, found);
            const auto& block = mBlocks.front();
            const auto within = static_cast<size_t>(offset - start);
            const auto copied = std::min(count, block.bytes.size() - within);
            std::memcpy(output, block.bytes.data() + within, copied);
            offset += copied;
            output += copied;
            count -= copied;
        }
    }
private:
    static constexpr size_t blockSize = 65536;
    struct Block { uint64_t start; std::vector<char> bytes; };

    void release() {
        if (mInflaterActive) inflateEnd(&mInflater);
        mInflaterActive = false;
        if (mArchive) archive_read_free(mArchive);
        mArchive = nullptr;
        mInitialized = false;
    }
    static la_ssize_t archiveRead(archive* input, void* opaque, const void** output) {
        auto& self = *static_cast<CompressedEntry*>(opaque);
        try {
            const auto count = self.fillInput();
            *output = self.mInput.data();
            return static_cast<la_ssize_t>(count);
        } catch (...) {
            archive_set_error(input, EIO, "Could not read ZIP Zstandard payload");
            return -1;
        }
    }
    size_t fillInput() {
        const auto count = static_cast<size_t>(std::min<uint64_t>(mInput.size(), mEntry.compressedSize - mInputRead));
        readAt(mFile, mStart + mInputRead, mInput.data(), count);
        mInputRead += count;
        return count;
    }
    void initialize() {
        if (mInitialized) return;
        release();
        mPosition = mInputRead = 0;
        mCrc = 0;
        mEnded = mFinished = false;
        if (mEntry.method == 8) {
            mInflater = {};
            if (inflateInit2(&mInflater, -MAX_WBITS) != Z_OK)
                throw std::runtime_error("Could not initialize ZIP inflater");
            mInflaterActive = true;
        } else {
            mArchive = archive_read_new();
            if (!mArchive) throw std::bad_alloc();
            archive_read_support_format_raw(mArchive);
            archive_read_support_format_empty(mArchive);
            archive_read_support_filter_zstd(mArchive);
            checkArchive(archive_read_open(mArchive, this, nullptr, archiveRead, nullptr));
            archive_entry* item = nullptr;
            const auto status = archive_read_next_header(mArchive, &item);
            if (status == ARCHIVE_EOF && !mEntry.size) mEnded = true;
            else checkArchive(status);
            if (archive_filter_code(mArchive, 0) != ARCHIVE_FILTER_ZSTD)
                throw std::runtime_error("Invalid ZIP Zstandard frame");
        }
        mInitialized = true;
    }
    void checkArchive(int status) {
        if (status == ARCHIVE_OK) return;
        const auto* detail = archive_error_string(mArchive);
        throw std::runtime_error(detail ? detail : "Invalid ZIP Zstandard data");
    }
    size_t pull(char* output, size_t count) {
        if (mEnded) return 0;
        if (mArchive) {
            const auto result = archive_read_data(mArchive, output, count);
            if (result < 0) throw std::runtime_error("Invalid or truncated ZIP Zstandard data");
            if (!result) mEnded = true;
            return static_cast<size_t>(result);
        }
        mInflater.next_out = reinterpret_cast<Bytef*>(output);
        mInflater.avail_out = static_cast<uInt>(count);
        while (mInflater.avail_out && !mEnded) {
            if (!mInflater.avail_in && mInputRead < mEntry.compressedSize) {
                mInflater.avail_in = static_cast<uInt>(fillInput());
                mInflater.next_in = reinterpret_cast<Bytef*>(mInput.data());
            }
            const auto inputBefore = mInflater.avail_in;
            const auto outputBefore = mInflater.avail_out;
            const auto status = inflate(&mInflater, Z_NO_FLUSH);
            if (status == Z_STREAM_END) {
                mEnded = true;
                if (mInputRead - mInflater.avail_in != mEntry.compressedSize)
                    throw std::runtime_error("ZIP compressed size mismatch");
            } else if (status != Z_OK ||
                       (inputBefore == mInflater.avail_in && outputBefore == mInflater.avail_out))
                throw std::runtime_error("Invalid or truncated ZIP deflate data");
        }
        return count - mInflater.avail_out;
    }
    void decode(char* output, size_t count) {
        size_t done = 0;
        while (done < count) {
            const auto written = pull(output + done, count - done);
            if (!written) throw std::runtime_error("ZIP entry is shorter than its declared size");
            mCrc = crc32(mCrc, reinterpret_cast<const Bytef*>(output + done), static_cast<uInt>(written));
            mPosition += written;
            done += written;
        }
    }
    void finish() {
        if (mFinished) return;
        char extra;
        if (pull(&extra, 1)) throw std::runtime_error("ZIP entry exceeds its declared size");
        if (mCrc != mEntry.crc) throw std::runtime_error("ZIP entry CRC mismatch");
        mFinished = true;
    }

    ZipSourceEntry mEntry;
    NativeSourceInputStream mFile;
    uint64_t mStart = 0, mPosition = 0, mInputRead = 0;
    uLong mCrc = 0;
    z_stream mInflater{};
    archive* mArchive = nullptr;
    bool mInflaterActive = false, mInitialized = false, mEnded = false, mFinished = false;
    std::array<char, blockSize> mInput;
    std::list<Block> mBlocks;
};

// Independent file handles and positions permit concurrent frame decoding.
// Stored entries read bounded slices of the archive; compressed entries decode
// on demand into bounded blocks and support arbitrary TIFF metadata seeks.
class ZipBuffer final : public std::streambuf {
public:
    explicit ZipBuffer(const Binding& binding) : mSize(binding.entry.size), mExpectedCrc(binding.entry.crc) {
        validate(binding);
        if (binding.entry.method) {
            mCompressed = std::make_unique<CompressedEntry>(binding);
        } else {
            mFile = std::make_unique<NativeSourceInputStream>(binding.archive->path, std::ios::binary);
            std::array<unsigned char, 30> header{};
            readAt(*mFile, binding.entry.localOffset, header.data(), header.size());
            if (u32(header.data()) != 0x04034b50 || u16(header.data() + 8) != 0 ||
                (u16(header.data() + 6) & (1 | 64)))
                throw std::runtime_error("Invalid stored ZIP local header");
            const auto nameSize = u16(header.data() + 26);
            mStart = binding.entry.localOffset + header.size() + nameSize + u16(header.data() + 28);
            std::error_code error;
            const auto archiveSize = nativeSourceFileSize(binding.archive->path, error);
            if (error || mStart > archiveSize || mSize > archiveSize - mStart ||
                binding.entry.compressedSize != mSize)
                throw std::runtime_error("Stored ZIP entry exceeds archive bounds");
            std::string name(nameSize, '\0');
            readAt(*mFile, binding.entry.localOffset + header.size(), name.data(), name.size());
            if (name != binding.entry.rawName)
                throw std::runtime_error("ZIP local and central directory names differ");
            if (!mSize && mExpectedCrc)
                throw std::runtime_error("Empty ZIP entry has an invalid CRC");
        }
    }
protected:
    int_type underflow() override {
        if (gptr() && gptr() < egptr()) return traits_type::to_int_type(*gptr());
        const auto count = read(mBuffer.data(), mBuffer.size());
        if (!count) return traits_type::eof();
        setg(mBuffer.data(), mBuffer.data(), mBuffer.data() + count);
        return traits_type::to_int_type(*gptr());
    }
    std::streamsize xsgetn(char* output, std::streamsize size) override {
        std::streamsize copied = 0;
        while (copied < size) {
            const auto available = gptr() ? egptr() - gptr() : 0;
            if (available) {
                const auto count = std::min<std::streamsize>(available, size - copied);
                std::memcpy(output + copied, gptr(), static_cast<size_t>(count));
                gbump(static_cast<int>(count));
                copied += count;
            } else {
                const auto count = read(output + copied, static_cast<size_t>(size - copied));
                if (!count) break;
                copied += static_cast<std::streamsize>(count);
            }
        }
        return copied;
    }
    pos_type seekoff(off_type offset, std::ios::seekdir direction, std::ios::openmode) override {
        const auto unread = gptr() ? egptr() - gptr() : 0;
        if (direction == std::ios::cur && offset == 0)
            return pos_type(static_cast<off_type>(mOffset - unread));
        const auto base = direction == std::ios::beg ? uint64_t{0} :
                          direction == std::ios::end ? mSize : mOffset - unread;
        if ((offset < 0 && uint64_t(-(offset + 1)) + 1 > base) ||
            (offset >= 0 && uint64_t(offset) > mSize - base)) return pos_type(off_type(-1));
        const auto position = offset < 0 ? base - (uint64_t(-(offset + 1)) + 1) : base + uint64_t(offset);
        if (position > uint64_t(std::numeric_limits<off_type>::max())) return pos_type(off_type(-1));
        if (position != mOffset) {
            mCheckCrc = position == 0;
            mCrc = 0;
        }
        mOffset = position;
        setg(nullptr, nullptr, nullptr);
        return pos_type(static_cast<off_type>(position));
    }
    pos_type seekpos(pos_type position, std::ios::openmode mode) override {
        return seekoff(off_type(position), std::ios::beg, mode);
    }
private:
    size_t read(char* output, size_t requested) {
        const size_t count = static_cast<size_t>(std::min<uint64_t>(requested, mSize - mOffset));
        if (mCompressed) mCompressed->read(mOffset, output, count);
        if (!count) return 0;
        if (!mCompressed) {
            readAt(*mFile, mStart + mOffset, output, count);
            if (mCheckCrc) {
                size_t done = 0;
                while (done < count) {
                    const auto chunk = static_cast<uInt>(std::min<size_t>(count - done, std::numeric_limits<uInt>::max()));
                    mCrc = crc32(mCrc, reinterpret_cast<const Bytef*>(output + done), chunk);
                    done += chunk;
                }
                if (mOffset + count == mSize && mCrc != mExpectedCrc)
                    throw std::runtime_error("ZIP entry CRC mismatch");
            }
        }
        mOffset += count;
        return count;
    }
    uint64_t mSize, mStart = 0, mOffset = 0;
    uint32_t mExpectedCrc;
    uLong mCrc = 0;
    bool mCheckCrc = true;
    std::unique_ptr<NativeSourceInputStream> mFile;
    std::unique_ptr<CompressedEntry> mCompressed;
    std::array<char, 65536> mBuffer;
};
} // namespace

std::vector<ZipSourceEntry> indexZipSource(const std::string& archivePath) {
    NativeSourceInputStream file(archivePath, std::ios::binary);
    std::error_code error;
    const auto size = nativeSourceFileSize(archivePath, error);
    if (!file || error || size < 22) throw std::runtime_error("Could not open ZIP archive");
    std::vector<unsigned char> tail(static_cast<size_t>(std::min<uintmax_t>(size, 22 + 65535)));
    const auto start = size - tail.size();
    readAt(file, start, tail.data(), tail.size());
    size_t end = tail.size();
    for (size_t pos = tail.size() - 22;; --pos) {
        if (u32(tail.data() + pos) == 0x06054b50 && pos + 22 + u16(tail.data() + pos + 20) == tail.size()) {
            end = pos; break;
        }
        if (!pos) break;
    }
    if (end == tail.size()) throw std::runtime_error("ZIP end record is missing");
    const auto* eocd = tail.data() + end;
    if (u16(eocd + 4) || u16(eocd + 6) || u16(eocd + 8) != u16(eocd + 10))
        throw std::runtime_error("Multi-volume ZIP archives are unsupported");
    uint64_t count = u16(eocd + 10), directorySize = u32(eocd + 12), directory = u32(eocd + 16);
    if (count == 0xffff || directorySize == 0xffffffff || directory == 0xffffffff) {
        if (start + end < 20) throw std::runtime_error("ZIP64 locator is missing");
        std::array<unsigned char, 20> locator{};
        readAt(file, start + end - 20, locator.data(), locator.size());
        if (u32(locator.data()) != 0x07064b50 || u32(locator.data() + 4) || u32(locator.data() + 16) != 1)
            throw std::runtime_error("Invalid ZIP64 locator");
        std::array<unsigned char, 56> record{};
        readAt(file, u64(locator.data() + 8), record.data(), record.size());
        if (u32(record.data()) != 0x06064b50 || u64(record.data() + 4) < 44 ||
            u32(record.data() + 16) || u32(record.data() + 20) || u64(record.data() + 24) != u64(record.data() + 32))
            throw std::runtime_error("Invalid ZIP64 directory");
        count = u64(record.data() + 32); directorySize = u64(record.data() + 40); directory = u64(record.data() + 48);
    }
    if (directory > start + end || directorySize > start + end - directory || count > directorySize / 46)
        throw std::runtime_error("ZIP directory exceeds archive bounds");
    const auto directoryEnd = directory + directorySize;
    std::vector<ZipSourceEntry> entries;
    for (uint64_t i = 0; i < count; ++i) {
        std::array<unsigned char, 46> header{};
        if (directoryEnd - directory < header.size()) throw std::runtime_error("Truncated ZIP directory");
        readAt(file, directory, header.data(), header.size());
        if (u32(header.data()) != 0x02014b50) throw std::runtime_error("Invalid ZIP directory entry");
        const auto nameSize = u16(header.data() + 28), extraSize = u16(header.data() + 30), commentSize = u16(header.data() + 32);
        const size_t variableSize = size_t(nameSize) + extraSize + commentSize;
        if (variableSize > directoryEnd - directory - header.size()) throw std::runtime_error("Truncated ZIP directory entry");
        std::vector<unsigned char> variable(variableSize);
        readAt(file, directory + header.size(), variable.data(), variable.size());
        ZipSourceEntry entry;
        if (nameSize) entry.name.assign(reinterpret_cast<const char*>(variable.data()), nameSize);
        if (entry.name.find('\0') != std::string::npos)
            throw std::runtime_error("ZIP entry name contains a null byte");
        entry.rawName = entry.name;
        const auto creator = header[5];
        const auto mode = creator == 3 || creator == 19 ? u32(header.data() + 38) >> 16 : 0;
        entry.regular = !entry.name.empty() && entry.name.back() != '/' && entry.name.back() != '\\' &&
            !(u32(header.data() + 38) & 0x10) &&
            ((mode & 0170000) == 0 || (mode & 0170000) == 0100000);
        entry.flags = u16(header.data() + 8); entry.method = u16(header.data() + 10);
        if (!(entry.flags & 0x800) && !validUtf8(entry.rawName)) entry.name = legacyZipName(entry.rawName);
        else if (!validUtf8(entry.rawName)) throw std::runtime_error("Invalid UTF-8 ZIP name");
        entry.crc = u32(header.data() + 16); entry.compressedSize = u32(header.data() + 20);
        entry.size = u32(header.data() + 24); entry.localOffset = u32(header.data() + 42);
        uint64_t disk = u16(header.data() + 34);
        const bool needsZip64 = entry.size == 0xffffffff || entry.compressedSize == 0xffffffff ||
            entry.localOffset == 0xffffffff || disk == 0xffff;
        bool hasZip64 = false;
        for (size_t pos = nameSize; pos + 4 <= size_t(nameSize) + extraSize;) {
            const auto tag = u16(variable.data() + pos), length = u16(variable.data() + pos + 2);
            pos += 4;
            if (length > size_t(nameSize) + extraSize - pos) throw std::runtime_error("Invalid ZIP extra field");
            if (tag == 0x7075 && length >= 5 && variable[pos] == 1 &&
                u32(variable.data() + pos + 1) == crc32(0, reinterpret_cast<const Bytef*>(entry.rawName.data()), static_cast<uInt>(entry.rawName.size()))) {
                entry.name.assign(reinterpret_cast<const char*>(variable.data() + pos + 5), length - 5);
                if (entry.name.find('\0') != std::string::npos || !validUtf8(entry.name))
                    throw std::runtime_error("Invalid ZIP Unicode name");
            }
            if (tag == 1) {
                if (hasZip64) throw std::runtime_error("Duplicate ZIP64 extra field");
                hasZip64 = true;
                size_t field = pos;
                const auto expand = [&](uint64_t& value, uint64_t sentinel, size_t width) {
                    if (value != sentinel) return;
                    if (width > pos + length - field) throw std::runtime_error("Truncated ZIP64 extra field");
                    value = width == 8 ? u64(variable.data() + field) : u32(variable.data() + field);
                    field += width;
                };
                expand(entry.size, 0xffffffff, 8); expand(entry.compressedSize, 0xffffffff, 8);
                expand(entry.localOffset, 0xffffffff, 8); expand(disk, 0xffff, 4);
            }
            pos += length;
        }
        if (needsZip64 && !hasZip64) throw std::runtime_error("ZIP64 entry sizes or offsets are missing");
        if (disk) throw std::runtime_error("Multi-volume ZIP entries are unsupported");
        if (entry.localOffset > size || size - entry.localOffset < 30 ||
            entry.compressedSize > size - entry.localOffset - 30 ||
            entry.size > uint64_t(std::numeric_limits<std::streamoff>::max()))
            throw std::runtime_error("Invalid ZIP entry offset or size");
        entries.push_back(std::move(entry));
        directory += header.size() + variableSize;
    }
    return entries;
}

void registerZipSource(const std::string& archivePath, const std::string& directory,
                       const std::vector<ZipSourceEntry>& entries) {
    const auto archive = archivePath.compare(0, 8, "content:") == 0 ? archivePath : key(archivePath);
    auto source = std::make_shared<ArchiveSource>(ArchiveSource{archive, key(directory), revision(archive)});
    std::unordered_map<std::string, Binding> bindings;
    for (const auto& entry : entries) {
        const auto relative = entryPath(entry.name);
        if (relative.empty() || relative == "." || relative.is_absolute() || relative.has_root_name() ||
            *relative.begin() == ".." || (entry.name.size() > 1 && entry.name[1] == ':'))
            throw std::runtime_error("Unsafe ZIP source path");
        if (!bindings.emplace(key(std::filesystem::path(directory) / relative), Binding{source, entry}).second)
            throw std::runtime_error("Duplicate ZIP source path");
    }
    std::lock_guard lock(sourceMutex);
    for (auto it = sources.begin(); it != sources.end();) {
        std::error_code error;
        if (it->second.archive->root == source->root ||
            !std::filesystem::exists(it->second.archive->root, error)) it = sources.erase(it);
        else ++it;
    }
    for (auto& binding : bindings) sources.insert_or_assign(binding.first, std::move(binding.second));
}

void unregisterZipSource(const std::string& directory) {
    const auto root = key(directory);
    std::lock_guard lock(sourceMutex);
    for (auto it = sources.begin(); it != sources.end();) {
        if (it->second.archive->root == root) it = sources.erase(it);
        else ++it;
    }
}

std::unique_ptr<std::streambuf> openZipSource(const std::filesystem::path& path) {
    const auto binding = lookup(path);
    return binding ? std::make_unique<ZipBuffer>(*binding) : nullptr;
}
std::optional<uintmax_t> zipSourceSize(const std::filesystem::path& path) {
    const auto binding = lookup(path);
    return binding ? std::optional<uintmax_t>(binding->entry.size) : std::nullopt;
}
std::string zipSourceRevision(const std::string& path) {
    const auto binding = lookup(path);
    return binding ? revision(binding->archive->path) + ':' + binding->entry.name : std::string();
}
bool isZipSource(const std::string& path) { return lookup(path).has_value(); }
} // namespace motioncam
