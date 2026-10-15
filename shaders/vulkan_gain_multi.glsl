#ifdef MULTI_MAP
uint multiCfaColor(uint phase) {
    return (p.mapShape.y >> (phase * 2u)) & 3u;
}

float multiMapValue(uint mapIndex, uvec2 point, uint channel, bool rgb) {
    uvec4 shape = gainMaps.shape[mapIndex];
    uint base = shape.w + (point.y * shape.x + point.x) * shape.z;
    if (shape.z == 1u) return gainMaps.gains[base];
    if (shape.z == 3u) {
        uint color = rgb ? channel : multiCfaColor(channel);
        return gainMaps.gains[base + color];
    }
    if (!rgb) return gainMaps.gains[base + channel];
    float sum = 0.0;
    uint count = 0u;
    for (uint phase = 0u; phase < 4u; ++phase) {
        if (multiCfaColor(phase) == channel) {
            sum += gainMaps.gains[base + phase];
            ++count;
        }
    }
    return count > 0u ? sum / float(count) : 1.0;
}

float multiGainAt(uint x, uint y, uint channel, bool rgb) {
    float product = 1.0;
    for (uint mapIndex = 0u; mapIndex < p.mapShape.x; ++mapIndex) {
        uvec4 bounds = gainMaps.bounds[mapIndex];
        if (x < bounds.x || y < bounds.y || x >= bounds.z || y >= bounds.w)
            continue;
        uvec4 shape = gainMaps.shape[mapIndex];
        uvec4 pitch = gainMaps.pitch[mapIndex];
        if (shape.z == 1u &&
            ((y - bounds.y) % pitch.x != 0u ||
             (x - bounds.x) % pitch.y != 0u))
            continue;
        vec4 axes = gainMaps.axes[mapIndex];
        vec2 normalized = (vec2(x, y) + vec2(0.5)) / vec2(p.dimensions.xy);
        vec2 grid = (normalized - axes.xy) / axes.zw;
        vec2 base = floor(grid);
        vec2 fraction = clamp(grid - base, 0.0, 1.0);
        uvec2 a = uvec2(clamp(base, vec2(0), vec2(shape.xy - uvec2(1))));
        uvec2 b = min(a + uvec2(1), shape.xy - uvec2(1));
        float upper = mix(multiMapValue(mapIndex, a, channel, rgb),
                          multiMapValue(mapIndex, uvec2(b.x, a.y), channel, rgb),
                          fraction.x);
        float lower = mix(multiMapValue(mapIndex, uvec2(a.x, b.y), channel, rgb),
                          multiMapValue(mapIndex, b, channel, rgb), fraction.x);
        float gain = mix(upper, lower, fraction.y);
        product *= !isnan(gain) && !isinf(gain) && gain > 0.0 ? gain : 1.0;
    }
    return product;
}
#endif
