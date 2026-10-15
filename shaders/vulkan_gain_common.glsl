uint linearizeCode(uint code) {
    if ((p.flags & 16u) == 0u) return code;
    uint pair = linearization.values[code >> 1u];
    return (code & 1u) != 0u ? pair >> 16u : pair & 0xffffu;
}

#ifndef SOURCE_TEXTURE
uint packedInputSample(uint index) {
    uint bits = p.mapShape.w;
    uint start = index * bits;
    uint shift = start & 31u;
    uint word = start >> 5u;
    uint code = src.samples[word] >> shift;
    if (shift + bits > 32u)
        code |= src.samples[word + 1u] << (32u - shift);
    return linearizeCode(code & ((1u << bits) - 1u));
}
#endif

float ditherAt(uint x, uint y) {
    bool hq = (p.flags & 2u) != 0u;
    uint rowTerm = y * (hq ? 1013904223u : 0x85ebca77u);
    uint frameTerm = (hq ? 0xdeadbeefu : 0u) ^
                     (p.ditherSeed * 0x9e3779b9u);
    uint seed;
    if (hq) {
        seed = (x * 1664525u + rowTerm) ^ frameTerm;
        seed ^= seed >> 16;
        seed *= 0x85ebca6bu;
        seed ^= seed >> 13;
        seed *= 0xc2b2ae35u;
        seed ^= seed >> 16;
    } else {
        seed = (x * 0x9e3779b1u) ^ rowTerm ^ frameTerm;
        seed ^= seed >> 16;
        seed *= 0x7feb352du;
        seed ^= seed >> 15;
    }
    return (float(seed & 0xffffu) / 65535.0 +
            float((seed >> 16) & 0xffffu) / 65535.0 - 1.0) * 0.5;
}

uint swapWordBytes(uint packed) {
    return ((packed & 0x000000ffu) << 24) |
           ((packed & 0x0000ff00u) << 8) |
           ((packed & 0x00ff0000u) >> 8) |
           ((packed & 0xff000000u) >> 24);
}
