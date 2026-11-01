#include "SourceInput.h"
#include <algorithm>
#include <array>
#include <cassert>
#include <cstdlib>
#include <fcntl.h>
#include <future>
#include <vector>

namespace {
int sourceFd = -1;
std::string sourcePath;
}
namespace motioncam {
int openSourceReadDescriptor(const std::string& path) {
    return path == sourcePath ? ::dup(sourceFd) : ::open(path.c_str(), O_RDONLY);
}
uintmax_t sourceFileSize(const std::filesystem::path& path, std::error_code& error) {
    return std::filesystem::file_size(path, error);
}
}
int main() {
    std::string pattern = (std::filesystem::temp_directory_path() / "motioncam-source-XXXXXX").string();
    sourceFd = ::mkstemp(pattern.data());
    assert(sourceFd >= 0);
    sourcePath = pattern;
    std::vector<char> bytes(200000);
    for (size_t i = 0; i < bytes.size(); ++i) bytes[i] = char(i * 37);
    {
        std::ofstream output(sourcePath, std::ios::binary);
        output.write(bytes.data(), bytes.size());
        assert(output.good());
    }
    {
        motioncam::SourceInputStream first(sourcePath), second(sourcePath, std::ios::binary | std::ios::ate);
        assert(first && second && second.tellg() == std::streampos(bytes.size()));
        std::array<char, 101> part;
        first.read(part.data(), part.size());
        assert(first.tellg() == 101 && std::equal(part.begin(), part.end(), bytes.begin()));
        first.seekg(-40, std::ios::cur);
        assert(first.tellg() == 61);
        first.read(part.data(), part.size());
        assert(std::equal(part.begin(), part.end(), bytes.begin() + 61));
        second.seekg(65530);
        second.read(part.data(), part.size());
        assert(std::equal(part.begin(), part.end(), bytes.begin() + 65530));
        assert(first.tellg() == 162);
        first.seekg(32);
        std::vector<char> bulk(150000);
        first.read(bulk.data(), bulk.size());
        assert(first && std::equal(bulk.begin(), bulk.end(), bytes.begin() + 32));
        assert(first.tellg() == 150032);
        first.seekg(-9, std::ios::cur);
        first.read(part.data(), part.size());
        assert(std::equal(part.begin(), part.end(), bytes.begin() + 150023));
        first.seekg(0);
        const std::vector<char> all((std::istreambuf_iterator<char>(first)), {});
        assert(all == bytes);
        first.clear();
        first.seekg(-8, std::ios::end);
        first.read(part.data(), 8);
        assert(std::equal(part.begin(), part.begin() + 8, bytes.end() - 8));
        first.read(part.data(), 1);
        assert(first.eof());
        first.clear();
        first.seekg(0);
        first.seekg(-1, std::ios::beg);
        assert(first.fail());
        first.clear();
        assert(first.tellg() == 0);
        first.close();
        assert(!first.is_open());
        motioncam::SourceInputStream missing(sourcePath + "-missing");
        assert(!missing && !missing.is_open());
    }
    // All readers duplicate one open file description. Concurrent random reads
    // must never move its kernel cursor or affect another reader's data.
    std::vector<std::future<void>> readers;
    for (size_t worker = 0; worker < 8; ++worker) {
        readers.push_back(std::async(std::launch::async, [&, worker] {
            motioncam::SourceInputStream input(sourcePath);
            std::array<char, 101> part;
            for (size_t i = 0; i < 500; ++i) {
                const size_t position = (i * 7919 + worker * 65521) % (bytes.size() - part.size());
                input.seekg(position);
                input.read(part.data(), part.size());
                assert(input && std::equal(part.begin(), part.end(), bytes.begin() + position));
                assert(input.tellg() == std::streampos(position + part.size()));
            }
        }));
    }
    for (auto& reader : readers) reader.get();
    assert(::lseek(sourceFd, 0, SEEK_CUR) == 0);
    {
        motioncam::SourceInputStream retained(sourcePath);
        ::close(sourceFd);
        sourceFd = -1;
        std::array<char, 101> part;
        retained.seekg(65530);
        retained.read(part.data(), part.size());
        assert(retained && std::equal(part.begin(), part.end(), bytes.begin() + 65530));
    }
    std::filesystem::remove(sourcePath);
}
