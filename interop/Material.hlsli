#ifndef MATERIAL_HLSL
#define MATERIAL_HLSL

#   if defined(FALLOUT4)
#       include "interop/MaterialFallout4.hlsli"
#   else
#       include "interop/MaterialSkyrim.hlsli"
#   endif

#endif