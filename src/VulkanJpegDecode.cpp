#include "VulkanJpegDecode.h"

#ifdef MOTIONCAM_HAS_DIRECTLOG_PLACEBO
#include "VulkanJpegContext.h"
#include "VulkanGalleryPreview.h"
#include <libplacebo/gpu.h>
#include <algorithm>
#include <array>
#include <chrono>
#include <climits>
#include <cstdlib>
#include <cstring>
#include <cstdio>
#include <limits>
#include <mutex>
#include <memory>
#include <string>
#include <unordered_map>

namespace motioncam {
namespace {

constexpr uint32_t kMetaWords = 43;
constexpr uint32_t kLookupBits = 7;
constexpr uint32_t kLookupSize = 1u << kLookupBits;
// One invocation entropy-decodes each tile serially. Large, single-scan DNGs
// are substantially slower than CPU decode and can monopolize the GPU.
constexpr uint64_t kMaxSerialTileSamples = 65536;
constexpr char residentMetaShader[] = R"glsl(#version 450
layout(local_size_x = 64) in;
layout(std430, binding = 0) readonly buffer Lengths { uint words[]; } lengths;
layout(std430, binding = 1) readonly buffer Table { uint words[]; } table;
layout(std430, binding = 2) writeonly buffer Metadata { uint words[]; } meta;
layout(std430, binding = 3) writeonly buffer Lookups { uint words[]; } lookup;
layout(push_constant) uniform Constants {
    uint tiles; uint width; uint height; uint channels;
    uint capacityBytes; uint maxBodyBytes; uint slotWords;
} p;
void main() {
    uint tile = gl_GlobalInvocationID.x;
    if (tile >= p.tiles) return;
    uint m = tile * 43u;
    uint begin = tile * p.slotWords * 4u;
    uint headerBytes = 41u + table.words[1] + 5u * p.channels;
    uint bodyBytes = lengths.words[tile];
    meta.words[m] = begin + headerBytes;
    meta.words[m + 1u] = bodyBytes;
    meta.words[m + 2u] = p.width;
    meta.words[m + 3u] = p.height;
    meta.words[m + 4u] = 16u;
    meta.words[m + 5u] = table.words[0];
    meta.words[m + 6u] = tile * p.width * p.height * p.channels;
    meta.words[m + 7u] = 0u;
    for (uint i = 0u; i < 16u; ++i)
        meta.words[m + 8u + i] = table.words[2u + i];
    for (uint i = 0u; i < 17u; ++i)
        meta.words[m + 24u + i] = table.words[18u + i];
    meta.words[m + 41u] = p.channels;
    bool good = bodyBytes > 0u && bodyBytes <= p.maxBodyBytes &&
                begin <= p.capacityBytes &&
                headerBytes + bodyBytes + 2u <= p.capacityBytes - begin &&
                table.words[0] >= 1u && table.words[0] <= 7u &&
                table.words[1] > 0u && table.words[1] <= 17u;
    if (tile != 0u) {
        meta.words[m + 42u] = good ? 1u : 0u;
        return;
    }
    uint base = 0u;
    for (uint i = 0u; i < 128u; ++i) lookup.words[base + i] = 0u;
    uint code = 0u, symbol = 0u;
    for (uint length = 1u; length <= 16u; ++length) {
        uint count = table.words[1u + length];
        if (length <= 7u) {
            for (uint i = 0u; i < count; ++i) {
                uint start = (code + i) << (7u - length);
                uint repeat = 1u << (7u - length);
                if (start + repeat > 128u) { good = false; break; }
                for (uint j = 0u; j < repeat; ++j)
                    lookup.words[base + start + j] =
                        (table.words[18u + symbol + i] << 8u) | length;
            }
        }
        symbol += count;
        code = (code + count) << 1u;
    }
    meta.words[m + 42u] = good ? 1u : 0u;
}
)glsl";
constexpr char parseShader[] = R"glsl(#version 450
layout(local_size_x = 64) in;
layout(std430, binding = 0) readonly buffer Stream { uint words[]; } stream;
layout(std430, binding = 1) buffer Metadata { uint words[]; } meta;
layout(std430, binding = 2) writeonly buffer Lookups { uint words[]; } lookup;
layout(std430, binding = 3) readonly buffer Seeds { uint words[]; } seed;
layout(push_constant) uniform Constants { uint tiles; } p;
uint byteAt(uint index) {
    return (stream.words[index >> 2u] >> ((index & 3u) * 8u)) & 255u;
}
uint big16(uint index) { return (byteAt(index) << 8u) | byteAt(index + 1u); }
void main() {
    uint tile = gl_GlobalInvocationID.x;
    if (tile >= p.tiles) return;
    uint m = tile * 43u;
    uint s = tile * 7u;
    uint begin = seed.words[s], size = seed.words[s + 1u], end = begin + size;
    uint width = seed.words[s + 2u], height = seed.words[s + 3u];
    uint bits = seed.words[s + 4u], channels = seed.words[s + 6u];
    meta.words[m] = begin;
    meta.words[m + 1u] = size;
    meta.words[m + 2u] = width;
    meta.words[m + 3u] = height;
    meta.words[m + 4u] = bits;
    meta.words[m + 6u] = seed.words[s + 5u];
    meta.words[m + 41u] = channels;
    bool good = size >= 16u && end >= begin &&
                byteAt(begin) == 255u && byteAt(begin + 1u) == 216u;
    bool frame = false, table = false, scan = false;
    uint ids[3] = uint[3](0u, 0u, 0u);
    uint pos = begin + 2u;
    for (uint section = 0u; section < 32u && good && !scan; ++section) {
        if (pos + 4u > end || byteAt(pos++) != 255u) { good = false; break; }
        while (pos < end && byteAt(pos) == 255u) ++pos;
        if (pos + 2u > end) { good = false; break; }
        uint marker = byteAt(pos++);
        uint length = big16(pos);
        if (length < 2u || length > end - pos) { good = false; break; }
        uint payload = pos + 2u, n = length - 2u;
        pos += length;
        if (marker == 195u) {
            if (frame || n != 6u + 3u * channels ||
                byteAt(payload) != bits || big16(payload + 1u) != height ||
                big16(payload + 3u) != width ||
                byteAt(payload + 5u) != channels) { good = false; break; }
            for (uint c = 0u; c < channels; ++c) {
                uint at = payload + 6u + 3u * c;
                ids[c] = byteAt(at);
                if (byteAt(at + 1u) != 17u || byteAt(at + 2u) != 0u)
                    good = false;
                for (uint earlier = 0u; earlier < c; ++earlier)
                    if (ids[earlier] == ids[c]) good = false;
            }
            frame = true;
        } else if (marker == 196u) {
            if (table || n < 17u || byteAt(payload) != 0u) {
                good = false; break;
            }
            uint total = 0u;
            for (uint i = 0u; i < 16u; ++i) {
                uint count = byteAt(payload + 1u + i);
                meta.words[m + 8u + i] = count;
                total += count;
            }
            if (total == 0u || total > 17u || n != 17u + total) {
                good = false; break;
            }
            for (uint i = 0u; i < total; ++i) {
                uint category = byteAt(payload + 17u + i);
                if (category > 16u) good = false;
                meta.words[m + 24u + i] = category;
            }
            table = true;
        } else if (marker == 218u) {
            if (!frame || !table || n != 4u + 2u * channels ||
                byteAt(payload) != channels) { good = false; break; }
            for (uint c = 0u; c < channels; ++c)
                if (byteAt(payload + 1u + 2u * c) != ids[c] ||
                    byteAt(payload + 2u + 2u * c) != 0u) good = false;
            uint selector = payload + 1u + 2u * channels;
            uint mode = byteAt(selector);
            if (mode < 1u || mode > 7u || byteAt(selector + 1u) != 0u ||
                byteAt(selector + 2u) != 0u ||
                pos + 2u > end || byteAt(end - 2u) != 255u ||
                byteAt(end - 1u) != 217u) { good = false; break; }
            for (uint at = pos; at < end - 2u && good;) {
                if ((at & 3u) == 0u && at + 4u <= end - 2u) {
                    uint packed = stream.words[at >> 2u];
                    if ((packed & 255u) != 255u &&
                        ((packed >> 8u) & 255u) != 255u &&
                        ((packed >> 16u) & 255u) != 255u &&
                        (packed >> 24u) != 255u) {
                        at += 4u;
                        continue;
                    }
                }
                if (byteAt(at++) != 255u) continue;
                if (at >= end - 2u || byteAt(at++) != 0u) good = false;
            }
            meta.words[m] = pos;
            meta.words[m + 1u] = end - 2u - pos;
            meta.words[m + 5u] = mode;
            scan = true;
        } else if (marker < 224u || marker > 239u) {
            good = false;
        }
    }
    good = good && frame && table && scan && meta.words[m + 1u] > 0u;
    uint lookupBase = tile * 128u;
    for (uint i = 0u; i < 128u; ++i) lookup.words[lookupBase + i] = 0u;
    if (good) {
        uint code = 0u, symbol = 0u;
        for (uint length = 1u; length <= 16u; ++length) {
            uint count = meta.words[m + 7u + length];
            if (length <= 7u) {
                for (uint i = 0u; i < count; ++i) {
                    uint start = (code + i) << (7u - length);
                    uint repeat = 1u << (7u - length);
                    if (start + repeat > 128u) { good = false; break; }
                    for (uint j = 0u; j < repeat; ++j)
                        lookup.words[lookupBase + start + j] =
                            (meta.words[m + 24u + symbol + i] << 8u) | length;
                }
            }
            symbol += count;
            code = (code + count) << 1u;
        }
    }
    meta.words[m + 7u] = tile;
    meta.words[m + 42u] = good ? 1u : 0u;
}
)glsl";
constexpr char decodeShader[] = R"glsl(#version 450
layout(local_size_x = 8) in;
layout(std430, binding = 0) readonly buffer Stream { uint words[]; } stream;
layout(std430, binding = 1) readonly buffer Metadata { uint words[]; } meta;
layout(std430, binding = 2) buffer Image { uint words[]; } image;
layout(std430, binding = 3) writeonly buffer Status { uint words[]; } status;
layout(std430, binding = 4) readonly buffer Lookups { uint words[]; } lookup;
#ifdef JPEG_RESIDENT_DIRECT
layout(push_constant) uniform Constants {
    uint tiles; uint fullWidth; uint tileWidth; uint tileHeight; uint tilesAcross;
} p;
#else
layout(push_constant) uniform Constants { uint tiles; } p;
#endif
uint getByte(uint index) {
    return (stream.words[index >> 2u] >> ((index & 3u) * 8u)) & 255u;
}
uint pixel(uint index) {
    uint word = image.words[index >> 1u];
    return ((index & 1u) == 0u) ? (word & 65535u) : (word >> 16u);
}
int predictor(uint mode, int a, int b, int c) {
    if (mode == 1u) return a;
    if (mode == 2u) return b;
    if (mode == 3u) return c;
    if (mode == 4u) return a + b - c;
    if (mode == 5u) return a + ((b - c) >> 1);
    if (mode == 6u) return b + ((a - c) >> 1);
    return (a + b) >> 1;
}
void main() {
    uint tile = gl_GlobalInvocationID.x;
    if (tile >= p.tiles) return;
    uint m = tile * 43u;
    if (meta.words[m + 42u] != 1u) { status.words[tile] = 0u; return; }
    uint streamOffset = meta.words[m], streamBytes = meta.words[m + 1u];
    uint width = meta.words[m + 2u], height = meta.words[m + 3u];
    uint bits = meta.words[m + 4u], mode = meta.words[m + 5u];
    uint outputBase = meta.words[m + 6u];
    uint channels = meta.words[m + 41u];
    uint lookupBase = meta.words[m + 7u] * 128u;
    uint rowStride = width * channels;
#ifdef JPEG_RESIDENT_DIRECT
    rowStride = p.fullWidth * channels;
    uint tileX = (tile % p.tilesAcross) * p.tileWidth;
    uint tileY = (tile / p.tilesAcross) * p.tileHeight;
#endif
    uint bytePos = 0u, bit = 0u, pendingValue = 0u;
    uint sampleIndex = 0u;
    uint first = 0u, index = 0u, mask = (1u << bits) - 1u;
    bool good = true;
    for (uint y = 0u; y < height && good; ++y) {
        int left[3] = int[3](0, 0, 0);
        int aboveLeft[3] = int[3](0, 0, 0);
        for (uint x = 0u; x < width && good; ++x) {
          for (uint component = 0u; component < channels; ++component) {
            uint category = 0xffffffffu;
            if (bytePos >= streamBytes) { good = false; break; }
            uint current = getByte(streamOffset + bytePos);
            uint nextByte = bytePos + (current == 255u ? 2u : 1u);
            uint window = current << 8u;
            if (nextByte < streamBytes)
                window |= getByte(streamOffset + nextByte);
            uint look = lookup.words[lookupBase +
                                     ((window << bit) >> 9u & 127u)];
            if (look != 0u) {
                category = look >> 8u;
                uint consumed = bit + (look & 255u);
                if (consumed >= 8u)
                    bytePos = nextByte;
                bit = consumed & 7u;
                if (bytePos > streamBytes ||
                    (bytePos == streamBytes && bit != 0u)) good = false;
            } else {
                first = 0u; index = 0u;
                uint code = 0u;
                for (uint len = 1u; len <= 16u; ++len) {
                    if (bytePos >= streamBytes) { good = false; break; }
                    current = getByte(streamOffset + bytePos);
                    code = (code << 1u) | ((current >> (7u - bit)) & 1u);
                    if (++bit == 8u) {
                        bit = 0u;
                        bytePos += current == 255u ? 2u : 1u;
                    }
                    uint count = meta.words[m + 7u + len];
                    if (code >= first && code - first < count) {
                        category = meta.words[m + 24u + index + code - first];
                        break;
                    }
                    index += count;
                    first = (first + count) << 1u;
                }
            }
            if (!good || category > 16u) { good = false; break; }
            int diff = 0;
            if (category == 16u) {
                diff = 32768;
            } else if (category != 0u) {
                if (bytePos >= streamBytes) { good = false; break; }
                uint b0 = getByte(streamOffset + bytePos);
                uint p1 = bytePos + (b0 == 255u ? 2u : 1u);
                uint needed = bit + category;
                uint b1 = 0u, b2 = 0u, p2 = p1;
                if (needed > 8u) {
                    if (p1 >= streamBytes) { good = false; break; }
                    b1 = getByte(streamOffset + p1);
                    p2 = p1 + (b1 == 255u ? 2u : 1u);
                }
                if (needed > 16u) {
                    if (p2 >= streamBytes) { good = false; break; }
                    b2 = getByte(streamOffset + p2);
                }
                uint value = ((b0 << 16u) | (b1 << 8u) | b2) >>
                             (24u - needed);
                value &= (1u << category) - 1u;
                if (needed >= 16u) bytePos = p2;
                else if (needed >= 8u) bytePos = p1;
                bit = needed & 7u;
                if (bytePos > streamBytes ||
                    (bytePos == streamBytes && bit != 0u)) { good = false; break; }
                diff = int(value);
                if (value < (1u << (category - 1u))) diff += 1 - int(1u << category);
            }
            uint i = sampleIndex++;
            uint outputIndex = outputBase + i;
#ifdef JPEG_RESIDENT_DIRECT
            outputIndex = ((tileY + y) * p.fullWidth + tileX + x) *
                          channels + component;
#endif
            int pred;
            if (y == 0u && x == 0u) pred = int(1u << (bits - 1u));
            else if (y == 0u) pred = left[component];
            else {
                const bool needsAbove = x == 0u || mode != 1u;
                int b = needsAbove ? int(pixel(outputIndex - rowStride)) : 0;
                if (x == 0u) pred = b;
                else {
                    int c = mode >= 3u && mode <= 6u
                        ? aboveLeft[component] : 0;
                    pred = predictor(mode, left[component], b, c);
                }
                if (mode >= 3u && mode <= 6u)
                    aboveLeft[component] = b;
            }
            uint value = uint(pred + diff) & mask;
            left[component] = int(value);
            if ((i & 1u) == 0u) { pendingValue = value; }
            else {
                image.words[outputIndex >> 1u] = pendingValue | (value << 16u);
            }
          }
        }
    }
    if (good && (sampleIndex & 1u) != 0u)
        image.words[(outputBase + sampleIndex - 1u) >> 1u] = pendingValue;
    status.words[tile] = good ? 1u : 0u;
}
)glsl";

