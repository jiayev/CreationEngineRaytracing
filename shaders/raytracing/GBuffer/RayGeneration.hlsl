#if DEBUG_TRACE_HEATMAP
#   define NV_SHADER_EXTN_SLOT u127
#   define NV_SHADER_EXTN_REGISTER_SPACE space0
#   include "include/nvapi/nvHLSLExtns.h"

#   include "include/nvapi/Profiling.hlsli"
#endif

#include "raytracing/GBuffer/Registers.hlsli"

#include "include/Common.hlsli"
#include "raytracing/include/Common.hlsli"
#include "raytracing/include/Payload.hlsli"
#include "raytracing/include/Geometry.hlsli"

#include "raytracing/include/Materials/TexLODHelpers.hlsli"

#include "include/Surface.hlsli"
#include "include/SurfaceMaker.hlsli"

#include "include/Lighting.hlsli"

#include "raytracing/include/Transparency.hlsli"

#include "Raytracing/Include/SHARC/Sharc.hlsli"
#include "Raytracing/Include/SHARC/SHaRCHelper.hlsli"

#if defined(GROUP_TILING)
#   define DXC_STATIC_DISPATCH_GRID_DIM 1
#   include "include/ThreadGroupTilingX.hlsli"
#endif

#ifndef THREAD_GROUP_SIZE
#define THREAD_GROUP_SIZE (32)
#endif

#if USE_RAY_QUERY
[numthreads(THREAD_GROUP_SIZE, THREAD_GROUP_SIZE, 1)]
#   if defined(GROUP_TILING)
void Main(uint2 GTid : SV_GroupThreadID, uint2 Gid : SV_GroupID)
#   else
void Main(uint2 idx : SV_DispatchThreadID)
#   endif
#else
[shader("raygeneration")]
void Main()
#endif
{
#if USE_RAY_QUERY
    uint2 size = Camera.RenderSize;  
#   if defined(GROUP_TILING)    
    uint2 idx = ThreadGroupTilingX((uint2)ceil(size / THREAD_GROUP_SIZE), THREAD_GROUP_SIZE.xx, 32, GTid.xy, Gid.xy);
#   endif
    if (any(idx >= size))
        return;
#else    
    uint2 idx = DispatchRaysIndex().xy;
    uint2 size = DispatchRaysDimensions().xy;
#endif
    
    float3 viewDir = GetView(idx, size, Camera.ProjInverse);
    
    RayDesc ray = SetupPrimaryRay(viewDir, Camera);
    
    const float3 direction = ray.Direction;
    
    uint randomSeed = InitRandomSeed(idx, size, Camera.FrameIndex);
    
#if DEBUG_TRACE_HEATMAP       
    uint startTime = NvGetSpecial( NV_SPECIALOP_GLOBAL_TIMER_LO );
#endif
    
    Payload payload = TraceRayStandard(Scene, ray, randomSeed);

#if DEBUG_TRACE_HEATMAP       
    uint endTime = NvGetSpecial( NV_SPECIALOP_GLOBAL_TIMER_LO );
    uint deltaTime = timediff(startTime, endTime);
    
    // Scale the time delta value to [0,1]
    static float heatmapScale = 300000.0f; // somewhat arbitrary scaling factor, experiment to find a value that works well in your app 
    float deltaTimeScaled =  clamp( (float)deltaTime / heatmapScale, 0.0f, 1.0f );

    // Compute the heatmap color and write it to the output pixel
    Output[idx] = float4(temperature(deltaTimeScaled), 1.0f);     
    
    return;
 #endif
    
    if (!payload.Hit())
    {
        Depth[idx] = 0.0f;
        MotionVectors[idx] = float2(0.0f, 0.0f);       
        Albedo[idx] = float4(0.0f, 0.0f, 0.0f, 0.0f);   
        NormalRoughness[idx] = float4(0.0f, 0.0f, 0.0f, 0.0f);   
        EmissiveMetallic[idx] = float4(0.0f, 0.0f, 0.0f, 0.0f);   
    }
          
    RayCone rayCone = RayCone::make(Raytracing.PixelConeSpreadAngle * payload.hitDistance, Raytracing.PixelConeSpreadAngle);   
    
    float3 position = Camera.Position.xyz + direction * payload.hitDistance;
    
    Instance instance;
    Material material;

    Surface surface = SurfaceMaker::make(position, payload, direction, rayCone, instance, material);
    
    float3 viewPos = viewDir * payload.hitDistance;
    
    float3 prevViewDir = GetView(idx, size, Camera.ProjInverse);
    
    float prevHitDistance = Depth[idx] / prevViewDir.y;
    
    float3 preViewPos = prevViewDir * prevHitDistance;
    
    Depth[idx] = viewPos.y;
    MotionVectors[idx] = (preViewPos.xyz - viewPos.xyz) * float3(0.5f * Camera.RenderSize.x, -0.5f * Camera.RenderSize.y, 1.0f);    
    Albedo[idx] = float4(surface.Albedo, 1.0f);   
    NormalRoughness[idx] = float4(surface.Normal * 0.5f + 0.5f, surface.Roughness);   
    EmissiveMetallic[idx] = float4(surface.Emissive, surface.Metallic);      
}