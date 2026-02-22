#include "SceneGraph.h"

#include "core/Mesh.h"

#include "Renderer.h"
#include "Util.h"

void SceneGraph::Initialize()
{
	auto device = Renderer::GetSingleton()->GetDevice();

	// Mesh Data Buffer
	m_MeshDataBuffer = Util::CreateStructuredBuffer<MeshData>(device, Constants::NUM_MESHES_MAX, "Mesh Data Buffer");

	// Instance Data Buffer
	m_InstanceDataBuffer = Util::CreateStructuredBuffer<InstanceData>(device, Constants::NUM_INSTANCES_MAX, "Instance Data Buffer");

	// Triangle bindless descriptor table
	{
		nvrhi::BindlessLayoutDesc bindlessLayoutDesc;
		bindlessLayoutDesc.visibility = nvrhi::ShaderType::All;
		bindlessLayoutDesc.firstSlot = 0;
		bindlessLayoutDesc.maxCapacity = Constants::NUM_MESHES_MAX;
		bindlessLayoutDesc.registerSpaces = {
			nvrhi::BindingLayoutItem::StructuredBuffer_SRV(1).setSize(UINT_MAX)
		};

		m_TriangleDescriptors = eastl::make_unique<BindlessTable>(device, bindlessLayoutDesc);
	}

	// Vertex bindless descriptor table
	{
		nvrhi::BindlessLayoutDesc bindlessLayoutDesc;
		bindlessLayoutDesc.visibility = nvrhi::ShaderType::All;
		bindlessLayoutDesc.firstSlot = 0;
		bindlessLayoutDesc.maxCapacity = Constants::NUM_MESHES_MAX;
		bindlessLayoutDesc.registerSpaces = {
			nvrhi::BindingLayoutItem::StructuredBuffer_SRV(2).setSize(UINT_MAX)
		};

		m_VertexDescriptors = eastl::make_unique<BindlessTable>(device, bindlessLayoutDesc);;
	}

	// Texture bindless descriptor table
	{
		nvrhi::BindlessLayoutDesc bindlessLayoutDesc;
		bindlessLayoutDesc.visibility = nvrhi::ShaderType::All;
		bindlessLayoutDesc.firstSlot = 0;
		bindlessLayoutDesc.maxCapacity = Constants::NUM_TEXTURES_MIN;
		bindlessLayoutDesc.registerSpaces = {
			nvrhi::BindingLayoutItem::Texture_SRV(3).setSize(UINT_MAX)
		};

		m_TextureDescriptors = eastl::make_unique<BindlessTable>(device, bindlessLayoutDesc);
	}
}

void SceneGraph::Update(nvrhi::ICommandList* commandList)
{
	for (auto& [path, model] : m_Models)
	{
		model->Update();
	}

	uint32_t meshIndex = 0;
	uint32_t instanceIndex = 0;

	for (auto& instance : m_Instances)
	{
		instance->Update();

		uint32_t firstMeshIndex = meshIndex;

		for (auto& mesh : instance->model->meshes)
		{
			m_MeshData[meshIndex] = mesh->GetData();
			meshIndex++;
		}

		// No visible shape in instance
		if (meshIndex == firstMeshIndex)
			continue;

		m_InstanceData[instanceIndex] = {
			instance->transform,
			LightData(),
			firstMeshIndex
		};

		instanceIndex++;
	}

	commandList->writeBuffer(m_MeshDataBuffer, m_MeshData.data(), meshIndex * sizeof(MeshData));
	commandList->writeBuffer(m_InstanceDataBuffer, m_InstanceData.data(), instanceIndex * sizeof(InstanceData));
}

void SceneGraph::CreateModel(RE::TESForm* form, const char* model, RE::NiAVObject* root)
{
	if (!root) {
		logger::warn("[RT] CreateModel - NULL root object for model: {}", model ? model : "unknown");
		return;
	}

	const REL::Relocation<const RE::NiRTTI*> rtti{ RE::NiMultiTargetTransformController::Ni_RTTI };
	auto* controller = reinterpret_cast<RE::NiMultiTargetTransformController*>(root->GetController(rtti.get()));

	if (controller) {
		eastl::hash_set<RE::NiNode*> parents;
		eastl::hash_set<RE::NiAVObject*> targets;

		for (uint16_t i = 0; i < controller->numInterps; i++) {
			auto* target = controller->targets[i];

			if (!target)
				continue;

			auto [it, emplaced] = targets.emplace(target);
			parents.emplace(target->parent);

			if (!emplaced)
				continue;

			CreateModelInternal(form, std::format("{}_{}", model, target->name.c_str()).c_str(), target);
		}

		for (auto* parent : parents) {
			for (auto& child : parent->GetChildren()) {
				if (targets.find(child.get()) != targets.end())
					continue;

				CreateModelInternal(form, std::format("{}_{}_{}", model, child->name.c_str(), child->parentIndex).c_str(), child.get());
			}
		}

		return;
	}

	CreateModelInternal(form, model, root);
}

