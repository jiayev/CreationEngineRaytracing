#ifndef RESTIRGI_BINDINGS_HLSLI
#define RESTIRGI_BINDINGS_HLSLI

// ReSTIR GI secondary surface storage textures
// These are filled by the path tracer during the FILL pass and read by the ReSTIR GI passes
RWTexture2D<float4> u_SecondarySurfacePositionNormal : register(u10);
RWTexture2D<float4> u_SecondarySurfaceRadiance       : register(u11);

// Octahedral encoding helpers (from StablePlanes.hlsli)
// Duplicated here for standalone inclusion in non-PT shaders
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

void ReSTIRGI_StoreSecondarySurfacePositionAndNormal(uint2 pixPos, float3 worldPos, float3 normal)
{
    u_SecondarySurfacePositionNormal[pixPos] = float4(worldPos, asfloat(ReSTIR_NDirToOctUnorm32(normal)));
}

void ReSTIRGI_StorePrimarySurfaceScatterPdf(uint2 pixPos, float scatterPdf)
{
    u_SecondarySurfaceRadiance[pixPos].a = scatterPdf;
}

void ReSTIRGI_AddSecondarySurfaceRadiance(uint2 pixPos, float3 secondaryRadiance)
{
    u_SecondarySurfaceRadiance[pixPos].rgb += secondaryRadiance;
}

void ReSTIRGI_Clear(uint2 pixPos)
{
    u_SecondarySurfaceRadiance[pixPos] = float4(0, 0, 0, 0);
    ReSTIRGI_StoreSecondarySurfacePositionAndNormal(pixPos, float3(0, 0, 0), float3(0, 0, 0));
}

bool ReSTIRGI_IsEmpty(uint2 pixPos)
{
    return u_SecondarySurfaceRadiance[pixPos].a == 0;
}

#endif // RESTIRGI_BINDINGS_HLSLI
