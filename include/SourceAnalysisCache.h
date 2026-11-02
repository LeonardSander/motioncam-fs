#pragma once

#include <algorithm>
#include <filesystem>
#include <mutex>
#include <vector>

namespace motioncam {
// Only generated analysis entries are eligible; source URI indexes live elsewhere.
inline void trimSourceAnalysisCache(const std::filesystem::path& root) {
    static std::mutex mutex;
    const std::lock_guard lock(mutex);
    constexpr uintmax_t limit = 64ULL * 1024 * 1024;
    struct Entry {
        std::filesystem::path path;
        std::filesystem::file_time_type time;
        uintmax_t bytes;
    };
    std::vector<Entry> files;
    uintmax_t total = 0;
    std::error_code error;
    std::filesystem::recursive_directory_iterator it(root, error), end;
    for (; !error && it != end; it.increment(error)) {
        if (!it->is_regular_file(error) || it->path().extension() != ".bin") continue;
        const auto bytes = it->file_size(error);
        if (error) break;
        const auto time = it->last_write_time(error);
        if (error) break;
        files.push_back({it->path(), time, bytes});
        total += bytes;
    }
    std::sort(files.begin(), files.end(), [](const Entry& a, const Entry& b) {
        return a.time < b.time;
    });
    for (const auto& file : files) {
        if (total <= limit) break;
        error.clear();
        if (std::filesystem::remove(file.path, error)) total -= file.bytes;
    }
}
} // namespace motioncam
