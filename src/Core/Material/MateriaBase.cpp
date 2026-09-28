#include "Core/Material/MaterialBase.h"
#include "Core/MaterialManager.h"
#include "Util.h"
#include "Renderer.h"

MaterialBase::MaterialBase(RE::BSShaderMaterial* shaderMaterial, uint64_t offset)
{
	m_Offset = offset;
	m_HashKey = shaderMaterial->hashKey;

	m_Data = eastl::make_unique<Data>();

	UpdateData(shaderMaterial);
}

MaterialBase::~MaterialBase()
{
	auto managerPtr = m_Manager.lock();
	if (!managerPtr) {
		logger::error("Missing material manager during material destruction");
		return;
	}

	managerPtr->Release(m_Offset);
}

void MaterialBase::SetManager(const eastl::shared_ptr<MaterialManager>& managerPtr)
{
	m_Manager = managerPtr;
}

void MaterialBase::UpdateData(RE::BSShaderMaterial* shaderMaterial)
{
	auto data = m_Data.get();

	data->Type = Type::Lighting;
	data->Feature = static_cast<uint16_t>(shaderMaterial->GetFeature());

	data->TexCoordOffset = Util::Math::Float2(shaderMaterial->texCoordOffset[0]);
	data->TexCoordScale = Util::Math::Float2(shaderMaterial->texCoordScale[0]);
}

void MaterialBase::UpdateTextures([[ maybe_unused ]] RE::BSShaderMaterial* shaderMaterial)
{

}

void MaterialBase::Update(RE::BSShaderMaterial* shaderMaterial)
{
	const auto frameIndex = Renderer::GetSingleton()->GetFrameIndex();
	auto lastUpdate = m_LastUpdate.load(std::memory_order_relaxed);
	if (lastUpdate == frameIndex ||
		!m_LastUpdate.compare_exchange_strong(lastUpdate, frameIndex, std::memory_order_relaxed))
		return;

	UpdateData(shaderMaterial);

	auto manager = m_Manager.lock();
	if (!manager) {
		logger::error("Missing material manager during material update");
		return;
	}

	manager->Update(this);
}

bool MaterialBase::RefreshTextures(RE::BSShaderMaterial* shaderMaterial)
{
	constexpr uint64_t refreshInterval = 120;
	const auto frameIndex = Renderer::GetSingleton()->GetFrameIndex();
	if (m_LastTextureUpdate != Constants::INVALID_FRAME_INDEX && frameIndex - m_LastTextureUpdate < refreshInterval)
		return false;

	m_LastTextureUpdate = frameIndex;
	PrepareTextures(shaderMaterial);
	UpdateTextures(shaderMaterial);

	if (auto manager = m_Manager.lock())
		manager->Update(this);

	return true;
}
