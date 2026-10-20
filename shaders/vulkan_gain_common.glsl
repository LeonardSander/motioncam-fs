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
    return motioncamDither(x, y, p.ditherSeed, (p.flags & 2u) != 0u);
}

uint swapWordBytes(uint packed) {
    return ((packed & 0x000000ffu) << 24) |
           ((packed & 0x0000ff00u) << 8) |
           ((packed & 0x00ff0000u) >> 8) |
           ((packed & 0xff000000u) >> 24);
}
