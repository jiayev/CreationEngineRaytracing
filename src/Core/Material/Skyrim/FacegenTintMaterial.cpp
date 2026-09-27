#include "Core/Material/Skyrim/FacegenTintMaterial.h"

#include "Util.h"
#include "Core/MaterialManager.h"
#include "Renderer.h"

FacegenTintMaterial::FacegenTintMaterial(RE::BSShaderMaterial* shaderMaterial, uint64_t offset)
{
	m_Offset = offset;
	m_HashKey = shaderMaterial->hashKey;

	m_Data = eastl::make_unique<FacegenTintMaterialData>();

	UpdateData(shaderMaterial);
	UpdateTextures(shaderMaterial);
}

void FacegenTintMaterial::UpdateData(RE::BSShaderMaterial* shaderMaterial)
{
	LightingMaterial::UpdateData(shaderMaterial);

	auto tintMaterial = reinterpret_cast<RE::BSLightingShaderMaterialFacegenTint*>(shaderMaterial);

	auto tintData = reinterpret_cast<Data*>(m_Data.get());
	tintData->TintColor = Util::Math::Float3(tintMaterial->tintColor);
}

void FacegenTintMaterial::UpdateTextures(RE::BSShaderMaterial* shaderMaterial)
{
	LightingMaterial::UpdateTextures(shaderMaterial);

	auto* data = reinterpret_cast<Data*>(m_Data.get());
	if (m_RFAOSTexture.Update(m_RFAOSSource, Renderer::GetSingleton()->GetWhiteTextureDescriptor()))
		data->RFAOSTexture = m_RFAOSTexture.texture.GetDescriptorIndex();
}

void FacegenTintMaterial::PrepareTextures(RE::BSShaderMaterial* shaderMaterial)
{
	m_RFAOSSource.reset(MaterialManager::GetSkinTexture(shaderMaterial));
}
