#pragma once

#include "Pass/RenderPass.h"

class IRenderNode
{
public:
	//virtual void AddNode(IRenderNode renderNode);

	virtual void ResolutionChanged(uint2 resolution) = 0;

	virtual void SettingsChanged(const Settings& settings) = 0;

	virtual void Execute(nvrhi::ICommandList* commandList) = 0;
};