#include "SourceInput.h"
#include "SourceMedia.h"

#include <fcntl.h>

// Console tests have local fixtures and no Android activity/document provider.
// SourceInputStream itself still uses the production Android pread buffer.
namespace rawpreamp {
void prefetchSourceReadDescriptor(const std::string&) noexcept {}
int openSourceReadDescriptor(const std::string& path) {
    return ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
}
void releaseSourceReadDescriptors(const std::string&) {}
FILE* openSourceFile(const std::string& path) { return std::fopen(path.c_str(), "rb"); }
uintmax_t nativeSourceFileSize(const std::filesystem::path& path, std::error_code& error) {
    return std::filesystem::file_size(path, error);
}
std::string nativeSourceRevision(const std::string&) { return {}; }
int openSourceMedia(AVFormatContext** context, const std::string& path) {
    return avformat_open_input(context, path.c_str(), nullptr, nullptr);
}
void closeSourceMedia(AVFormatContext** context) { avformat_close_input(context); }
}
