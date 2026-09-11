// SPDX-FileCopyrightText: Copyright 2026 qwertypower (DEVAR Entertainment LLC)
// SPDX-License-Identifier: GPL-3.0-or-later

#include "stdafx.h"
#include "dis_shaders.hpp"

#include "Emu/RSX/Program/GLSLTypes.h"
#include "Emu/RSX/Program/SPIRVCommon.h"

#include <mutex>

namespace dis
{
	namespace
	{
		const char* const s_dis_source_luma_r16 = R"GLSL(#version 450


precision highp float;
precision highp int;

layout(local_size_x = 8, local_size_y = 8, local_size_z = 1) in;

layout(set = 0, binding = 0) uniform sampler2D colorMap;
layout(set = 0, binding = 5, r16f) writeonly uniform image2D lumaMap;

float uluminance(vec3 c) {
    return (0.299 * c.x + 0.587 * c.y + 0.114 * c.z) * 255.0;
}

void main() {
    ivec2 pix = ivec2(gl_GlobalInvocationID.xy);
    ivec2 size = textureSize(colorMap, 0);
    if (pix.x >= size.x || pix.y >= size.y) return;

    imageStore(lumaMap, pix, vec4(uluminance(texelFetch(colorMap, pix, 0).xyz), 0.0, 0.0, 1.0));
}
)GLSL";

		const char* const s_dis_source_luma_r32 = R"GLSL(#version 450


precision highp float;
precision highp int;

layout(local_size_x = 8, local_size_y = 8, local_size_z = 1) in;

layout(set = 0, binding = 0) uniform sampler2D colorMap;
layout(set = 0, binding = 5, r32f) writeonly uniform image2D lumaMap;

float uluminance(vec3 c) {
    return (0.299 * c.x + 0.587 * c.y + 0.114 * c.z) * 255.0;
}

void main() {
    ivec2 pix = ivec2(gl_GlobalInvocationID.xy);
    ivec2 size = textureSize(colorMap, 0);
    if (pix.x >= size.x || pix.y >= size.y) return;

    imageStore(lumaMap, pix, vec4(uluminance(texelFetch(colorMap, pix, 0).xyz), 0.0, 0.0, 1.0));
}
)GLSL";

		const char* const s_dis_source_gradient = R"GLSL(#version 450


precision highp float;
precision highp int;

layout(local_size_x = 8, local_size_y = 8, local_size_z = 1) in;

layout(set = 0, binding = 0) uniform sampler2D lumaMap;
layout(set = 0, binding = 5, rg32f) uniform image2D gradientMap;

layout(push_constant) uniform PC {
    float lesser;
    float upper;
    float normVal;
} pc;

void main() {
    ivec2 pix = ivec2(gl_GlobalInvocationID.xy);
    ivec2 sz = imageSize(gradientMap);
    if (pix.x >= sz.x || pix.y >= sz.y) return;

    ivec2 mx = sz - 1;
    float a00 = texelFetch(lumaMap, clamp(pix + ivec2(-1, -1), ivec2(0), mx), 0).x;
    float a10 = texelFetch(lumaMap, clamp(pix + ivec2(0, -1), ivec2(0), mx), 0).x;
    float a20 = texelFetch(lumaMap, clamp(pix + ivec2(1, -1), ivec2(0), mx), 0).x;
    float a01 = texelFetch(lumaMap, clamp(pix + ivec2(-1, 0), ivec2(0), mx), 0).x;
    float a21 = texelFetch(lumaMap, clamp(pix + ivec2(1, 0), ivec2(0), mx), 0).x;
    float a02 = texelFetch(lumaMap, clamp(pix + ivec2(-1, 1), ivec2(0), mx), 0).x;
    float a12 = texelFetch(lumaMap, clamp(pix + ivec2(0, 1), ivec2(0), mx), 0).x;
    float a22 = texelFetch(lumaMap, clamp(pix + ivec2(1, 1), ivec2(0), mx), 0).x;

    float sx = (pc.lesser * a00 + pc.upper * a01 + pc.lesser * a02)
             - (pc.lesser * a20 + pc.upper * a21 + pc.lesser * a22);
    float sy = (pc.lesser * a00 + pc.upper * a10 + pc.lesser * a20)
             - (pc.lesser * a02 + pc.upper * a12 + pc.lesser * a22);

    imageStore(gradientMap, pix, vec4(sx * pc.normVal, sy * pc.normVal, 0.0, 0.0));
}
)GLSL";

		const char* const s_dis_source_inverse_search = R"GLSL(#version 450


precision highp float;
precision highp int;