constexpr char scatterShader[] = R"glsl(#version 450
layout(local_size_x = 256) in;
layout(std430, binding = 0) readonly buffer Tiles { uint words[]; } tiles;
layout(std430, binding = 1) readonly buffer Metadata { uint words[]; } meta;
layout(std430, binding = 2) writeonly buffer Image { uint words[]; } image;
layout(push_constant) uniform Constants {
    uint width; uint height; uint tileWidth; uint tileHeight; uint tilesAcross;
    uint channels;
} p;
uint sampleAt(uint rowSample, uint y) {
    uint x = rowSample / p.channels;
    uint component = rowSample % p.channels;
    uint tile = (y / p.tileHeight) * p.tilesAcross + x / p.tileWidth;
    uint local = ((y % p.tileHeight) * p.tileWidth + x % p.tileWidth) *
                 p.channels + component;
    uint base = meta.words[tile * 43u + 6u];
    uint word = tiles.words[(base + local) >> 1u];
    return (local & 1u) == 0u ? word & 65535u : word >> 16u;
}
void main() {
    uint pair = gl_GlobalInvocationID.x;
    uint wordsPerRow = (p.width * p.channels) >> 1u;
    if (pair >= wordsPerRow * p.height) return;
    uint y = pair / wordsPerRow, rowSample = (pair % wordsPerRow) * 2u;
    image.words[pair] = sampleAt(rowSample, y) |
                        (sampleAt(rowSample + 1u, y) << 16u);
}
)glsl";

