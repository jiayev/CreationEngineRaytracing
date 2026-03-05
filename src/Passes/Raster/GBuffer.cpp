#include "GBuffer.h"
#include "Renderer.h"
#include "Scene.h"

namespace Pass
{
	GBuffer::GBuffer(Renderer* renderer)
		: RenderPass(renderer)
	{
		m_RaytracingData = eastl::make_unique<RaytracingData>();

		m_RaytracingBuffer = renderer->GetDevice()->createBuffer(nvrhi::utils::CreateVolatileConstantBufferDesc(
			sizeof(RaytracingData), "Raytracing Data", Constants::MAX_CB_VERSIONS));

		m_LinearWrapSampler = renderer->GetDevice()->createSampler(
			nvrhi::SamplerDesc()
			.setAllAddressModes(nvrhi::SamplerAddressMode::Wrap)
			.setAllFilters(true));

		CreatePipeline();
	}

	void GBuffer::CreatePipeline()
	{
		auto* scene = Scene::GetSingleton();

		auto* sceneGraph = scene->GetSceneGraph();

		nvrhi::BindingSetDesc bindingSetDesc;
		bindingSetDesc.bindings = {
			nvrhi::BindingSetItem::ConstantBuffer(0, scene->GetCameraBuffer()),
			nvrhi::BindingSetItem::ConstantBuffer(1, m_RaytracingBuffer),
			nvrhi::BindingSetItem::ConstantBuffer(2, scene->GetFeatureBuffer()),
			nvrhi::BindingSetItem::PushConstants(3, sizeof(uint2)),
			nvrhi::BindingSetItem::StructuredBuffer_SRV(0, sceneGraph->GetInstanceBuffer()),
			nvrhi::BindingSetItem::StructuredBuffer_SRV(1, sceneGraph->GetMeshBuffer()),
			nvrhi::BindingSetItem::Sampler(0, m_LinearWrapSampler)
		};

		nvrhi::utils::CreateBindingSetAndLayout(GetRenderer()->GetDevice(), nvrhi::ShaderType::All, 0, bindingSetDesc, m_BindingLayout, m_BindingSet);

		eastl::vector<DxcDefine> defines = { { L"RASTER", L"" } };

		auto device = GetRenderer()->GetDevice();

		winrt::com_ptr<IDxcBlob> vertexBlob, pixelBlob;
		ShaderUtils::CompileShader(vertexBlob, L"data/shaders/GBufferRaster.hlsl", defines, L"vs_6_5", L"MainVS");
		ShaderUtils::CompileShader(pixelBlob, L"data/shaders/GBufferRaster.hlsl", defines, L"ps_6_5", L"MainPS");

		m_VertexShader = device->createShader({ nvrhi::ShaderType::Vertex, "", "MainVS" }, vertexBlob->GetBufferPointer(), vertexBlob->GetBufferSize());
		m_PixelShader = device->createShader({ nvrhi::ShaderType::Pixel, "", "MainPS" }, pixelBlob->GetBufferPointer(), pixelBlob->GetBufferSize());
	}

	void GBuffer::ResolutionChanged([[maybe_unused]] uint2 resolution)
	{
		m_AlbedoTexture = nullptr;
		m_NormalRoughnessTexture = nullptr;
		m_EmissiveMetallicTexture = nullptr;
		m_DepthTexture = nullptr;

		m_FrameBuffer = nullptr;
		m_GraphicsPipeline = nullptr;
	}