#define DIS_INVERSE_ITERS 8

#define DIS_MAX_MATCH_RMS 36.0

layout(local_size_x = 8, local_size_y = 8, local_size_z = 1) in;

layout(set = 0, binding = 0) uniform sampler2D lastLumaMap;
layout(set = 0, binding = 1) uniform sampler2D nextLumaMap;
layout(set = 0, binding = 2) uniform sampler2D lastGradientMap;
layout(set = 0, binding = 3) uniform sampler2D flowMap;
layout(set = 0, binding = 4) uniform sampler2D lastFlowMap;
layout(set = 0, binding = 5, rgba32f) uniform image2D sparseFlowMap;

layout(push_constant) uniform PC {
    int level;
    int coarseLevel;
} pc;

float uluminance(vec3 c) {
    return (0.299 * c.x + 0.587 * c.y + 0.114 * c.z) * 255.0;
}

float myDeterminant(mat2 m) {
    return m[0][0] * m[1][1] - m[0][1] * m[1][0];
}

mat2 myInverse(mat2 m) {
    float det = myDeterminant(m);
    if (abs(det) < 1e-10) return mat2(0.0);
    return mat2(m[1][1], -m[0][1], -m[1][0], m[0][0]) / det;
}

void main() {
    const float patchSize = 8.0;

    ivec2 pixSparse = ivec2(gl_GlobalInvocationID.xy);
    ivec2 sparseSize = imageSize(sparseFlowMap);
    if (pixSparse.x >= sparseSize.x || pixSparse.y >= sparseSize.y) return;

    ivec2 pix = pixSparse * 3;
    ivec2 denseSize = textureSize(lastLumaMap, 0);
    ivec2 denseMax = denseSize - 1;

    float lastImageData[64];
    vec2 gradData[64];

    vec2 gradSum = vec2(0.0);
    mat2 H = mat2(0.0);

    for (int i = 0; i < 8; i++) {
        for (int j = 0; j < 8; j++) {
            ivec2 q = clamp(pix + ivec2(i, j), ivec2(0), denseMax);
            gradData[i * 8 + j] = -texelFetch(lastGradientMap, q, 0).xy;

            H[0][0] += gradData[i * 8 + j].x * gradData[i * 8 + j].x;
            H[1][1] += gradData[i * 8 + j].y * gradData[i * 8 + j].y;
            H[0][1] += gradData[i * 8 + j].x * gradData[i * 8 + j].y;

            lastImageData[i * 8 + j] =
                texelFetch(lastLumaMap, q, 0).x;

            gradSum += gradData[i * 8 + j];
        }
    }

    H[1][0] = H[0][1];
    if (myDeterminant(H) < 1e-6) {
        H[0][0] += 1e-6;
        H[1][1] += 1e-6;
    }
    mat2 H_inv = myInverse(H);

    vec2 flow;
    if (pc.level == pc.coarseLevel) {
        flow = vec2(0.0);
    } else {
        ivec2 fmMax = textureSize(flowMap, 0) - 1;
        vec4 cf = texelFetch(flowMap, clamp(ivec2(pix / 2) + 2, ivec2(0), fmMax), 0);
        flow = cf.xy * vec2(denseSize);
        if (any(isnan(flow)) || any(isinf(flow))) flow = vec2(0.0);
    }
    vec2 initialFlow = flow;

    vec2 invImageSize = 1.0 / vec2(denseSize);
    const float N = 64.0;

    const float N_INV = 1.0 / N;
    float prevSSD = 1e10;
    for (int iter = 0; iter < DIS_INVERSE_ITERS; iter++) {
        vec2 warpOrigin = clamp(vec2(pix) + flow, vec2(0.0), vec2(denseSize) - patchSize);
        float sd = 0.0;
        float sd2 = 0.0;
        vec2 sIg = vec2(0.0);

        for (int i = 0; i < 8; i++) {
            for (int j = 0; j < 8; j++) {
                vec2 tc = (warpOrigin + vec2(i, j) + 0.5) * invImageSize;
                float warped = textureLod(nextLumaMap, tc, 0.0).x;
                float diff = warped - lastImageData[i * 8 + j];
                sd += diff;
                sd2 += diff * diff;
                sIg += gradData[i * 8 + j] * diff;
            }
        }

        vec2 dU = sIg - sd * gradSum / N;
        float SSD = sd2 - sd * sd / N;

        flow -= H_inv * dU;

        if (SSD >= prevSSD) break;
        prevSSD = SSD;
    }

    vec2 wantOrigin = vec2(pix) + flow;
    vec2 maxOrigin = vec2(denseSize) - patchSize;
    bool clamped = any(lessThan(wantOrigin, vec2(-0.5)))
                || any(greaterThan(wantOrigin, maxOrigin + 0.5));

    bool unmatched = prevSSD * N_INV > DIS_MAX_MATCH_RMS * DIS_MAX_MATCH_RMS;

    if (any(isnan(flow)) || any(isinf(flow)) || clamped || unmatched ||
        length(flow - initialFlow) > patchSize) {
        flow = initialFlow;
    }

    flow *= invImageSize;
    imageStore(sparseFlowMap, pixSparse, vec4(flow, -1.0, 1.0));
}
)GLSL";

		const char* const s_dis_source_propagate = R"GLSL(#version 450


