#pragma once

#include <PCH.h>

#include "Pass/RenderPass.h"
#include "CameraData.hlsli"
#include "ShaderUtils.h"
#include "framework/DescriptorTableManager.h"
#include "Util.h"

#include "Pass/Raytracing/Common/SceneTLAS.h"

namespace Pass::Raytracing
{
	class GlobalIllumination : public RenderPass
	{
		nvrhi::ShaderLibraryHandle m_ShaderLibrary;
		nvrhi::rt::PipelineHandle m_RayPipeline;
		nvrhi::rt::ShaderTableHandle m_ShaderTable;
		nvrhi::ShaderHandle m_ComputeShader;
		nvrhi::ComputePipelineHandle m_ComputePipeline;

		nvrhi::BindingLayoutHandle m_BindingLayout;
		nvrhi::BindingSetHandle m_BindingSet;

		nvrhi::SamplerHandle m_LinearWrapSampler;

		SceneTLAS* m_SceneTLAS;

		bool m_DirtyBindings = true;

		/*ResourceHandle m_DirectInput;
		ResourceHandle m_DiffuseOutput;
		ResourceHandle m_SpecularOutput;*/

	public:
		GlobalIllumination(Renderer* renderer, SceneTLAS* sceneTLAS);

		virtual void CreatePipeline() override;

		virtual void ResolutionChanged(uint2 resolution) override;

		void CreateRootSignature();

		bool CreateRayTracingPipeline();

		bool CreateComputePipeline();

		void CheckBindings();

		virtual void Execute(nvrhi::ICommandList* commandList) override;
	};
}