// ============================================================================
// ReSTIRGI.hlsli — Self-contained ReSTIR GI reservoir and resampling functions
// Adapted from RTXDI SDK's GI module for CreationEngineRaytracing.
// ============================================================================

#ifndef RESTIRGI_HLSLI
#define RESTIRGI_HLSLI

#include "interop/ReSTIRGIData.hlsli"

#ifndef NDIRTOOCT_DEFINED
#define NDIRTOOCT_DEFINED

float2 _ReSTIR_OctEncode(float3 n)
{
    n /= (abs(n.x) + abs(n.y) + abs(n.z));
    if (n.z < 0.0)
    {
        float2 octSign = float2(n.x >= 0.0 ? 1.0 : -1.0, n.y >= 0.0 ? 1.0 : -1.0);
        n.xy = (1.0 - abs(n.yx)) * octSign;
    }
    return n.xy * 0.5 + 0.5;
}

float3 _ReSTIR_OctDecode(float2 f)
{
    f = f * 2.0 - 1.0;
    float3 n = float3(f.x, f.y, 1.0 - abs(f.x) - abs(f.y));
    float t = saturate(-n.z);
    n.x += (n.x >= 0.0) ? -t : t;
    n.y += (n.y >= 0.0) ? -t : t;
    return normalize(n);
}

uint ReSTIR_NDirToOctUnorm32(float3 n)
{
    float2 p = _ReSTIR_OctEncode(n);
    return uint(saturate(p.x) * 0xFFFE) | (uint(saturate(p.y) * 0xFFFE) << 16);
}

float3 ReSTIR_OctToNDirUnorm32(uint pUnorm)
{
    float2 p;
    p.x = saturate(float(pUnorm & 0xFFFF) / float(0xFFFE));
    p.y = saturate(float(pUnorm >> 16) / float(0xFFFE));
    return _ReSTIR_OctDecode(p);
}

#endif // NDIRTOOCT_DEFINED

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

// ============================================================================
// Constants
// ============================================================================

#define RESTIRGI_RESERVOIR_BLOCK_SIZE 16

float _ReSTIRGI_Luminance(float3 color)
{
    return dot(color, float3(0.2126, 0.7152, 0.0722));
}

#define RESTIRGI_BIAS_CORRECTION_OFF       0
#define RESTIRGI_BIAS_CORRECTION_BASIC     1
#define RESTIRGI_BIAS_CORRECTION_RAYTRACED 3

static const float RESTIRGI_INVALID_PDF = 0.0f;

// ============================================================================
// GI Reservoir structure (unpacked, working copy)
// ============================================================================

struct GIReservoir
{
    float3 position;     // Secondary surface world position
    float3 normal;       // Secondary surface world normal
    float3 radiance;     // Radiance at secondary surface
    float  weightSum;    // Reservoir weight sum
    uint   M;            // Number of samples seen
    uint   age;          // Age in frames

    static GIReservoir makeEmpty()
    {
        GIReservoir r;
        r.position = float3(0, 0, 0);
        r.normal   = float3(0, 0, 1);
        r.radiance = float3(0, 0, 0);
        r.weightSum = 0;
        r.M        = 0;
        r.age      = 0;
        return r;
    }

    bool isValid()
    {
        return M > 0 && weightSum > 0;
    }
};

// ============================================================================
// Reservoir creation from path tracer output
// ============================================================================

GIReservoir MakeGIReservoir(float3 secondaryPos, float3 secondaryNormal, float3 secondaryRadiance, float scatterPdf)
{
    GIReservoir r;
    r.position = secondaryPos;
    r.normal   = secondaryNormal;
    r.radiance = secondaryRadiance;
    r.M        = 1;
    r.age      = 0;

    // Target PDF = luminance of reflected radiance (will be evaluated properly during resampling)
    float targetPdf = max(0, _ReSTIRGI_Luminance(secondaryRadiance));
    r.weightSum = (scatterPdf > 0 && targetPdf > 0) ? (targetPdf / scatterPdf) : 0;
    return r;
}

// ============================================================================
// Packing / Unpacking (LogLUV for radiance, octahedral for normal)
// ============================================================================