precision highp float;
precision highp int;

layout(local_size_x = 8, local_size_y = 8, local_size_z = 1) in;

layout(set = 0, binding = 0) uniform sampler2D lastLumaMap;
layout(set = 0, binding = 1) uniform sampler2D nextLumaMap;
layout(set = 0, binding = 2) uniform sampler2D flowIn;
layout(set = 0, binding = 5, rgba32f) uniform image2D flowOut;

layout(push_constant) uniform PC {
    int dist;
} pc;

void main() {
    ivec2 s = ivec2(gl_GlobalInvocationID.xy);
    ivec2 mx = textureSize(flowIn, 0) - 1;
    if (s.x > mx.x || s.y > mx.y) return;

    vec4 ownIn = texelFetch(flowIn, s, 0);
    vec2 own = ownIn.xy;
    float ownSsd = ownIn.z;
    bool needOwn = !(ownSsd >= 0.0);

    const ivec2 dirs[4] =
        ivec2[4](ivec2(-1, 0), ivec2(1, 0), ivec2(0, -1), ivec2(0, 1));
    vec2 cand[4];
    int candCount = 0;
    for (int i = 0; i < 4; i++) {
        ivec2 q = clamp(s + dirs[i] * pc.dist, ivec2(0), mx);
        if (q == s) continue;
        cand[candCount++] = texelFetch(flowIn, q, 0).xy;
    }
    if (candCount == 0) {
        imageStore(flowOut, s, vec4(own, ownSsd, 1.0));
        return;
    }

    ivec2 org = s * 3;
    ivec2 denseSize = textureSize(lastLumaMap, 0);
    ivec2 denseMax = denseSize - 1;
    vec2 invImageSize = 1.0 / vec2(denseSize);

    float refLum[64];
    for (int dy = 0; dy < 8; dy++) {
        for (int dx = 0; dx < 8; dx++) {
            ivec2 p = clamp(org + ivec2(dx, dy), ivec2(0), denseMax);
            refLum[dy * 8 + dx] = texelFetch(lastLumaMap, p, 0).x;
        }
    }

    float sd[5];
    float sd2[5];
    for (int i = 0; i < 5; i++) {
        sd[i] = 0.0;
        sd2[i] = 0.0;
    }

    for (int dy = 0; dy < 8; dy++) {
        for (int dx = 0; dx < 8; dx++) {
            int i = dy * 8 + dx;
            vec2 base = (vec2(org) + vec2(dx, dy) + 0.5) * invImageSize;
            float r = refLum[i];

            if (needOwn) {
                float d0 = textureLod(nextLumaMap, base + own, 0.0).x - r;
                sd[0] += d0;
                sd2[0] += d0 * d0;
            }

            for (int c = 0; c < candCount; c++) {
                float dc = textureLod(nextLumaMap, base + cand[c], 0.0).x - r;
                sd[c + 1] += dc;
                sd2[c + 1] += dc * dc;
            }
        }
    }

    vec2 best = own;
    float bestSsd = needOwn ? (sd2[0] - sd[0] * sd[0] / 64.0) : ownSsd;
    for (int c = 0; c < candCount; c++) {
        float ssd = sd2[c + 1] - sd[c + 1] * sd[c + 1] / 64.0;
        if (ssd < bestSsd) {
            bestSsd = ssd;
            best = cand[c];
        }
    }

    imageStore(flowOut, s, vec4(best, bestSsd, 1.0));
}
)GLSL";

		const char* const s_dis_source_densify = R"GLSL(#version 450


precision highp float;
precision highp int;

layout(local_size_x = 8, local_size_y = 8, local_size_z = 1) in;

layout(set = 0, binding = 0) uniform sampler2D sparseFlowMap;
layout(set = 0, binding = 1) uniform sampler2D lastImage;
layout(set = 0, binding = 2) uniform sampler2D nextImage;
layout(set = 0, binding = 5, rg32f) uniform image2D denseFlowMap;

