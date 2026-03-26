#ifndef RESTIRGIDATA_HLSLI
#define RESTIRGIDATA_HLSLI

#include "Interop.h"

// ============================================================================
// ReSTIR GI constant buffer data — shared between CPU and GPU
// Modeled after RTXPT's RtxdiBridgeConstants (GI-relevant subset)
// ============================================================================

// Reservoir buffer layout parameters (matches RTXDI SDK's RTXDI_ReservoirBufferParameters)
INTEROP_DATA_STRUCT(GIReservoirBufferParams, 16)
{
    uint reservoirBlockRowPitch;
    uint reservoirArrayPitch;
    uint pad1;
    uint pad2;
};
VALIDATE_CBUFFER(GIReservoirBufferParamsData, 16);

// Runtime parameters (matches RTXDI SDK's RTXDI_RuntimeParameters)
INTEROP_DATA_STRUCT(GIRuntimeParams, 16)
{
    uint neighborOffsetMask;
    uint activeCheckerboardField;   // 0 = no checkerboard, 1 = odd, 2 = even
    uint pad1;
    uint pad2;
};
VALIDATE_CBUFFER(GIRuntimeParamsData, 16);

// Temporal resampling parameters
INTEROP_DATA_STRUCT(GITemporalResamplingParams, 16)
{
    float depthThreshold;
    float normalThreshold;
    uint  enablePermutationSampling;
    uint  maxHistoryLength;

    uint  maxReservoirAge;
    uint  enableBoilingFilter;
    float boilingFilterStrength;
    uint  enableFallbackSampling;

    uint  temporalBiasCorrectionMode;
    uint  uniformRandomNumber;
    uint  pad2;
    uint  pad3;
};
VALIDATE_CBUFFER(GITemporalResamplingParamsData, 16);

// Spatial resampling parameters
INTEROP_DATA_STRUCT(GISpatialResamplingParams, 16)
{
    float spatialDepthThreshold;
    float spatialNormalThreshold;
    uint  numSpatialSamples;
    float spatialSamplingRadius;

    uint  spatialBiasCorrectionMode;
    uint  pad1;
    uint  pad2;
    uint  pad3;
};
VALIDATE_CBUFFER(GISpatialResamplingParamsData, 16);

// Final shading parameters
INTEROP_DATA_STRUCT(GIFinalShadingParams, 16)
{
    uint enableFinalVisibility;
    uint enableFinalMIS;
    uint pad1;
    uint pad2;
};
VALIDATE_CBUFFER(GIFinalShadingParamsData, 16);

// Buffer indices for ping-pong reservoir management
INTEROP_DATA_STRUCT(GIBufferIndices, 16)
{
    uint secondarySurfaceReSTIRDIOutputBufferIndex;
    uint temporalResamplingInputBufferIndex;
    uint temporalResamplingOutputBufferIndex;
    uint spatialResamplingInputBufferIndex;

    uint spatialResamplingOutputBufferIndex;
    uint finalShadingInputBufferIndex;
    uint pad1;
    uint pad2;
};
VALIDATE_CBUFFER(GIBufferIndicesData, 16);

// Packed GI reservoir for structured buffer storage (32 bytes)
struct PackedGIReservoir
{
    float3 position;            // 12 bytes
    uint   packed_miscData_age_M; // 4 bytes: M(14) | age(14) | misc(4)
    uint   packed_radiance;     // 4 bytes: LogLUV packed
    float  weight;              // 4 bytes: weightSum
    uint   packed_normal;       // 4 bytes: octahedral encoded
    uint   unused;              // 4 bytes: padding to 32
};

// Main ReSTIR GI constant buffer
INTEROP_STRUCT(ReSTIRGIConstants, 16)
{
    INTEROP_DATA_TYPE(GIRuntimeParams) runtimeParams;
    INTEROP_DATA_TYPE(GIReservoirBufferParams) reservoirBufferParams;
    INTEROP_DATA_TYPE(GIBufferIndices) bufferIndices;
    INTEROP_DATA_TYPE(GITemporalResamplingParams) temporalResamplingParams;
    INTEROP_DATA_TYPE(GISpatialResamplingParams) spatialResamplingParams;
    INTEROP_DATA_TYPE(GIFinalShadingParams) finalShadingParams;

    uint  frameIndex;
    float rayEpsilon;
    uint  enableTemporalResampling;
    uint  varyAgeThreshold;

    uint2 frameDim;
    uint  denoisingEnabled;        // When true, GI final shading writes to StablePlanesBuffer
    uint  stablePlaneRenderWidth;  // Render width for StablePlane addressing
};
VALIDATE_CBUFFER(ReSTIRGIConstants, 16);

#endif // RESTIRGIDATA_HLSLI
