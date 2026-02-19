#pragma once

#include <PCH.h>

struct ScreenPass
{
	nvrhi::GraphicsPipelineHandle m_pipeline;

	virtual void Init();
	void CreatePipeline();
	virtual void CreatePipelineDesc(nvrhi::GraphicsPipelineDesc& pipeline_desc);
	virtual void Render();
};