#pragma once

#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <streambuf>
#include <string>
#include <vector>

namespace motioncam {
struct ZipSourceEntry {
    std::string name; // UTF-8 discovery path
    std::string rawName; // Exact local/central directory bytes
    uint64_t size = 0;
    uint64_t compressedSize = 0;
    uint64_t localOffset = 0;
    uint32_t crc = 0;
    uint16_t method = 0;
    uint16_t flags = 0;
    bool regular = true;
};

// The directory contains empty discovery entries, never media payloads. Its
// lifetime is managed by the same owner as an extracted 7z staging directory.
std::vector<ZipSourceEntry> indexZipSource(const std::string& archivePath);
void registerZipSource(const std::string& archivePath, const std::string& directory,
                       const std::vector<ZipSourceEntry>& entries);
void unregisterZipSource(const std::string& directory);
std::unique_ptr<std::streambuf> openZipSource(const std::filesystem::path& path);
std::optional<uintmax_t> zipSourceSize(const std::filesystem::path& path);
std::string zipSourceRevision(const std::string& path);
bool isZipSource(const std::string& path);
} // namespace motioncam
