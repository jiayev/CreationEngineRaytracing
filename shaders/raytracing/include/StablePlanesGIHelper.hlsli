// StablePlanesGIHelper.hlsli — Lightweight stable planes helpers for non-path-tracing passes.
// Provides the StablePlane struct layout and Fp16 packing functions needed by ReSTIR GI
// final shading to read/write PackedNoisyRadianceAndSpecAvg.
//
// For full stable planes functionality (BUILD/FILL, branch IDs, etc.),
// include StablePlanes.hlsli instead.

#ifndef __STABLE_PLANES_GI_HELPER_HLSLI__
#define __STABLE_PLANES_GI_HELPER_HLSLI__

// Guard: skip if full StablePlanes.hlsli is already included
#ifndef __STABLE_PLANES_HLSLI__

// ============================================================================
// Fp16 ↔ Fp32 packing
// ============================================================================

#ifndef HLF_MAX
#define HLF_MAX 65504.0f
#endif

uint Fp32ToFp16_2(float2 v)
{
    const uint2 r = f32tof16(clamp(v, -HLF_MAX, HLF_MAX));
    return (r.y << 16) | (r.x & 0xFFFF);
}

float2 Fp16ToFp32_2(uint r)
{
    uint2 v;
    v.x = (r & 0xFFFF);
    v.y = (r >> 16);
    return f16tof32(v);
}

uint2 Fp32ToFp16(float4 v)
{
    const uint d0 = Fp32ToFp16_2(v.xy);
    const uint d1 = Fp32ToFp16_2(v.zw);
    return uint2(d0, d1);
}

float4 Fp16ToFp32(uint2 d)
{
    const float2 d0 = Fp16ToFp32_2(d.x);
    const float2 d1 = Fp16ToFp32_2(d.y);
    return float4(d0.xy, d1.xy);
}

// ============================================================================
// StablePlane struct (80 bytes = 20 dwords) — layout-compatible with StablePlanes.hlsli
// ============================================================================

struct StablePlane
{
    float3  RayOrigin;
    float   LastRayTCurrent;
    float3  RayDir;
    float   SceneLength;
    uint3   PackedThpAndMVs;
    uint    VertexIndexAndRoughness;
    uint3   DenoiserPackedBSDFEstimate;
    uint    PackedNormal;
    uint2   PackedNoisyRadianceAndSpecAvg;
    uint    FlagsAndVertexIndex;
    uint    PackedCounters;

    float3  GetNoisyRadiance()
    {
        float4 s = Fp16ToFp32(PackedNoisyRadianceAndSpecAvg);
        return (s * s).xyz;
    }
};

#endif // __STABLE_PLANES_HLSLI__
#endif // __STABLE_PLANES_GI_HELPER_HLSLI__
