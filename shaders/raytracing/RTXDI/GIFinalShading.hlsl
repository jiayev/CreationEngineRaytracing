// GI Final Shading — Compute pass for ReSTIR GI final contribution
// Evaluates the BRDF at the primary surface with the selected reservoir sample,
// optionally traces a visibility ray, and writes the result.

#pragma pack_matrix(row_major)

#define NON_PATH_TRACING_PASS 1

#include "interop/CameraData.hlsli"
#include "interop/ReSTIRGIData.hlsli"
#include "raytracing/include/ReSTIRGIBindings.hlsli"
#include "raytracing/include/ReSTIRGI.hlsli"

float RGBToLuminance(float3 color)
{
    return dot(color, float3(0.2126, 0.7152, 0.0722));
}

// Random number generation
#ifndef RESTIRGI_RANDOM_HELPERS_DEFINED
#define RESTIRGI_RANDOM_HELPERS_DEFINED

uint InitRandomSeed(uint2 coord, uint2 size, uint frameCount)
{
    return coord.x + coord.y * size.x + frameCount * 719393;
}

uint PCGHash(uint seed)
{
    uint state = seed * 747796405u + 2891336453u;
    uint word = ((state >> ((state >> 28u) + 4u)) ^ state) * 277803737u;
    return (word >> 22u) ^ word;
}

float Random(inout uint seed)
{
    seed = PCGHash(seed);
    return float(seed) / 4294967296.0;
}

#endif // RESTIRGI_RANDOM_HELPERS_DEFINED

// ----- Resources -----

ConstantBuffer<CameraData>            Camera          : register(b0);
ConstantBuffer<ReSTIRGIConstants>     GIConst         : register(b1);

Texture2D<float>                      DepthTexture            : register(t0);
Texture2D<float4>                     NormalRoughnessTexture  : register(t1);
Texture2D<float4>                     DiffuseAlbedoTexture    : register(t2);

RaytracingAccelerationStructure       SceneBVH                : register(t3);

RWStructuredBuffer<PackedGIReservoir> GIReservoirBuffer       : register(u0);
RWTexture2D<float4>                   OutputColor             : register(u1);  // Main output (additive)

// ----- Final Shading -----

static const float kRayEpsilon = 1e-3;

[numthreads(8, 8, 1)]
void Main(uint2 GlobalIndex : SV_DispatchThreadID, uint2 LocalIndex : SV_GroupThreadID, uint2 GroupIdx : SV_GroupID)
{
    uint2 pixelPosition = ReservoirPosToPixelPos(GlobalIndex, GIConst.runtimeParams.activeCheckerboardField);

    if (any(pixelPosition >= GIConst.frameDim))
        return;

    float depth = DepthTexture[pixelPosition];

    if (depth <= 0)
        return;

    // Load final reservoir
    GIReservoir finalReservoir = LoadGIReservoir(
        GIConst.reservoirBufferParams,
        GlobalIndex,
        GIConst.bufferIndices.finalShadingInputBufferIndex,
        GIReservoirBuffer);

    if (!finalReservoir.isValid())
        return;

    // Reconstruct world position from depth
    // NDC → View → World using ProjInverse and ViewInverse
    float2 uv = (float2(pixelPosition) + 0.5) / float2(GIConst.frameDim);
    float2 ndc = float2(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0);
    float4 viewPos = mul(Camera.ProjInverse, float4(ndc, depth, 1.0));
    viewPos.xyz /= viewPos.w;
    float3 surfacePos = mul(Camera.ViewInverse, float4(viewPos.xyz, 1.0)).xyz;

    float3 surfaceNormal = NormalRoughnessTexture[pixelPosition].xyz * 2.0 - 1.0;
    float3 albedo = DiffuseAlbedoTexture[pixelPosition].rgb;

    // Direction from surface to reservoir sample position
    float3 L = finalReservoir.position - surfacePos;
    float hitDistance = length(L);
    L /= max(hitDistance, 1e-6);

    // Simple Lambertian evaluation — the full BSDF would require more surface data
    float NdotL = saturate(dot(surfaceNormal, L));
    float3 brdf = albedo * (NdotL / 3.14159265);

    float3 finalRadiance = finalReservoir.radiance * finalReservoir.weightSum;

    // Optional visibility test
    if (GIConst.finalShadingParams.enableFinalVisibility && hitDistance > kRayEpsilon)
    {
        RayDesc ray;
        ray.Origin = surfacePos + surfaceNormal * GIConst.rayEpsilon;
        ray.Direction = L;
        ray.TMin = 0;
        ray.TMax = hitDistance - GIConst.rayEpsilon * 2.0;

        RayQuery<RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH | RAY_FLAG_SKIP_CLOSEST_HIT_SHADER> q;
        q.TraceRayInline(SceneBVH, RAY_FLAG_NONE, 0xFF, ray);
        q.Proceed();

        if (q.CommittedStatus() == COMMITTED_TRIANGLE_HIT)
        {
            finalRadiance = 0;
        }
    }

    float3 attenuatedRadiance = brdf * finalRadiance;

    // MIS blending with initial sample (optional)
    if (GIConst.finalShadingParams.enableFinalMIS)
    {
        const float4 secondaryPositionNormal = u_SecondarySurfacePositionNormal[pixelPosition];
        const float3 secondaryRadiance = u_SecondarySurfaceRadiance[pixelPosition].xyz;
        const float  primaryScatterPdf = u_SecondarySurfaceRadiance[pixelPosition].w;

        if (primaryScatterPdf > 0)
        {
            float3 secPos = secondaryPositionNormal.xyz;
            float3 L0 = secPos - surfacePos;
            float hitDist0 = length(L0);
            L0 /= max(hitDist0, 1e-6);

            float NdotL0 = saturate(dot(surfaceNormal, L0));
            float3 brdf0 = albedo * (NdotL0 / 3.14159265);

            float initialWeight = RGBToLuminance(brdf0) / max(RGBToLuminance(brdf0 + brdf), 1e-6);
            initialWeight = saturate(initialWeight);
            initialWeight = initialWeight * initialWeight * initialWeight;

            float finalWeight = 1.0 - initialWeight;

            float3 initialRadiance = secondaryRadiance;
            attenuatedRadiance = brdf * finalRadiance * finalWeight + brdf0 * initialRadiance * initialWeight;
        }
    }

    // Sanitize output
    if (any(isinf(attenuatedRadiance)) || any(isnan(attenuatedRadiance)))
        attenuatedRadiance = 0;

    // Add ReSTIR GI contribution to the main output
    OutputColor[pixelPosition] += float4(attenuatedRadiance, 0);
}