float uluminance(vec3 c) {
    return (0.299 * c.x + 0.587 * c.y + 0.114 * c.z) * 255.0;
}

void main() {
    ivec2 pix = ivec2(gl_GlobalInvocationID.xy);
    ivec2 denseSize = imageSize(denseFlowMap);
    if (pix.x >= denseSize.x || pix.y >= denseSize.y) return;

    ivec2 sparseSize = textureSize(sparseFlowMap, 0);
    if (sparseSize.x <= 0 || sparseSize.y <= 0) {
        imageStore(denseFlowMap, pix, vec4(0.0, 0.0, 0.0, 1.0));
        return;
    }

    vec2 invDenseSize = 1.0 / vec2(denseSize);
    vec2 uv = (vec2(pix) + 0.5) * invDenseSize;
    float lastLum = textureLod(lastImage, uv, 0.0).x;

    vec2 acc = vec2(0.0);
    float accW = 0.0;

    ivec2 s0 = ivec2(pix.x / 3, pix.y / 3) - 2;
    for (int dy = 0; dy <= 2; dy++) {
        for (int dx = 0; dx <= 2; dx++) {
            ivec2 s = s0 + ivec2(dx, dy);
            if (s.x < 0 || s.y < 0 || s.x >= sparseSize.x || s.y >= sparseSize.y) continue;
            vec4 flow = texelFetch(sparseFlowMap, s, 0);
            float diff =
                textureLod(nextImage, uv + flow.xy, 0.0).x - lastLum;
            float w = 1.0 / max(abs(diff), 1.0);
            acc += flow.xy * w;
            accW += w;
        }
    }

    vec2 denseFlow = accW > 0.0 ? acc / accW : vec2(0.0);
    imageStore(denseFlowMap, pix, vec4(denseFlow, 0.0, 1.0));
}
)GLSL";

		const char* const s_dis_source_interpolate = R"GLSL(#version 450


precision highp float;
precision highp int;

layout(local_size_x = 8, local_size_y = 8, local_size_z = 1) in;

layout(set = 0, binding = 0) uniform sampler2D prevColor;
layout(set = 0, binding = 1) uniform sampler2D nextColor;
layout(set = 0, binding = 2) uniform sampler2D flowTex;
layout(set = 0, binding = 5, rgba8) uniform image2D outImage;

layout(push_constant) uniform PC {
    float t;
    int debugMode;
} pc;

layout(constant_id = 0) const int manualFlowFilter = 0;

vec2 sampleFlow(vec2 uv) {
    if (manualFlowFilter == 0) return textureLod(flowTex, uv, 0.0).xy;

    vec2 sz = vec2(textureSize(flowTex, 0));
    vec2 p = uv * sz - 0.5;
    vec2 frac = fract(p);
    ivec2 i0 = ivec2(floor(p));
    ivec2 mx = ivec2(sz) - 1;

    vec2 a = texelFetch(flowTex, clamp(i0,                ivec2(0), mx), 0).xy;
    vec2 b = texelFetch(flowTex, clamp(i0 + ivec2(1, 0), ivec2(0), mx), 0).xy;
    vec2 c = texelFetch(flowTex, clamp(i0 + ivec2(0, 1), ivec2(0), mx), 0).xy;
    vec2 e = texelFetch(flowTex, clamp(i0 + ivec2(1, 1), ivec2(0), mx), 0).xy;
    return mix(mix(a, b, frac.x), mix(c, e, frac.x), frac.y);
}

vec3 hsv2rgb(vec3 c) {
    vec4 K = vec4(1.0, 2.0 / 3.0, 1.0 / 3.0, 3.0);
    vec3 p = abs(fract(c.xxx + K.xyz) * 6.0 - K.www);
    return c.z * mix(K.xxx, clamp(p - K.xxx, 0.0, 1.0), c.y);
}

