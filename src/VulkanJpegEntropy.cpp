#include "VulkanJpegEntropy.h"

#ifdef MOTIONCAM_HAS_DIRECTLOG_PLACEBO
#include "VulkanJpegContext.h"
#include <libplacebo/vulkan.h>
#include <libplacebo/gpu.h>
#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <mutex>
#include <vector>

namespace motioncam {
namespace {
constexpr uint32_t kBlockSamples = 256;
constexpr char shader[] = R"glsl(#version 450
layout(local_size_x = 64) in;
layout(std430, binding = 0) readonly buffer Residuals { uint words[]; } src;
layout(std430, binding = 1) readonly buffer Codes { uvec2 values[]; } table;
layout(std430, binding = 2) writeonly buffer Packed { uint words[]; } packed;
layout(std430, binding = 3) writeonly buffer Lengths { uint bits[]; } lengths;
layout(push_constant) uniform Constants {
    uint sampleCount;
    uint stride;
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
int residualAt(uint index) {
    if (p.mode == 0u) {
        uint value = sampleAt(index);
        return value >= 32768u ? int(value) - 65536 : int(value);
    }
    uint rowSamples = p.width * p.channels;
    uint row = index / rowSamples;
    uint x = (index % rowSamples) / p.channels;
    int value = int(sampleAt(index));
    int left = x > 0u ? int(sampleAt(index - p.channels)) : 0;
    int above = row > 0u ? int(sampleAt(index - rowSamples)) : 0;
    int upperLeft = row > 0u && x > 0u
        ? int(sampleAt(index - rowSamples - p.channels)) : 0;
    int predicted = row == 0u && x == 0u ? int(1u << (p.bits - 1u))
        : row == 0u ? left : x == 0u ? above
        : prediction(left, above, upperLeft);
    int diff = (value - predicted) & 65535;
    return diff >= 32768 ? diff - 65536 : diff;
}
void main() {
    uint block = gl_GlobalInvocationID.x;
    uint first = block * 256u;
    if (first >= p.sampleCount) return;
    uint end = min(first + 256u, p.sampleCount);
    uint current = 0u;
    uint word = 0u;
    uint pending = 0u;
    uint total = 0u;
    for (uint i = first; i < end; ++i) {
        int diff = residualAt(i);
        uint magnitude = uint(abs(diff));
        uint category = magnitude == 0u ? 0u : uint(findMSB(magnitude) + 1);
        uint extra = category == 16u ? 0u : category;
        uvec2 huffman = table.values[category];
        uint threshold = category == 0u ? 0u : 1u << (category - 1u);
        if (diff < int(threshold)) diff += int((1u << category) - 1u);
        uint mask = extra == 0u ? 0u : (1u << extra) - 1u;
        uint value = (huffman.x << extra) | (uint(diff) & mask);
        uint bits = huffman.y + extra;
        uint available = 32u - pending;
        uint take = min(bits, available);
        uint high = value >> (bits - take);
        uint highMask = take == 32u ? 0xffffffffu : (1u << take) - 1u;
        current |= (high & highMask) << (available - take);
        pending += take;
        total += bits;
        if (pending == 32u) {
            packed.words[block * p.stride + word++] = current;
            current = 0u;
            pending = 0u;
        }
        if (bits > take) {
            uint tail = bits - take;
            current = (value & ((1u << tail) - 1u)) << (32u - tail);
            pending = tail;
        }
    }
    if (pending != 0u) packed.words[block * p.stride + word] = current;
    lengths.bits[block] = total;
}
)glsl";
constexpr char joinShader[] = R"glsl(#version 450
layout(local_size_x = 64) in;
layout(std430, binding = 0) readonly buffer Packed { uint words[]; } src;
layout(std430, binding = 1) readonly buffer Offsets { uint bits[]; } offsets;
layout(std430, binding = 2) readonly buffer Lengths { uint bits[]; } lengths;
layout(std430, binding = 3) buffer Joined { uint words[]; } dst;
layout(push_constant) uniform Constants { uint blocks; uint stride; } p;
void main() {
    uint block = gl_GlobalInvocationID.x;
    if (block >= p.blocks) return;
    uint start = offsets.bits[block];
    uint bits = lengths.bits[block];
    if (bits == 0u) return;
    uint first = start >> 5u;
    uint last = (start + bits - 1u) >> 5u;
    uint shift = start & 31u;
    uint rawWords = (bits + 31u) >> 5u;
    for (uint word = first; word <= last; ++word) {
        uint j = word - first;
        uint right = j < rawWords ? src.words[block * p.stride + j] : 0u;
        uint value = right;
        if (shift != 0u) {
            uint left = j == 0u ? 0u : src.words[block * p.stride + j - 1u];
            value = (left << (32u - shift)) | (right >> shift);
        }
        bool boundary = (word == first && shift != 0u) ||
                        (word == last && ((start + bits) & 31u) != 0u);
        if (boundary) atomicOr(dst.words[word], value);
        else dst.words[word] = value;
    }
}
)glsl";
struct State {
    pl_gpu gpu = nullptr;
    pl_pass pass = nullptr;
    pl_pass joinPass = nullptr;
    pl_buf source = nullptr, sourceStage = nullptr;
    pl_buf codes = nullptr, codesStage = nullptr;
    pl_buf packed = nullptr, packedStage = nullptr;
    pl_buf lengths = nullptr, lengthsStage = nullptr;
    pl_buf offsets = nullptr, offsetsStage = nullptr;
    pl_buf joined = nullptr, joinedStage = nullptr;
    ~State() {
        if (gpu) {
            pl_gpu_finish(gpu);
            pl_buf_destroy(gpu, &source);
            pl_buf_destroy(gpu, &sourceStage);
            pl_buf_destroy(gpu, &codes);
            pl_buf_destroy(gpu, &codesStage);
            pl_buf_destroy(gpu, &packed);
            pl_buf_destroy(gpu, &packedStage);
            pl_buf_destroy(gpu, &lengths);
            pl_buf_destroy(gpu, &lengthsStage);
            pl_buf_destroy(gpu, &offsets);
            pl_buf_destroy(gpu, &offsetsStage);
            pl_buf_destroy(gpu, &joined);
            pl_buf_destroy(gpu, &joinedStage);
            pl_pass_destroy(gpu, &pass);
            pl_pass_destroy(gpu, &joinPass);
        }
    }
    bool init(pl_gpu sharedGpu) {
        if (pass) return true;
        gpu = sharedGpu;
        pl_desc descriptors[4] = {
            {"src", PL_DESC_BUF_STORAGE, 0, PL_DESC_ACCESS_READONLY},
            {"table", PL_DESC_BUF_STORAGE, 1, PL_DESC_ACCESS_READONLY},
            {"packed", PL_DESC_BUF_STORAGE, 2, PL_DESC_ACCESS_WRITEONLY},
            {"lengths", PL_DESC_BUF_STORAGE, 3, PL_DESC_ACCESS_WRITEONLY}
        };
        pl_pass_params params = {};
        params.type = PL_PASS_COMPUTE;
        params.descriptors = descriptors;
        params.num_descriptors = 4;
        params.push_constants_size = 7 * sizeof(uint32_t);
        params.glsl_shader = shader;
        pass = pl_pass_create(gpu, &params);
        return pass != nullptr;
    }
    bool initJoin() {
        if (joinPass) return true;
        pl_desc descriptors[4] = {
            {"src", PL_DESC_BUF_STORAGE, 0, PL_DESC_ACCESS_READONLY},
            {"offsets", PL_DESC_BUF_STORAGE, 1, PL_DESC_ACCESS_READONLY},
            {"lengths", PL_DESC_BUF_STORAGE, 2, PL_DESC_ACCESS_READONLY},
            {"dst", PL_DESC_BUF_STORAGE, 3, PL_DESC_ACCESS_READWRITE}
        };
        pl_pass_params params = {};
        params.type = PL_PASS_COMPUTE;
        params.descriptors = descriptors;
        params.num_descriptors = 4;
        params.push_constants_size = 2 * sizeof(uint32_t);
        params.glsl_shader = joinShader;
        joinPass = pl_pass_create(gpu, &params);
        return joinPass != nullptr;
    }

};
State& state() { static State value; return value; }

bool stuffJoined(const std::vector<uint32_t>& joined, size_t rawBytes,
                 uint8_t* output, int capacity, int* written) {
    int bytes = 0;
    for (size_t word = 0; word < joined.size() && word * 4u < rawBytes; ++word) {
        const uint32_t value = joined[word];
        const size_t available = std::min<size_t>(4u, rawBytes - word * 4u);
        if (available == 4u) {
            const uint32_t inverted = ~value;
            const bool hasFf = ((inverted - 0x01010101u) &
                                ~inverted & 0x80808080u) != 0;
            if (!hasFf && bytes <= capacity - 4) {
                const uint32_t native = ((value & 0xff000000u) >> 24u) |
                    ((value & 0x00ff0000u) >> 8u) |
                    ((value & 0x0000ff00u) << 8u) |
                    ((value & 0x000000ffu) << 24u);
                std::memcpy(output + bytes, &native, sizeof(native));
                bytes += 4;
                continue;
            }
        }
        for (size_t i = 0; i < available; ++i) {
            const uint8_t byte = static_cast<uint8_t>(value >> (24u - i * 8u));
            if (bytes >= capacity) return false;
            output[bytes++] = byte;
            if (byte == 0xff) {
                if (bytes >= capacity) return false;
                output[bytes++] = 0;
            }
        }
    }
    *written = bytes;
    return true;
}

bool joinOnGpu(pl_gpu gpu, State& s, const std::vector<uint32_t>& blockBits,
               uint32_t wordsPerBlock, uint8_t* output, int capacity,
               int* written) {
    const auto started = std::chrono::steady_clock::now();
    if (!s.initJoin() || blockBits.empty() || blockBits.size() > UINT32_MAX)
        return false;
    std::vector<uint32_t> offsets(blockBits.size());
    uint64_t totalBits = 0;
    for (size_t block = 0; block < blockBits.size(); ++block) {
        const uint32_t bits = blockBits[block];
        if (!bits || bits > wordsPerBlock * 32u ||
            totalBits > UINT32_MAX - bits) return false;
        offsets[block] = static_cast<uint32_t>(totalBits);
        totalBits += bits;
    }
    const size_t words = static_cast<size_t>((totalBits + 31u) / 32u);
    const size_t joinedBytes = words * sizeof(uint32_t);
    const size_t offsetBytes = offsets.size() * sizeof(uint32_t);
    if (!words || joinedBytes > gpu->limits.max_ssbo_size ||
        offsetBytes > gpu->limits.max_ssbo_size) return false;
    pl_buf_params offsetsDevice = {}, offsetsHost = {};
    offsetsDevice.size = offsetBytes;
    offsetsDevice.storable = true;
    offsetsDevice.memory_type = PL_BUF_MEM_DEVICE;
    offsetsHost.size = offsetBytes;
    offsetsHost.host_writable = true;
    offsetsHost.memory_type = PL_BUF_MEM_HOST;
    pl_buf_params joinedDevice = {}, joinedHost = {};
    joinedDevice.size = joinedBytes;
    joinedDevice.storable = true;
    joinedDevice.memory_type = PL_BUF_MEM_DEVICE;
    joinedHost.size = joinedBytes;
    joinedHost.host_writable = true;
    joinedHost.host_readable = true;
    joinedHost.memory_type = PL_BUF_MEM_HOST;
    if (!pl_buf_recreate(gpu, &s.offsets, &offsetsDevice) ||
        !pl_buf_recreate(gpu, &s.offsetsStage, &offsetsHost) ||
        !pl_buf_recreate(gpu, &s.joined, &joinedDevice) ||
        !pl_buf_recreate(gpu, &s.joinedStage, &joinedHost)) return false;
    std::vector<uint32_t> joined(words, 0);
    pl_buf_write(gpu, s.offsetsStage, 0, offsets.data(), offsetBytes);
    pl_buf_write(gpu, s.joinedStage, 0, joined.data(), joinedBytes);
    pl_buf_copy(gpu, s.offsets, 0, s.offsetsStage, 0, offsetBytes);
    pl_buf_copy(gpu, s.joined, 0, s.joinedStage, 0, joinedBytes);
    uint32_t constants[2] = {
        static_cast<uint32_t>(blockBits.size()), wordsPerBlock
    };
    pl_desc_binding bindings[4] = {};
    bindings[0].object = s.packed;
    bindings[1].object = s.offsets;
    bindings[2].object = s.lengths;
    bindings[3].object = s.joined;
    pl_pass_run_params run = {};
    run.pass = s.joinPass;
    run.desc_bindings = bindings;
    run.push_constants = constants;
    run.compute_groups[0] = static_cast<int>((blockBits.size() + 63u) / 64u);
    run.compute_groups[1] = run.compute_groups[2] = 1;
    pl_pass_run(gpu, &run);
    pl_buf_copy(gpu, s.joinedStage, 0, s.joined, 0, joinedBytes);
    if (!pl_buf_read(gpu, s.joinedStage, 0, joined.data(), joinedBytes))
        return false;
    const auto readAt = std::chrono::steady_clock::now();
    const size_t rawBytes = static_cast<size_t>((totalBits + 7u) / 8u);
    if (!stuffJoined(joined, rawBytes, output, capacity, written)) return false;
    if (std::getenv("MOTIONCAM_JPEG_ENCODE_PROFILE"))
        std::fprintf(stderr, "JPEG encode GPU join: gpu_ms=%.3f stuffing_ms=%.3f raw_bytes=%zu bytes=%d\n",
                     std::chrono::duration<double, std::milli>(readAt - started).count(),
                     std::chrono::duration<double, std::milli>(
                         std::chrono::steady_clock::now() - readAt).count(),
                     rawBytes, *written);
    return true;
}


}

