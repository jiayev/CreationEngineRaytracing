#pragma once

struct RendererParams {
	ID3D11Device5* d3d11Device = nullptr;
	ID3D12Device5* d3d12Device = nullptr;
	ID3D12CommandQueue* commandQueue = nullptr;
	ID3D12CommandQueue* computeCommandQueue = nullptr;
	ID3D12CommandQueue* copyCommandQueue = nullptr;
};