constexpr char validateShader[] = R"glsl(#version 450
layout(local_size_x = 1) in;
layout(std430, binding = 0) readonly buffer Status { uint words[]; } status;
layout(std430, binding = 1) writeonly buffer Result { uint words[]; } result;
layout(push_constant) uniform Constants { uint tiles; } p;
void main() {
    uint valid = 1u;
    for (uint i = 0u; i < p.tiles; ++i)
        valid &= status.words[i] == 1u ? 1u : 0u;
    result.words[0] = valid;
}
)glsl";

struct ParsedTile {
    std::array<uint32_t, kMetaWords> meta{};
    std::array<uint32_t, kLookupSize> lookup{};
    size_t scanOffset = 0, scanLength = 0;
};

uint16_t be16(const uint8_t* p) { return (uint16_t(p[0]) << 8u) | p[1]; }

bool parseTile(const VulkanJpegTile& tile, ParsedTile& result) {
    if (!tile.bytes || tile.size < 16 || tile.size > UINT32_MAX ||
        tile.bytes[0] != 0xff || tile.bytes[1] != 0xd8 ||
        !tile.width || !tile.height || tile.width > 65535 ||
        tile.height > 65535 || tile.bits < 8 || tile.bits > 16 ||
        (tile.channels != 1 && tile.channels != 3))
        return false;
    bool frame = false, table = false, scan = false;
    std::array<uint8_t, 3> componentIds{};
    size_t pos = 2;
    while (pos + 4 <= tile.size) {
        if (tile.bytes[pos++] != 0xff) return false;
        while (pos < tile.size && tile.bytes[pos] == 0xff) ++pos;
        if (pos >= tile.size) return false;
        const uint8_t marker = tile.bytes[pos++];
        if (marker == 0xd9) break;
        if (marker == 0xd8 || marker == 0x01 ||
            (marker >= 0xd0 && marker <= 0xd7)) return false;
        if (pos + 2 > tile.size) return false;
        const uint16_t length = be16(tile.bytes + pos);
        if (length < 2 || length > tile.size - pos) return false;
        const uint8_t* payload = tile.bytes + pos + 2;
        const size_t n = length - 2;
        pos += length;
        if (marker == 0xc3) {
            if (frame || n != 6u + 3u * tile.channels ||
                payload[0] != tile.bits ||
                be16(payload + 1) != tile.height ||
                be16(payload + 3) != tile.width ||
                payload[5] != tile.channels)
                return false;
            for (uint32_t i = 0; i < tile.channels; ++i) {
                const size_t at = 6u + i * 3u;
                if (payload[at + 1u] != 0x11 || payload[at + 2u] != 0)
                    return false;
                componentIds[i] = payload[at];
                for (uint32_t earlier = 0; earlier < i; ++earlier)
                    if (componentIds[earlier] == componentIds[i]) return false;
            }
            frame = true;
        } else if (marker == 0xc4) {
            size_t at = 0;
            while (at < n) {
                if (n - at < 17 || payload[at++] != 0) return false;
                uint32_t total = 0;
                for (size_t i = 0; i < 16; ++i)
                    total += result.meta[8 + i] = payload[at + i];
                at += 16;
                if (!total || total > 17 || total > n - at || table) return false;
                for (size_t i = 0; i < total; ++i) {
                    if (payload[at + i] > 16) return false;
                    result.meta[24 + i] = payload[at + i];
                }
                at += total;
                table = true;
            }
        } else if (marker == 0xda) {
            if (!frame || !table || scan ||
                n != 4u + 2u * tile.channels || payload[0] != tile.channels)
                return false;
            for (uint32_t i = 0; i < tile.channels; ++i)
                if (payload[1u + 2u * i] != componentIds[i] ||
                    payload[2u + 2u * i] != 0) return false;
            const size_t selector = 1u + 2u * tile.channels;
            if (payload[selector] < 1 || payload[selector] > 7 ||
                payload[selector + 1u] != 0 || payload[selector + 2u] != 0)
                return false;
            result.meta[2] = tile.width;
            result.meta[3] = tile.height;
            result.meta[4] = tile.bits;
            result.meta[5] = payload[selector];
            result.meta[41] = tile.channels;
            const size_t scanStart = pos;
            while (pos < tile.size) {
                const uint8_t byte = tile.bytes[pos++];
                if (byte != 0xff) continue;
                if (pos >= tile.size) return false;
                const uint8_t next = tile.bytes[pos++];
                if (next == 0) continue;
                if (next == 0xd9 && pos == tile.size) {
                    result.scanOffset = scanStart;
                    result.scanLength = pos - scanStart - 2u;
                    scan = true;
                    break;
                }
                return false;
            }
            break;
        } else if (marker == 0xdd) {
            // Restart intervals need a separate decoder.
            return false;
        } else if (marker < 0xe0 || marker > 0xef) {
            // Exclude unsupported coding and extension markers.
            return false;
        }
    }
    if (!frame || !table || !scan || !result.scanLength) return false;
    uint32_t code = 0, symbol = 0;
    for (uint32_t length = 1; length <= 16; ++length) {
        const uint32_t count = result.meta[7 + length];
        if (length <= kLookupBits) {
            for (uint32_t i = 0; i < count; ++i) {
                const uint32_t start = (code + i) << (kLookupBits - length);
                const uint32_t repeat = 1u << (kLookupBits - length);
                if (start + repeat > result.lookup.size()) return false;
                std::fill_n(result.lookup.begin() + start, repeat,
                    (result.meta[24 + symbol + i] << 8u) | length);
            }
        }
        symbol += count;
        code = (code + count) << 1u;
    }
    result.meta[42] = 1u;
    return true;
}