int jpegEntropyVulkan(const int16_t* residuals, int sampleCount,
                      const uint16_t codes[18],
                      const uint16_t codeBits[18],
                      uint8_t* output, int capacity,
                      int* written, void* callbackContext) {
    const auto started = std::chrono::steady_clock::now();
    auto* imageSource = residuals ? nullptr :
        static_cast<JpegEntropyImageSource*>(callbackContext);
    if ((!residuals && !imageSource) || !codes || !codeBits || !output || !written ||
        sampleCount <= 0 || capacity <= 0) return 1;
    if (imageSource) {
        if (imageSource->gpuUsed) *imageSource->gpuUsed = false;
        if (!imageSource->pixels || !imageSource->width || !imageSource->height ||
            (imageSource->channels != 1 && imageSource->channels != 3) ||
            imageSource->bits < 8 || imageSource->bits > 16 ||
            imageSource->predictor < 1 || imageSource->predictor > 7 ||
            static_cast<uint64_t>(imageSource->width) * imageSource->height *
                imageSource->channels != static_cast<uint32_t>(sampleCount))
            return 1;
    }
    const uint16_t endianProbe = 1;
    if (*reinterpret_cast<const uint8_t*>(&endianProbe) != 1) return 1;
    const size_t samples = static_cast<size_t>(sampleCount);
    const size_t blocks = (samples + kBlockSamples - 1u) / kBlockSamples;
    const size_t sourceBytes = (samples + 1u) / 2u * 4u;
    uint32_t maxSampleBits = 0;
    for (uint32_t category = 0; category <= 16u; ++category) {
        if (!codeBits[category] || codeBits[category] > 16u) return 1;
        maxSampleBits = std::max<uint32_t>(
            maxSampleBits, codeBits[category] +
                (category == 16u ? 0u : category));
    }
    const uint32_t wordsPerBlock =
        (kBlockSamples * maxSampleBits + 31u) / 32u;
    const size_t packedBytes = blocks * wordsPerBlock * 4u;
    const size_t lengthsBytes = blocks * 4u;
    if (packedBytes > static_cast<size_t>(INT32_MAX) * 4u) return 1;
    auto& context = jpegContext();
    auto& s = state();
    std::lock_guard lock(context.mutex);
    if (!context.init() || !s.init(context.vulkan->gpu)) return 1;
    pl_gpu gpu = context.vulkan->gpu;
    pl_buf resident = imageSource ? jpegResidentImageVulkan(
        imageSource->pixels, imageSource->width, imageSource->height,
        imageSource->channels, imageSource->bits) : nullptr;
    if (imageSource && !resident) return 1;
    if (sourceBytes > gpu->limits.max_ssbo_size ||
        packedBytes > gpu->limits.max_ssbo_size ||
        lengthsBytes > gpu->limits.max_ssbo_size ||
        (blocks + 63u) / 64u > static_cast<size_t>(gpu->limits.max_dispatch[0]))
        return 1;
    pl_buf_params src = {}, srcStage = {}, table = {}, tableStage = {};
    pl_buf_params packed = {}, packedStage = {}, lengths = {}, lengthsStage = {};
    src.size = sourceBytes; src.storable = true;
    src.memory_type = PL_BUF_MEM_DEVICE;
    srcStage.size = sourceBytes; srcStage.host_writable = true;
    srcStage.memory_type = PL_BUF_MEM_HOST;
    table.size = 18u * 2u * 4u; table.storable = true;
    table.memory_type = PL_BUF_MEM_DEVICE;
    tableStage.size = table.size; tableStage.host_writable = true;
    tableStage.memory_type = PL_BUF_MEM_HOST;
    packed.size = packedBytes; packed.storable = true;
    packed.memory_type = PL_BUF_MEM_DEVICE;
    packedStage.size = packedBytes; packedStage.host_readable = true;
    packedStage.memory_type = PL_BUF_MEM_HOST;
    lengths.size = lengthsBytes; lengths.storable = true;
    lengths.memory_type = PL_BUF_MEM_DEVICE;
    lengthsStage.size = lengthsBytes; lengthsStage.host_readable = true;
    lengthsStage.memory_type = PL_BUF_MEM_HOST;
    if ((!resident && (!pl_buf_recreate(gpu, &s.source, &src) ||
                      !pl_buf_recreate(gpu, &s.sourceStage, &srcStage))) ||
        !pl_buf_recreate(gpu, &s.codes, &table) ||
        !pl_buf_recreate(gpu, &s.codesStage, &tableStage) ||
        !pl_buf_recreate(gpu, &s.packed, &packed) ||
        !pl_buf_recreate(gpu, &s.packedStage, &packedStage) ||
        !pl_buf_recreate(gpu, &s.lengths, &lengths) ||
        !pl_buf_recreate(gpu, &s.lengthsStage, &lengthsStage)) return 1;
    if (!resident) {
        pl_buf_write(gpu, s.sourceStage, 0, residuals,
                     samples * sizeof(int16_t));
        if (samples & 1u) {
            const int16_t zero = 0;
            pl_buf_write(gpu, s.sourceStage, samples * sizeof(int16_t),
                         &zero, sizeof(zero));
        }
    }
    std::array<uint32_t, 36> tableData{};
    for (size_t i = 0; i < 18; ++i) {
        tableData[i * 2] = codes[i];
        tableData[i * 2 + 1] = codeBits[i];
        if (codeBits[i] > 16) return 1;
    }
    pl_buf_write(gpu, s.codesStage, 0, tableData.data(), table.size);
    if (!resident)
        pl_buf_copy(gpu, s.source, 0, s.sourceStage, 0, sourceBytes);
    pl_buf_copy(gpu, s.codes, 0, s.codesStage, 0, table.size);
    const auto uploaded = std::chrono::steady_clock::now();
    uint32_t constants[7] = {
        static_cast<uint32_t>(samples), wordsPerBlock,
        imageSource ? imageSource->width : 0u,
        imageSource ? imageSource->height : 0u,
        imageSource ? imageSource->channels : 0u,
        imageSource ? imageSource->bits : 0u,
        imageSource ? imageSource->predictor : 0u
    };
    pl_desc_binding bindings[4] = {};
    bindings[0].object = resident ? resident : s.source;
    bindings[1].object = s.codes;
    bindings[2].object = s.packed;
    bindings[3].object = s.lengths;
    pl_pass_run_params run = {};
    run.pass = s.pass;
    run.desc_bindings = bindings;
    run.push_constants = constants;
    run.compute_groups[0] = static_cast<int>((blocks + 63u) / 64u);
    run.compute_groups[1] = run.compute_groups[2] = 1;
    pl_pass_run(gpu, &run);
    pl_buf_copy(gpu, s.lengthsStage, 0, s.lengths, 0, lengthsBytes);
    std::vector<uint32_t> blockBits(blocks);
    if (!pl_buf_read(gpu, s.lengthsStage, 0,
                     blockBits.data(), lengthsBytes)) return 1;
    if (joinOnGpu(gpu, s, blockBits, wordsPerBlock, output,
                  capacity, written)) {
        if (imageSource && imageSource->gpuUsed) *imageSource->gpuUsed = true;
        return 0;
    }
    pl_buf_copy(gpu, s.packedStage, 0, s.packed, 0, packedBytes);
    std::vector<uint32_t> packedWords(packedBytes / 4u);
    if (!pl_buf_read(gpu, s.packedStage, 0,
                     packedWords.data(), packedBytes)) return 1;
    const auto packedReady = std::chrono::steady_clock::now();
    uint64_t pending = 0;
    unsigned pendingBits = 0;
    int bytes = 0;
    auto emit = [&](uint8_t byte) -> bool {
        if (bytes >= capacity) return false;
        output[bytes++] = byte;
        if (byte == 0xff) {
            if (bytes >= capacity) return false;
            output[bytes++] = 0;
        }
        return true;
    };
    for (size_t block = 0; block < blocks; ++block) {
        const uint32_t bits = blockBits[block];
        if (bits > wordsPerBlock * 32u ||
            (block + 1u < blocks && bits == 0u)) return 1;
        for (uint32_t offset = 0; offset < bits; offset += 32u) {
            const unsigned take = std::min(32u, bits - offset);
            const uint32_t word = packedWords[block * wordsPerBlock + offset / 32u];
            const uint32_t value = take == 32u ? word : word >> (32u - take);
            pending = (pending << take) | value;
            pendingBits += take;
            if (pendingBits >= 32u) {
                const uint32_t four = static_cast<uint32_t>(
                    pending >> (pendingBits - 32u));
                const uint32_t inverted = ~four;
                const bool hasFf = ((inverted - 0x01010101u) &
                                    ~inverted & 0x80808080u) != 0;
                if (!hasFf && bytes <= capacity - 4) {
                    const uint32_t native = ((four & 0xff000000u) >> 24u) |
                        ((four & 0x00ff0000u) >> 8u) |
                        ((four & 0x0000ff00u) << 8u) |
                        ((four & 0x000000ffu) << 24u);
                    std::memcpy(output + bytes, &native, sizeof(native));
                    bytes += 4;
                } else {
                    if (!emit(static_cast<uint8_t>(four >> 24u)) ||
                        !emit(static_cast<uint8_t>(four >> 16u)) ||
                        !emit(static_cast<uint8_t>(four >> 8u)) ||
                        !emit(static_cast<uint8_t>(four))) return 1;
                }
                pendingBits -= 32u;
                pending &= (uint64_t{1} << pendingBits) - 1u;
            }
            while (pendingBits >= 8u) {
                if (!emit(static_cast<uint8_t>(pending >> (pendingBits - 8u))))
                    return 1;
                pendingBits -= 8u;
                pending &= (uint64_t{1} << pendingBits) - 1u;
            }
        }
    }
    if (pendingBits &&
        !emit(static_cast<uint8_t>(pending << (8u - pendingBits)))) return 1;
    *written = bytes;
    if (imageSource && imageSource->gpuUsed) *imageSource->gpuUsed = true;
    if (std::getenv("MOTIONCAM_JPEG_ENCODE_PROFILE"))
        std::fprintf(stderr, "JPEG encode GPU entropy: upload_ms=%.3f pack_read_ms=%.3f join_ms=%.3f samples=%zu bytes=%d\n",
                     std::chrono::duration<double, std::milli>(uploaded - started).count(),
                     std::chrono::duration<double, std::milli>(packedReady - uploaded).count(),
                     std::chrono::duration<double, std::milli>(
                         std::chrono::steady_clock::now() - packedReady).count(),
                     samples, bytes);
    return 0;
}
}
#else
namespace motioncam {
int jpegEntropyVulkan(const int16_t*, int, const uint16_t[18],
                      const uint16_t[18], uint8_t*, int, int*, void*) {
    return 1;
}
}
#endif
