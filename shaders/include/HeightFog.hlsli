#ifndef HEIGHT_FOG_HLSLI
#define HEIGHT_FOG_HLSLI

// ============================================================================
// HeightFog.hlsli — Physically-based Exponential Height Fog for Path Tracing
//
// Treats the rasterization height-fog as a participating medium with:
//   Density:       ρ(z) = D × S × 2^(-max(F×(z-H), C))
//   Extinction:    σ_t  = ρ(z)
//   Scattering:    σ_s  = Albedo × σ_t
//   Absorption:    σ_a  = (1 - Albedo) × σ_t
//   Phase:         Henyey-Greenstein(g)
//
// Uses data from two constant buffers:
//   Features.ExponentialHeightFog  (b2) — density model (D, F, H)
//   HeightFogPT                    (b4) — physical scattering params
//
// Follows UE5 PathTracingFog.ush architecture with analytic transmittance.
// ============================================================================

// ---------------------------------------------------------------------------
// Fog volume properties (read from both CBs)
// ---------------------------------------------------------------------------

// Returns extinction coefficient σ_t at height Z
float FogGetDensity(float z)
{
    float D = Features.ExponentialHeightFog.fogDensity * HeightFogPT.ExtinctionScale;
    float F = max(Features.ExponentialHeightFog.fogHeightFalloff, 1e-6f);
    float H = Features.ExponentialHeightFog.fogHeight;
    float C = -log2(clamp(HeightFogPT.FogDensityClamp, 1.0f, 256.0f)); // negative clamp in exponent space

    return D * exp2(-max(F * (z - H), C));
}

// Returns the full volume properties at a world-space position
struct FogVolumeResult
{
    float  SigmaT;     // extinction coefficient
    float3 SigmaS;     // scattering coefficient (colored by albedo)
    float  PhaseG;     // HG asymmetry parameter
};

FogVolumeResult FogGetSigma(float3 worldPos)
{
    FogVolumeResult result = (FogVolumeResult)0;
    result.SigmaT = FogGetDensity(worldPos.z);
    result.SigmaS = HeightFogPT.FogAlbedo * result.SigmaT;
    result.PhaseG = HeightFogPT.FogPhaseG;
    return result;
}

// ---------------------------------------------------------------------------
// Density bounds (majorant/minorant) for a ray segment
// Exploits monotonicity of the exponential density function in Z
// ---------------------------------------------------------------------------

float2 FogGetDensityBounds(float3 origin, float3 direction, float tMin, float tMax)
{
    float densityA = FogGetDensity(origin.z + tMin * direction.z);
    float densityB = FogGetDensity(origin.z + tMax * direction.z);

    // Density is monotonic in Z, so endpoints give exact bounds
    float lo = min(densityA, densityB);
    float hi = max(densityA, densityB);
    return float2(lo, hi); // (minorant, majorant)
}

// ---------------------------------------------------------------------------
// Analytic transmittance along a ray segment [tMin, tMax]
// Uses closed-form integration of the exponential density function.
// Handles the density clamp boundary (uniform vs exponential region)
// and near-horizontal rays (Dz → 0) with a Taylor expansion.
// ---------------------------------------------------------------------------

float3 FogGetTransmittance(float3 origin, float3 direction, float tMin, float tMax)
{
    if (tMin >= tMax)
        return float3(1.0f, 1.0f, 1.0f);

    float D = Features.ExponentialHeightFog.fogDensity * HeightFogPT.ExtinctionScale;
    float F = max(Features.ExponentialHeightFog.fogHeightFalloff, 1e-6f);
    float H = Features.ExponentialHeightFog.fogHeight;
    float C = -log2(clamp(HeightFogPT.FogDensityClamp, 1.0f, 256.0f));

    float OH = origin.z - H;
    float Dz = direction.z;

    float tau;
    if (abs(Dz) < 1e-3f)
    {
        // Near-horizontal ray: density is ~constant along the ray
        tau = (tMax - tMin) * exp2(-max(F * OH, C));
    }
    else
    {
        // Split into exponentially-varying and clamped-uniform regions
        float clipZ = C / F + H;
        float tMid = clamp(-(origin.z - clipZ) / Dz, tMin, tMax);

        // Exponential region
        float ta = Dz > 0.0f ? tMid : tMin;
        float tb = Dz > 0.0f ? tMax : tMid;
        float expA = exp2(-max(F * (OH + Dz * ta), C));
        float expB = exp2(-max(F * (OH + Dz * tb), C));
        tau = (expA - expB) * rcp(Dz * F * log(2.0f));

        // Uniform region (density clamped)
        float uniformLen = Dz > 0.0f ? (tMid - tMin) : (tMax - tMid);
        tau += uniformLen * exp2(-C);
    }

    float opticalDepth = D * tau;

    // Beer-Lambert: T = exp(-σ_t integrated)
    // Return float3 for compatibility with throughput multiplication
    return exp(-opticalDepth);
}

// ---------------------------------------------------------------------------
// Ray-cylinder intersection for fog volume bounding
// Cylinder centered at camera XY, radius FogRadius, full Z range
// (simplified from UE's version since we use a single large cylinder)
// ---------------------------------------------------------------------------

float2 FogIntersect(float3 origin, float3 direction, float tMin, float tMax)
{
    float radius = HeightFogPT.FogRadius;

    // If radius is very large, the fog is effectively unbounded
    if (radius > 1e6f)
        return float2(tMin, tMax);

    // Camera-centered cylinder in XY
    float2 oc = origin.xy;  // camera is at origin in its own frame, but worldPos is used
    float a = dot(direction.xy, direction.xy);
    float b = dot(oc, direction.xy);
    float c = dot(oc, oc) - radius * radius;

    float disc = b * b - a * c;
    if (disc < 0.0f)
        return float2(tMax, tMin); // no intersection (empty interval)

    float sqrtDisc = sqrt(disc);
    float t0 = (-b - sqrtDisc) / a;
    float t1 = (-b + sqrtDisc) / a;

    return float2(max(t0, tMin), min(t1, tMax));
}

