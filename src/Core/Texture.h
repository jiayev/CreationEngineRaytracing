#pragma once

#include <PCH.h>
#include "Framework/DescriptorTableManager.h"

struct Texture
{
	eastl::weak_ptr<DescriptorHandle> texture;
	DescriptorHandle* defaultTexture;

	/*Texture() = default;
	Texture(DescriptorHandle* defaultTexture) : defaultTexture(defaultTexture) {}*/
	//Texture(eastl::shared_ptr<DescriptorHandle> defaultTexture) : defaultTexture(defaultTexture.get()) {}
};