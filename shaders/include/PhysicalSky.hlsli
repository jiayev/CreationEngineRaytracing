#ifndef CER_PHYSICAL_SKY_HLSLI
#define CER_PHYSICAL_SKY_HLSLI

#if defined(SKYRIM) && defined(PHYSICAL_SKY_RESOURCES)
#include "include/PhysicalSkyShadowVolume.hlsli"

float2 PhysicalSkyTrLutUv(float r, float cosSunZenith)
{
    const PhysSkyData data = Features.PhysicalSky;
    r = clamp(r, data.rPlanet, data.rAtmosphere);
    const float atmosphereHorizon = sqrt((data.rAtmosphere - data.rPlanet) * (data.rAtmosphere + data.rPlanet));
    const float localHorizon = sqrt(max(0.0, (r - data.rPlanet) * (r + data.rPlanet)));
    const float mu = max(cosSunZenith, -localHorizon / r);
    const float rMu = r * mu;
    const float distance = max(0.0, sqrt(max(0.0, rMu * rMu + (data.rAtmosphere - r) * (data.rAtmosphere + r))) - rMu);
    const float nearest = data.rAtmosphere - r;
    const float farthest = localHorizon + atmosphereHorizon;
    const float2 unitUv = saturate(float2((distance - nearest) / max(farthest - nearest, 1.0), localHorizon / atmosphereHorizon));
    const float2 texel = 1.0 / float2(256, 64);
    return 0.5 * texel + unitUv * (1.0 - texel);
}

float3 SamplePhysicalSkyTransmittance(float3 position, float3 lightDir)
{
    const PhysSkyData data = Features.PhysicalSky;
    if (!data.enabled)
        return 1.0;

    float3 transmittance = 1.0;
    if (data.trMix > 1e-8) {
        const float3 planetPos = float3(position.xy - Camera.Position.xy, position.z - data.zBottom + data.rPlanet);
        const float radius = length(planetPos);
        const float sinHorizon = saturate(data.rPlanet / max(radius, 1.0));
        const float cosHorizon = -sqrt(max(0.0, 1.0 - sinHorizon * sinHorizon));
        const float mu = dot(planetPos, lightDir) / max(radius, 1.0);
        const float discWidth = max(0.00465 * sinHorizon, 1e-6);
        const float visibility = radius >= data.rPlanet ? smoothstep(-discWidth, discWidth, mu - cosHorizon) : 0.0;
        transmittance = lerp(1.0.xxx, PhysicalSkyTransmittance.SampleLevel(ClampSampler, PhysicalSkyTrLutUv(radius, mu), 0).rgb * visibility, data.trMix);
    }

    if (data.enableVolumetricClouds && data.volCloudLowThickness > 0.0) {
        const float3 shadowDirection = data.volCloudUseSun ? data.sunDir : lightDir;
        if (dot(shadowDirection, lightDir) > 0.99) {
            uint3 dimensions;
            PhysicalSkyCloudShadow.GetDimensions(dimensions.x, dimensions.y, dimensions.z);
            const float2 center = CloudShadowVolume::GridCenter(Camera.Position.xy, data.shadowVolumeRange, dimensions.xy);
            const float3 boundsMin = float3(center - 0.5 * data.shadowVolumeRange, data.volCloudLowBottom);
            const float3 boundsMax = float3(center + 0.5 * data.shadowVolumeRange, data.volCloudLowBottom + data.volCloudLowThickness);
            const float3 uvw = CloudShadowVolume::GetSampleUvw(position - float3(0, 0, data.zBottom), shadowDirection, boundsMin, boundsMax);
            transmittance *= CloudShadowVolume::SampleTransmittance(PhysicalSkyCloudShadow, ClampSampler, uvw);
        }
    }
    return transmittance;
}
#endif

#endif
