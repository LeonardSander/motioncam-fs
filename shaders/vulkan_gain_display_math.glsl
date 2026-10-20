// Shared by the DNG gain bake and the gallery's fused DirectLog display pass.
// These functions have no storage-layout dependencies, so each output keeps
// its existing buffer packing and dispatch geometry.
float motioncamGainOrIdentity(float gain) {
    return !isnan(gain) && !isinf(gain) && gain > 0.0 ? gain : 1.0;
}

// Keep interpolation in the caller's shader expression. This works for both
// scalar and RGB gain grids without changing either pipeline's dispatch.
#define MOTIONCAM_GAIN_BILERP(upperLeft, upperRight, lowerLeft, lowerRight, fraction) \
    mix(mix((upperLeft), (upperRight), (fraction).x), \
        mix((lowerLeft), (lowerRight), (fraction).x), (fraction).y)

float motioncamDither(uint x, uint y, uint frameSeed, bool hq) {
    uint rowTerm = y * (hq ? 1013904223u : 0x85ebca77u);
    uint frameTerm = (hq ? 0xdeadbeefu : 0u) ^
                     (frameSeed * 0x9e3779b9u);
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
    precise float low = float(seed & 0xffffu) / 65535.0;
    precise float high = float((seed >> 16) & 65535u) / 65535.0;
    precise float sum = low + high;
    precise float centered = sum - 1.0;
    return centered * 0.5;
}

float motioncamLog60(float normalized, uint encodedWhite) {
    return log2(1.0 + 60.0 * normalized) / log2(61.0) *
           float(encodedWhite);
}

vec3 motioncamLog60(vec3 normalized, uint encodedWhite) {
    return log2(vec3(1.0) + 60.0 * normalized) / log2(61.0) *
           float(encodedWhite);
}
