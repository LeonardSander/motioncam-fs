#include "HardwareVideoDecoder.h"

#include <cstdio>
#include <initializer_list>

extern "C" {
#include <libavutil/hwcontext.h>
}

namespace {
bool hasDeviceConfig(const AVCodec* codec, AVHWDeviceType type) {
    if (!codec) return false;
    for (int i = 0; const AVCodecHWConfig* config = avcodec_get_hw_config(codec, i); ++i)
        if (config->device_type == type &&
            (config->methods & AV_CODEC_HW_CONFIG_METHOD_HW_DEVICE_CTX)) return true;
    return false;
}

bool check(AVCodecID id, AVHWDeviceType type, const char* wrapper = nullptr) {
    const AVCodec* codec = wrapper
        ? rawpreamp::findWrappedHardwareDecoder(id, wrapper)
        : avcodec_find_decoder_by_name(avcodec_get_name(id));
    const bool found = hasDeviceConfig(codec, type);
    std::fprintf(found ? stdout : stderr, "%s: %s decoder support %s\n",
        avcodec_get_name(id), av_hwdevice_get_type_name(type), found ? "present" : "MISSING");
    return found;
}
}

int main() {
    // Inspect the linked libraries only: CI needs no GPU, driver or Android JVM.
    bool passed = true;
    for (AVCodecID id : {AV_CODEC_ID_H264, AV_CODEC_ID_HEVC, AV_CODEC_ID_AV1}) {
#if defined(__ANDROID__)
        passed &= check(id, AV_HWDEVICE_TYPE_MEDIACODEC, "mediacodec");
#elif defined(__APPLE__)
        passed &= check(id, AV_HWDEVICE_TYPE_VIDEOTOOLBOX);
#elif defined(_WIN32)
        passed &= check(id, AV_HWDEVICE_TYPE_D3D11VA);
        passed &= check(id, AV_HWDEVICE_TYPE_DXVA2);
        passed &= check(id, AV_HWDEVICE_TYPE_D3D12VA);
        passed &= check(id, AV_HWDEVICE_TYPE_VULKAN);
#if defined(_M_X64) || defined(_M_IX86) || defined(__x86_64__) || defined(__i386__)
        passed &= check(id, AV_HWDEVICE_TYPE_CUDA);
        passed &= check(id, AV_HWDEVICE_TYPE_QSV, "qsv");
#endif
#elif defined(__linux__)
        passed &= check(id, AV_HWDEVICE_TYPE_VAAPI);
        passed &= check(id, AV_HWDEVICE_TYPE_VULKAN);
#if defined(__x86_64__) || defined(__i386__)
        passed &= check(id, AV_HWDEVICE_TYPE_CUDA);
        passed &= check(id, AV_HWDEVICE_TYPE_QSV, "qsv");
#endif
#endif
    }
#if defined(__APPLE__)
    passed &= check(AV_CODEC_ID_PRORES, AV_HWDEVICE_TYPE_VIDEOTOOLBOX);
#elif defined(_WIN32) || (defined(__linux__) && !defined(__ANDROID__))
    passed &= check(AV_CODEC_ID_PRORES, AV_HWDEVICE_TYPE_VULKAN);
#endif
    if (!avcodec_find_decoder_by_name("libdav1d")) {
        std::fprintf(stderr, "AV1 software fallback libdav1d is MISSING\n");
        passed = false;
    }
    if (rawpreamp::findWrappedHardwareDecoder(AV_CODEC_ID_NONE, "qsv")) return 1;
    return passed ? 0 : 1;
}
