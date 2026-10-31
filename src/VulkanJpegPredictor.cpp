#include "VulkanJpegPredictor.h"
#include "SharedVulkanDevice.h"

#ifdef MOTIONCAM_HAS_DIRECTLOG_PLACEBO
#include "VulkanJpegContext.h"
#include <libplacebo/vulkan.h>
#include <libplacebo/gpu.h>
#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <mutex>

namespace motioncam {
VulkanJpegContext& jpegContext() {
    static VulkanJpegContext context;
    return context;
}
VulkanJpegContext::~VulkanJpegContext() {
    pl_vulkan_destroy(&vulkan);
    pl_log_destroy(&log);
}
bool VulkanJpegContext::init() {
    if (unavailable) return false;
    if (vulkan) return true;
    log = pl_log_create(PL_API_VER, nullptr);
    if (!log) { unavailable = true; return false; }
    vulkan = createGalleryVulkanContext(log);
    if (!vulkan || !vulkan->gpu->glsl.compute) {
        unavailable = true;
        return false;
    }
    return true;
}
namespace {
constexpr char shader[] = R"glsl(#version 450
layout(local_size_x = 256) in;
layout(std430, binding = 0) readonly buffer Source { uint words[]; } src;
layout(std430, binding = 1) buffer Histogram { uint counts[]; } dst;
layout(push_constant) uniform Constants {
    uint width;
    uint height;
    uint channels;
    uint bits;
} p;
uint sampleAt(uint index) {
    uint word = src.words[index >> 1u];
    return (index & 1u) == 0u ? word & 65535u : word >> 16u;
}
int prediction(uint mode, int left, int above, int upperLeft) {
    if (mode == 1u) return left;
    if (mode == 2u) return above;
    if (mode == 3u) return upperLeft;
    if (mode == 4u) return left + above - upperLeft;
    if (mode == 5u) return left + ((above - upperLeft) >> 1);
    if (mode == 6u) return above + ((left - upperLeft) >> 1);
    return (left + above) >> 1;
}
void main() {
    uint index = gl_GlobalInvocationID.x;
    uint rowSamples = p.width * p.channels;
    uint count = rowSamples * p.height;
    if (index >= count) return;
    uint row = index / rowSamples;
    uint col = index % rowSamples;
    uint x = col / p.channels;
    if (count >= 1000000u &&
        ((((row >> 1u) * 3u + (x >> 1u)) & 7u) != 0u)) return;
    int value = int(sampleAt(index));
    int left = x > 0u ? int(sampleAt(index - p.channels)) : 0;
    int above = row > 0u ? int(sampleAt(index - rowSamples)) : 0;
    int upperLeft = row > 0u && x > 0u
        ? int(sampleAt(index - rowSamples - p.channels)) : 0;
    for (uint mode = 1u; mode <= 7u; ++mode) {
        int predicted = row == 0u && x == 0u ? int(1u << (p.bits - 1u))
            : row == 0u ? left : x == 0u ? above
            : prediction(mode, left, above, upperLeft);
        int difference = (value - predicted) & 65535;
        if (difference >= 32768) difference -= 65536;
        uint magnitude = uint(abs(difference));
        uint category = magnitude == 0u ? 0u : uint(findMSB(magnitude) + 1);
        atomicAdd(dst.counts[mode * 18u + category], 1u);
    }
}
)glsl";
constexpr char residualShader[] = R"glsl(#version 450
layout(local_size_x = 256) in;
layout(std430, binding = 0) readonly buffer Source { uint words[]; } src;
layout(std430, binding = 1) writeonly buffer Residuals { uint words[]; } dst;
layout(push_constant) uniform Constants {
    uint width;
    uint height;
    uint channels;
    uint bits;
    uint mode;
} p;
uint sampleAt(uint index) {
    uint word = src.words[index >> 1u];
    return (index & 1u) == 0u ? word & 65535u : word >> 16u;
}
int prediction(int left, int above, int upperLeft) {
    if (p.mode == 1u) return left;
    if (p.mode == 2u) return above;
    if (p.mode == 3u) return upperLeft;
    if (p.mode == 4u) return left + above - upperLeft;
    if (p.mode == 5u) return left + ((above - upperLeft) >> 1);
    if (p.mode == 6u) return above + ((left - upperLeft) >> 1);
    return (left + above) >> 1;
}
uint residualAt(uint index, uint count) {
    if (index >= count) return 0u;
    uint rowSamples = p.width * p.channels;
    uint row = index / rowSamples;
    uint col = index % rowSamples;
    uint x = col / p.channels;
    int value = int(sampleAt(index));
    int left = x > 0u ? int(sampleAt(index - p.channels)) : 0;
    int above = row > 0u ? int(sampleAt(index - rowSamples)) : 0;
    int upperLeft = row > 0u && x > 0u
        ? int(sampleAt(index - rowSamples - p.channels)) : 0;
    int predicted = row == 0u && x == 0u ? int(1u << (p.bits - 1u))
        : row == 0u ? left : x == 0u ? above
        : prediction(left, above, upperLeft);
    return uint(value - predicted) & 65535u;
}
void main() {
    uint wordIndex = gl_GlobalInvocationID.x;
    uint count = p.width * p.height * p.channels;
    uint index = wordIndex * 2u;
    if (index >= count) return;
    uint lo = residualAt(index, count);
    uint hi = residualAt(index + 1u, count);
    dst.words[wordIndex] = lo | (hi << 16u);
}
)glsl";

