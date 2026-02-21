#ifndef SHAPE_HLSL
#define SHAPE_HLSL

#include "Interop.h"
#include "interop/Material.hlsli"

INTEROP_DATA_STRUCT(Mesh, 4)
{
    INTEROP_DATA_TYPE(Material) Material;   
    uint GeometryIdx;
    uint2 Pad;
    INTEROP_ROW_MAJOR(float3x4) Transform;
};
VALIDATE_ALIGNMENT(MeshData, 4);

#endif