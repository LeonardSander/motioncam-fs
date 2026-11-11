#pragma once
#include <string>
extern "C" {
#include <libavformat/avformat.h>
}
namespace rawpreamp {
#ifdef __ANDROID__
int openSourceMedia(AVFormatContext** context, const std::string& path);
void closeSourceMedia(AVFormatContext** context);
#else
inline int openSourceMedia(AVFormatContext** context, const std::string& path) {
    return avformat_open_input(context, path.c_str(), nullptr, nullptr);
}
inline void closeSourceMedia(AVFormatContext** context) { avformat_close_input(context); }
#endif
}