void main() {
    ivec2 pix = ivec2(gl_GlobalInvocationID.xy);
    ivec2 size = imageSize(outImage);
    if (pix.x >= size.x || pix.y >= size.y) return;

    vec2 uv = (vec2(pix) + 0.5) / vec2(size);
    vec2 texel = 1.0 / vec2(size);

    const float GUARD_PX = 20.0;
    vec2 guard = GUARD_PX * texel;

    vec2 dEdge = min(uv, 1.0 - uv);
    vec2 ramp = clamp((dEdge - guard) / max(guard, texel), 0.0, 1.0);
    vec2 eased = ramp * ramp * (3.0 - 2.0 * ramp);
    float edgeMix = min(eased.x, eased.y);

    vec2 f = sampleFlow(uv);
    if (edgeMix < 1.0) {
        vec2 fInner = sampleFlow(clamp(uv, guard, 1.0 - guard));
        f = mix(fInner, f, edgeMix);
    }

    if (pc.debugMode != 0) {
        float m = length(f) * float(size.x) / 16.0;
        float hue = atan(f.y, f.x) / 6.2831853 + 0.5;
        vec3 fc = hsv2rgb(vec3(hue, clamp(m, 0.0, 1.0), min(1.0, 0.15 + m)));
        imageStore(outImage, pix, vec4(fc, 1.0));
        return;
    }

    vec2 uv0 = uv - pc.t * f;
    vec2 uv1 = uv + (1.0 - pc.t) * f;

    vec3 c0 = textureLod(prevColor, clamp(uv0, vec2(0.0), vec2(1.0)), 0.0).xyz;
    vec3 c1 = textureLod(nextColor, clamp(uv1, vec2(0.0), vec2(1.0)), 0.0).xyz;

    vec3 single = pc.t < 0.5 ? c0 : c1;

    const float FEATHER_PX = 8.0;
    vec2 feather = FEATHER_PX / vec2(size);
    vec2 e0 = max(max(-uv0, uv0 - vec2(1.0)), vec2(0.0)) / feather;
    vec2 e1 = max(max(-uv1, uv1 - vec2(1.0)), vec2(0.0)) / feather;
    float out0 = clamp(max(e0.x, e0.y), 0.0, 1.0);
    float out1 = clamp(max(e1.x, e1.y), 0.0, 1.0);

    float w0 = (1.0 - pc.t) * (1.0 - out0);
    float w1 = pc.t * (1.0 - out1);
    float wsum = w0 + w1;

    vec3 result = wsum > 1e-4
        ? (c0 * w0 + c1 * w1) / wsum
        : (out0 <= out1 ? c0 : c1);

    float occl = smoothstep(0.10, 0.40, dot(abs(c0 - c1), vec3(1.0)));
    result = mix(result, single, occl);

    imageStore(outImage, pix, vec4(result, 1.0));
}
)GLSL";

		const char* const s_dis_source_vr_prep = R"GLSL(#version 450


precision highp float;
precision highp int;

layout(local_size_x = 8, local_size_y = 8, local_size_z = 1) in;

layout(set = 0, binding = 0) uniform sampler2D prevColor;
layout(set = 0, binding = 1) uniform sampler2D nextColor;
layout(set = 0, binding = 2) uniform sampler2D flowDense;
layout(set = 0, binding = 8, rg32f) uniform image2D prep;
layout(set = 0, binding = 9, rg32f) uniform image2D dW;

float uluminance(vec3 c) {
    return (0.299 * c.x + 0.587 * c.y + 0.114 * c.z) * 255.0;
}

void main() {
    ivec2 p = ivec2(gl_GlobalInvocationID.xy);
    ivec2 sz = imageSize(prep);
    if (p.x >= sz.x || p.y >= sz.y) return;

    vec2 invSize = 1.0 / vec2(sz);
    float i0 = uluminance(texelFetch(prevColor, p, 0).xyz);
    vec2 fn = texelFetch(flowDense, p, 0).xy;
    float w = uluminance(textureLod(nextColor, (vec2(p) + 0.5) * invSize + fn, 0.0).xyz);

    imageStore(prep, p, vec4(0.5 * (i0 + w), w - i0, 0.0, 0.0));
    imageStore(dW, p, vec4(0.0));
}
)GLSL";

		const char* const s_dis_source_vr_d1 = R"GLSL(#version 450


precision highp float;
precision highp int;

layout(local_size_x = 8, local_size_y = 8, local_size_z = 1) in;

layout(set = 0, binding = 0) uniform sampler2D prep;
layout(set = 0, binding = 8, rgba32f) uniform image2D d1;

void main() {
    ivec2 p = ivec2(gl_GlobalInvocationID.xy);
    ivec2 sz = imageSize(d1);
    if (p.x >= sz.x || p.y >= sz.y) return;
    ivec2 mx = sz - 1;

    vec2 xp = texelFetch(prep, clamp(p + ivec2(1, 0), ivec2(0), mx), 0).xy;
    vec2 xm = texelFetch(prep, clamp(p + ivec2(-1, 0), ivec2(0), mx), 0).xy;
    vec2 yp = texelFetch(prep, clamp(p + ivec2(0, 1), ivec2(0), mx), 0).xy;
    vec2 ym = texelFetch(prep, clamp(p + ivec2(0, -1), ivec2(0), mx), 0).xy;

    imageStore(d1, p, vec4(xp.x - xm.x, yp.x - ym.x, xp.y - xm.y, yp.y - ym.y));
}
)GLSL";

		const char* const s_dis_source_vr_d2 = R"GLSL(#version 450