struct DecodeState {
    pl_gpu gpu = nullptr;
    pl_pass parser = nullptr;
    pl_pass residentMeta = nullptr;
    pl_pass pass = nullptr;
    pl_pass residentDirect = nullptr;
    pl_pass scatter = nullptr;
    pl_pass validate = nullptr;
    pl_timer timer = nullptr;
    pl_timer scatterTimer = nullptr;
    pl_timer parserTimer = nullptr;
    pl_buf stream = nullptr, metadata = nullptr, image = nullptr;
    pl_buf streamUpload = nullptr, metadataUpload = nullptr;
    pl_buf seed = nullptr, seedUpload = nullptr;
    pl_buf status = nullptr, readback = nullptr, statusReadback = nullptr;
    pl_buf lookup = nullptr, lookupUpload = nullptr;
    pl_buf assembled = nullptr;
    pl_buf valid = nullptr;
    pl_buf validReadback = nullptr;
    ~DecodeState() {
        if (!gpu) return;
        releaseVulkanGalleryTextureDevice(gpu);
        pl_gpu_finish(gpu);
        for (auto* buffer : {&stream, &metadata, &image, &status,
                             &streamUpload, &metadataUpload, &seed, &seedUpload,
                             &readback, &statusReadback, &lookup, &lookupUpload,
                             &assembled, &valid, &validReadback})
            pl_buf_destroy(gpu, buffer);
        pl_pass_destroy(gpu, &pass);
        pl_pass_destroy(gpu, &residentDirect);
        pl_pass_destroy(gpu, &parser);
        pl_pass_destroy(gpu, &residentMeta);
        pl_pass_destroy(gpu, &scatter);
        pl_pass_destroy(gpu, &validate);
        pl_timer_destroy(gpu, &timer);
        pl_timer_destroy(gpu, &scatterTimer);
        pl_timer_destroy(gpu, &parserTimer);
    }
    bool init(pl_gpu device) {
        if (pass)
            return gpu == device && parser && residentMeta && residentDirect &&
                   scatter && validate;
        gpu = device;
        pl_desc descriptors[] = {
            {"stream", PL_DESC_BUF_STORAGE, 0, PL_DESC_ACCESS_READONLY},
            {"meta", PL_DESC_BUF_STORAGE, 1, PL_DESC_ACCESS_READONLY},
            {"image", PL_DESC_BUF_STORAGE, 2, PL_DESC_ACCESS_READWRITE},
            {"status", PL_DESC_BUF_STORAGE, 3, PL_DESC_ACCESS_WRITEONLY},
            {"lookup", PL_DESC_BUF_STORAGE, 4, PL_DESC_ACCESS_READONLY},
        };
        pl_pass_params params{};
        params.type = PL_PASS_COMPUTE;
        params.glsl_shader = decodeShader;
        params.descriptors = descriptors;
        params.num_descriptors = 5;
        params.push_constants_size = sizeof(uint32_t);
        pass = pl_pass_create(gpu, &params);
        std::string directSource = decodeShader;
        directSource.insert(directSource.find('\n') + 1u,
                            "#define JPEG_RESIDENT_DIRECT\n");
        params.glsl_shader = directSource.c_str();
        params.push_constants_size = 5 * sizeof(uint32_t);
        residentDirect = pl_pass_create(gpu, &params);
        pl_desc parserDescriptors[] = {
            {"stream", PL_DESC_BUF_STORAGE, 0, PL_DESC_ACCESS_READONLY},
            {"meta", PL_DESC_BUF_STORAGE, 1, PL_DESC_ACCESS_READWRITE},
            {"lookup", PL_DESC_BUF_STORAGE, 2, PL_DESC_ACCESS_WRITEONLY},
            {"seed", PL_DESC_BUF_STORAGE, 3, PL_DESC_ACCESS_READONLY},
        };
        params.glsl_shader = parseShader;
        params.descriptors = parserDescriptors;
        params.num_descriptors = 4;
        params.push_constants_size = sizeof(uint32_t);
        parser = pl_pass_create(gpu, &params);
        pl_desc residentMetaDescriptors[] = {
            {"lengths", PL_DESC_BUF_STORAGE, 0, PL_DESC_ACCESS_READONLY},
            {"table", PL_DESC_BUF_STORAGE, 1, PL_DESC_ACCESS_READONLY},
            {"meta", PL_DESC_BUF_STORAGE, 2, PL_DESC_ACCESS_WRITEONLY},
            {"lookup", PL_DESC_BUF_STORAGE, 3, PL_DESC_ACCESS_WRITEONLY},
        };
        params.glsl_shader = residentMetaShader;
        params.descriptors = residentMetaDescriptors;
        params.num_descriptors = 4;
        params.push_constants_size = 7 * sizeof(uint32_t);
        residentMeta = pl_pass_create(gpu, &params);
        pl_desc scatterDescriptors[] = {
            {"tiles", PL_DESC_BUF_STORAGE, 0, PL_DESC_ACCESS_READONLY},
            {"meta", PL_DESC_BUF_STORAGE, 1, PL_DESC_ACCESS_READONLY},
            {"image", PL_DESC_BUF_STORAGE, 2, PL_DESC_ACCESS_WRITEONLY},
        };
        params.glsl_shader = scatterShader;
        params.descriptors = scatterDescriptors;
        params.num_descriptors = 3;
        params.push_constants_size = 6 * sizeof(uint32_t);
        scatter = pl_pass_create(gpu, &params);
        pl_desc validateDescriptors[] = {
            {"status", PL_DESC_BUF_STORAGE, 0, PL_DESC_ACCESS_READONLY},
            {"result", PL_DESC_BUF_STORAGE, 1, PL_DESC_ACCESS_WRITEONLY},
        };
        params.glsl_shader = validateShader;
        params.descriptors = validateDescriptors;
        params.num_descriptors = 2;
        params.push_constants_size = sizeof(uint32_t);
        validate = pl_pass_create(gpu, &params);
        timer = pl_timer_create(gpu);
        scatterTimer = pl_timer_create(gpu);
        parserTimer = pl_timer_create(gpu);
        return parser && residentMeta && pass && residentDirect &&
               scatter && validate;
    }
    void decodeImage(pl_buf input, uint32_t tileCount, bool directOutput,
                     uint32_t width, uint32_t tileWidth, uint32_t tileHeight, bool profile) {
        pl_desc_binding bindings[5]{};
        bindings[0].object = input;
        bindings[1].object = metadata;
        bindings[2].object = directOutput ? assembled : image;
        bindings[3].object = status;
        bindings[4].object = lookup;
        uint32_t constants[5] = {
            tileCount, width, tileWidth, tileHeight, tileWidth ? width / tileWidth : 0u};
        pl_pass_run_params run{};
        run.pass = directOutput ? residentDirect : pass;
        run.desc_bindings = bindings;
        run.push_constants = directOutput ? constants : &tileCount;
        if (profile) run.timer = timer;
        run.compute_groups[0] = int((tileCount + 7u) / 8u);
        run.compute_groups[1] = run.compute_groups[2] = 1;
        pl_pass_run(gpu, &run);
    }
    void scatterImage(uint32_t width, uint32_t height, uint32_t tileWidth,
                      uint32_t tileHeight, uint32_t channels, bool profile) {
        uint32_t constants[6] = {width, height, tileWidth, tileHeight, width / tileWidth, channels};
        pl_desc_binding bindings[3]{};
        bindings[0].object = image;
        bindings[1].object = metadata;
        bindings[2].object = assembled;
        pl_pass_run_params run{};
        run.pass = scatter;
        run.desc_bindings = bindings;
        run.push_constants = constants;
        if (profile) run.timer = scatterTimer;
        run.compute_groups[0] = int((uint64_t(width) * height * channels / 2u + 255u) / 256u);
        run.compute_groups[1] = run.compute_groups[2] = 1;
        pl_pass_run(gpu, &run);
    }
    void validateTiles(uint32_t tileCount) {
        pl_desc_binding bindings[2]{};
        bindings[0].object = status;
        bindings[1].object = valid;
        pl_pass_run_params run{};
        run.pass = validate;
        run.desc_bindings = bindings;
        run.push_constants = &tileCount;
        run.compute_groups[0] = run.compute_groups[1] = run.compute_groups[2] = 1;
        pl_pass_run(gpu, &run);
    }
    bool buffer(pl_buf& target, size_t bytes, bool readable = false,
                bool writable = false) {
        pl_buf_params params{};
        params.size = bytes;
        params.storable = !readable && !writable;
        params.host_readable = readable;
        params.host_writable = writable;
        params.memory_type = readable || writable ? PL_BUF_MEM_HOST : PL_BUF_MEM_DEVICE;
        return bytes && bytes <= gpu->limits.max_ssbo_size &&
               pl_buf_recreate(gpu, &target, &params);
    }
};

