#ifndef FACEGEN_TINT_MATERIAL_DATA_HLSL
#define FACEGEN_TINT_MATERIAL_DATA_HLSL

#include "interop/Interop.h"
#include "interop/Material/Skyrim/LightingMaterialData.hlsli"

INTEROP_STRUCT(FacegenTintMaterialDataExtra, 4)
{
    half3 TintColor;
    uint16_t RFAOSTexture;
};
VALIDATE_ALIGNMENT(FacegenTintMaterialDataExtra, 4);
VALIDATE_SIZE(FacegenTintMaterialDataExtra, 8);
VALIDATE_OFFSET(FacegenTintMaterialDataExtra, RFAOSTexture, 6);

INTEROP_STRUCT(FacegenTintMaterialData : LightingMaterialData, 4)
{
    half3 TintColor;
    uint16_t RFAOSTexture;
};
VALIDATE_ALIGNMENT(FacegenTintMaterialData, 4);
VALIDATE_SIZE(FacegenTintMaterialData, sizeof(LightingMaterialData) + sizeof(FacegenTintMaterialDataExtra));

#endif // FACEGEN_TINT_MATERIAL_DATA_HLSL