precision highp float;
precision highp int;

layout(local_size_x = 8, local_size_y = 8, local_size_z = 1) in;

layout(set = 0, binding = 0) uniform sampler2D d1;
layout(set = 0, binding = 8, rgba32f) uniform image2D d2;

void main() {
    ivec2 p = ivec2(gl_GlobalInvocationID.xy);
    ivec2 sz = imageSize(d2);
    if (p.x >= sz.x || p.y >= sz.y) return;
    ivec2 mx = sz - 1;

    vec2 xp = texelFetch(d1, clamp(p + ivec2(1, 0), ivec2(0), mx), 0).xy;
    vec2 xm = texelFetch(d1, clamp(p + ivec2(-1, 0), ivec2(0), mx), 0).xy;
    vec2 yp = texelFetch(d1, clamp(p + ivec2(0, 1), ivec2(0), mx), 0).xy;
    vec2 ym = texelFetch(d1, clamp(p + ivec2(0, -1), ivec2(0), mx), 0).xy;

    imageStore(d2, p, vec4(xp.x - xm.x, yp.x - ym.x, yp.y - ym.y, 0.0));
}
)GLSL";

		const char* const s_dis_source_vr_w = R"GLSL(#version 450


precision highp float;
precision highp int;

layout(local_size_x = 8, local_size_y = 8, local_size_z = 1) in;

layout(set = 0, binding = 0) uniform sampler2D flowDense;
layout(set = 0, binding = 1) uniform sampler2D dW;
layout(set = 0, binding = 8, r32f) uniform image2D wt;

layout(push_constant) uniform PC {
    float alpha2;
    float eps2;
} pc;

void main() {
    ivec2 p = ivec2(gl_GlobalInvocationID.xy);
    ivec2 sz = imageSize(wt);
    if (p.x >= sz.x || p.y >= sz.y) return;
    ivec2 mx = sz - 1;
    vec2 uSize = vec2(sz);

    vec2 c  = texelFetch(flowDense, p, 0).xy * uSize + texelFetch(dW, p, 0).xy;
    vec2 dx = texelFetch(flowDense, ivec2(min(p.x + 1, mx.x), p.y), 0).xy * uSize
            + texelFetch(dW, ivec2(min(p.x + 1, mx.x), p.y), 0).xy - c;
    vec2 dy = texelFetch(flowDense, ivec2(p.x, min(p.y + 1, mx.y)), 0).xy * uSize
            + texelFetch(dW, ivec2(p.x, min(p.y + 1, mx.y)), 0).xy - c;

    float val = pc.alpha2 / sqrt(dot(dx, dx) + dot(dy, dy) + pc.eps2);
    imageStore(wt, p, vec4(val, 0.0, 0.0, 0.0));
}
)GLSL";

		const char* const s_dis_source_vr_coef = R"GLSL(#version 450


precision highp float;
precision highp int;

layout(local_size_x = 8, local_size_y = 8, local_size_z = 1) in;

layout(set = 0, binding = 0) uniform sampler2D prep;
layout(set = 0, binding = 1) uniform sampler2D d1;
layout(set = 0, binding = 2) uniform sampler2D d2;
layout(set = 0, binding = 3) uniform sampler2D dW;
layout(set = 0, binding = 4) uniform sampler2D flowDense;
layout(set = 0, binding = 5) uniform sampler2D wt;
layout(set = 0, binding = 8, rgba32f) uniform image2D A;
layout(set = 0, binding = 9, rg32f) uniform image2D B;

layout(push_constant) uniform PC {
    float delta2;
    float gamma2;
    float zeta2;
    float eps2;
} pc;

