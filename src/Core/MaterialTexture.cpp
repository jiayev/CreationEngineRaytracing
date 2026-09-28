#include "Core/MaterialTexture.h"
#include "Core/MaterialManager.h"
#include "Utils/Adapter.h"

bool MaterialTexture::Update(const RE::NiPointer<RE::NiSourceTexture>& a_sourceTexture, const eastl::shared_ptr<DescriptorHandle>& a_defaultDescriptor, TextureType a_type)
{
	return Update(reinterpret_cast<RE::NiTexture*>(a_sourceTexture.get()), a_defaultDescriptor, a_type);
}

bool MaterialTexture::Update(RE::NiTexture* a_sourceTexture, const eastl::shared_ptr<DescriptorHandle>& a_defaultDescriptor, TextureType a_type)
{
	auto* rendererTexture = Util::Adapter::GetRendererTexture(a_sourceTexture);
	auto* resource = rendererTexture ? rendererTexture->texture : nullptr;
	if (sourceTexture == a_sourceTexture && sourceResource == resource && texture.texture &&
		(!resource || texture.texture != a_defaultDescriptor))
		return false;

	texture = MaterialManager::GetTexture(a_sourceTexture, a_defaultDescriptor, a_type);
	sourceTexture = a_sourceTexture;
	sourceResource = resource;

	return true;
}
