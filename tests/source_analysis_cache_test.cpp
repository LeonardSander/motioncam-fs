#include "SourceAnalysisCache.h"

#include <chrono>
#include <fstream>
#include <stdexcept>

int main() {
    namespace fs = std::filesystem;
    auto check = [](bool value) { if (!value) throw std::runtime_error("Cache eviction regression"); };
    const auto root = fs::temp_directory_path() / ("mcfuse-analysis-budget-" +
        std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    struct Cleanup {
        fs::path path;
        ~Cleanup() { std::error_code error; fs::remove_all(path, error); }
    } cleanup{root};
    motioncam::trimSourceAnalysisCache(root);
    check(!fs::exists(root));
    fs::create_directories(root / "dng-analysis");
    fs::create_directories(root / "other-analysis");
    auto create = [](const fs::path& path, uintmax_t bytes) {
        std::ofstream(path, std::ios::binary).close();
        fs::resize_file(path, bytes);
    };
    constexpr uintmax_t MiB = 1024 * 1024;
    const auto old = root / "dng-analysis" / "old.bin";
    const auto current = root / "other-analysis" / "new.bin";
    const auto index = root / "source-index.json";
    const auto temporary = root / "pending.tmp";
    create(old, 32 * MiB);
    create(current, 40 * MiB);
    create(index, 40 * MiB);
    create(temporary, 40 * MiB);
    fs::last_write_time(old, fs::file_time_type::clock::now() - std::chrono::hours(1));
    motioncam::trimSourceAnalysisCache(root);
    check(!fs::exists(old));
    check(fs::exists(current) && fs::exists(index) && fs::exists(temporary));
    // Under the budget, repeated cleanup must retain valid cache entries.
    motioncam::trimSourceAnalysisCache(root);
    check(fs::exists(current));
    const auto newer = root / "dng-analysis" / "newer.bin";
    create(newer, 40 * MiB);
    fs::last_write_time(current, fs::file_time_type::clock::now() - std::chrono::minutes(1));
    motioncam::trimSourceAnalysisCache(root);
    check(!fs::exists(current) && fs::exists(newer));
}