struct State {
    pl_gpu gpu = nullptr;
    pl_pass pass = nullptr;
    pl_pass residualPass = nullptr;
    pl_buf source = nullptr, upload = nullptr, counts = nullptr, readback = nullptr;
    pl_buf residualOutput = nullptr, residualReadback = nullptr;
    const uint16_t* residentPixels = nullptr;
    uint32_t residentWidth = 0, residentHeight = 0;
    uint32_t residentChannels = 0, residentBits = 0;
    ~State() {
        if (gpu) {
            pl_gpu_finish(gpu);
            pl_buf_destroy(gpu, &source);
            pl_buf_destroy(gpu, &upload);
            pl_buf_destroy(gpu, &counts);
            pl_buf_destroy(gpu, &readback);
            pl_buf_destroy(gpu, &residualOutput);
            pl_buf_destroy(gpu, &residualReadback);
            pl_pass_destroy(gpu, &pass);
            pl_pass_destroy(gpu, &residualPass);
        }
    }
    bool init(pl_gpu sharedGpu) {
        if (pass) return true;
        gpu = sharedGpu;
        pl_desc descriptors[2] = {
            {"src", PL_DESC_BUF_STORAGE, 0, PL_DESC_ACCESS_READONLY},
            {"dst", PL_DESC_BUF_STORAGE, 1, PL_DESC_ACCESS_READWRITE}
        };
        pl_pass_params params = {};
        params.type = PL_PASS_COMPUTE;
        params.descriptors = descriptors;
        params.num_descriptors = 2;
        params.push_constants_size = 4 * sizeof(uint32_t);
        params.glsl_shader = shader;
        pass = pl_pass_create(gpu, &params);
        return pass != nullptr;
    }
    bool ensureResidualPass() {
        if (residualPass) return true;
        pl_desc descriptors[2] = {
            {"src", PL_DESC_BUF_STORAGE, 0, PL_DESC_ACCESS_READONLY},
            {"dst", PL_DESC_BUF_STORAGE, 1, PL_DESC_ACCESS_WRITEONLY}
        };
        pl_pass_params params = {};
        params.type = PL_PASS_COMPUTE;
        params.descriptors = descriptors;
        params.num_descriptors = 2;
        params.push_constants_size = 5 * sizeof(uint32_t);
        params.glsl_shader = residualShader;
        residualPass = pl_pass_create(gpu, &params);
        return residualPass != nullptr;
    }
};
State& state() { static State value; return value; }
}

pl_buf jpegResidentImageVulkan(const uint16_t* pixels, uint32_t width,
                               uint32_t height, uint32_t channels,
                               uint32_t bits) {
    const auto& s = state();
    return s.residentPixels == pixels && s.residentWidth == width &&
           s.residentHeight == height && s.residentChannels == channels &&
           s.residentBits == bits ? s.source : nullptr;
}