void main() {
    ivec2 p = ivec2(gl_GlobalInvocationID.xy);
    ivec2 sz = imageSize(A);
    if (p.x >= sz.x || p.y >= sz.y) return;
    ivec2 mx = sz - 1;
    vec2 uSize = vec2(sz);

    vec4 d1v = texelFetch(d1, p, 0);
    vec4 d2v = texelFetch(d2, p, 0);
    float Iz = texelFetch(prep, p, 0).y;
    vec2 dWp = texelFetch(dW, p, 0).xy;
    float Ix = d1v.x, Iy = d1v.y, Ixz = d1v.z, Iyz = d1v.w;
    float Ixx = d2v.x, Ixy = d2v.y, Iyy = d2v.z;
    float dU = dWp.x, dV = dWp.y;

    float dn = Ix * Ix + Iy * Iy + pc.zeta2;
    float Ik1z = Iz + Ix * dU + Iy * dV;
    float w = (pc.delta2 / sqrt(Ik1z * Ik1z / dn + pc.eps2)) / dn;
    float a11 = w * Ix * Ix + pc.zeta2;
    float a12 = w * Ix * Iy;
    float a22 = w * Iy * Iy + pc.zeta2;
    float b1 = -w * Iz * Ix;
    float b2 = -w * Iz * Iy;

    float dnx = Ixx * Ixx + Ixy * Ixy + pc.zeta2;
    float dny = Iyy * Iyy + Ixy * Ixy + pc.zeta2;
    float Ik1zx = Ixz + Ixx * dU + Ixy * dV;
    float Ik1zy = Iyz + Ixy * dU + Iyy * dV;
    w = pc.gamma2 / sqrt(Ik1zx * Ik1zx / dnx + Ik1zy * Ik1zy / dny + pc.eps2);
    a11 += w * (Ixx * Ixx / dnx + Ixy * Ixy / dny);
    a12 += w * (Ixx * Ixy / dnx + Ixy * Iyy / dny);
    a22 += w * (Ixy * Ixy / dnx + Iyy * Iyy / dny);
    b1 -= w * (Ixx * Ixz / dnx + Ixy * Iyz / dny);
    b2 -= w * (Ixy * Ixz / dnx + Iyy * Iyz / dny);

    float wc = texelFetch(wt, p, 0).r;
    float wl = texelFetch(wt, ivec2(max(p.x - 1, 0), p.y), 0).r;
    float wu = texelFetch(wt, ivec2(p.x, max(p.y - 1, 0)), 0).r;
    vec2 W  = texelFetch(flowDense, p, 0).xy * uSize;
    vec2 Wr = texelFetch(flowDense, ivec2(min(p.x + 1, mx.x), p.y), 0).xy * uSize;
    vec2 Wl = texelFetch(flowDense, ivec2(max(p.x - 1, 0), p.y), 0).xy * uSize;
    vec2 Wd = texelFetch(flowDense, ivec2(p.x, min(p.y + 1, mx.y)), 0).xy * uSize;
    vec2 Wu = texelFetch(flowDense, ivec2(p.x, max(p.y - 1, 0)), 0).xy * uSize;

    float addA = 0.0;
    vec2 addB = vec2(0.0);
    if (p.x < mx.x) { addA += wc; addB += wc * (Wr - W); }
    if (p.x > 0)    { addA += wl; addB -= wl * (W - Wl); }
    if (p.y < mx.y) { addA += wc; addB += wc * (Wd - W); }
    if (p.y > 0)    { addA += wu; addB -= wu * (W - Wu); }

    imageStore(A, p, vec4(a11 + addA, a12, a22 + addA, 0.0));
    imageStore(B, p, vec4(vec2(b1, b2) + addB, 0.0, 0.0));
}
)GLSL";

		const char* const s_dis_source_vr_sor = R"GLSL(#version 450


precision highp float;
precision highp int;

layout(local_size_x = 8, local_size_y = 8, local_size_z = 1) in;

layout(set = 0, binding = 0) uniform sampler2D A;
layout(set = 0, binding = 1) uniform sampler2D B;
layout(set = 0, binding = 2) uniform sampler2D wt;
layout(set = 0, binding = 3) uniform sampler2D dWin;
layout(set = 0, binding = 8, rg32f) uniform image2D dWout;

layout(push_constant) uniform PC {
    float omega;
    int parity;
} pc;

