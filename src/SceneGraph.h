#pragma once

#include "core/Model.h"
#include "core/Instance.h"

#include "Mesh.hlsli"
#include "Instance.hlsli"

#include "Constants.h"

class BindlessLayout : public nvrhi::RefCounter<nvrhi::IBindingLayout>
{
public:
	nvrhi::BindlessLayoutDesc desc;
	nvrhi::static_vector<D3D12_DESCRIPTOR_RANGE1, 32> descriptorRanges;
	D3D12_ROOT_PARAMETER1 rootParameter{};

	BindlessLayout(const nvrhi::BindlessLayoutDesc& desc);

	const nvrhi::BindingLayoutDesc* getDesc() const override { return nullptr; }
	const nvrhi::BindlessLayoutDesc* getBindlessDesc() const override { return &desc; }
};

class SceneGraph
{
	eastl::unordered_map<RE::BSDismemberSkinInstance*, eastl::vector<Mesh*>> dismemberReferences;

	// Model Path, Model data ptr
	eastl::unordered_map<eastl::string, eastl::unique_ptr<Model>> m_Models;

	// Root node ptr, Instance data
	eastl::vector<eastl::unique_ptr<Instance>> m_Instances;

	eastl::unordered_map<RE::NiAVObject*, Instance*> m_InstanceNodes;
	eastl::unordered_map<RE::FormID, eastl::vector<Instance*>> m_InstancesFormIDs;

	eastl::array<MeshData, Constants::NUM_MESHES_MAX> m_MeshData;
	nvrhi::BufferHandle m_MeshDataBuffer;

	eastl::array<InstanceData, Constants::NUM_INSTANCES_MAX> m_InstanceData;
	nvrhi::BufferHandle m_InstanceDataBuffer;

	eastl::deque<eastl::string> m_MSNConvertionQueue;

	struct BindlessTable
	{
		nvrhi::BindingLayoutHandle m_Layout;
		eastl::shared_ptr<DescriptorTableManager> m_DescriptorTable;

		BindlessTable(nvrhi::DeviceHandle device, nvrhi::BindlessLayoutDesc desc, bool resizeToMaxCapacity)
		{
			m_Layout = device->createBindlessLayout(desc);

			/*auto bindingLayout = reinterpret_cast<BindlessLayout*>(m_Layout.Get());

			for (auto& descriptorRange : bindingLayout->descriptorRanges)
			{
				descriptorRange.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;
			}*/

			m_DescriptorTable = eastl::make_shared<DescriptorTableManager>(device, m_Layout, resizeToMaxCapacity);
		}
	};
	
	eastl::unique_ptr<BindlessTable> m_TriangleDescriptors;
	eastl::unique_ptr<BindlessTable> m_VertexDescriptors;
	eastl::unique_ptr<BindlessTable> m_TextureDescriptors;

	void CreateModelInternal(RE::TESForm* form, const char* path, RE::NiAVObject* node);
	void AddInstance(RE::FormID formID, RE::NiAVObject* node, eastl::string path);

public:
	void Initialize();

	inline auto& GetTriangleDescriptors() const { return m_TriangleDescriptors; }
	inline auto& GetVertexDescriptors() const { return m_VertexDescriptors; }
	inline auto& GetTextureDescriptors() const { return m_TextureDescriptors; }

	inline auto& GetMeshDataBuffer() const { return m_MeshDataBuffer; }
	inline auto& GetInstanceDataBuffer() const { return m_InstanceDataBuffer; }

	inline auto& GetInstances() const { return m_Instances; }

	void Update(nvrhi::ICommandList* commandList);

	void CreateModel(RE::TESForm* form, const char* model, RE::NiAVObject* root);
	void CreateActorModel(RE::Actor* actor, const char* name, RE::NiAVObject* root);
	void CreateLandModel(RE::TESObjectLAND* land);
};