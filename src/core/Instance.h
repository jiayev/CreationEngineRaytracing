#pragma once

#include "core/Model.h"

struct Instance
{
	enum State : uint8_t
	{
		Hidden = 1 << 0,
		Detached = 1 << 1
	};

	// Instance form id
	RE::FormID formID;

	// Model ptr
	Model* model;

	// Used for BLAS instance
	float3x4 transform;

	// Makes sure we only update once per frame
	uint64_t lastUpdate = 0;

	nvrhi::rt::InstanceDesc GetInstanceDesc() const
	{
		nvrhi::rt::InstanceDesc instanceDesc;
		instanceDesc.bottomLevelAS = model->blas;
		assert(instanceDesc.bottomLevelAS);
		instanceDesc.instanceMask = 1;
		instanceDesc.instanceID = 0;
		memcpy(instanceDesc.transform, transform.m, sizeof(transform.m));
		return instanceDesc;
	}
};