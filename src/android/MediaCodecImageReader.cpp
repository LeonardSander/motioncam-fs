#include "android/MediaCodecImageReader.h"
#ifdef __ANDROID__
#include <android/hardware_buffer.h>
#include <chrono>
#include <spdlog/spdlog.h>
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavcodec/mediacodec.h>
#include <libavutil/hwcontext.h>
#include <libavutil/hwcontext_mediacodec.h>
}
namespace motioncam {
MediaCodecImageReader::~MediaCodecImageReader() {
    if (mReader) {
        AImageReader_setImageListener(mReader, nullptr);
        AImageReader_delete(mReader);
    }
}
void MediaCodecImageReader::imageAvailable(void* context, AImageReader*) {
    auto* self = static_cast<MediaCodecImageReader*>(context);
    {
        std::lock_guard lock(self->mMutex);
        ++self->mSerial;
    }
    self->mAvailable.notify_all();
}
AVBufferRef* MediaCodecImageReader::createDevice(int width, int height) {
    auto* reader = new MediaCodecImageReader;
    if (AImageReader_newWithUsage(width, height, AIMAGE_FORMAT_PRIVATE,
            AHARDWAREBUFFER_USAGE_GPU_SAMPLED_IMAGE, 4, &reader->mReader) != AMEDIA_OK) {
        delete reader;
        return nullptr;
    }
    ANativeWindow* window = nullptr;
    AImageReader_ImageListener listener{reader, imageAvailable};
    if (AImageReader_getWindow(reader->mReader, &window) != AMEDIA_OK || !window ||
        AImageReader_setImageListener(reader->mReader, &listener) != AMEDIA_OK) {
        delete reader;
        return nullptr;
    }
    AVBufferRef* device = av_hwdevice_ctx_alloc(AV_HWDEVICE_TYPE_MEDIACODEC);
    if (!device) { delete reader; return nullptr; }
    auto* context = reinterpret_cast<AVHWDeviceContext*>(device->data);
    auto* media = static_cast<AVMediaCodecDeviceContext*>(context->hwctx);
    media->native_window = window;
    context->user_opaque = reader;
    context->free = [](AVHWDeviceContext* ctx) {
        delete static_cast<MediaCodecImageReader*>(ctx->user_opaque);
    };
    if (av_hwdevice_ctx_init(device) < 0) av_buffer_unref(&device);
    return device;
}
MediaCodecImageReader* MediaCodecImageReader::fromDevice(AVBufferRef* device) {
    if (!device) return nullptr;
    auto* context = reinterpret_cast<AVHWDeviceContext*>(device->data);
    return context->type == AV_HWDEVICE_TYPE_MEDIACODEC
        ? static_cast<MediaCodecImageReader*>(context->user_opaque) : nullptr;
}
bool MediaCodecImageReader::acquireRenderedFrame(AVFrame* frame,
        AImage** image, int* fenceFd) {
    if (!frame || frame->format != AV_PIX_FMT_MEDIACODEC || !frame->data[3] ||
        !image || !fenceFd) return false;
    *image = nullptr;
    *fenceFd = -1;
    std::unique_lock lock(mMutex);
    uint64_t observed = mSerial;
    lock.unlock();
    if (av_mediacodec_release_buffer(
            reinterpret_cast<AVMediaCodecBuffer*>(frame->data[3]), 1) < 0)
        return false;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(1);
    for (;;) {
        const media_status_t status = AImageReader_acquireNextImageAsync(
            mReader, image, fenceFd);
        if (status == AMEDIA_OK) return *image != nullptr;
        if (status != AMEDIA_IMGREADER_NO_BUFFER_AVAILABLE) {
            spdlog::warn("MediaCodec ImageReader acquire failed status={}", int(status));
            return false;
        }
        lock.lock();
        const bool available = mAvailable.wait_until(lock, deadline,
            [&] { return mSerial != observed; });
        observed = mSerial;
        lock.unlock();
        if (!available) return false;
    }
}
}
#endif