DecodeState& state() { static DecodeState value; return value; }
struct ResidentStates {
    std::mutex mutex;
    std::unordered_map<pl_gpu, std::unique_ptr<DecodeState>> states;
};
ResidentStates& residentStates() {
    static ResidentStates value;
    return value;
}
} // namespace

bool jpegDecodeResidentTiledImageVulkan(
    const void* gpuHandle, const void* jpegHandle,
    const void* lengthsHandle, const void* tableHandle,
    uint32_t width, uint32_t height, uint32_t channels,
    uint32_t tileWidth, uint32_t tileHeight, uint32_t tileCount,
    uint32_t slotWords,
    const std::function<bool(const VulkanDecodedJpegImage&)>& consume) {
    auto gpu = static_cast<pl_gpu>(const_cast<void*>(gpuHandle));
    auto stream = static_cast<pl_buf>(const_cast<void*>(jpegHandle));
    auto lengths = static_cast<pl_buf>(const_cast<void*>(lengthsHandle));
    auto table = static_cast<pl_buf>(const_cast<void*>(tableHandle));
    const uint64_t tileSamples = uint64_t(tileWidth) * tileHeight * channels;
    const uint64_t samples = uint64_t(width) * height * channels;
    if (!gpu || !stream || !lengths || !table || !consume ||
        (channels != 1u && channels != 3u) || !tileWidth || !tileHeight ||
        width % tileWidth || height % tileHeight ||
        uint64_t(width / tileWidth) * (height / tileHeight) != tileCount ||
        !tileCount || !slotWords ||
        uint64_t(slotWords) * tileCount * 4u > stream->params.size ||
        stream->params.size > UINT32_MAX ||
        tileSamples > kMaxSerialTileSamples ||
        (tileSamples & 1u) || (uint64_t(tileWidth) * channels & 1u) ||
        samples > UINT32_MAX ||
        tileCount > uint32_t(gpu->limits.max_dispatch[0]) * 8ull ||
        (samples / 2u + 255u) / 256u >
            uint32_t(gpu->limits.max_dispatch[0])) return false;
    auto& owner = residentStates();
    std::lock_guard lock(owner.mutex);
    auto& slot = owner.states[gpu];
    if (!slot) slot = std::make_unique<DecodeState>();
    DecodeState& s = *slot;
    const bool profile = std::getenv("MOTIONCAM_JPEG_DECODE_PROFILE") != nullptr;
    const bool directOutput =
        std::getenv("MOTIONCAM_JPEG_RESIDENT_SCATTER") == nullptr;
    const size_t metadataBytes = size_t(tileCount) * kMetaWords * 4u;
    const size_t lookupBytes = kLookupSize * 4u;
    if (!s.init(gpu) ||
        !s.buffer(s.metadata, metadataBytes) ||
        !s.buffer(s.lookup, lookupBytes) ||
        (!directOutput && !s.buffer(s.image, size_t(samples) * 2u)) ||
        !s.buffer(s.status, size_t(tileCount) * 4u) ||
        !s.buffer(s.assembled, size_t(samples) * 2u) ||
        !s.buffer(s.valid, 4u)) return false;
    uint32_t metaConstants[7] = {
        tileCount, tileWidth, tileHeight, channels,
        uint32_t(stream->params.size), uint32_t(tileSamples * 3u), slotWords};
    pl_desc_binding metaBindings[4]{};
    metaBindings[0].object = lengths;
    metaBindings[1].object = table;
    metaBindings[2].object = s.metadata;
    metaBindings[3].object = s.lookup;
    pl_pass_run_params metaRun{};
    metaRun.pass = s.residentMeta;
    metaRun.desc_bindings = metaBindings;
    metaRun.push_constants = metaConstants;
    if (profile) metaRun.timer = s.parserTimer;
    metaRun.compute_groups[0] = int((tileCount + 63u) / 64u);
    metaRun.compute_groups[1] = metaRun.compute_groups[2] = 1;
    pl_pass_run(gpu, &metaRun);
    s.decodeImage(stream, tileCount, directOutput, width, tileWidth, tileHeight, profile);
    if (!directOutput) s.scatterImage(width, height, tileWidth, tileHeight, channels, profile);
    s.validateTiles(tileCount);
    const bool consumed = consume({gpu, s.assembled, s.valid, width, height,
                                   channels, 16u, width * channels / 2u});
    if (profile) {
        pl_gpu_finish(gpu);
        std::fprintf(stderr,
            "GPU resident JPEG decode: channels=%u metadata=%.3f entropy=%.3f scatter=%.3f ms\n",
            channels, double(pl_timer_query(gpu, s.parserTimer)) / 1000000.0,
            double(pl_timer_query(gpu, s.timer)) / 1000000.0,
            directOutput ? 0.0 :
                double(pl_timer_query(gpu, s.scatterTimer)) / 1000000.0);
    }
    return consumed;
}

