#pragma once

#include <PCH.h>

#include "Pass/RenderPass.h"
#include "CameraData.hlsli"
#include "ReSTIRGIData.hlsli"
#include "ShaderUtils.h"
#include "Util.h"

#include "Pass/Raytracing/Common/SceneTLAS.h"

namespace Pass
{
	class ReSTIRGI : public RenderPass
	{
		struct SubPass
		{
			nvrhi::ShaderHandle shader;
			nvrhi::ComputePipelineHandle pipeline;
			nvrhi::BindingLayoutHandle bindingLayout;
			nvrhi::BindingSetHandle bindingSet;
		};

		SubPass m_TemporalPass;
		SubPass m_SpatialPass;
		SubPass m_FinalShadingPass;

		SceneTLAS* m_SceneTLAS;

		// ReSTIR GI constant buffer
		nvrhi::BufferHandle m_ConstantBuffer;

		// GI Reservoir buffer (structured, double-buffered via array pitch)
		nvrhi::BufferHandle m_ReservoirBuffer;

		// Secondary surface textures (filled by path tracer, read by ReSTIR GI)
		nvrhi::TextureHandle m_SecondarySurfacePositionNormal;
		nvrhi::TextureHandle m_SecondarySurfaceRadiance;

		// Output texture
		nvrhi::TextureHandle m_RestirGIOutput;

		// ReSTIR GI parameters
		ReSTIRGIConstants m_GIConstants = {};

		uint32_t m_FrameIndex = 0;

		bool m_DirtyBindings = true;
		bool m_Enabled = true;

		static constexpr uint32_t RESERVOIR_BUFFER_COUNT = 3; // Triple buffering for temporal + spatial
		static constexpr uint32_t PACKED_RESERVOIR_SIZE = 32; // sizeof(PackedGIReservoir)
		static constexpr uint32_t RESERVOIR_BLOCK_SIZE = 16;

	public:
		ReSTIRGI(Renderer* renderer, SceneTLAS* sceneTLAS);

		virtual void CreatePipeline() override;

		virtual void ResolutionChanged(uint2 resolution) override;

		virtual void Execute(nvrhi::ICommandList* commandList) override;

		void CheckBindings();

		void CreateResources(uint2 resolution);

		void UpdateConstants(uint2 resolution);

		nvrhi::ITexture* GetSecondarySurfacePositionNormal() const { return m_SecondarySurfacePositionNormal; }
		nvrhi::ITexture* GetSecondarySurfaceRadiance() const { return m_SecondarySurfaceRadiance; }
		nvrhi::ITexture* GetOutput() const { return m_RestirGIOutput; }
	};
}