// LogLUV encoding (matches RTXDI SDK format)
uint PackLogLUV(float3 rgb)
{
    float maxComp = max(max(rgb.r, rgb.g), max(rgb.b, 1e-6));
    float logVal = (log2(maxComp) + 10.0) / 20.0;
    logVal = saturate(logVal);
    uint e = uint(logVal * 255.0 + 0.5);

    float scale = (maxComp > 1e-6) ? (255.0 / (maxComp * 20.0 + 1e-6)) : 0;
    // Simple 8-8-8-8 packing: R,G,B in low 24 bits, exponent in top 8 bits
    float3 normalizedRGB = saturate(rgb / max(maxComp, 1e-6));
    uint r_val = uint(normalizedRGB.r * 255.0 + 0.5);
    uint g_val = uint(normalizedRGB.g * 255.0 + 0.5);
    uint b_val = uint(normalizedRGB.b * 255.0 + 0.5);

    return r_val | (g_val << 8) | (b_val << 16) | (e << 24);
}

float3 UnpackLogLUV(uint packed)
{
    float r_norm = float(packed & 0xFF) / 255.0;
    float g_norm = float((packed >> 8) & 0xFF) / 255.0;
    float b_norm = float((packed >> 16) & 0xFF) / 255.0;
    float e = float((packed >> 24) & 0xFF) / 255.0;

    float maxComp = exp2(e * 20.0 - 10.0);
    return float3(r_norm, g_norm, b_norm) * maxComp;
}

// ============================================================================
// Buffer addressing (block-linear layout matching RTXDI)
// ============================================================================

uint2 PixelPosToReservoirPos(uint2 pixelPos, uint activeCheckerboardField)
{
    // No checkerboard — direct mapping
    if (activeCheckerboardField == 0)
        return pixelPos;

    // Checkerboard: compress X by 2
    return uint2(pixelPos.x / 2, pixelPos.y);
}

uint2 ReservoirPosToPixelPos(uint2 reservoirPos, uint activeCheckerboardField)
{
    if (activeCheckerboardField == 0)
        return reservoirPos;

    return uint2(reservoirPos.x * 2 + ((reservoirPos.y + activeCheckerboardField) & 1), reservoirPos.y);
}

uint GetReservoirLinearIndex(uint2 reservoirPos, GIReservoirBufferParams params)
{
    uint2 blockIdx   = reservoirPos / RESTIRGI_RESERVOIR_BLOCK_SIZE;
    uint2 posInBlock = reservoirPos % RESTIRGI_RESERVOIR_BLOCK_SIZE;
    return (blockIdx.y * params.reservoirBlockRowPitch + blockIdx.x) *
            (RESTIRGI_RESERVOIR_BLOCK_SIZE * RESTIRGI_RESERVOIR_BLOCK_SIZE) +
            posInBlock.y * RESTIRGI_RESERVOIR_BLOCK_SIZE + posInBlock.x;
}

uint GetReservoirBufferIndex(uint2 reservoirPos, uint bufferIndex, GIReservoirBufferParams params)
{
    return GetReservoirLinearIndex(reservoirPos, params) + bufferIndex * params.reservoirArrayPitch;
}

// ============================================================================
// Store/Load from structured buffer
// ============================================================================

void StoreGIReservoir(
    GIReservoir r,
    GIReservoirBufferParams params,
    uint2 reservoirPos,
    uint bufferIndex,
    RWStructuredBuffer<PackedGIReservoir> reservoirBuffer)
{
    uint index = GetReservoirBufferIndex(reservoirPos, bufferIndex, params);

    PackedGIReservoir packed;
    packed.position = r.position;

    // Pack M (low 14 bits), age (14 bits), misc (4 bits) into uint
    uint M_clamped = min(r.M, 0x3FFF);
    uint age_clamped = min(r.age, 0x3FFF);
    packed.packed_miscData_age_M = M_clamped | (age_clamped << 14);

    packed.packed_radiance = PackLogLUV(r.radiance);
    packed.weight = r.weightSum;
    packed.packed_normal = ReSTIR_NDirToOctUnorm32(r.normal);
    packed.unused = 0;

    reservoirBuffer[index] = packed;
}

GIReservoir LoadGIReservoir(
    GIReservoirBufferParams params,
    uint2 reservoirPos,
    uint bufferIndex,
    RWStructuredBuffer<PackedGIReservoir> reservoirBuffer)
{
    uint index = GetReservoirBufferIndex(reservoirPos, bufferIndex, params);

    PackedGIReservoir packed = reservoirBuffer[index];

    GIReservoir r;
    r.position = packed.position;
    r.M   = packed.packed_miscData_age_M & 0x3FFF;
    r.age = (packed.packed_miscData_age_M >> 14) & 0x3FFF;
    r.radiance = UnpackLogLUV(packed.packed_radiance);
    r.weightSum = packed.weight;
    r.normal = ReSTIR_OctToNDirUnorm32(packed.packed_normal);

    return r;
}