void main() {
    ivec2 p = ivec2(gl_GlobalInvocationID.xy);
    ivec2 sz = imageSize(dWout);
    if (p.x >= sz.x || p.y >= sz.y) return;
    ivec2 mx = sz - 1;

    vec2 d = texelFetch(dWin, p, 0).xy;
    if (((p.x + p.y) & 1) != pc.parity) {
        imageStore(dWout, p, vec4(d, 0.0, 0.0));
        return;
    }

    float wc = texelFetch(wt, p, 0).r;
    float wl = p.x > 0    ? texelFetch(wt, ivec2(p.x - 1, p.y), 0).r : 0.0;
    float wu = p.y > 0    ? texelFetch(wt, ivec2(p.x, p.y - 1), 0).r : 0.0;
    float wr = p.x < mx.x ? wc : 0.0;
    float wd = p.y < mx.y ? wc : 0.0;

    vec2 dl = texelFetch(dWin, ivec2(max(p.x - 1, 0), p.y), 0).xy;
    vec2 dr = texelFetch(dWin, ivec2(min(p.x + 1, mx.x), p.y), 0).xy;
    vec2 du = texelFetch(dWin, ivec2(p.x, max(p.y - 1, 0)), 0).xy;
    vec2 dd = texelFetch(dWin, ivec2(p.x, min(p.y + 1, mx.y)), 0).xy;

    vec2 sigma = wl * dl + wr * dr + wu * du + wd * dd;
    vec4 A = texelFetch(A, p, 0);
    vec2 Bv = texelFetch(B, p, 0).xy;

    float nu = d.x + pc.omega * ((sigma.x + Bv.x - d.y * A.y) / A.x - d.x);
    float nv = d.y + pc.omega * ((sigma.y + Bv.y - nu * A.y) / A.z - d.y);
    imageStore(dWout, p, vec4(nu, nv, 0.0, 0.0));
}
)GLSL";

		const char* const s_dis_source_vr_add = R"GLSL(#version 450


precision highp float;
precision highp int;

layout(local_size_x = 8, local_size_y = 8, local_size_z = 1) in;

layout(set = 0, binding = 0) uniform sampler2D flowDense;
layout(set = 0, binding = 1) uniform sampler2D dW;
layout(set = 0, binding = 8, rg32f) uniform image2D flowRefined;

void main() {
    ivec2 p = ivec2(gl_GlobalInvocationID.xy);
    ivec2 sz = imageSize(flowRefined);
    if (p.x >= sz.x || p.y >= sz.y) return;
    vec2 uSize = vec2(sz);
    vec2 invSize = 1.0 / uSize;

    vec2 W = texelFetch(flowDense, p, 0).xy * uSize;
    vec2 f = W + texelFetch(dW, p, 0).xy;
    float lim = 4.0 * uSize.x;
    bvec2 ok = lessThan(abs(f), vec2(lim));
    vec2 fr = vec2(ok.x ? f.x : W.x, ok.y ? f.y : W.y);

    imageStore(flowRefined, p, vec4(fr * invSize, 0.0, 1.0));
}
)GLSL";

		constexpr usz dis_shader_count = 14;

		const char* const s_dis_sources[dis_shader_count] =
		{
			s_dis_source_luma_r16,
			s_dis_source_luma_r32,
			s_dis_source_gradient,
			s_dis_source_inverse_search,
			s_dis_source_propagate,
			s_dis_source_densify,
			s_dis_source_interpolate,
			s_dis_source_vr_prep,
			s_dis_source_vr_d1,
			s_dis_source_vr_d2,
			s_dis_source_vr_w,
			s_dis_source_vr_coef,
			s_dis_source_vr_sor,
			s_dis_source_vr_add,
		};

		const char* const s_dis_names[dis_shader_count] =
		{
			"luma_r16",
			"luma_r32",
			"gradient",
			"inverse_search",
			"propagate",
			"densify",
			"interpolate",
			"vr_prep",
			"vr_d1",
			"vr_d2",
			"vr_w",
			"vr_coef",
			"vr_sor",
			"vr_add",
		};

		enum class dis_shader_state : u8
		{
			pending = 0,
			ready = 1,
			failed = 2
		};

		std::mutex s_dis_shader_lock;
		std::array<std::vector<u32>, dis_shader_count> s_dis_shader_cache;
		std::array<dis_shader_state, dis_shader_count> s_dis_shader_state{};
	}

	const std::vector<u32>* GetDisShader(DisShader id)
	{
		const usz index = static_cast<usz>(id);

		if (index >= dis_shader_count)
		{
			return nullptr;
		}

		std::lock_guard lock(s_dis_shader_lock);

		if (s_dis_shader_state[index] == dis_shader_state::ready)
		{
			return &s_dis_shader_cache[index];
		}

		if (s_dis_shader_state[index] == dis_shader_state::failed)
		{
			return nullptr;
		}

		std::string source = s_dis_sources[index];
		std::vector<u32> spirv;

		if (!spirv::compile_glsl_to_spv(spirv, source, ::glsl::glsl_compute_program, ::glsl::glsl_rules_vulkan) || spirv.empty())
		{
			s_dis_shader_state[index] = dis_shader_state::failed;
			rsx_log.error("Frame generation: failed to compile DIS shader %s", s_dis_names[index]);
			return nullptr;
		}

		s_dis_shader_cache[index] = std::move(spirv);
		s_dis_shader_state[index] = dis_shader_state::ready;
		return &s_dis_shader_cache[index];
	}
}
