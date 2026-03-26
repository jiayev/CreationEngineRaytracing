// GI Spatial Resampling — Compute pass for ReSTIR GI spatial resampling
// Loads temporally resampled reservoirs and merges with spatial neighbors.

#pragma pack_matrix(row_major)

#define NON_PATH_TRACING_PASS 1

#include "interop/CameraData.hlsli"
#include "interop/ReSTIRGIData.hlsli"
#include "raytracing/include/ReSTIRGI.hlsli"

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

ConstantBuffer<CameraData>          Camera          : register(b0);
ConstantBuffer<ReSTIRGIConstants>   GIConst         : register(b1);

Texture2D<float>                    DepthTexture            : register(t0);
Texture2D<float4>                   NormalRoughnessTexture  : register(t1);

RWStructuredBuffer<PackedGIReservoir> GIReservoirBuffer     : register(u0);

// ----- Entry Point -----

[numthreads(8, 8, 1)]
void Main(uint2 GlobalIndex : SV_DispatchThreadID, uint2 LocalIndex : SV_GroupThreadID, uint2 GroupIdx : SV_GroupID)
{
    uint2 pixelPosition = ReservoirPosToPixelPos(GlobalIndex, GIConst.runtimeParams.activeCheckerboardField);

    if (any(pixelPosition >= GIConst.frameDim))
        return;

    float depth = DepthTexture[pixelPosition];

    GIReservoir resultReservoir = GIReservoir::makeEmpty();

    if (depth > 0)
    {
        float3 normal = NormalRoughnessTexture[pixelPosition].xyz * 2.0 - 1.0;

        uint randomSeed = InitRandomSeed(pixelPosition, GIConst.frameDim, GIConst.frameIndex * 13 + 6);

        GIReservoir inputReservoir = LoadGIReservoir(
            GIConst.reservoirBufferParams,
            GlobalIndex,
            GIConst.bufferIndices.spatialResamplingInputBufferIndex,
            GIReservoirBuffer);

        resultReservoir = GISpatialResampling(
            pixelPosition,
            float3(0, 0, 0),
            normal,
            depth,
            inputReservoir,
            GIConst.reservoirBufferParams,
            GIConst.runtimeParams,
            GIConst.spatialResamplingParams,
            GIConst.bufferIndices.spatialResamplingInputBufferIndex,
            GIReservoirBuffer,
            DepthTexture,
            NormalRoughnessTexture,
            GIConst.frameDim,
            randomSeed);
    }

    StoreGIReservoir(resultReservoir, GIConst.reservoirBufferParams, GlobalIndex,
        GIConst.bufferIndices.spatialResamplingOutputBufferIndex, GIReservoirBuffer);
}