// ============================================================================
// Reservoir Update (Weighted Reservoir Sampling)
// ============================================================================

bool UpdateReservoir(inout GIReservoir r, GIReservoir newSample, float targetPdf, float random)
{
    float weight = (targetPdf > 0) ? (targetPdf * newSample.weightSum * newSample.M) : 0;

    r.weightSum += weight;
    r.M += newSample.M;

    if (random * r.weightSum < weight)
    {
        r.position = newSample.position;
        r.normal   = newSample.normal;
        r.radiance = newSample.radiance;
        r.age      = newSample.age;
        return true;
    }
    return false;
}

void FinalizeResampling(inout GIReservoir r, float targetPdf)
{
    float denominator = targetPdf * r.M;
    r.weightSum = (denominator > 0) ? (r.weightSum / denominator) : 0;
}

// ============================================================================
// Target PDF evaluation
// ============================================================================

float EvalGITargetPdf(float3 sampleRadiance)
{
    return max(0, _ReSTIRGI_Luminance(sampleRadiance));
}

// ============================================================================
// Temporal Resampling
// ============================================================================

GIReservoir GITemporalResampling(
    uint2 pixelPosition,
    float3 surfacePosition,
    float3 surfaceNormal,
    float  surfaceDepth,
    GIReservoir initialReservoir,
    float2 motionVectorNDCHalf,    // Motion vector in NDC half-space (from computeMotionVector)
    GIReservoirBufferParams reservoirParams,
    GIRuntimeParams runtimeParams,
    GITemporalResamplingParams temporalParams,
    uint inputBufferIndex,
    RWStructuredBuffer<PackedGIReservoir> reservoirBuffer,
    Texture2D<float> depthTexture,
    Texture2D<float4> normalRoughnessTexture,
    uint2 frameDim,
    inout uint randomSeed)
{
    // Start with initial reservoir
    GIReservoir result = initialReservoir;

    // Convert NDC half-space motion vector to pixel offset
    // computeMotionVector outputs: (prevNDC - currNDC) * float3(0.5, -0.5, 1)
    // So pixel offset = motionVector * frameDim
    float2 pixelMotion = motionVectorNDCHalf * float2(frameDim);
    float2 prevPixelF = float2(pixelPosition) + 0.5 + pixelMotion;
    int2 prevPixel = int2(floor(prevPixelF));

    // Bounds check
    if (prevPixel.x < 0 || prevPixel.y < 0 || prevPixel.x >= (int)frameDim.x || prevPixel.y >= (int)frameDim.y)
        return result;

    // Load temporal neighbor reservoir
    uint2 prevReservoirPos = PixelPosToReservoirPos(prevPixel, runtimeParams.activeCheckerboardField);
    GIReservoir temporalReservoir = LoadGIReservoir(reservoirParams, prevReservoirPos, inputBufferIndex, reservoirBuffer);

    if (!temporalReservoir.isValid())
        return result;

    // Validate depth similarity
    float prevDepth = depthTexture[prevPixel];
    if (abs(surfaceDepth - prevDepth) > temporalParams.depthThreshold * max(surfaceDepth, prevDepth))
        return result;

    // Validate normal similarity
    float3 prevNormal = normalRoughnessTexture[prevPixel].xyz * 2.0 - 1.0;
    if (dot(surfaceNormal, prevNormal) < temporalParams.normalThreshold)
        return result;

    // Cap history length
    if (temporalReservoir.M > temporalParams.maxHistoryLength)
    {
        float ratio = float(temporalParams.maxHistoryLength) / float(temporalReservoir.M);
        temporalReservoir.weightSum *= ratio;
        temporalReservoir.M = temporalParams.maxHistoryLength;
    }

    // Cap reservoir age
    temporalReservoir.age++;
    if (temporalReservoir.age > temporalParams.maxReservoirAge)
        return result;

    // Compute target PDF for temporal sample at current surface
    float temporalTargetPdf = EvalGITargetPdf(temporalReservoir.radiance);

    // Merge temporal into result
    float rng = Random(randomSeed);
    UpdateReservoir(result, temporalReservoir, temporalTargetPdf, rng);

    // Finalize
    float currentTargetPdf = EvalGITargetPdf(result.radiance);
    FinalizeResampling(result, currentTargetPdf);

    return result;
}

// ============================================================================
// Spatial Resampling
// ============================================================================

