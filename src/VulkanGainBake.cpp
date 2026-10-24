#include "VulkanGainBake.h"

#ifdef MOTIONCAM_HAS_DIRECTLOG_PLACEBO
#include "VulkanGainBakeShader.h"
#include <libplacebo/vulkan.h>
#include <libplacebo/gpu.h>
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <future>
#include <limits>
#include <memory>
#include <unordered_map>
#include <mutex>
#include <string>
#include <thread>
#if defined(__x86_64__) && (defined(__GNUC__) || defined(__clang__))
#include <immintrin.h>
#endif

namespace motioncam {
namespace {
#if defined(__x86_64__) && (defined(__GNUC__) || defined(__clang__))
__attribute__((target("avx2"))) void streamCopy(uint8_t* destination,
                                                  const uint8_t* source,
                                                  size_t bytes) {
    size_t prefix = std::min(bytes, (size_t{32} -
        (reinterpret_cast<uintptr_t>(destination) & 31u)) & 31u);
    if (prefix) std::memcpy(destination, source, prefix);
    size_t offset = prefix;
    for (; offset + 32 <= bytes; offset += 32) {
        const auto value = _mm256_loadu_si256(
            reinterpret_cast<const __m256i*>(source + offset));
        _mm256_stream_si256(reinterpret_cast<__m256i*>(destination + offset), value);
    }
    _mm_sfence();
    if (offset < bytes)
        std::memcpy(destination + offset, source + offset, bytes - offset);
}
#endif
struct alignas(16) Constants {
    float black[4], destinationBlack[4], scale[4];
    uint32_t dimensions[4], mapShape[4];
    float destinationWhite;
    uint32_t encodedWhite, ditherSeed, flags, rowBytes, wordsPerRow;
};
struct Device {
    pl_log log = nullptr;
    pl_vulkan vk = nullptr;
    pl_gpu gpu = nullptr;
    bool textureSource = false;
    bool mappedOutput = false;
    bool splitOutput = false;
    bool mappedReadbackUnavailable = false;
    pl_pass pass = nullptr, cfaPass = nullptr;
    pl_pass multiPass = nullptr, multiCfaPass = nullptr;
    pl_timer passTimer = nullptr;
    pl_buf source = nullptr, map = nullptr, output = nullptr, linearization = nullptr;
    pl_buf directOutput = nullptr;
    pl_buf sourceStage = nullptr, mapStage = nullptr, outputStage = nullptr;
    pl_buf outputStageSecond = nullptr;
    pl_buf linearizationStage = nullptr;
    ~Device() {
        if (gpu) {
            pl_gpu_finish(gpu);
            pl_buf_destroy(gpu, &source);
            pl_buf_destroy(gpu, &map);
            pl_buf_destroy(gpu, &output);
            pl_buf_destroy(gpu, &directOutput);
            pl_buf_destroy(gpu, &linearization);
            pl_buf_destroy(gpu, &sourceStage);
            pl_buf_destroy(gpu, &mapStage);
            pl_buf_destroy(gpu, &outputStage);
            pl_buf_destroy(gpu, &outputStageSecond);
            pl_buf_destroy(gpu, &linearizationStage);
            pl_pass_destroy(gpu, &pass);
            pl_pass_destroy(gpu, &cfaPass);
            pl_pass_destroy(gpu, &multiPass);
            pl_pass_destroy(gpu, &multiCfaPass);
            pl_timer_destroy(gpu, &passTimer);
        }
        pl_vulkan_destroy(&vk);
        pl_log_destroy(&log);
    }
    bool init() {
        const auto started = std::chrono::steady_clock::now();
        log = pl_log_create(PL_API_VER, nullptr);
        if (!log) return false;
        vk = pl_vulkan_create(log, nullptr);
        if (!vk || !vk->gpu->glsl.compute) return false;
        gpu = vk->gpu;
        const auto vulkanReady = std::chrono::steady_clock::now();
        pl_desc desc[4] = {};
        desc[0] = {"src", PL_DESC_BUF_STORAGE, 0, PL_DESC_ACCESS_READONLY};
        desc[1] = {"gainMap", PL_DESC_BUF_STORAGE, 1, PL_DESC_ACCESS_READONLY};
        desc[2] = {"dst", PL_DESC_BUF_STORAGE, 2, PL_DESC_ACCESS_WRITEONLY};
        desc[3] = {"linearization", PL_DESC_BUF_STORAGE, 3, PL_DESC_ACCESS_READONLY};
        pl_pass_params pp = {};
        pp.type = PL_PASS_COMPUTE;
        pp.descriptors = desc;
        pp.num_descriptors = 4;
        pp.push_constants_size = sizeof(Constants);
        pp.glsl_shader = vulkanGainBakeShader;
        pass = pl_pass_create(vk->gpu, &pp);
        pp.glsl_shader = vulkanCfaGainBakeShader;
        cfaPass = pl_pass_create(vk->gpu, &pp);
        if (std::getenv("MOTIONCAM_GALLERY_PERF_PROFILE"))
            passTimer = pl_timer_create(gpu);
        if (std::getenv("MOTIONCAM_GALLERY_PERF_PROFILE")) {
            const auto millis = [](auto a, auto b) {
                return std::chrono::duration<double, std::milli>(b - a).count();
            };
            std::fprintf(stderr,
                "GALLERY_PERF event=vulkan_gain_init vulkan_ms=%.3f shader_ms=%.3f\n",
                millis(started, vulkanReady),
                millis(vulkanReady, std::chrono::steady_clock::now()));
        }
        return pass && cfaPass;
    }
    bool initExternal(pl_gpu externalGpu) {
        if (!externalGpu || !externalGpu->glsl.compute) return false;
        gpu = externalGpu;
        textureSource = true;
        pl_desc desc[4] = {};
        desc[0] = {"sourceImage", PL_DESC_SAMPLED_TEX, 0, PL_DESC_ACCESS_READONLY};
        desc[1] = {"gainMap", PL_DESC_BUF_STORAGE, 1, PL_DESC_ACCESS_READONLY};
        desc[2] = {"dst", PL_DESC_BUF_STORAGE, 2, PL_DESC_ACCESS_WRITEONLY};
        desc[3] = {"linearization", PL_DESC_BUF_STORAGE, 3, PL_DESC_ACCESS_READONLY};
        pl_pass_params pp = {};
        pp.type = PL_PASS_COMPUTE;
        pp.descriptors = desc;
        pp.num_descriptors = 4;
        pp.push_constants_size = sizeof(Constants);
        const auto shaderSource = [](const char* source) {
            const char* newline = std::strchr(source, '\n');
            return newline ? std::string(source, newline + 1) +
                    "#define SOURCE_TEXTURE 1\n" + (newline + 1)
                : std::string();
        };
        std::string rgb = shaderSource(vulkanGainBakeShader);
        std::string cfa = shaderSource(vulkanCfaGainBakeShader);
        pp.glsl_shader = rgb.c_str();
        pass = pl_pass_create(gpu, &pp);
        pp.glsl_shader = cfa.c_str();
        cfaPass = pl_pass_create(gpu, &pp);
        if (std::getenv("MOTIONCAM_GALLERY_PERF_PROFILE"))
            passTimer = pl_timer_create(gpu);
        return pass && cfaPass;
    }
    bool ensureMultiPass(bool cfa) {
        pl_pass& selected = cfa ? multiCfaPass : multiPass;
        if (selected) return true;
        pl_desc desc[4] = {};
        desc[0] = {textureSource ? "sourceImage" : "src",
                   textureSource ? PL_DESC_SAMPLED_TEX : PL_DESC_BUF_STORAGE,
                   0, PL_DESC_ACCESS_READONLY};
        desc[1] = {"gainMap", PL_DESC_BUF_STORAGE, 1, PL_DESC_ACCESS_READONLY};
        desc[2] = {"dst", PL_DESC_BUF_STORAGE, 2, PL_DESC_ACCESS_WRITEONLY};
        desc[3] = {"linearization", PL_DESC_BUF_STORAGE, 3, PL_DESC_ACCESS_READONLY};
        pl_pass_params pp = {};
        pp.type = PL_PASS_COMPUTE;
        pp.descriptors = desc;
        pp.num_descriptors = 4;
        pp.push_constants_size = sizeof(Constants);
        const char* source = cfa ? vulkanCfaGainBakeShader : vulkanGainBakeShader;
        const char* newline = std::strchr(source, '\n');
        if (!newline) return false;
        const std::string shader = std::string(source, newline + 1) +
            (textureSource ? "#define SOURCE_TEXTURE 1\n" : "") +
            "#define MULTI_MAP 1\n" + (newline + 1);
        pp.glsl_shader = shader.c_str();
        selected = pl_pass_create(gpu, &pp);
        return selected != nullptr;
    }
    bool reserve(size_t sourceBytes, size_t mapBytes, size_t outputBytes,
                 size_t linearizationBytes) {
        const auto max = gpu->limits.max_ssbo_size;
        if ((!textureSource && sourceBytes > max) || mapBytes > max || outputBytes > max ||
            linearizationBytes > max) return false;
        pl_buf_params src = {}, gains = {}, dst = {}, lut = {};
        src.size = sourceBytes;
        src.storable = true;
        src.memory_type = PL_BUF_MEM_DEVICE;
        gains.size = mapBytes;
        gains.storable = true;
        gains.memory_type = PL_BUF_MEM_DEVICE;
        dst.size = outputBytes;
        dst.storable = true;
        dst.memory_type = PL_BUF_MEM_DEVICE;
        lut.size = linearizationBytes;
        lut.storable = true;
        lut.memory_type = PL_BUF_MEM_DEVICE;
        pl_buf_params srcUpload = {}, mapUpload = {}, dstDownload = {}, lutUpload = {};
        srcUpload.size = sourceBytes;
        srcUpload.host_writable = true;
        srcUpload.memory_type = PL_BUF_MEM_HOST;
        mapUpload.size = mapBytes;
        mapUpload.host_writable = true;
        mapUpload.memory_type = PL_BUF_MEM_HOST;
        const bool wantMapped = outputBytes >= 32 * 1024 * 1024 &&
            !mappedReadbackUnavailable;
        const bool trySplit = wantMapped && outputBytes >= 48 * 1024 * 1024;
        const size_t firstBytes = (outputBytes / 2) & ~size_t{63};
        dstDownload.size = trySplit ? firstBytes : outputBytes;
        dstDownload.host_readable = true;
        dstDownload.memory_type = PL_BUF_MEM_HOST;
        dstDownload.host_mapped = wantMapped;
        lutUpload.size = linearizationBytes;
        lutUpload.host_writable = true;
        lutUpload.memory_type = PL_BUF_MEM_HOST;
        // Integrated GPUs can expose device-local storage directly to the
        // CPU. Let the shader write to that mapping and omit the readback copy.
        bool integrated = false;
        if (const auto vulkan = pl_vulkan_get(gpu); vulkan && vulkan->get_proc_addr) {
            const auto getProperties = reinterpret_cast<PFN_vkGetPhysicalDeviceProperties>(
                vulkan->get_proc_addr(vulkan->instance, "vkGetPhysicalDeviceProperties"));
            if (getProperties) {
                VkPhysicalDeviceProperties properties = {};
                getProperties(vulkan->phys_device, &properties);
                integrated = properties.deviceType == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU;
            }
        }
        if (integrated && gpu->limits.max_mapped_vram >= outputBytes) {
            pl_buf_params direct = {};
            direct.size = outputBytes;
            direct.storable = true;
            direct.host_readable = true;
            direct.host_mapped = true;
            direct.memory_type = PL_BUF_MEM_DEVICE;
            if (!pl_buf_recreate(gpu, &directOutput, &direct) || !directOutput->data)
                pl_buf_destroy(gpu, &directOutput);
        } else {
            pl_buf_destroy(gpu, &directOutput);
        }
        if (directOutput) {
            mappedOutput = true;
            splitOutput = false;
        } else {
            const bool mappedReady = dstDownload.host_mapped &&
                pl_buf_recreate(gpu, &outputStage, &dstDownload);
            mappedOutput = mappedReady && outputStage->data;
            splitOutput = false;
            if (mappedOutput && trySplit) {
                dstDownload.size = outputBytes - firstBytes;
                splitOutput = pl_buf_recreate(gpu, &outputStageSecond, &dstDownload) &&
                    outputStageSecond->data;
                if (!splitOutput) {
                    dstDownload.size = outputBytes;
                    mappedOutput = pl_buf_recreate(gpu, &outputStage, &dstDownload) &&
                        outputStage->data;
                }
            }
            if (dstDownload.host_mapped && !mappedOutput)
                mappedReadbackUnavailable = true;
            if (!mappedOutput) {
                // A split readback is useful only with mapped buffers. If mapping
                // failed, the ordinary readback must hold the complete output.
                dstDownload.host_mapped = false;
                dstDownload.size = outputBytes;
            }
            if (!splitOutput && outputStageSecond)
                pl_buf_destroy(gpu, &outputStageSecond);
        }
        return (textureSource || (pl_buf_recreate(gpu, &source, &src) &&
                                  pl_buf_recreate(gpu, &sourceStage, &srcUpload))) &&
            pl_buf_recreate(gpu, &map, &gains) &&
            (directOutput || pl_buf_recreate(gpu, &output, &dst)) &&
            pl_buf_recreate(gpu, &linearization, &lut) &&
            pl_buf_recreate(gpu, &mapStage, &mapUpload) &&
            (directOutput || mappedOutput ||
             pl_buf_recreate(gpu, &outputStage, &dstDownload)) &&
            pl_buf_recreate(gpu, &linearizationStage, &lutUpload);
    }
};
struct BakeState {
    std::mutex mutex;
    std::unique_ptr<Device> device;
    std::unordered_map<pl_gpu, std::unique_ptr<Device>> textureDevices;
    std::once_flag warmOnce;
    std::future<void> warmup;
};
BakeState& bakeState() {
    static BakeState state;
    return state;
}
} // namespace

void warmVulkanGainBake() {
    auto& state = bakeState();
    std::call_once(state.warmOnce, [&state] {
        state.warmup = std::async(std::launch::async, [&state] {
            std::lock_guard lock(state.mutex);
            if (!state.device) {
                auto candidate = std::make_unique<Device>();
                if (candidate->init()) state.device = std::move(candidate);
            }
        });
    });
}

void releaseVulkanGainBakeTextureDevice(const void* gpu) {
    auto& state = bakeState();
    std::lock_guard lock(state.mutex);
    state.textureDevices.erase(static_cast<pl_gpu>(gpu));
}

bool bakeGainVulkan(const uint16_t* source,
                    const VulkanGainBakeParams& params,
                    uint8_t* destination, size_t destinationBytes, bool cfa) {
    const auto& map = params.map;
    const bool genericMaps = !params.maps.empty();
    const bool externalSource = params.textureSource &&
        params.textureSource->gpu && params.textureSource->texture;
    if (externalSource && std::getenv("MOTIONCAM_TEST_FAIL_VULKAN_GAIN_BAKE"))
        return false;
    if ((!source && !externalSource) || !destination || !params.width || !params.height ||
        params.channels != (cfa ? 1u : 3u) ||
        params.outputBits < 1 || params.outputBits > 16) return false;
    const bool hasLinearization = params.linearization &&
        !params.linearization->empty();
    if (hasLinearization && params.linearization->size() > 65536u) return false;
    const bool hasLogLookup = params.logLookup != nullptr;
    if (hasLogLookup && (!params.log60 || hasLinearization || cfa ||
                         params.logLookup->size() != 65536u)) return false;
    const uint32_t mapCount = genericMaps
        ? static_cast<uint32_t>(params.maps.size())
        : cfa ? params.cfaMapCount : 1u;
    if (genericMaps ? mapCount > 32u : (mapCount != 1u && mapCount != 4u))
        return false;
    if (!genericMaps && !cfa && mapCount != 1u) return false;
    const auto& first = genericMaps ? params.maps.front()
        : cfa ? params.cfaMaps[0] : map;
    for (uint32_t i = 0; i < mapCount; ++i) {
        const auto& current = genericMaps ? params.maps[i]
            : cfa ? params.cfaMaps[i] : map;
        if (!current.data || !current.width || !current.height ||
            !current.spacingH || !current.spacingV ||
            (genericMaps && (!current.rowPitch || !current.colPitch ||
                (current.channels != 1u && current.channels != 3u &&
                 current.channels != 4u))) ||
            (!genericMaps && cfa && current.channels != (mapCount == 4 ? 1u :
                (first.channels == 4 ? 4u : 1u))) ||
            (!genericMaps && !cfa && current.channels != 1 && current.channels != 3) ||
            (!genericMaps && (current.width != first.width ||
                             current.height != first.height)) ||
            current.data->size() != static_cast<size_t>(current.width) *
                current.height * current.channels) return false;
    }
    const auto packStarted = std::chrono::steady_clock::now();
    const size_t rowSamples = static_cast<size_t>(params.width) * params.channels;
    const size_t totalSamples = rowSamples * params.height;
    uint32_t sourceBits = params.sourceNominalWhite <= 255u ? 8u :
        params.sourceNominalWhite <= 4095u ? 12u :
        params.sourceNominalWhite <= 16383u ? 14u : 16u;
    std::unique_ptr<uint32_t[]> packedSource;
    if (!externalSource && sourceBits < 16) {
        packedSource.reset(new uint32_t[(totalSamples * sourceBits + 31u) / 32u]);
        const uint32_t samplesPerBlock = sourceBits == 14u ? 16u :
            sourceBits == 12u ? 8u : 4u;
        const size_t completeBlocks = totalSamples / samplesPerBlock;
        const uint32_t workers = std::min<uint32_t>(8u,
            std::max<size_t>(1u, completeBlocks / 65536u));
        std::vector<uint32_t> overflow(workers, 0);
        auto packBlocks = [&](uint32_t worker) {
            const size_t firstBlock = completeBlocks * worker / workers;
            const size_t lastBlock = completeBlocks * (worker + 1u) / workers;
            uint32_t excess = 0;
            if (sourceBits == 14u) {
                for (size_t block = firstBlock; block < lastBlock; ++block) {
                    const uint16_t* input = source + block * 16u;
                    uint32_t* output = packedSource.get() + block * 7u;
                    uint32_t v[16];
                    for (uint32_t i = 0; i < 16u; ++i) {
                        v[i] = input[i];
                        excess |= v[i] >> 14u;
                    }
                    output[0] = v[0] | (v[1] << 14u) | (v[2] << 28u);
                    output[1] = (v[2] >> 4u) | (v[3] << 10u) | (v[4] << 24u);
                    output[2] = (v[4] >> 8u) | (v[5] << 6u) | (v[6] << 20u);
                    output[3] = (v[6] >> 12u) | (v[7] << 2u) |
                                (v[8] << 16u) | (v[9] << 30u);
                    output[4] = (v[9] >> 2u) | (v[10] << 12u) | (v[11] << 26u);
                    output[5] = (v[11] >> 6u) | (v[12] << 8u) | (v[13] << 22u);
                    output[6] = (v[13] >> 10u) | (v[14] << 4u) | (v[15] << 18u);
                }
            } else if (sourceBits == 12u) {
                for (size_t block = firstBlock; block < lastBlock; ++block) {
                    const uint16_t* input = source + block * 8u;
                    uint32_t* output = packedSource.get() + block * 3u;
                    uint32_t v[8];
                    for (uint32_t i = 0; i < 8u; ++i) {
                        v[i] = input[i];
                        excess |= v[i] >> 12u;
                    }
                    output[0] = v[0] | (v[1] << 12u) | (v[2] << 24u);
                    output[1] = (v[2] >> 8u) | (v[3] << 4u) |
                                (v[4] << 16u) | (v[5] << 28u);
                    output[2] = (v[5] >> 4u) | (v[6] << 8u) | (v[7] << 20u);
                }
            } else {
                for (size_t block = firstBlock; block < lastBlock; ++block) {
                    const uint16_t* input = source + block * 4u;
                    uint32_t* output = packedSource.get() + block;
                    excess |= (input[0] | input[1] | input[2] | input[3]) >> 8u;
                    *output = uint32_t{input[0]} | (uint32_t{input[1]} << 8u) |
                        (uint32_t{input[2]} << 16u) | (uint32_t{input[3]} << 24u);
                }
            }
            overflow[worker] = excess;
        };
        std::vector<std::thread> threads;
        threads.reserve(workers - 1u);
        for (uint32_t worker = 1; worker < workers; ++worker)
            threads.emplace_back(packBlocks, worker);
        packBlocks(0);
        for (auto& thread : threads) thread.join();
        uint32_t excess = 0;
        for (const uint32_t value : overflow) excess |= value;
        size_t word = completeBlocks * samplesPerBlock * sourceBits / 32u;
        uint64_t pending = 0;
        uint32_t pendingBits = 0;
        for (size_t index = completeBlocks * samplesPerBlock;
             index < totalSamples; ++index) {
            const uint32_t sample = source[index];
            excess |= sample >> sourceBits;
            pending |= uint64_t{sample} << pendingBits;
            pendingBits += sourceBits;
            if (pendingBits >= 32u) {
                packedSource[word++] = static_cast<uint32_t>(pending);
                pending >>= 32u;
                pendingBits -= 32u;
            }
        }
        if (pendingBits) packedSource[word] = static_cast<uint32_t>(pending);
        if (excess) {
            sourceBits = 16;
            packedSource.reset();
        }
    }
    const size_t sourceWords = (totalSamples * sourceBits + 31u) / 32u;
    const size_t sourceBytes = sourceWords * sizeof(uint32_t);
    const auto packFinished = std::chrono::steady_clock::now();
    // Mounted reads run on different FUSE workers. Share one Vulkan device
    // instead of allocating a device and full-frame buffers per worker.
    auto& state = bakeState();
    std::lock_guard deviceLock(state.mutex);
    const auto started = std::chrono::steady_clock::now();
    auto& device = externalSource
        ? state.textureDevices[static_cast<pl_gpu>(params.textureSource->gpu)]
        : state.device;
    if (!device) {
        device = std::make_unique<Device>();
        if (!(externalSource
                  ? device->initExternal(static_cast<pl_gpu>(params.textureSource->gpu))
                  : device->init())) { device.reset(); return false; }
    }
    if (genericMaps && !device->ensureMultiPass(cfa)) return false;
    const size_t rowBytes = (rowSamples * params.outputBits + 7) / 8;
    const size_t wordsPerRow = (rowBytes + 3) / 4;
    size_t mapBytes = genericMaps ? 2048u : cfa ? 128u : 32u;
    if (genericMaps) {
        for (const auto& current : params.maps) {
            if (current.data->size() >
                (std::numeric_limits<size_t>::max() - mapBytes) / sizeof(float))
                return false;
            mapBytes += current.data->size() * sizeof(float);
        }
    } else {
        mapBytes += (cfa ? static_cast<size_t>(mapCount) * first.data->size()
                         : map.data->size()) * sizeof(float);
    }
    const size_t outputBytes = wordsPerRow * params.height * sizeof(uint32_t);
    const size_t linearizationBytes = hasLogLookup ? 65536u * sizeof(float)
        : hasLinearization ? 65536u * 2u : 4u;
    if (sourceWords > std::numeric_limits<uint32_t>::max() ||
        wordsPerRow * params.height > std::numeric_limits<uint32_t>::max() ||
        rowBytes > std::numeric_limits<uint32_t>::max() ||
        destinationBytes != rowBytes * params.height ||
        !device->reserve(sourceBytes, mapBytes, outputBytes,
                         linearizationBytes)) return false;
    const auto reserved = std::chrono::steady_clock::now();
    Constants constants = {};
    std::copy(params.black.begin(), params.black.end(), constants.black);
    std::copy(params.destinationBlack.begin(), params.destinationBlack.end(),
              constants.destinationBlack);
    std::copy(params.scale.begin(), params.scale.end(), constants.scale);
    constants.dimensions[0] = params.width;
    constants.dimensions[1] = params.height;
    constants.dimensions[2] = params.channels;
    constants.dimensions[3] = params.outputBits;
    constants.mapShape[0] = genericMaps ? mapCount : first.width;
    constants.mapShape[1] = genericMaps
        ? static_cast<uint32_t>(params.cfaPhase[0]) |
          (static_cast<uint32_t>(params.cfaPhase[1]) << 2u) |
          (static_cast<uint32_t>(params.cfaPhase[2]) << 4u) |
          (static_cast<uint32_t>(params.cfaPhase[3]) << 6u)
        : first.height;
    constants.mapShape[2] = genericMaps ? 0u : cfa ? mapCount : map.channels;
    constants.mapShape[3] = sourceBits;
    constants.destinationWhite = params.destinationWhite;
    constants.encodedWhite = params.encodedWhite;
    constants.ditherSeed = params.ditherSeed;
    constants.flags = (params.log60 ? 1u : 0u) |
        (params.logHighQuality ? 2u : 0u) | (params.debugGain ? 4u : 0u) |
        (cfa && first.channels == 4 ? 8u : 0u) |
        (hasLinearization ? 16u : 0u) | (hasLogLookup ? 32u : 0u);
    constants.rowBytes = static_cast<uint32_t>(rowBytes);
    constants.wordsPerRow = static_cast<uint32_t>(wordsPerRow);
    if (externalSource) {
        // The decoder has already rendered this image on this Vulkan device.
    } else if (sourceBits < 16) {
        pl_buf_write(device->gpu, device->sourceStage, 0,
                     packedSource.get(), sourceBytes);
    } else if (totalSamples & 1u) {
        // The strip ends with one 16-bit sample. The shader reads 32-bit
        // words, so supply a zero-filled high half without reading past it.
        const size_t completeBytes = sourceBytes - sizeof(uint32_t);
        if (completeBytes)
            pl_buf_write(device->gpu, device->sourceStage, 0,
                         source, completeBytes);
        uint32_t finalWord = 0;
        std::memcpy(&finalWord,
                    reinterpret_cast<const uint8_t*>(source) + completeBytes,
                    sizeof(uint16_t));
        pl_buf_write(device->gpu, device->sourceStage,
                     completeBytes, &finalWord, sizeof(finalWord));
    } else {
        pl_buf_write(device->gpu, device->sourceStage, 0,
                     source, sourceBytes);
    }
    if (genericMaps) {
        uint32_t floatOffset = 0;
        for (uint32_t i = 0; i < mapCount; ++i) {
            const auto& current = params.maps[i];
            if (current.data->size() >
                std::numeric_limits<uint32_t>::max() - floatOffset)
                return false;
            uint32_t bounds[4] = {current.left, current.top,
                                  current.right, current.bottom};
            float axes[4] = {current.originH, current.originV,
                             current.spacingH, current.spacingV};
            uint32_t shape[4] = {current.width, current.height,
                                 current.channels, floatOffset};
            uint32_t pitch[4] = {current.rowPitch, current.colPitch, 0u, 0u};
            pl_buf_write(device->gpu, device->mapStage,
                         i * 16u, bounds, sizeof(bounds));
            pl_buf_write(device->gpu, device->mapStage,
                         512u + i * 16u, axes, sizeof(axes));
            pl_buf_write(device->gpu, device->mapStage,
                         1024u + i * 16u, shape, sizeof(shape));
            pl_buf_write(device->gpu, device->mapStage,
                         1536u + i * 16u, pitch, sizeof(pitch));
            pl_buf_write(device->gpu, device->mapStage,
                         2048u + static_cast<size_t>(floatOffset) * sizeof(float),
                         current.data->data(), current.data->size() * sizeof(float));
            floatOffset += static_cast<uint32_t>(current.data->size());
        }
    } else if (cfa) {
        for (uint32_t i = 0; i < mapCount; ++i) {
            const auto& current = params.cfaMaps[i];
            uint32_t bounds[4] = {current.left, current.top,
                                  current.right, current.bottom};
            float axes[4] = {current.originH, current.originV,
                             current.spacingH, current.spacingV};
            pl_buf_write(device->gpu, device->mapStage,
                         i * 16, bounds, sizeof(bounds));
            pl_buf_write(device->gpu, device->mapStage,
                         64 + i * 16, axes, sizeof(axes));
            pl_buf_write(device->gpu, device->mapStage,
                         128 + static_cast<size_t>(i) * first.data->size() * sizeof(float),
                         current.data->data(), current.data->size() * sizeof(float));
        }
    } else {
    uint32_t mapBounds[4] = {map.left, map.top, map.right, map.bottom};
    float mapAxes[4] = {map.originH, map.originV, map.spacingH, map.spacingV};
    pl_buf_write(device->gpu, device->mapStage, 0, mapBounds, sizeof(mapBounds));
    pl_buf_write(device->gpu, device->mapStage, 16, mapAxes, sizeof(mapAxes));
    pl_buf_write(device->gpu, device->mapStage, 32,
                 map.data->data(), map.data->size() * sizeof(float));
    }
    if (!externalSource)
        pl_buf_copy(device->gpu, device->source, 0,
                    device->sourceStage, 0, sourceBytes);
    pl_buf_copy(device->gpu, device->map, 0,
                device->mapStage, 0, mapBytes);
    if (hasLogLookup) {
        pl_buf_write(device->gpu, device->linearizationStage, 0,
                     params.logLookup->data(), linearizationBytes);
        pl_buf_copy(device->gpu, device->linearization, 0,
                    device->linearizationStage, 0, linearizationBytes);
    } else if (hasLinearization) {
        std::vector<uint32_t> packedTable(32768u);
        const auto& table = *params.linearization;
        for (uint32_t index = 0; index < 32768u; ++index) {
            const uint32_t low = table[std::min<size_t>(index * 2u,
                                                        table.size() - 1u)];
            const uint32_t high = table[std::min<size_t>(index * 2u + 1u,
                                                         table.size() - 1u)];
            packedTable[index] = low | (high << 16u);
        }
        pl_buf_write(device->gpu, device->linearizationStage, 0,
                     packedTable.data(), linearizationBytes);
        pl_buf_copy(device->gpu, device->linearization, 0,
                    device->linearizationStage, 0, linearizationBytes);
    }
    const auto uploaded = std::chrono::steady_clock::now();
    pl_desc_binding bindings[4] = {};
    bindings[0].object = externalSource
        ? params.textureSource->texture : static_cast<const void*>(device->source);
    bindings[0].sample_mode = PL_TEX_SAMPLE_NEAREST;
    bindings[1].object = device->map;
    bindings[2].object = device->directOutput
        ? device->directOutput : device->output;
    bindings[3].object = device->linearization;
    pl_pass_run_params run = {};
    run.pass = genericMaps
        ? cfa ? device->multiCfaPass : device->multiPass
        : cfa ? device->cfaPass : device->pass;
    run.desc_bindings = bindings;
    run.push_constants = &constants;
    run.timer = device->passTimer;
    const size_t workItems = !cfa && params.outputBits == 8 && params.width % 4 == 0
        ? params.width / 4
        : !cfa && params.outputBits == 16 && params.width % 2 == 0
            ? params.width / 2 : wordsPerRow;
    run.compute_groups[0] = static_cast<int>((workItems + 255) / 256);
    run.compute_groups[1] = static_cast<int>(params.height);
    run.compute_groups[2] = 1;
    if (run.compute_groups[0] > device->gpu->limits.max_dispatch[0] ||
        run.compute_groups[1] > device->gpu->limits.max_dispatch[1]) return false;
    pl_pass_run(device->gpu, &run);
    const VulkanPackedImage packedImage{
        device->gpu, device->directOutput ? device->directOutput : device->output,
        params.width, params.height, params.channels, params.outputBits,
        static_cast<uint32_t>(wordsPerRow)};
    if (params.consumePackedOutput &&
        params.consumePackedOutput(packedImage)) {
        if (std::getenv("MOTIONCAM_GALLERY_PERF_PROFILE"))
            std::fprintf(stderr,
                "GALLERY_PERF event=vulkan_gain_gpu_jpeg_handoff width=%u height=%u channels=%u bits=%u\n",
                params.width, params.height, params.channels,
                params.outputBits);
        return true;
    }
    const size_t firstDownloadBytes = device->splitOutput
        ? (outputBytes / 2) & ~size_t{63} : outputBytes;
    if (!device->directOutput)
        pl_buf_copy(device->gpu, device->outputStage, 0,
                    device->output, 0, firstDownloadBytes);
    if (!device->directOutput && device->splitOutput) {
        // Submit the first half before recording the second. Otherwise both
        // copies share a fence and polling the first half waits for both.
        pl_gpu_flush(device->gpu);
        pl_buf_copy(device->gpu, device->outputStageSecond, 0,
                    device->output, firstDownloadBytes,
                    outputBytes - firstDownloadBytes);
    }
    const auto submitted = std::chrono::steady_clock::now();
    const bool alignedRows = rowBytes == wordsPerRow * sizeof(uint32_t);
    std::vector<uint32_t> words;
    if (!alignedRows) words.resize(wordsPerRow * params.height);
    void* readTarget = alignedRows ? static_cast<void*>(destination)
                                   : static_cast<void*>(words.data());
    bool readOk = false;
    double gpuWaitMs = 0.0, hostCopyMs = 0.0;
    const auto elapsedMs = [](auto a, auto b) {
        return std::chrono::duration<double, std::milli>(b - a).count();
    };
    if (device->mappedOutput) {
        auto copyMapped = [&](pl_buf stage, size_t destinationOffset,
                              size_t bytes) {
            const auto waiting = std::chrono::steady_clock::now();
            while (pl_buf_poll(device->gpu, stage, UINT64_MAX)) {}
            const auto ready = std::chrono::steady_clock::now();
            gpuWaitMs += elapsedMs(waiting, ready);
            if (pl_gpu_is_failed(device->gpu)) return false;
            const auto* sourceBytes = stage->data;
            auto* targetBytes = static_cast<uint8_t*>(readTarget) + destinationOffset;
            const size_t workers = std::min<size_t>(8, std::max<size_t>(1,
                bytes / (8 * 1024 * 1024)));
#if defined(__x86_64__) && (defined(__GNUC__) || defined(__clang__))
            const bool useStreamingCopy = __builtin_cpu_supports("avx2");
#endif
            auto copyPart = [&](size_t index) {
                const size_t begin = (bytes * index / workers) & ~size_t{63};
                const size_t end = index + 1 == workers ? bytes
                    : (bytes * (index + 1) / workers) & ~size_t{63};
#if defined(__x86_64__) && (defined(__GNUC__) || defined(__clang__))
                if (useStreamingCopy)
                    streamCopy(targetBytes + begin, sourceBytes + begin, end - begin);
                else
#endif
                    std::memcpy(targetBytes + begin, sourceBytes + begin, end - begin);
            };
            std::vector<std::thread> threads;
            threads.reserve(workers - 1);
            for (size_t index = 1; index < workers; ++index)
                threads.emplace_back(copyPart, index);
            copyPart(0);
            for (auto& thread : threads) thread.join();
            hostCopyMs += elapsedMs(ready, std::chrono::steady_clock::now());
            return true;
        };
        readOk = copyMapped(device->directOutput
                                ? device->directOutput : device->outputStage,
                            0, firstDownloadBytes);
        if (readOk && device->splitOutput)
            readOk = copyMapped(device->outputStageSecond, firstDownloadBytes,
                                outputBytes - firstDownloadBytes);
    } else {
        readOk = pl_buf_read(device->gpu, device->outputStage, 0,
                             readTarget, outputBytes);
        gpuWaitMs = elapsedMs(submitted, std::chrono::steady_clock::now());
    }
    if (!readOk || pl_gpu_is_failed(device->gpu)) {
        device.reset();
        return false;
    }
    const auto downloaded = std::chrono::steady_clock::now();
    if (!alignedRows)
        for (uint32_t y = 0; y < params.height; ++y)
            std::memcpy(destination + static_cast<size_t>(y) * rowBytes,
                        words.data() + static_cast<size_t>(y) * wordsPerRow, rowBytes);
    if (std::getenv("MOTIONCAM_GALLERY_PERF_PROFILE")) {
        const auto ms = [](auto a, auto b) {
            return std::chrono::duration<double, std::milli>(b - a).count();
        };
        std::fprintf(stderr,
            "GALLERY_PERF event=vulkan_gain_stage pack_ms=%.3f reserve_ms=%.3f upload_ms=%.3f submit_ms=%.3f readback_ms=%.3f gpu_wait_ms=%.3f shader_gpu_ms=%.3f host_copy_ms=%.3f copy_ms=%.3f mapped_readback=%d split_readback=%d direct_mapped_output=%d input_bits=%u input_bytes=%zu output_bytes=%zu\n",
            ms(packStarted, packFinished),
            ms(started, reserved), ms(reserved, uploaded),
            ms(uploaded, submitted), ms(submitted, downloaded),
            gpuWaitMs, double(pl_timer_query(device->gpu, device->passTimer)) / 1e6,
            hostCopyMs,
            ms(downloaded, std::chrono::steady_clock::now()),
            device->mappedOutput ? 1 : 0, device->splitOutput ? 1 : 0,
            device->directOutput ? 1 : 0,
            sourceBits, externalSource ? 0u : sourceBytes, rowBytes * params.height);
    }
    return true;
}
bool bakeRgbGainVulkan(const uint16_t* source,
                       const VulkanGainBakeParams& params,
                       uint8_t* destination, size_t destinationBytes) {
    return bakeGainVulkan(source, params, destination, destinationBytes, false);
}
bool bakeCfaGainVulkan(const uint16_t* source,
                       const VulkanGainBakeParams& params,
                       uint8_t* destination, size_t destinationBytes) {
    return bakeGainVulkan(source, params, destination, destinationBytes, true);
}
} // namespace motioncam
#else
namespace motioncam {
void warmVulkanGainBake() {}
void releaseVulkanGainBakeTextureDevice(const void*) {}
bool bakeRgbGainVulkan(const uint16_t*, const VulkanGainBakeParams&,
                       uint8_t*, size_t) { return false; }
bool bakeCfaGainVulkan(const uint16_t*, const VulkanGainBakeParams&,
                       uint8_t*, size_t) { return false; }
}
#endif
