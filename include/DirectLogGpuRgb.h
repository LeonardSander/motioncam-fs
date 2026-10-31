#pragma once

#include <memory>
#include <cstdint>
#include <cstddef>
#include <array>

struct AVFrame;
#ifdef __ANDROID__
struct AImage;
#endif

namespace motioncam {

class DirectLogGpuRgb {
public:
    enum class TransferCurve { None, HLG, LOG60 };
    // Shared GLSL for the renderer hooks and native hardware-buffer sampling.
    static const char* transferShader();
    DirectLogGpuRgb();
    ~DirectLogGpuRgb();
    DirectLogGpuRgb(const DirectLogGpuRgb&) = delete;
    DirectLogGpuRgb& operator=(const DirectLogGpuRgb&) = delete;

    bool render(const AVFrame* source, int width, int height, bool smoothChroma,
                TransferCurve curve, bool fullRange, uint16_t* packedRgb,
                const std::array<uint8_t, 4>* cfaPhase = nullptr,
                bool deferDownload = false);
#ifdef __ANDROID__
    // Consumes image and acquireFenceFd on every path.
    bool renderAndroidImage(const AVFrame* source, AImage* image, int acquireFenceFd,
        int width, int height, bool smoothChroma, TransferCurve curve,
        bool fullRange, uint16_t* packedRgb,
        const std::array<uint8_t, 4>* cfaPhase, bool deferDownload);
#endif
    bool pendingTexture(const void*& gpu, const void*& texture) const;
    std::shared_ptr<void> deviceLease() const;
    bool downloadPending(uint16_t* pixels, size_t samples);
    bool prepareForSeek();

private:
    struct State;
    std::shared_ptr<State> mState;
};

} // namespace motioncam
