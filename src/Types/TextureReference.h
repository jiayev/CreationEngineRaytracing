#pragma once

struct TextureReference
{
	nvrhi::TextureHandle texture;
	eastl::shared_ptr<DescriptorHandle> descriptorHandle;

	TextureReference(nvrhi::TextureHandle texture, DescriptorTableManager* descriptorTableManager) :
		texture(texture)
	{
		descriptorHandle = eastl::make_shared<DescriptorHandle>(descriptorTableManager->CreateDescriptorHandle(nvrhi::BindingSetItem::Texture_SRV(0, texture)));
	}
};