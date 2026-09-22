#pragma once

#include "Renderer.h"
#include "Core/Mesh/BaseMesh.h"
#include "Core/BLASCluster.h"
#include "Core/Mesh/SubIndexSegmentMesh.h"
#include "Scene.h"
#include "Events/ITLASUpdateListener.h"

class TopLevelAS
{
	eastl::array<nvrhi::rt::AccelStructHandle, Constants::MAX_FRAMES_IN_FLIGHT> m_Handle;
	eastl::vector<nvrhi::rt::InstanceDesc> m_InstanceDescs;
	eastl::array<nvrhi::BufferHandle, Constants::MAX_FRAMES_IN_FLIGHT> m_InstanceBuffers;
	uint32_t m_NumInstances[Constants::MAX_FRAMES_IN_FLIGHT] = {0};

	eastl::vector<ITLASUpdateListener*> m_Listeners;

	void NotifyResized()
	{
		for (auto& listener : m_Listeners)
			listener->OnTLASResized(*this);
	}

public:
	nvrhi::rt::IAccelStruct* GetHandle()
	{
		return m_Handle[Renderer::GetSingleton()->GetCurrentSlot()];
	}

	void AddListener(ITLASUpdateListener* listener)
	{
		m_Listeners.push_back(listener);
	}

	void RemoveListener(ITLASUpdateListener* listener)
	{
		eastl::erase(m_Listeners, listener);
	}

	void Update(nvrhi::ICommandList* commandList, const eastl::vector<BLASCluster*>& clusters)
	{
		auto* renderer = Renderer::GetSingleton();
		const auto ringSlot = renderer->GetCurrentSlot();
		auto* compactor = renderer->GetBLASCompactor();
		m_InstanceDescs.clear();
		m_InstanceDescs.reserve(clusters.size());

		for (const auto& cluster : clusters) {
			if (!cluster->Valid())
				continue;

			const auto firstInstance = m_InstanceDescs.size();
			cluster->AppendInstanceDescs(m_InstanceDescs);
			if (compactor) {
				const auto address = cluster->GetBLASDeviceAddress();
				for (size_t i = firstInstance; i < m_InstanceDescs.size(); ++i)
					m_InstanceDescs[i].blasDeviceAddress = address;
				if (cluster->GetBLAS()) {
					compactor->Retain(cluster->GetBLAS());
					commandList->setAccelStructState(cluster->GetBLAS(), nvrhi::ResourceStates::AccelStructBuildBlas);
				}
				if (cluster->GetCompaction())
					compactor->Retain(cluster->GetCompaction());
			}
		}

		auto* scene = Scene::GetSingleton();

		const uint32_t numInstances = scene->GetSceneGraph()->GetNumInstancesFrame();
		const uint32_t topLevelInstances = static_cast<uint32_t>(m_InstanceDescs.size());

		if (numInstances != topLevelInstances)
			logger::critical("TopLevelAS::UpdateInstance - Mismatch in number of instances ({}) and TLAS instances ({}).", numInstances, topLevelInstances);

		if (!m_Handle[ringSlot] || topLevelInstances > m_NumInstances[ringSlot] - Constants::TLAS_INSTANCES_THRESHOLD) {
			float topLevelInstancesRatio = std::ceil(topLevelInstances / static_cast<float>(Constants::TLAS_INSTANCES_STEP));

			uint32_t topLevelMaxInstances = static_cast<uint32_t>(topLevelInstancesRatio) * Constants::TLAS_INSTANCES_STEP;

			m_NumInstances[ringSlot] = std::max(topLevelMaxInstances + Constants::TLAS_INSTANCES_STEP, Constants::TLAS_INSTANCES_MIN);

			logger::debug("TopLevelAS::UpdateInstance - TLAS Max Instances [{}]: {}", ringSlot, m_NumInstances[ringSlot]);

			nvrhi::rt::AccelStructDesc tlasDesc;
			tlasDesc.isTopLevel = true;
			tlasDesc.topLevelMaxInstances = m_NumInstances[ringSlot];
			tlasDesc.buildFlags = nvrhi::rt::AccelStructBuildFlags::PreferFastTrace;
			m_Handle[ringSlot] = Renderer::GetSingleton()->GetDevice()->createAccelStruct(tlasDesc);

			NotifyResized();
		}

		const auto& markers = scene->m_Settings.DebugSettings.Markers;

		if (markers)
			commandList->beginMarker("TLAS Update");

		if (compactor) {
			const uint64_t bytes = uint64_t(m_NumInstances[ringSlot]) * sizeof(nvrhi::rt::InstanceDesc);
			auto& buffer = m_InstanceBuffers[ringSlot];
			if (!buffer || buffer->getDesc().byteSize < bytes) {
				buffer = renderer->GetDevice()->createBuffer(nvrhi::BufferDesc()
					.setByteSize(bytes).setIsAccelStructBuildInput(true)
					.enableAutomaticStateTracking(nvrhi::ResourceStates::AccelStructBuildInput)
					.setDebugName("TLAS Instances"));
			}
			if (!m_InstanceDescs.empty())
				commandList->writeBuffer(buffer, m_InstanceDescs.data(), m_InstanceDescs.size() * sizeof(nvrhi::rt::InstanceDesc));
			compactor->Retain(buffer);
			commandList->commitBarriers();
			commandList->buildTopLevelAccelStructFromBuffer(m_Handle[ringSlot], buffer, 0, m_InstanceDescs.size(), nvrhi::rt::AccelStructBuildFlags::PreferFastTrace);
		} else {
			commandList->buildTopLevelAccelStruct(m_Handle[ringSlot], m_InstanceDescs.data(), m_InstanceDescs.size(), nvrhi::rt::AccelStructBuildFlags::PreferFastTrace);
		}

		if (markers)
			commandList->endMarker();
	}
};