void releaseVulkanJpegDecodeDevice(const void* gpuHandle) {
    auto& owner = residentStates();
    std::lock_guard lock(owner.mutex);
    owner.states.erase(static_cast<pl_gpu>(const_cast<void*>(gpuHandle)));
}

static bool decodeTiles(const std::vector<VulkanJpegTile>& tiles,
                        std::vector<std::vector<uint16_t>>* pixels,
                        uint32_t width, uint32_t height,
                        uint32_t tileWidth, uint32_t tileHeight,
                        const std::function<bool(const VulkanDecodedJpegImage&)>* consume,
                        const VulkanJpegContiguousSource* contiguous,
                        bool readValidation = false) {
    const bool profile = std::getenv("MOTIONCAM_JPEG_DECODE_PROFILE") != nullptr;
    const bool gpuParse = contiguous ||
        std::getenv("MOTIONCAM_GPU_JPEG_GPU_PARSE") != nullptr;
    const auto started = std::chrono::steady_clock::now();
    if (tiles.empty() || tiles.size() > UINT32_MAX ||
        std::getenv("MOTIONCAM_GPU_JPEG_DECODE_DISABLE")) return false;
    if (contiguous && (!contiguous->bytes || !contiguous->size ||
                       contiguous->size > UINT32_MAX)) return false;
    std::vector<ParsedTile> parsed;
    if (!gpuParse) parsed.resize(tiles.size());
    std::vector<uint32_t> metadata;
    std::vector<uint32_t> seeds;
    if (gpuParse) seeds.resize(tiles.size() * 7u);
    else metadata.resize(tiles.size() * kMetaWords);
    uint64_t scanBytes = 0, sampleCount = 0;
    for (size_t i = 0; i < tiles.size(); ++i) {
        if (gpuParse) {
            if (!tiles[i].bytes || tiles[i].size < 16u ||
                tiles[i].size > UINT32_MAX || !tiles[i].width || !tiles[i].height ||
                tiles[i].width > 65535u || tiles[i].height > 65535u ||
                tiles[i].bits < 8u || tiles[i].bits > 16u ||
                (tiles[i].channels != 1u && tiles[i].channels != 3u)) return false;
            uint32_t* seed = seeds.data() + i * 7u;
            seed[2] = tiles[i].width;
            seed[3] = tiles[i].height;
            seed[4] = tiles[i].bits;
            seed[6] = tiles[i].channels;
        } else if (!parseTile(tiles[i], parsed[i])) return false;
        const uint64_t n = uint64_t(tiles[i].width) * tiles[i].height *
                           tiles[i].channels;
        const size_t bytes = gpuParse ? tiles[i].size : parsed[i].scanLength;
        if ((n > kMaxSerialTileSamples &&
             !std::getenv("MOTIONCAM_GPU_JPEG_ALLOW_LARGE_SCAN")) ||
            scanBytes + bytes > UINT32_MAX ||
            sampleCount + ((n + 1u) & ~1ull) > UINT32_MAX) return false;
        if (contiguous) {
            const uintptr_t base = reinterpret_cast<uintptr_t>(contiguous->bytes);
            const uintptr_t pointer = reinterpret_cast<uintptr_t>(tiles[i].bytes);
            if (pointer < base || pointer - base > contiguous->size ||
                bytes > contiguous->size - (pointer - base)) return false;
            if (gpuParse) seeds[i * 7u] = uint32_t(pointer - base);
            else parsed[i].meta[0] = uint32_t(pointer - base);
        } else {
            if (gpuParse) seeds[i * 7u] = uint32_t(scanBytes);
            else parsed[i].meta[0] = uint32_t(scanBytes);
        }
        if (gpuParse) {
            seeds[i * 7u + 1u] = uint32_t(bytes);
            seeds[i * 7u + 5u] = uint32_t(sampleCount);
        } else {
            parsed[i].meta[1] = uint32_t(bytes);
            parsed[i].meta[6] = uint32_t(sampleCount);
        }
        scanBytes += bytes;
        sampleCount += (n + 1u) & ~1ull;
    }
    if (contiguous) scanBytes = contiguous->size;
    std::unordered_map<uint64_t, std::vector<uint32_t>> lookupBuckets;
    std::vector<std::array<uint32_t, kLookupSize>> uniqueLookups;
    if (!gpuParse) for (auto& tile : parsed) {
        uint64_t hash = 1469598103934665603ull;
        for (uint32_t value : tile.lookup) {
            hash ^= value;
            hash *= 1099511628211ull;
        }
        auto& bucket = lookupBuckets[hash];
        auto match = std::find_if(bucket.begin(), bucket.end(), [&](uint32_t index) {
            return uniqueLookups[index] == tile.lookup;
        });
        if (match == bucket.end()) {
            tile.meta[7] = uint32_t(uniqueLookups.size());
            bucket.push_back(tile.meta[7]);
            uniqueLookups.push_back(tile.lookup);
        } else {
            tile.meta[7] = *match;
        }
    }
    const auto parsedAt = std::chrono::steady_clock::now();
    auto& context = jpegContext();
    std::lock_guard lock(context.mutex);
    if (!context.init()) return false;
    auto gpu = context.vulkan->gpu;
    auto& s = state();
    if (!s.init(gpu) || tiles.size() > uint32_t(gpu->limits.max_dispatch[0]) * 8ull)
        return false;
    const size_t streamSize = size_t((scanBytes + 3u) & ~3ull);
    const size_t imageSize = size_t(sampleCount) * 2u;
    // Reuse the encoder round-trip decoder's direct CFA output layout.
    const bool directOutput = consume && tiles.front().channels == 1u &&
        !std::getenv("MOTIONCAM_JPEG_RESIDENT_SCATTER");
    const size_t statusSize = tiles.size() * 4u;
    const size_t metaSize = tiles.size() * kMetaWords * 4u;
    const size_t seedSize = seeds.size() * 4u;
    const size_t lookupSize = (gpuParse ? tiles.size() : uniqueLookups.size()) *
                              kLookupSize * 4u;
    if (!s.buffer(s.stream, streamSize) || !s.buffer(s.metadata, metaSize) ||
        !s.buffer(s.streamUpload, streamSize, false, true) ||
        (gpuParse ? (!s.buffer(s.seed, seedSize) ||
                     !s.buffer(s.seedUpload, seedSize, false, true))
                  : !s.buffer(s.metadataUpload, metaSize, false, true)) ||
        !s.buffer(s.lookup, lookupSize) ||
        (!gpuParse && !s.buffer(s.lookupUpload, lookupSize, false, true)) ||
        (!directOutput && !s.buffer(s.image, imageSize)) || !s.buffer(s.status, statusSize) ||
        (!consume && !s.buffer(s.readback, imageSize, true)) ||
        (consume ? (!s.buffer(s.valid, 4u) ||
                    (readValidation && !s.buffer(s.validReadback, 4u, true)))
                 : !s.buffer(s.statusReadback, statusSize, true))) return false;
    if (consume) {
        if (!width || !height || !tileWidth || !tileHeight ||
            (uint64_t(width) * tiles.front().channels & 1u) ||
            (uint64_t(tileWidth) * tiles.front().channels & 1u) ||
            width % tileWidth || height % tileHeight ||
            uint64_t(width / tileWidth) * (height / tileHeight) != tiles.size() ||
            uint64_t(width) * height * tiles.front().channels > UINT32_MAX ||
            (uint64_t(width) * height * tiles.front().channels / 2u + 255u) / 256u >
                uint32_t(gpu->limits.max_dispatch[0]) ||
            !s.buffer(s.assembled,
                      uint64_t(width) * height * tiles.front().channels * 2u))
            return false;
        const uint32_t bits = tiles.front().bits;
        const uint32_t channels = tiles.front().channels;
        if (std::any_of(tiles.begin(), tiles.end(), [&](const VulkanJpegTile& tile) {
                return tile.width != tileWidth || tile.height != tileHeight ||
                    tile.bits != bits || tile.channels != channels;
            })) return false;
    }
    std::vector<uint8_t> stream;
    if (!contiguous) stream.resize(streamSize, 0);
    std::vector<uint32_t> lookup;
    if (!gpuParse) lookup.resize(uniqueLookups.size() * kLookupSize);
    for (size_t i = 0; i < tiles.size(); ++i) {
        if (!contiguous) {
            const size_t offset = gpuParse ? seeds[i * 7u]
                                           : parsed[i].meta[0];
            std::memcpy(stream.data() + offset,
                        tiles[i].bytes + (gpuParse ? 0u : parsed[i].scanOffset),
                        gpuParse ? tiles[i].size : parsed[i].scanLength);
        }
        if (!gpuParse)
            std::copy(parsed[i].meta.begin(), parsed[i].meta.end(),
                      metadata.begin() + i * kMetaWords);
    }
    if (!gpuParse)
        for (size_t i = 0; i < uniqueLookups.size(); ++i)
            std::copy(uniqueLookups[i].begin(), uniqueLookups[i].end(),
                      lookup.begin() + i * kLookupSize);
    if (contiguous) {
        const size_t complete = contiguous->size & ~size_t{3};
        if (complete)
            pl_buf_write(gpu, s.streamUpload, 0, contiguous->bytes, complete);
        if (streamSize > complete) {
            const uint32_t zero = 0;
            uint32_t last = zero;
            std::memcpy(&last, contiguous->bytes + complete,
                        contiguous->size - complete);
            pl_buf_write(gpu, s.streamUpload, complete, &last, 4u);
        }
    } else {
        pl_buf_write(gpu, s.streamUpload, 0, stream.data(), stream.size());
    }
    if (gpuParse)
        pl_buf_write(gpu, s.seedUpload, 0, seeds.data(), seedSize);
    else
        pl_buf_write(gpu, s.metadataUpload, 0, metadata.data(), metaSize);
    if (!gpuParse)
        pl_buf_write(gpu, s.lookupUpload, 0, lookup.data(), lookupSize);
    pl_buf_copy(gpu, s.stream, 0, s.streamUpload, 0, streamSize);
    if (gpuParse)
        pl_buf_copy(gpu, s.seed, 0, s.seedUpload, 0, seedSize);
    else
        pl_buf_copy(gpu, s.metadata, 0, s.metadataUpload, 0, metaSize);
    if (gpuParse) {
        uint32_t tileCount = uint32_t(tiles.size());
        pl_desc_binding parserBindings[4]{};
        parserBindings[0].object = s.stream;
        parserBindings[1].object = s.metadata;
        parserBindings[2].object = s.lookup;
        parserBindings[3].object = s.seed;
        pl_pass_run_params parserRun{};
        parserRun.pass = s.parser;
        parserRun.desc_bindings = parserBindings;
        parserRun.push_constants = &tileCount;
        if (profile) parserRun.timer = s.parserTimer;
        parserRun.compute_groups[0] = int((tileCount + 63u) / 64u);
        parserRun.compute_groups[1] = parserRun.compute_groups[2] = 1;
        pl_pass_run(gpu, &parserRun);
    } else {
        pl_buf_copy(gpu, s.lookup, 0, s.lookupUpload, 0, lookupSize);
    }
    const auto uploadedAt = std::chrono::steady_clock::now();
    uint32_t tileCount = uint32_t(tiles.size());
    s.decodeImage(s.stream, tileCount, directOutput, width, tileWidth, tileHeight, profile);
    if (!consume)
        pl_buf_copy(gpu, s.readback, 0, s.image, 0, imageSize);
    if (!consume) {
        pl_buf_copy(gpu, s.statusReadback, 0, s.status, 0, statusSize);
    }
    if (consume) s.validateTiles(tileCount);
    const auto dispatchedAt = std::chrono::steady_clock::now();
    if (consume && !directOutput)
        s.scatterImage(width, height, tileWidth, tileHeight, tiles.front().channels, profile);
    if (consume) {
        if (readValidation) {
            pl_buf_copy(gpu, s.validReadback, 0, s.valid, 0, 4u);
            uint32_t valid = 0;
            if (!pl_buf_read(gpu, s.validReadback, 0, &valid, 4u) || valid != 1u)
                return false;
        }
        const VulkanDecodedJpegImage image{
            gpu, s.assembled, s.valid, width, height, tiles.front().channels,
            tiles.front().bits, width * tiles.front().channels / 2u};
        const auto preparedForConsumer = std::chrono::steady_clock::now();
        const bool consumed = (*consume)(image);
        if (profile) {
            std::array<uint32_t, 8> predictorCounts{};
            const size_t stride = std::max<size_t>(1u, tiles.size() / 64u);
            for (size_t i = 0; i < tiles.size(); i += stride) {
                ParsedTile sample;
                if (parseTile(tiles[i], sample) && sample.meta[5] <= 7u)
                    ++predictorCounts[sample.meta[5]];
            }
            const auto ms = [](auto from, auto to) {
                return std::chrono::duration<double, std::milli>(to - from).count();
            };
            std::fprintf(stderr,
                "GPU JPEG resident stages: channels=%u tiles=%zu unique_tables=%zu parse=%.3f prepare=%.3f submit=%.3f consume=%.3f parser_kernel=%.3f decode_kernel=%.3f scatter_kernel=%.3f ms\n",
                tiles.front().channels, tiles.size(), uniqueLookups.size(),
                ms(started, parsedAt), ms(parsedAt, uploadedAt),
                ms(uploadedAt, preparedForConsumer),
                ms(preparedForConsumer, std::chrono::steady_clock::now()),
                double(pl_timer_query(gpu, s.parserTimer)) / 1000000.0,
                double(pl_timer_query(gpu, s.timer)) / 1000000.0,
                double(pl_timer_query(gpu, s.scatterTimer)) / 1000000.0);
            std::fprintf(stderr,
                "GPU JPEG sampled predictor modes: p1=%u p2=%u p3=%u p4=%u p5=%u p6=%u p7=%u\n",
                predictorCounts[1], predictorCounts[2], predictorCounts[3],
                predictorCounts[4], predictorCounts[5], predictorCounts[6],
                predictorCounts[7]);
        }
        return consumed;
    }
    std::vector<uint32_t> statuses(tiles.size());
    if (!pl_buf_read(gpu, s.statusReadback, 0, statuses.data(), statusSize) ||
        !std::all_of(statuses.begin(), statuses.end(),
                     [](uint32_t value) { return value == 1u; })) return false;
    std::vector<uint16_t> decoded(sampleCount);
    if (!pl_buf_read(gpu, s.readback, 0, decoded.data(), imageSize)) return false;
    const auto readAt = std::chrono::steady_clock::now();
    std::vector<std::vector<uint16_t>> result(tiles.size());
    for (size_t i = 0; i < tiles.size(); ++i) {
        const size_t first = gpuParse ? seeds[i * 7u + 5u]
                                      : metadata[i * kMetaWords + 6u];
        const size_t count = size_t(tiles[i].width) * tiles[i].height *
                             tiles[i].channels;
        result[i].assign(decoded.begin() + first, decoded.begin() + first + count);
    }
    *pixels = std::move(result);
    if (profile) {
        const double kernelMs = double(pl_timer_query(gpu, s.timer)) / 1000000.0;
        const auto ms = [](auto from, auto to) {
            return std::chrono::duration<double, std::milli>(to - from).count();
        };
        std::fprintf(stderr, "GPU JPEG decode stages: parse=%.3f prepare=%.3f submit=%.3f wait/read=%.3f copy=%.3f ms\n",
                     ms(started, parsedAt), ms(parsedAt, uploadedAt),
                     ms(uploadedAt, dispatchedAt), ms(dispatchedAt, readAt),
                     ms(readAt, std::chrono::steady_clock::now()));
        if (gpuParse)
            std::fprintf(stderr,
                         "GPU JPEG first tile header parsed on GPU; stream_bytes=%u\n",
                         seeds[1]);
        else
            std::fprintf(stderr,
                         "GPU JPEG first tile predictor=%u scan_bytes=%u\n",
                         metadata[5], metadata[1]);
        std::fprintf(stderr, "GPU JPEG unique Huffman lookups=%zu/%zu\n",
                     uniqueLookups.size(), tiles.size());
        std::fprintf(stderr, "GPU JPEG decode kernel %.3f ms\n", kernelMs);
    }
    return true;
}

