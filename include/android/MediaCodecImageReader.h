#pragma once
#ifdef __ANDROID__
#include <media/NdkImageReader.h>
#include <condition_variable>
#include <mutex>
#include <cstdint>
struct AVCodecContext;
struct AVFrame;
struct AVBufferRef;
namespace motioncam {
// Reader lifetime follows FFmpeg's hardware device, including retained frames.
class MediaCodecImageReader {
public:
    static AVBufferRef* createDevice(int width, int height);
    // Only pass a device created by createDevice; retain it while using this pointer.
    static MediaCodecImageReader* fromDevice(AVBufferRef* device);
    // Caller owns the returned image and acquire fd. Import the acquire fence
    // before sampling, then return the image with AImage_deleteAsync and the
    // GPU completion fence. This method only waits for buffer availability.
    bool acquireRenderedFrame(AVFrame* frame, AImage** image, int* fenceFd);
private:
    ~MediaCodecImageReader();
    static void imageAvailable(void* context, AImageReader*);
    AImageReader* mReader = nullptr;
    std::mutex mMutex;
    std::condition_variable mAvailable;
    uint64_t mSerial = 0;
};
}
#endif
