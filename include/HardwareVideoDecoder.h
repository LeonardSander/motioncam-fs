#pragma once

#include <cstring>

extern "C" {
#include <libavcodec/avcodec.h>
}

namespace rawpreamp {
inline const AVCodec* findWrappedHardwareDecoder(AVCodecID id, const char* wrapper) {
    void* iterator = nullptr;
    while (const AVCodec* codec = av_codec_iterate(&iterator)) {
        if (av_codec_is_decoder(codec) && codec->id == id && codec->wrapper_name &&
            std::strcmp(codec->wrapper_name, wrapper) == 0)
            return codec;
    }
    return nullptr;
}
}
