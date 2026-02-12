#include "State.h"

extern "C" CERT_API void Initialize(ID3D12Device5* device, ID3D12CommandQueue* commandQueue)
{
	auto* state = State::GetSingleton();
	state->Initialize(device, commandQueue);
}

extern "C" CERT_API void AttachModel(RE::TESForm* form, RE::NiAVObject* root)
{
	auto* state = State::GetSingleton();
	state->AttachModel(form, root);
}