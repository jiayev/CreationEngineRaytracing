#pragma once

#include "Constants.h"

#include "Types\CommunityShaders\BSLightingShaderMaterialPBR.h"

namespace Util
{
	namespace Material
	{
		float ShininessToRoughness(float shininess);

		stl::enumeration<PBRShaderFlags, uint32_t> GetPBRShaderFlags(const BSLightingShaderMaterialPBR* pbrMaterial);
	}
}