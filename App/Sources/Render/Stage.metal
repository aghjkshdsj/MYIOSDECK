// SPDX-License-Identifier: GPL-3.0-or-later
// Metal stage shaders: a full-screen procedural scene with a tunable cost, used
// to measure the GPU and the frame pacing path that game frames will use.

#include <metal_stdlib>
using namespace metal;

struct StageUniforms {
    float2 resolution;
    float time;
    uint iterations; // per-pixel work: the GPU load knob
};

struct VOut {
    float4 position [[position]];
    float2 uv;
};

// One oversized triangle covers the screen: no vertex buffer, no seam.
vertex VOut stage_vertex(uint vid [[vertex_id]]) {
    float2 p = float2((vid << 1) & 2, vid & 2);
    VOut o;
    o.position = float4(p * 2.0 - 1.0, 0.0, 1.0);
    o.uv = float2(p.x, 1.0 - p.y);
    return o;
}

static float3 palette(float t) {
    // Steam-ish blues to cyan.
    return 0.5 + 0.5 * cos(6.28318 * (float3(0.55, 0.62, 0.70) * t + float3(0.0, 0.10, 0.20)));
}

fragment half4 stage_fragment(VOut in [[stage_in]], constant StageUniforms &u [[buffer(0)]]) {
    float2 frag = in.uv * u.resolution;
    float2 p = (frag * 2.0 - u.resolution) / min(u.resolution.x, u.resolution.y);
    float3 col = float3(0.0);
    float2 q = p;
    float t = u.time * 0.4;
    // Iterated domain warp: cost scales linearly with u.iterations.
    for (uint i = 0; i < u.iterations; i++) {
        float fi = float(i);
        q = abs(q) / dot(q, q) - float2(0.92 + 0.05 * sin(t + fi * 0.1), 0.58);
        q = float2(q.x * cos(t * 0.3) - q.y * sin(t * 0.3), q.x * sin(t * 0.3) + q.y * cos(t * 0.3));
        col += palette(length(q) * 0.15 + fi * 0.02 + t * 0.1) * 0.012;
    }
    col = col / (1.0 + col); // tone map
    col *= 1.0 - 0.35 * dot(p * 0.6, p * 0.6); // vignette
    return half4(half3(col), 1.0h);
}
