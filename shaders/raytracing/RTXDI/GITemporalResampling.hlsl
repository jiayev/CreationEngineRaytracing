// GI Temporal Resampling — Compute pass for ReSTIR GI temporal resampling
// Creates initial reservoirs from path tracer secondary surface data and merges with temporal history.

#pragma pack_matrix(row_major)

#define NON_PATH_TRACING_PASS 1

#define RESTIRGI_ENABLE_BOILING_FILTER
#define RESTIRGI_BOILING_FILTER_GROUP_SIZE 8

#include "interop/CameraData.hlsli"
#include "interop/ReSTIRGIData.hlsli"
#include "raytracing/include/ReSTIRGIBindings.hlsli"
#include "raytracing/include/ReSTIRGI.hlsli"

// Random number generation (from Common.hlsli)
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

ConstantBuffer<CameraData>          Camera          : register(b0);
ConstantBuffer<ReSTIRGIConstants>   GIConst         : register(b1);

Texture2D<float>                    DepthTexture            : register(t0);
Texture2D<float4>                   NormalRoughnessTexture  : register(t1);
Texture2D<float4>                   MotionVectorsTexture    : register(t2);

RWStructuredBuffer<PackedGIReservoir> GIReservoirBuffer     : register(u0);

// ----- Entry Point -----

[numthreads(8, 8, 1)]
void Main(uint2 GlobalIndex : SV_DispatchThreadID, uint2 LocalIndex : SV_GroupThreadID, uint2 GroupIdx : SV_GroupID)
{
    uint2 pixelPosition = ReservoirPosToPixelPos(GlobalIndex, GIConst.runtimeParams.activeCheckerboardField);

    if (any(pixelPosition >= GIConst.frameDim))
        return;

    // Load current surface info
    float depth = DepthTexture[pixelPosition];

    // Invalid surface — store empty reservoir
    if (depth <= 0)
    {
        GIReservoir empty = GIReservoir::makeEmpty();
        StoreGIReservoir(empty, GIConst.reservoirBufferParams, GlobalIndex,
            GIConst.bufferIndices.temporalResamplingOutputBufferIndex, GIReservoirBuffer);
        return;
    }

    float3 normal = NormalRoughnessTexture[pixelPosition].xyz * 2.0 - 1.0;

    // Create initial reservoir from path tracer secondary surface data
    const float4 secondaryPositionNormal = u_SecondarySurfacePositionNormal[pixelPosition];
    const float3 secondaryRadiance = u_SecondarySurfaceRadiance[pixelPosition].xyz;
    const float  primaryScatterPdf = u_SecondarySurfaceRadiance[pixelPosition].w;

    GIReservoir initialReservoir = GIReservoir::makeEmpty();
    if (primaryScatterPdf > 0)
    {
        float3 secPos = secondaryPositionNormal.xyz;
        float3 secNormal = ReSTIR_OctToNDirUnorm32(asuint(secondaryPositionNormal.w));
        initialReservoir = MakeGIReservoir(secPos, secNormal, secondaryRadiance, primaryScatterPdf);
    }

    GIReservoir resultReservoir;

    if (GIConst.enableTemporalResampling)
    {
        uint randomSeed = InitRandomSeed(pixelPosition, GIConst.frameDim, GIConst.frameIndex * 13 + 5);

        // Get motion vector (NDC half-space from PT)
        float3 motionVector = MotionVectorsTexture[pixelPosition].xyz;

        resultReservoir = GITemporalResampling(
            pixelPosition,
            float3(0, 0, 0),
            normal,
            depth,
            initialReservoir,
            motionVector.xy,
            GIConst.reservoirBufferParams,
            GIConst.runtimeParams,
            GIConst.temporalResamplingParams,
            GIConst.bufferIndices.temporalResamplingInputBufferIndex,
            GIReservoirBuffer,
            DepthTexture,
            NormalRoughnessTexture,
            GIConst.frameDim,
            randomSeed);
    }
    else
    {
        // Finalize even without temporal resampling — must match convention expected by spatial
        resultReservoir = initialReservoir;
        float tpdf = EvalGITargetPdf(resultReservoir.radiance);
        FinalizeResampling(resultReservoir, tpdf);
    }

    // Sanitize
    if (any(isinf(resultReservoir.radiance)) || any(isnan(resultReservoir.radiance)) ||
        any(isinf(resultReservoir.position)) || any(isnan(resultReservoir.position)) ||
        isinf(resultReservoir.weightSum) || isnan(resultReservoir.weightSum))
    {
        resultReservoir = GIReservoir::makeEmpty();
    }

#ifdef RESTIRGI_ENABLE_BOILING_FILTER
    if (GIConst.temporalResamplingParams.enableBoilingFilter > 0)
    {
        GIBoilingFilter(LocalIndex, GIConst.temporalResamplingParams.boilingFilterStrength, resultReservoir);
    }
#endif

    StoreGIReservoir(resultReservoir, GIConst.reservoirBufferParams, GlobalIndex,
        GIConst.bufferIndices.temporalResamplingOutputBufferIndex, GIReservoirBuffer);
}
