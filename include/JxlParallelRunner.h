#pragma once

#include <jxl/decode.h>
#include <jxl/encode.h>
#include <jxl/thread_parallel_runner.h>

#include <algorithm>
#include <cstddef>
#include <cstdlib>
#include <thread>

namespace motioncam {

// libjxl uses Highway to dispatch its SIMD kernels on x86 and ARM64. Give
// large images a bounded per-operation thread pool as well. Encoding can run
// on several frames concurrently, so callers use a smaller cap for it.
class JxlParallelRunner {
public:
    explicit JxlParallelRunner(size_t sampleCount, size_t maxWorkers = 3) {
        if (sampleCount < 1024 * 1024) return;
        const unsigned int cores = std::thread::hardware_concurrency();
        size_t workers = std::min<size_t>(cores > 0 ? cores - 1 : 0, maxWorkers);
        if (const char* configured = std::getenv("MOTIONCAM_GALLERY_PERF_JXL_WORKERS")) {
            char* end = nullptr;
            const long requested = std::strtol(configured, &end, 10);
            if (end != configured && *end == '\0' && requested >= 0 && requested <= 16)
                workers = static_cast<size_t>(requested);
        }
        if (workers > 0) runner_ = JxlThreadParallelRunnerCreate(nullptr, workers);
    }

    ~JxlParallelRunner() {
        if (runner_) JxlThreadParallelRunnerDestroy(runner_);
    }

    JxlParallelRunner(const JxlParallelRunner&) = delete;
    JxlParallelRunner& operator=(const JxlParallelRunner&) = delete;

    bool attach(JxlDecoder* decoder) const {
        return !runner_ || JxlDecoderSetParallelRunner(
            decoder, JxlThreadParallelRunner, runner_) == JXL_DEC_SUCCESS;
    }

    bool attach(JxlEncoder* encoder) const {
        return !runner_ || JxlEncoderSetParallelRunner(
            encoder, JxlThreadParallelRunner, runner_) == JXL_ENC_SUCCESS;
    }

private:
    void* runner_ = nullptr;
};

} // namespace motioncam