bool jpegDecodeTilesVulkan(const std::vector<VulkanJpegTile>& tiles,
                           std::vector<std::vector<uint16_t>>& pixels,
                           const VulkanJpegContiguousSource* contiguous) {
    return decodeTiles(tiles, &pixels, 0, 0, 0, 0, nullptr, contiguous);
}

bool jpegDecodeTiledImageVulkan(
    const std::vector<VulkanJpegTile>& tiles, uint32_t width, uint32_t height,
    uint32_t tileWidth, uint32_t tileHeight,
    const std::function<bool(const VulkanDecodedJpegImage&)>& consume,
    const VulkanJpegContiguousSource* contiguous) {
    return consume && decodeTiles(tiles, nullptr, width, height,
                                  tileWidth, tileHeight, &consume, contiguous);
}

bool jpegDecodeTiledImageVulkanRetained(
    const std::vector<VulkanJpegTile>& tiles, uint32_t width, uint32_t height,
    uint32_t tileWidth, uint32_t tileHeight, VulkanDecodedJpegImage& image,
    const VulkanJpegContiguousSource* contiguous) {
    VulkanDecodedJpegImage retained;
    const std::function<bool(const VulkanDecodedJpegImage&)> consume =
        [&](const VulkanDecodedJpegImage& decoded) {
            auto& s = state();
            retained = decoded;
            auto buffer = s.assembled;
            retained.lease = std::shared_ptr<void>(new pl_buf(buffer),
                [gpu = s.gpu](void* pointer) {
                    auto* owned = static_cast<pl_buf*>(pointer);
                    pl_buf_destroy(gpu, owned);
                    delete owned;
                });
            s.assembled = nullptr;
            // This status allocation is reused by the next decode. The output
            // was validated before the callback, so consumers do not need it.
            retained.validationBuffer = nullptr;
            return true;
        };
    if (!decodeTiles(tiles, nullptr, width, height, tileWidth, tileHeight,
                     &consume, contiguous, true)) return false;
    image = std::move(retained);
    return true;
}