GIReservoir GISpatialResampling(
    uint2 pixelPosition,
    float3 surfacePosition,
    float3 surfaceNormal,
    float  surfaceDepth,
    GIReservoir inputReservoir,
    GIReservoirBufferParams reservoirParams,
    GIRuntimeParams runtimeParams,
    GISpatialResamplingParams spatialParams,
    uint sourceBufferIndex,
    RWStructuredBuffer<PackedGIReservoir> reservoirBuffer,
    Texture2D<float> depthTexture,
    Texture2D<float4> normalRoughnessTexture,
    uint2 frameDim,
    inout uint randomSeed)
{
    GIReservoir result = inputReservoir;
    float currentTargetPdf = EvalGITargetPdf(result.radiance);
    result.weightSum = currentTargetPdf * result.weightSum * result.M;

    uint validNeighbors = 0;
    uint totalM = result.M;

    [loop]
    for (uint i = 0; i < spatialParams.numSpatialSamples; i++)
    {
        // Random offset within sampling radius
        float angle = Random(randomSeed) * 6.28318530718;
        float radius = sqrt(Random(randomSeed)) * spatialParams.spatialSamplingRadius;
        int2 offset = int2(cos(angle) * radius, sin(angle) * radius);
        int2 neighborPixel = int2(pixelPosition) + offset;

        // Bounds check
        if (neighborPixel.x < 0 || neighborPixel.y < 0 ||
            neighborPixel.x >= (int)frameDim.x || neighborPixel.y >= (int)frameDim.y)
            continue;

        // Depth similarity check
        float neighborDepth = depthTexture[neighborPixel];
        if (abs(surfaceDepth - neighborDepth) > spatialParams.spatialDepthThreshold * max(surfaceDepth, neighborDepth))
            continue;

        // Normal similarity check
        float3 neighborNormal = normalRoughnessTexture[neighborPixel].xyz * 2.0 - 1.0;
        if (dot(surfaceNormal, neighborNormal) < spatialParams.spatialNormalThreshold)
            continue;

        // Load neighbor reservoir
        uint2 neighborReservoirPos = PixelPosToReservoirPos(neighborPixel, runtimeParams.activeCheckerboardField);
        GIReservoir neighborReservoir = LoadGIReservoir(reservoirParams, neighborReservoirPos,
            sourceBufferIndex, reservoirBuffer);

        if (!neighborReservoir.isValid())
            continue;

        // Evaluate target PDF at current surface
        float neighborTargetPdf = EvalGITargetPdf(neighborReservoir.radiance);

        float weight = neighborTargetPdf * neighborReservoir.weightSum * neighborReservoir.M;
        result.weightSum += weight;
        totalM += neighborReservoir.M;

        if (Random(randomSeed) * result.weightSum < weight)
        {
            result.position = neighborReservoir.position;
            result.normal   = neighborReservoir.normal;
            result.radiance = neighborReservoir.radiance;
            result.age      = neighborReservoir.age;
        }

        validNeighbors++;
    }

    result.M = totalM;

    // Finalize
    float finalTargetPdf = EvalGITargetPdf(result.radiance);
    FinalizeResampling(result, finalTargetPdf);

    return result;
}

// ============================================================================
// Boiling Filter (threaded, requires groupshared memory)
// ============================================================================

#ifdef RESTIRGI_ENABLE_BOILING_FILTER
#ifndef RESTIRGI_BOILING_FILTER_GROUP_SIZE
#define RESTIRGI_BOILING_FILTER_GROUP_SIZE 8
#endif

groupshared float s_boilingFilterWeights[RESTIRGI_BOILING_FILTER_GROUP_SIZE * RESTIRGI_BOILING_FILTER_GROUP_SIZE];

void GIBoilingFilter(uint2 localIndex, float filterStrength, inout GIReservoir reservoir)
{
    uint threadIdx = localIndex.y * RESTIRGI_BOILING_FILTER_GROUP_SIZE + localIndex.x;

    s_boilingFilterWeights[threadIdx] = reservoir.weightSum;

    GroupMemoryBarrierWithGroupSync();

    // Compute average weight in the tile
    float sum = 0;
    uint count = RESTIRGI_BOILING_FILTER_GROUP_SIZE * RESTIRGI_BOILING_FILTER_GROUP_SIZE;
    [unroll]
    for (uint i = 0; i < count; i++)
        sum += s_boilingFilterWeights[i];

    float average = sum / float(count);

    // If this reservoir's weight greatly exceeds the average, reduce it
    if (reservoir.weightSum > average * (1.0 + filterStrength * 10.0))
    {
        reservoir.weightSum = average * (1.0 + filterStrength * 10.0);
    }
}
#endif // RESTIRGI_ENABLE_BOILING_FILTER

#endif // RESTIRGI_HLSLI
