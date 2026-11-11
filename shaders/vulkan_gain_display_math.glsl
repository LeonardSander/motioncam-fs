// Shared by the DNG gain bake and the gallery's fused Native Video display pass.
// These functions have no storage-layout dependencies, so each output keeps
// its existing buffer packing and dispatch geometry.
float rawpreampGainOrIdentity(float gain) {
    return !isnan(gain) && !isinf(gain) && gain > 0.0 ? gain : 1.0;
}

// Keep interpolation in the caller's shader expression. This works for both
// scalar and RGB gain grids without changing either pipeline's dispatch.
#define RAWPREAMP_GAIN_BILERP(upperLeft, upperRight, lowerLeft, lowerRight, fraction) \
    mix(mix((upperLeft), (upperRight), (fraction).x), \
        mix((lowerLeft), (lowerRight), (fraction).x), (fraction).y)

float rawpreampDither(uint x, uint y, uint frameSeed, bool hq) {
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

float rawpreampLog60(float normalized, uint encodedWhite) {
    return log2(1.0 + 60.0 * normalized) / log2(61.0) *
           float(encodedWhite);
}

vec3 rawpreampLog60(vec3 normalized, uint encodedWhite) {
    return log2(vec3(1.0) + 60.0 * normalized) / log2(61.0) *
           float(encodedWhite);
}

// LLOG60: same curve as RawLogTransform.h, with continuous value and first derivative.
float rawpreampLLog60(float x, uint encodedWhite) {
    x = clamp(x, 0.0, 1.0);
    const float a = 0.980913053208808;
    const float m = 0.244314924085355;
    const float join = 0.971335276138720;
    float y;
    if (x >= 0.99) y = x;
    else if (x <= 0.96) y = a * log2(1.0 + 60.0*x) / log2(61.0);
    else {
        float t = x - 0.96;
        y = join + m*t + 12.594751265244078*t*t;
    }
    return y * float(encodedWhite);
}

vec3 rawpreampLLog60(vec3 x, uint encodedWhite) {
    return vec3(rawpreampLLog60(x.r, encodedWhite),
                rawpreampLLog60(x.g, encodedWhite),
                rawpreampLLog60(x.b, encodedWhite));
}