void SceneGraph::CreateActorModel(RE::Actor* actor, const char* name, RE::NiAVObject* root)
{
	Util::Traversal::ScenegraphFadeNodes(root, [&](RE::BSFadeNode* fadeNode) -> RE::BSVisit::BSVisitControl {
		const bool isRoot = (fadeNode == root);

		auto fadeNodeName = std::format("{}.{}", name, fadeNode->name.c_str());
		CreateModelInternal(actor, isRoot ? name : fadeNodeName.c_str(), fadeNode);

		return RE::BSVisit::BSVisitControl::kContinue;
	});
}

void SceneGraph::CreateLandModel(RE::TESObjectLAND* land)
{
	auto* cell = land->parentCell;

	if (!cell->IsExteriorCell())
		return;

	auto& runtimeData = cell->GetRuntimeData();

	auto* exteriorData = runtimeData.cellData.exterior;

	auto* loadedData = land->loadedData;

	if (!loadedData || !loadedData->mesh)
		return;

	logger::trace("[RT] TESObjectLAND_Attach3D - {}", std::format("Landscape_{}_{}", exteriorData->cellX, exteriorData->cellY).c_str());

	for (uint i = 0; i < 4; i++) {
		auto mesh = loadedData->mesh[i];

		if (!mesh)
			continue;

		CreateModelInternal(land, std::format("Landscape_{}_{}_Quad_{}", exteriorData->cellX, exteriorData->cellY, i).c_str(), mesh);
	}
}

