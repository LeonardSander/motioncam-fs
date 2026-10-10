#pragma once

#include <memory>
#include <cstdint>
#include <array>

struct AVFrame;

namespace motioncam {

class DirectLogGpuRgb {
public:
    enum class TransferCurve { None, HLG, LOG60 };
    DirectLogGpuRgb();
    ~DirectLogGpuRgb();
    DirectLogGpuRgb(const DirectLogGpuRgb&) = delete;
    DirectLogGpuRgb& operator=(const DirectLogGpuRgb&) = delete;

    bool render(const AVFrame* source, int width, int height, bool smoothChroma,
                TransferCurve curve, bool fullRange, uint16_t* packedRgb,
                const std::array<uint8_t, 4>* cfaPhase = nullptr);

private:
    struct State;
    std::unique_ptr<State> mState;
};

} // namespace motioncam