bool jpegPredictorHistogramVulkan(const uint16_t* pixels, uint32_t width,
                                  uint32_t height, uint32_t channels,
                                  uint32_t bits, int& predictor,
                                  int histogram[18],
                                  std::vector<int16_t>* residuals) {
    const auto started = std::chrono::steady_clock::now();
    if (residuals) residuals->clear();
    if (!pixels || !histogram || !width || !height ||
        (channels != 1 && channels != 3) || bits < 8 || bits > 16 ||
        static_cast<uint64_t>(width) * height * channels > UINT32_MAX / 2u)
        return false;
    const uint16_t endianProbe = 1;
    if (*reinterpret_cast<const uint8_t*>(&endianProbe) != 1) return false;
    auto& context = jpegContext();
    auto& s = state();
    std::lock_guard lock(context.mutex);
    s.residentPixels = nullptr;
    if (!context.init() || !s.init(context.vulkan->gpu)) return false;
    pl_gpu gpu = context.vulkan->gpu;
    const size_t samples = static_cast<size_t>(width) * height * channels;
    const size_t sourceBytes = (samples + 1u) / 2u * sizeof(uint32_t);
    constexpr size_t histogramBytes = 8 * 18 * sizeof(uint32_t);
    if (sourceBytes > gpu->limits.max_ssbo_size) return false;
    pl_buf_params source = {}, upload = {}, counts = {}, readback = {};
    source.size = sourceBytes; source.storable = true;
    source.memory_type = PL_BUF_MEM_DEVICE;
    upload.size = sourceBytes; upload.host_writable = true;
    upload.memory_type = PL_BUF_MEM_HOST;
    counts.size = histogramBytes; counts.storable = true;
    counts.memory_type = PL_BUF_MEM_DEVICE;
    readback.size = histogramBytes; readback.host_readable = true;
    readback.host_writable = true;
    readback.memory_type = PL_BUF_MEM_HOST;
    if (!pl_buf_recreate(gpu, &s.source, &source) ||
        !pl_buf_recreate(gpu, &s.upload, &upload) ||
        !pl_buf_recreate(gpu, &s.counts, &counts) ||
        !pl_buf_recreate(gpu, &s.readback, &readback)) return false;
    pl_buf_write(gpu, s.upload, 0, pixels, samples * sizeof(uint16_t));
    if (samples & 1u) {
        const uint16_t zero = 0;
        pl_buf_write(gpu, s.upload, samples * sizeof(uint16_t), &zero, sizeof(zero));
    }
    std::array<uint32_t, 8 * 18> zero{};
    pl_buf_write(gpu, s.readback, 0, zero.data(), histogramBytes);
    pl_buf_copy(gpu, s.source, 0, s.upload, 0, sourceBytes);
    pl_buf_copy(gpu, s.counts, 0, s.readback, 0, histogramBytes);
    uint32_t constants[4] = {width, height, channels, bits};
    pl_desc_binding bindings[2] = {};
    bindings[0].object = s.source;
    bindings[1].object = s.counts;
    pl_pass_run_params run = {};
    run.pass = s.pass;
    run.desc_bindings = bindings;
    run.push_constants = constants;
    run.compute_groups[0] = static_cast<int>((samples + 255u) / 256u);
    run.compute_groups[1] = run.compute_groups[2] = 1;
    if (run.compute_groups[0] > gpu->limits.max_dispatch[0]) return false;
    pl_pass_run(gpu, &run);
    pl_buf_copy(gpu, s.readback, 0, s.counts, 0, histogramBytes);
    std::array<uint32_t, 8 * 18> result{};
    if (!pl_buf_read(gpu, s.readback, 0, result.data(), histogramBytes)) return false;
    s.residentPixels = pixels;
    s.residentWidth = width;
    s.residentHeight = height;
    s.residentChannels = channels;
    s.residentBits = bits;
    const auto histogramReady = std::chrono::steady_clock::now();
    predictor = 1;
    uint64_t best = UINT64_MAX;
    for (int mode = 1; mode <= 7; ++mode) {
        uint64_t cost = 0;
        for (int category = 0; category <= 16; ++category)
            cost += static_cast<uint64_t>(category) * result[mode * 18 + category];
        if (cost < best) { best = cost; predictor = mode; }
    }
    for (int category = 0; category < 18; ++category) {
        const uint32_t count = result[predictor * 18 + category];
        if (count > INT32_MAX) return false;
        histogram[category] = static_cast<int>(count);
        if (samples >= 1000000u && category <= 16 && !histogram[category])
            histogram[category] = 1;
    }
    if (residuals && s.ensureResidualPass()) {
        pl_buf_params output = {}, stage = {};
        output.size = sourceBytes;
        output.storable = true;
        output.memory_type = PL_BUF_MEM_DEVICE;
        stage.size = sourceBytes;
        stage.host_readable = true;
        stage.memory_type = PL_BUF_MEM_HOST;
        const size_t words = (samples + 1u) / 2u;
        const size_t groups = (words + 255u) / 256u;
        if (sourceBytes <= gpu->limits.max_ssbo_size &&
            groups <= static_cast<size_t>(gpu->limits.max_dispatch[0]) &&
            pl_buf_recreate(gpu, &s.residualOutput, &output) &&
            pl_buf_recreate(gpu, &s.residualReadback, &stage)) {
            uint32_t residualConstants[5] = {
                width, height, channels, bits, static_cast<uint32_t>(predictor)
            };
            pl_desc_binding residualBindings[2] = {};
            residualBindings[0].object = s.source;
            residualBindings[1].object = s.residualOutput;
            pl_pass_run_params residualRun = {};
            residualRun.pass = s.residualPass;
            residualRun.desc_bindings = residualBindings;
            residualRun.push_constants = residualConstants;
            residualRun.compute_groups[0] = static_cast<int>(groups);
            residualRun.compute_groups[1] = residualRun.compute_groups[2] = 1;
            pl_pass_run(gpu, &residualRun);
            pl_buf_copy(gpu, s.residualReadback, 0,
                        s.residualOutput, 0, sourceBytes);
            residuals->resize(words * 2u);
            if (pl_buf_read(gpu, s.residualReadback, 0,
                            residuals->data(), sourceBytes))
                residuals->resize(samples);
            else residuals->clear();
        }
    }
    if (std::getenv("MOTIONCAM_JPEG_ENCODE_PROFILE"))
        std::fprintf(stderr, "JPEG encode GPU predictor: histogram_ms=%.3f residual_ms=%.3f samples=%zu\n",
                     std::chrono::duration<double, std::milli>(histogramReady - started).count(),
                     std::chrono::duration<double, std::milli>(
                         std::chrono::steady_clock::now() - histogramReady).count(),
                     samples);
    return true;
}
}
#else
namespace motioncam {
bool jpegPredictorHistogramVulkan(const uint16_t*, uint32_t, uint32_t,
                                  uint32_t, uint32_t, int&, int[18],
                                  std::vector<int16_t>*) {
    return false;
}
}
#endif