// ---------------------------------------------------------------------------
// Henyey-Greenstein phase function
// ---------------------------------------------------------------------------

float HenyeyGreensteinPhase(float g, float cosTheta)
{
    float g2 = g * g;
    float denom = 1.0f + g2 + 2.0f * g * cosTheta;
    return (1.0f - g2) / (4.0f * K_PI * denom * sqrt(denom));
}

// Importance-sample a direction from the HG phase function
// Returns the sampled direction in world space given the incoming direction
float3 SampleHGDirection(float g, float3 wo, inout uint randomSeed)
{
    float u1 = Random(randomSeed);
    float u2 = Random(randomSeed);

    float cosTheta;
    if (abs(g) < 1e-3f)
    {
        // Isotropic: uniform sphere sampling
        cosTheta = 1.0f - 2.0f * u1;
    }
    else
    {
        // HG inversion: cos(θ) = 1/(2g) × (1+g² - ((1-g²)/(1+g-2g·u))²)
        float s = (1.0f - g * g) / (1.0f + g - 2.0f * g * u1);
        cosTheta = (1.0f + g * g - s * s) / (2.0f * g);
    }

    float sinTheta = sqrt(max(0.0f, 1.0f - cosTheta * cosTheta));
    float phi = 2.0f * K_PI * u2;

    // Build local coordinate frame from incoming direction
    float3 w = normalize(wo);
    float3 u, v;
    if (abs(w.x) > 0.9f)
        u = normalize(cross(float3(0, 1, 0), w));
    else
        u = normalize(cross(float3(1, 0, 0), w));
    v = cross(w, u);

    return normalize(sinTheta * cos(phi) * u + sinTheta * sin(phi) * v + cosTheta * w);
}

// PDF of sampling direction with HG phase function
float HenyeyGreensteinPDF(float g, float cosTheta)
{
    return HenyeyGreensteinPhase(g, cosTheta);
}

// ---------------------------------------------------------------------------
// Distance sampling in exponential medium
// Sample a scattering distance along a ray within the fog volume
// using analytic inversion of the exponential density CDF.
// Returns: sampled distance (or > tMax if no scatter event)
// ---------------------------------------------------------------------------

struct FogScatterSample
{
    float t;          // sampled distance along ray
    bool  scattered;  // true if scatter event occurred within [tMin, tMax]
    float sigmaT;     // extinction at scatter point
    float3 sigmaS;    // scattering coefficient at scatter point
    float phaseG;     // HG g parameter
};

FogScatterSample FogSampleDistance(float3 origin, float3 direction, float tMin, float tMax, inout uint randomSeed)
{
    FogScatterSample result = (FogScatterSample)0;
    result.scattered = false;
    result.t = tMax;

    float D = Features.ExponentialHeightFog.fogDensity * HeightFogPT.ExtinctionScale;
    float F = max(Features.ExponentialHeightFog.fogHeightFalloff, 1e-6f);
    float H = Features.ExponentialHeightFog.fogHeight;
    float C = -log2(clamp(HeightFogPT.FogDensityClamp, 1.0f, 256.0f));

    // Use ratio tracking with the majorant
    float2 bounds = FogGetDensityBounds(origin, direction, tMin, tMax);
    float majorant = bounds.y;

    if (majorant < 1e-10f)
        return result; // negligible density

    // Null-collision (Woodcock) tracking
    float t = tMin;
    [loop]
    for (uint step = 0; step < 128; step++)
    {
        // Sample free-flight distance from majorant
        float xi = Random(randomSeed);
        t -= log(max(xi, 1e-20f)) / (D * majorant);

        if (t >= tMax)
            break;

        // Evaluate true density at this point
        float z = origin.z + t * direction.z;
        float trueDensity = exp2(-max(F * (z - H), C));
        float ratio = trueDensity / majorant;

        // Accept with probability ratio (real collision vs null collision)
        if (Random(randomSeed) < ratio)
        {
            result.t = t;
            result.scattered = true;
            result.sigmaT = D * trueDensity;
            result.sigmaS = HeightFogPT.FogAlbedo * result.sigmaT;
            result.phaseG = HeightFogPT.FogPhaseG;
            return result;
        }
    }

    return result;
}

// ---------------------------------------------------------------------------
// Helper: check if physical fog is active
// ---------------------------------------------------------------------------

bool IsFogEnabled()
{
    return HeightFogPT.Enabled != 0
        && Features.ExponentialHeightFog.enabled != 0
        && Features.ExponentialHeightFog.fogDensity > 0.0f
        && HeightFogPT.ExtinctionScale > 0.0f;
}

// ---------------------------------------------------------------------------
// Helper: fog transmittance for a shadow ray from surface to light
// ---------------------------------------------------------------------------

float3 FogShadowTransmittance(float3 surfacePos, float3 lightDir, float distance)
{
    if (!IsFogEnabled())
        return float3(1.0f, 1.0f, 1.0f);

    float2 fogInterval = FogIntersect(surfacePos, lightDir, 0.0f, distance);
    if (fogInterval.x >= fogInterval.y)
        return float3(1.0f, 1.0f, 1.0f);

    return FogGetTransmittance(surfacePos, lightDir, fogInterval.x, fogInterval.y);
}

#endif // HEIGHT_FOG_HLSLI