	void GBuffer::Execute(nvrhi::ICommandList* commandList)
	{
		auto* sceneGraph = Scene::GetSingleton()->GetSceneGraph();

		auto* renderer = GetRenderer();

		auto resolution = renderer->GetResolution();

		if (!m_FrameBuffer)
		{
			
			auto device = renderer->GetDevice();

			nvrhi::TextureDesc desc;
			desc.width = resolution.x;
			desc.height = resolution.y;
			desc.initialState = nvrhi::ResourceStates::RenderTarget;
			desc.isRenderTarget = true;
			desc.useClearValue = true;
			desc.clearValue = nvrhi::Color(0.f);
			desc.keepInitialState = true;
			desc.isTypeless = false;
			desc.isUAV = false;
			desc.mipLevels = 1;

			desc.format = nvrhi::Format::RGBA16_FLOAT;
			desc.debugName = "GBuffer Albedo";
			m_AlbedoTexture = device->createTexture(desc);

			desc.format = nvrhi::Format::R10G10B10A2_UNORM;
			desc.debugName = "GBuffer Normal/Roughness";
			m_NormalRoughnessTexture = device->createTexture(desc);

			desc.format = nvrhi::Format::RGBA16_FLOAT;
			desc.debugName = "GBuffer Emissive/Metallic";
			m_EmissiveMetallicTexture = device->createTexture(desc);

			const nvrhi::Format depthFormats[] = {
				nvrhi::Format::D24S8,
				nvrhi::Format::D32S8,
				nvrhi::Format::D32,
				nvrhi::Format::D16 };

			const nvrhi::FormatSupport depthFeatures =
				nvrhi::FormatSupport::Texture |
				nvrhi::FormatSupport::DepthStencil |
				nvrhi::FormatSupport::ShaderLoad;

			desc.format = nvrhi::utils::ChooseFormat(device, depthFeatures, depthFormats, std::size(depthFormats));
			desc.isTypeless = true;
			desc.initialState = nvrhi::ResourceStates::DepthWrite;
			desc.clearValue = nvrhi::Color(1.f);
			desc.debugName = "GBuffer Depth Texture";
			m_DepthTexture = device->createTexture(desc);

			auto frameBufferDesc = nvrhi::FramebufferDesc()
				.addColorAttachment(m_AlbedoTexture)
				.addColorAttachment(m_NormalRoughnessTexture)
				.addColorAttachment(m_EmissiveMetallicTexture)
				.setDepthAttachment(m_DepthTexture);

			m_FrameBuffer = device->createFramebuffer(frameBufferDesc);
		}

		const auto& fbinfo = m_FrameBuffer->getFramebufferInfo();

		if (!m_GraphicsPipeline)
		{
			nvrhi::GraphicsPipelineDesc pipelineDesc;
			pipelineDesc.VS = m_VertexShader;
			pipelineDesc.PS = m_PixelShader;
			pipelineDesc.primType = nvrhi::PrimitiveType::TriangleList;
			pipelineDesc.bindingLayouts = {
				m_BindingLayout,
				sceneGraph->GetTriangleDescriptors()->m_Layout,
				sceneGraph->GetVertexDescriptors()->m_Layout,
				sceneGraph->GetTextureDescriptors()->m_Layout
			};
			pipelineDesc.renderState.depthStencilState.depthTestEnable = true;
			pipelineDesc.renderState.depthStencilState.depthFunc = nvrhi::ComparisonFunc::LessOrEqual;
			pipelineDesc.renderState.rasterState.frontCounterClockwise = true;
			pipelineDesc.renderState.rasterState.setCullBack();

			m_GraphicsPipeline = GetRenderer()->GetDevice()->createGraphicsPipeline(pipelineDesc, fbinfo);
		}

		for (const auto& colorAttachment : m_FrameBuffer->getDesc().colorAttachments)
			commandList->clearTextureFloat(colorAttachment.texture, nvrhi::AllSubresources, nvrhi::Color(0.f));

		commandList->clearDepthStencilTexture(m_FrameBuffer->getDesc().depthAttachment.texture, nvrhi::AllSubresources, true, 1.f, true, 0);

		{
			auto& settings = Scene::GetSingleton()->m_Settings;

			m_RaytracingData->Roughness = settings.MaterialSettings.Roughness;
			m_RaytracingData->Metalness = settings.MaterialSettings.Metalness;

			m_RaytracingData->Emissive = settings.LightingSettings.Emissive;
			m_RaytracingData->Effect = settings.LightingSettings.Effect;
			m_RaytracingData->Sky = settings.LightingSettings.Sky;
			m_RaytracingData->EmittanceColor = float3(1.0f, 1.0f, 1.0f);

			commandList->writeBuffer(m_RaytracingBuffer, m_RaytracingData.get(), sizeof(RaytracingData));
		}

		nvrhi::GraphicsState state;
		state.pipeline = m_GraphicsPipeline;
		state.framebuffer = m_FrameBuffer;
		state.bindings = {
			m_BindingSet,
			sceneGraph->GetTriangleDescriptors()->m_DescriptorTable->GetDescriptorTable(),
			sceneGraph->GetVertexDescriptors()->m_DescriptorTable->GetDescriptorTable(),
			sceneGraph->GetTextureDescriptors()->m_DescriptorTable->GetDescriptorTable()
		};

		state.viewport.addViewportAndScissorRect(fbinfo.getViewport());

		commandList->setGraphicsState(state);

		const auto& instances = sceneGraph->GetInstances();

		for (uint i = 0; i < instances.size(); i++)
		{
			const auto& instance = instances[i];

			const auto& model = instance->model;

			for (uint m = 0; m < model->meshes.size(); m++)
			{
				auto constants = uint2(i, m);
				commandList->setPushConstants(&constants, sizeof(constants));

				nvrhi::DrawArguments args;
				args.vertexCount = model->meshes[m]->triangleCount * 3;
				args.instanceCount = 1;
				commandList->draw(args);
			}
		}

		auto region = nvrhi::TextureSlice{ 0, 0, 0, resolution.x, resolution.y, 1 };
		commandList->copyTexture(GetRenderer()->GetMainTexture(), region, m_AlbedoTexture, region);
	}
}