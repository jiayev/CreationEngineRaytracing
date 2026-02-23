#pragma once

class BSLightingShaderMaterialPBRLandscape : public RE::BSLightingShaderMaterialBase
{
public:
	inline static constexpr auto FEATURE = static_cast<RE::BSShaderMaterial::Feature>(33);

	inline static constexpr auto BaseColorTexture = static_cast<RE::BSTextureSet::Texture>(0);
	inline static constexpr auto NormalTexture = static_cast<RE::BSTextureSet::Texture>(1);
	inline static constexpr auto DisplacementTexture = static_cast<RE::BSTextureSet::Texture>(3);
	inline static constexpr auto RmaosTexture = static_cast<RE::BSTextureSet::Texture>(5);

	inline static constexpr uint32_t NumTiles = 6;

	inline bool HasGlint() const 
	{
		{
			for (uint32_t textureIndex = 0; textureIndex < numLandscapeTextures; ++textureIndex) {
				if (glintParameters[textureIndex].enabled) {
					return true;
				}
			}
			return false;
		}
	}

	inline static std::unordered_map<BSLightingShaderMaterialPBRLandscape*, std::array<void*, NumTiles>> All;

	// members
	std::uint32_t numLandscapeTextures = 0;

	// We need terrainOverlayTexture, terrainNoiseTexture, landBlendParams, terrainTexOffsetX, terrainTexOffsetY, terrainTexFade
	// to be at the same offsets as in vanilla BSLightingShaderMaterialLandscape so we arrange PBR fields in specific way
	std::array<RE::NiPointer<RE::NiSourceTexture>, NumTiles> landscapeBaseColorTextures;
	std::array<bool, NumTiles> isPbr;
	std::array<float, NumTiles> roughnessScales;

	RE::NiPointer<RE::NiSourceTexture> terrainOverlayTexture;
	RE::NiPointer<RE::NiSourceTexture> terrainNoiseTexture;
	RE::NiColorA landBlendParams;

	std::array<RE::NiPointer<RE::NiSourceTexture>, NumTiles> landscapeNormalTextures;

	float terrainTexOffsetX = 0.f;
	float terrainTexOffsetY = 0.f;
	float terrainTexFade = 0.f;

	std::array<RE::NiPointer<RE::NiSourceTexture>, NumTiles> landscapeDisplacementTextures;
	std::array<RE::NiPointer<RE::NiSourceTexture>, NumTiles> landscapeRMAOSTextures;
	std::array<float, NumTiles> displacementScales;
	std::array<float, NumTiles> specularLevels;

	std::array<GlintParameters, NumTiles> glintParameters;
};
static_assert(offsetof(BSLightingShaderMaterialPBRLandscape, terrainOverlayTexture) == offsetof(RE::BSLightingShaderMaterialLandscape, terrainOverlayTexture));
static_assert(offsetof(BSLightingShaderMaterialPBRLandscape, terrainNoiseTexture) == offsetof(RE::BSLightingShaderMaterialLandscape, terrainNoiseTexture));
static_assert(offsetof(BSLightingShaderMaterialPBRLandscape, landBlendParams) == offsetof(RE::BSLightingShaderMaterialLandscape, landBlendParams));
static_assert(offsetof(BSLightingShaderMaterialPBRLandscape, terrainTexOffsetX) == offsetof(RE::BSLightingShaderMaterialLandscape, terrainTexOffsetX));
static_assert(offsetof(BSLightingShaderMaterialPBRLandscape, terrainTexOffsetY) == offsetof(RE::BSLightingShaderMaterialLandscape, terrainTexOffsetY));
static_assert(offsetof(BSLightingShaderMaterialPBRLandscape, terrainTexFade) == offsetof(RE::BSLightingShaderMaterialLandscape, terrainTexFade));