void SceneGraph::CreateModelInternal(RE::TESForm* form, const char* path, RE::NiAVObject* pRoot)
{
	if (!pRoot)
		return;

	if (!path || strlen(path) == 0)
		return;

	if (m_InstanceNodes.find(pRoot) != m_InstanceNodes.end()) {
		logger::warn("[RT] CreateModel \"{}\" - Instance/Model for 0x{:08X} already present.", path, reinterpret_cast<uintptr_t>(pRoot));
		return;
	}

	auto formID = form->GetFormID();

	// We only need one buffer per model
	if (m_Models.find(path) != m_Models.end()) {
		AddInstance(formID, pRoot, path);
		return;
	}

	logger::trace("[RT] CreateModel \"{}\"", typeid(*pRoot).name());

	const auto* bsxFlags = pRoot->GetExtraData<RE::BSXFlags>("BSX");

	if (bsxFlags) {
		if (static_cast<int32_t>(bsxFlags->value) & static_cast<int32_t>(RE::BSXFlags::Flag::kEditorMarker))
			return;

		logger::debug("[RT] CreateModel - BSX Flags [0x{:x}]: {}", bsxFlags->value, Util::GetFlagsString<RE::BSXFlags::Flag>(bsxFlags->value));
	}

	logger::debug("[RT] CreateModel - Path: {}, FormID [0x{:08X}], NiNode [0x{:08X}]: {}", path, formID, reinterpret_cast<uintptr_t>(pRoot), pRoot->name);

	auto formType = form->GetFormType();

	auto rootWorldInverse = pRoot->world.Invert();

	eastl::vector<eastl::unique_ptr<Mesh>> meshes;

	// Will traverse and skip non-root fade nodes (and their children)
	auto* validFadeNode = (formType == RE::FormType::ActorCharacter ? reinterpret_cast<RE::BSFadeNode*>(pRoot) : nullptr);

	Util::Traversal::ScenegraphRTGeometries(pRoot, validFadeNode, [&](RE::BSGeometry* pGeometry)->RE::BSVisit::BSVisitControl {
		const char* name = pGeometry->name.c_str();

		logger::trace("\t\t[RT] CreateModel::TraverseScenegraphGeometries - {}", name);

		const auto& geometryType = pGeometry->GetType();

		if (geometryType.none(RE::BSGeometry::Type::kTriShape, RE::BSGeometry::Type::kDynamicTriShape)) {
			logger::warn("\t\t[RT] CreateModel::TraverseScenegraphGeometries - Unsupported Geometry: {} for {}", magic_enum::enum_name(geometryType.get()), name);
			return RE::BSVisit::BSVisitControl::kContinue;
		}

		const auto& geometryRuntimeData = pGeometry->GetGeometryRuntimeData();

		auto* effect = geometryRuntimeData.properties[RE::BSGeometry::States::kEffect].get();

		if (!effect) {
			logger::debug("\t\t[RT] CreateModel::TraverseScenegraphGeometries - No Effect");
			return RE::BSVisit::BSVisitControl::kContinue;
		}

		bool isLightingShader = netimmerse_cast<RE::BSLightingShaderProperty*>(effect) != nullptr;
		bool isEffectShader = netimmerse_cast<RE::BSEffectShaderProperty*>(effect) != nullptr;

		// Only lighting and effect shader for now
		if (!isLightingShader && !isEffectShader) {
			logger::warn("\t\t[RT] CreateModel::TraverseScenegraphGeometries - Unsupported shader type: {}", effect->GetRTTI()->name);
			return RE::BSVisit::BSVisitControl::kContinue;
		}

		auto shaderProperty = netimmerse_cast<RE::BSShaderProperty*>(effect);
		bool skinned = shaderProperty && shaderProperty->flags.any(RE::BSShaderProperty::EShaderPropertyFlag::kSkinned);

		auto& geomFlags = pGeometry->GetFlags();

		if (geomFlags.any(RE::NiAVObject::Flag::kHidden) && !skinned) {
			logger::debug("\t\t[RT] CreateModel::TraverseScenegraphGeometries - Is Hidden");
			return RE::BSVisit::BSVisitControl::kContinue;
		}

		auto flags = Mesh::Flags::None;

		// Landscape needs special handling of triangles
		if (formType == RE::FormType::Land)
			flags |= Mesh::Flags::Landscape;

		if (geometryType.all(RE::BSGeometry::Type::kDynamicTriShape))
			flags |= Mesh::Flags::Dynamic;

		float3x4 localToRoot;
		XMStoreFloat3x4(&localToRoot, Util::GetXMFromNiTransform(rootWorldInverse * pGeometry->world));

		if (auto* triShapeRD = geometryRuntimeData.rendererData) {  // Non-Skinned
			auto* pTriShape = netimmerse_cast<RE::BSTriShape*>(pGeometry);

			const auto& triShapeRuntime = pTriShape->GetTrishapeRuntimeData();

			if (triShapeRuntime.vertexCount == 0) {
				logger::error("\t\t[RT] CreateModel::TraverseScenegraphGeometries - Vertex count of 0 for {}: {}", path ? path : "N/A", name ? name : "N/A");
				return RE::BSVisit::BSVisitControl::kContinue;
			}

			if (triShapeRuntime.triangleCount == 0) {
				logger::error("\t\t[RT] CreateModel::TraverseScenegraphGeometries - Triangle count of 0 for {}: {}", path ? path : "N/A", name ? name : "N/A");
				return RE::BSVisit::BSVisitControl::kContinue;
			}

			auto mesh = eastl::make_unique<Mesh>(flags, name, pGeometry, localToRoot);

			mesh->BuildMesh(triShapeRD, triShapeRuntime.vertexCount, triShapeRuntime.triangleCount, 0);
			mesh->BuildMaterial(geometryRuntimeData, formID);

			meshes.push_back(eastl::move(mesh));
		}
		else if (auto* skinInstance = geometryRuntimeData.skinInstance.get()) {  // Skinned
			auto& skinPartition = skinInstance->skinPartition;

			if (!skinPartition) {
				logger::warn("\t\t[RT] CreateModel::TraverseScenegraphGeometries - Invalid SkinPartition");
				return RE::BSVisit::BSVisitControl::kContinue;
			}

			if (skinPartition->vertexCount == 0) {
				logger::error("\t\t[RT] CreateModel::TraverseScenegraphGeometries - Vertex count of 0 for {}: {}", path ? path : "N/A", name ? name : "N/A");
				return RE::BSVisit::BSVisitControl::kContinue;
			}

			const auto skinNumPartitions = skinPartition->numPartitions;

			logger::debug("\t\t[RT] CreateModel::TraverseScenegraphGeometries - Partitions: {}, VertexCount: {}, Unk24: [0x{:X}]", skinNumPartitions, skinPartition->vertexCount, skinPartition->unk24);

			// This looks diabolical
			static REL::Relocation<const RE::NiRTTI*> dismemberRTTI{ RE::BSDismemberSkinInstance::Ni_RTTI };

			eastl::vector<RE::BSDismemberSkinInstance::Data> dismemberData(skinNumPartitions, { true, false, 0 });

			decltype(dismemberReferences.begin()) it;
			bool emplacedDismemberRef = false;

			if (skinInstance->GetRTTI() == dismemberRTTI.get()) {
				auto* dismemberSkinInstance = reinterpret_cast<RE::BSDismemberSkinInstance*>(skinInstance);

				auto& dismemberRuntime = dismemberSkinInstance->GetRuntimeData();

				const auto dismemberNumPartitions = static_cast<uint32_t>(dismemberRuntime.numPartitions);

				if (skinNumPartitions != dismemberNumPartitions)
					logger::error("\t\t[RT] CreateModel::TraverseScenegraphGeometries - Skin and Dismember partition count mismatch");

				std::memcpy(dismemberData.data(), dismemberRuntime.partitions, dismemberNumPartitions * sizeof(RE::BSDismemberSkinInstance::Data));

				eastl::tie(it, emplacedDismemberRef) = dismemberReferences.try_emplace(dismemberSkinInstance, eastl::vector<Mesh*>(skinNumPartitions));
			}

			for (size_t i = 0; i < skinPartition->partitions.size(); i++) {
				auto& partition = skinPartition->partitions[i];
				auto& dismemberPartition = dismemberData[i];

				// Fix for modded geometry
				if (partition.triangles == 0) {
					logger::error("\t\t[RT] CreateModel::TraverseScenegraphGeometries - Triangle count of 0 for {}: {}", path ? path : "N/A", name ? name : "N/A");
					continue;
				}

				// Fix for modded geometry
				if (partition.bonesPerVertex > 0)
					flags |= Mesh::Flags::Skinned;

				auto mesh = eastl::make_unique<Mesh>(flags, name, pGeometry, localToRoot, dismemberPartition.editorVisible, dismemberPartition.slot);

				// Diabolical Part II
				if (emplacedDismemberRef)
					it->second[i] = mesh.get();

				mesh->BuildMesh(partition.buffData, skinPartition->vertexCount, partition.triangles, partition.bonesPerVertex);
				mesh->BuildMaterial(geometryRuntimeData, formID);

				meshes.push_back(eastl::move(mesh));
			}
		}

		return RE::BSVisit::BSVisitControl::kContinue;
		});

	if (auto shapeCount = meshes.size(); shapeCount > 0) {
		auto model = eastl::make_unique<Model>(path, pRoot, meshes);

		auto& modelName = model->m_Name;

		auto [it, emplaced] = m_Models.try_emplace(modelName, eastl::move(model));

		if (emplaced) {
			auto* modelPtr = it->second.get();

			if (modelPtr->ShouldQueueMSNConversion())
				m_MSNConvertionQueue.emplace_back(modelName);

			modelPtr->CreateBuffers(this, Renderer::GetSingleton()->GetCommandList());
			modelPtr->BuildBLAS(Renderer::GetSingleton()->GetCommandList());

			AddInstance(formID, pRoot, modelName);

			logger::debug("[RT] CreateModel - Commited {} TriShapes to [0x{:08X}]", shapeCount, reinterpret_cast<uintptr_t>(modelPtr));
		}
		else {
			logger::warn("[RT] CreateModel - Emplace failed for {} TriShapes", shapeCount);
		}
	}
	else {
		logger::debug("[RT] CreateModel - No TriShapes to commit");
	}
}

void SceneGraph::AddInstance(RE::FormID formID, RE::NiAVObject* node, eastl::string path)
{
	logger::debug("[RT] AddInstance [0x{:08X}] - {}, Path: {}", formID, node->name, path);

	auto instanceNodeIt = m_InstanceNodes.find(node);
	if (instanceNodeIt != m_InstanceNodes.end())
		return;

	auto modelIt = m_Models.find(path);
	if (modelIt == m_Models.end())
		return;


	auto [instanceIt, emplaced] = m_InstanceNodes.try_emplace(node, nullptr);
	if (!emplaced)
		return;

	auto instance = eastl::make_unique<Instance>(formID, node, modelIt->second.get());

	if (auto nodesIt = m_InstancesFormIDs.find(formID); nodesIt != m_InstancesFormIDs.end()) {
		nodesIt->second.push_back(instance.get());
	}
	else {
		m_InstancesFormIDs.try_emplace(formID, eastl::vector<Instance*>{ instance.get() });
	}

	instanceIt->second = instance.get();

	m_Instances.emplace_back(eastl::move(instance));

	modelIt->second->AddRef();
}