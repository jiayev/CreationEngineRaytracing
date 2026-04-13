#ifndef HEIGHTFOGPTDATA_HLSL
#define HEIGHTFOGPTDATA_HLSL

#include "Interop.h"

// Physical fog parameters for path tracing.
// Read in conjunction with ExponentialHeightFogSettings (FeatureData, b2)
// which provides the density model (fogDensity, fogHeight, fogHeightFalloff).
//
// This buffer supplies the optical/scattering parameters that convert
// the rasterization height-fog density into a participating medium:
//   σ_t = fogDensity × ExtinctionScale × 2^(-max(Falloff×(z-H), FalloffClamp))
//   σ_s = FogAlbedo × σ_t
//   σ_a = (1 - FogAlbedo) × σ_t
INTEROP_STRUCT(HeightFogPTData, 16)
{
    uint Enabled;            // 0 = off, 1 = on
    float ExtinctionScale;   // Global density multiplier (default 1.0)
    float FogPhaseG;         // Henyey-Greenstein g parameter [-1,1] (default 0.5)
    float FogRadius;         // XY cylinder radius for volume culling (game units)

    float3 FogAlbedo;        // Scattering albedo (0=pure absorption, 1=pure scattering)
    float FogDensityClamp;   // Max density multiplier clamp (default 8.0, prevents exp blow-up below FogHeight)

    uint ScatterMode;        // 0 = single scatter (transmittance only), 1 = multi scatter (volume scattering events)
    uint Pad0;
    uint Pad1;
    uint Pad2;
};
VALIDATE_CBUFFER(HeightFogPTData, 16);

#endif
