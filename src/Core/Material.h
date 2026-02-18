#pragma once

#include <PCH.h>

#include "Types.h"
#include "Vertex.hlsli"
#include "Triangle.hlsli"
#include "Skinning.hlsli"
#include "Framework/DescriptorTableManager.h"

struct Material
{
	enum class AlphaMode : uint8_t
	{
		None = 0,
		Blend = 1,
		Test = 2
	};

	enum class PBRShaderFlags : uint32_t
	{
		HasEmissive = 1 << 0,
		HasDisplacement = 1 << 1,
		HasFeaturesTexture0 = 1 << 2,
		HasFeaturesTexture1 = 1 << 3,
		Subsurface = 1 << 4,
		TwoLayer = 1 << 5,
		ColoredCoat = 1 << 6,
		InterlayerParallax = 1 << 7,
		CoatNormal = 1 << 8,
		Fuzz = 1 << 9,
		HairMarschner = 1 << 10,
		Glint = 1 << 11,
		ProjectedGlint = 1 << 12,
	};

	REX::EnumSet<RE::BSShaderProperty::EShaderPropertyFlag, std::uint64_t> shaderFlags;
	RE::BSShader::Type shaderType;
	RE::BSShaderMaterial::Feature Feature;
	stl::enumeration<PBRShaderFlags, uint16_t> PBRFlags;

	AlphaMode alphaMode = AlphaMode::None;
};

DEFINE_ENUM_FLAG_OPERATORS(Material::AlphaMode);