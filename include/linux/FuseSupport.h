#pragma once

#include <cerrno>
#include <cstddef>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <istream>
#include <limits>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

namespace rawpreamp::linux_fuse {

inline void setReadOnlyAttributes(struct stat& attributes, bool directory,
                                  size_t size, std::time_t modified) {
    std::memset(&attributes, 0, sizeof(attributes));
    attributes.st_mode = directory ? S_IFDIR | 0755 : S_IFREG | 0444;
    attributes.st_nlink = directory ? 2 : 1;
    attributes.st_size = directory ? 4096 : size;
    attributes.st_uid = getuid();
    attributes.st_gid = getgid();
    attributes.st_blksize = 4096;
    attributes.st_atime = attributes.st_mtime = attributes.st_ctime = modified;
    // Generated files are fully readable even though they allocate no disk
    // space. Resolve on Linux treats zero allocated blocks as offline media.
    // st_blocks uses 512-byte units, independent of the filesystem block size.
    if (!directory) attributes.st_blocks = size / 512 + (size % 512 != 0);
}

inline std::string decodeMountField(const std::string& field) {
    std::string decoded;
    for (size_t i = 0; i < field.size(); ++i) {
        if (field[i] == '\\' && i + 3 < field.size()) {
            const auto escape = field.substr(i, 4);
            if (escape == "\\040" || escape == "\\011" ||
                escape == "\\012" || escape == "\\134") {
                decoded += escape == "\\040" ? ' ' : escape == "\\011" ? '\t' :
                           escape == "\\012" ? '\n' : '\\';
                i += 3;
                continue;
            }
        }
        decoded += field[i];
    }
    return decoded;
}

struct MountRecord {
    std::string path;
    std::string type;
    std::string source;
};

inline std::vector<MountRecord> readMounts(std::istream& input) {
    std::vector<MountRecord> mounts;
    for (std::string line; std::getline(input, line);) {
        std::istringstream fields(line);
        std::vector<std::string> values;
        for (std::string field; fields >> field;) values.push_back(field);
        if (values.size() < 10) continue;
        for (size_t i = 6; i + 3 < values.size(); ++i) {
            if (values[i] != "-") continue;
            mounts.push_back({decodeMountField(values[4]), values[i + 1],
                              decodeMountField(values[i + 2])});
            break;
        }
    }
    return mounts;
}

// The PID is only a conservative liveness check, not an ownership token.
inline std::optional<int> ownerPid(const MountRecord& mount) {
    constexpr const char* prefix = "RawPreamp@";
    if (mount.type != "fuse.rawpreamp" || mount.source.rfind(prefix, 0) != 0)
        return std::nullopt;
    const auto digits = mount.source.substr(std::char_traits<char>::length(prefix));
    if (digits.empty()) return std::nullopt;
    int pid = 0;
    for (const char digit : digits) {
        if (digit < '0' || digit > '9' ||
            pid > (std::numeric_limits<int>::max() - (digit - '0')) / 10)
            return std::nullopt;
        pid = pid * 10 + digit - '0';
    }
    return pid > 0 ? std::optional<int>(pid) : std::nullopt;
}

inline bool canRecoverMount(const MountRecord& mount, bool providerDead, int probeError) {
    if (ownerPid(mount) && providerDead) return true;
    // Legacy mounts lack an identity. ENOTCONN proves that their userspace
    // connection is gone; a failed or timed-out probe does not.
    return mount.type == "fuse" && mount.source == "/dev/fuse" && probeError == ENOTCONN;
}

inline bool containsPath(const std::filesystem::path& parent,
                         const std::filesystem::path& child) {
    auto part = child.begin();
    for (const auto& component : parent) {
        if (component.empty()) continue;
        if (part == child.end() || component != *part++) return false;
    }
    return true;
}

inline void checkSourcePath(const std::filesystem::path& source,
                            const std::filesystem::path& destination) {
    if (containsPath(destination, source))
        throw std::runtime_error("Mount destination would hide the source: " + destination.string());
    // Putting a projection inside a source sequence can cause directory scans
    // to re-import generated frames as source frames.
    if (std::filesystem::is_directory(source) && containsPath(source, destination))
        throw std::runtime_error("Mount destination must be outside the source sequence: " +
                                 destination.string());
}

} // namespace rawpreamp::linux_fuse
