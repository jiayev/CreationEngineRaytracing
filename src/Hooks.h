#pragma once

namespace Hooks
{
#if defined(SKYRIM)
	struct TES_AttachModel
	{
		static void thunk(RE::TES* a1, RE::TESObjectREFR* refr, RE::TESObjectCELL* cell, void* queuedTree, char a5, RE::NiNode* a6);
		static inline REL::Relocation<decltype(thunk)> func;
	};

	struct Main_RenderPlayerView
	{
		static void thunk(void* a1, bool a2, bool a3);
		static inline REL::Relocation<decltype(thunk)> func;
	};

	struct CreateTextureFromDDS
	{
		static RE::NiSourceTexture* thunk(RE::BSResource::CompressedArchiveStream* a1, char* path, ID3D11ShaderResourceView* srv, char a4, bool a5);
		static inline REL::Relocation<decltype(thunk)> func;
	};
#elif defined(FALLOUT4)

#endif

	struct ID3D11Device_CreateTexture2D
	{
		static HRESULT WINAPI thunk(ID3D11Device* This, const D3D11_TEXTURE2D_DESC* pDesc, const D3D11_SUBRESOURCE_DATA* pInitialData, ID3D11Texture2D** ppTexture2D);
		static inline REL::Relocation<decltype(thunk)> func;
	};

	void Install();
	void InstallEarlyHooks();
	void InstallD3D11Hooks(ID3D11Device* device);
}