bool downloadVulkanDecodedJpegImage(const VulkanDecodedJpegImage& image,
                                  uint16_t* pixels, size_t samples) {
    if (!pixels || !image.gpu || !image.buffer || !image.lease ||
        samples != size_t(image.width) * image.height * image.channels ||
        image.wordsPerRow * 2u != image.width * image.channels) return false;
    auto& context = jpegContext();
    std::lock_guard lock(context.mutex);
    const auto gpu = static_cast<pl_gpu>(image.gpu);
    pl_buf_params params{};
    params.size = samples * sizeof(uint16_t);
    params.host_readable = true;
    params.memory_type = PL_BUF_MEM_HOST;
    pl_buf readback = pl_buf_create(gpu, &params);
    if (!readback) return false;
    pl_buf_copy(gpu, readback, 0, static_cast<pl_buf>(image.buffer), 0, params.size);
    const bool okay = pl_buf_read(gpu, readback, 0, pixels, params.size);
    pl_buf_destroy(gpu, &readback);
    return okay;
}

bool jpegDecodeTiledImageVulkanValidated(
    const std::vector<VulkanJpegTile>& tiles, uint32_t width, uint32_t height,
    uint32_t tileWidth, uint32_t tileHeight,
    const VulkanJpegContiguousSource* contiguous) {
    const std::function<bool(const VulkanDecodedJpegImage&)> consume =
        [](const VulkanDecodedJpegImage&) { return true; };
    return decodeTiles(tiles, nullptr, width, height, tileWidth, tileHeight,
                       &consume, contiguous, true);
}

} // namespace motioncam
#else
namespace motioncam {
bool jpegDecodeTilesVulkan(const std::vector<VulkanJpegTile>&,
                           std::vector<std::vector<uint16_t>>&,
                           const VulkanJpegContiguousSource*) { return false; }
bool jpegDecodeTiledImageVulkan(
    const std::vector<VulkanJpegTile>&, uint32_t, uint32_t, uint32_t, uint32_t,
    const std::function<bool(const VulkanDecodedJpegImage&)>&,
    const VulkanJpegContiguousSource*) { return false; }
bool jpegDecodeTiledImageVulkanRetained(
    const std::vector<VulkanJpegTile>&, uint32_t, uint32_t, uint32_t, uint32_t,
    VulkanDecodedJpegImage&, const VulkanJpegContiguousSource*) { return false; }
bool downloadVulkanDecodedJpegImage(const VulkanDecodedJpegImage&, uint16_t*, size_t) {
    return false;
}
bool jpegDecodeTiledImageVulkanValidated(
    const std::vector<VulkanJpegTile>&, uint32_t, uint32_t, uint32_t, uint32_t,
    const VulkanJpegContiguousSource*) { return false; }
bool jpegDecodeResidentTiledImageVulkan(
    const void*, const void*, const void*, const void*,
    uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t,
    const std::function<bool(const VulkanDecodedJpegImage&)>&) { return false; }
void releaseVulkanJpegDecodeDevice(const void*) {}
}
#endif
