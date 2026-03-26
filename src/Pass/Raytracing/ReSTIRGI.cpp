#include "ReSTIRGI.h"
#include "Renderer.h"
#include "Scene.h"

namespace Pass
{
	ReSTIRGI::ReSTIRGI(Renderer* renderer, SceneTLAS* sceneTLAS)
		: RenderPass(renderer), m_SceneTLAS(sceneTLAS)
	{
		auto resolution = renderer->GetResolution();
		CreateResources(resolution);
		CreatePipeline();
	}

	void ReSTIRGI::CreateResources(uint2 resolution)
	{
		auto device = GetRenderer()->GetDevice();
		const uint width = resolution.x;
		const uint height = resolution.y;

		// Constant buffer
		m_ConstantBuffer = device->createBuffer(nvrhi::utils::CreateVolatileConstantBufferDesc(
			sizeof(ReSTIRGIConstants), "ReSTIR GI Constants", 16));

		// Reservoir buffer: block-linear layout, triple buffered
		uint blockRowPitch = (width + RESERVOIR_BLOCK_SIZE - 1) / RESERVOIR_BLOCK_SIZE;
		uint blockColPitch = (height + RESERVOIR_BLOCK_SIZE - 1) / RESERVOIR_BLOCK_SIZE;
		uint reservoirsPerArray = blockRowPitch * blockColPitch * RESERVOIR_BLOCK_SIZE * RESERVOIR_BLOCK_SIZE;
		uint totalReservoirs = reservoirsPerArray * RESERVOIR_BUFFER_COUNT;

		{
			nvrhi::BufferDesc desc;
			desc.byteSize = totalReservoirs * PACKED_RESERVOIR_SIZE;
			desc.structStride = PACKED_RESERVOIR_SIZE;
			desc.canHaveUAVs = true;
			desc.keepInitialState = true;
			desc.initialState = nvrhi::ResourceStates::UnorderedAccess;
			desc.debugName = "GI Reservoir Buffer";
			m_ReservoirBuffer = device->createBuffer(desc);
		}

		// Store buffer params
		m_GIConstants.reservoirBufferParams.reservoirBlockRowPitch = blockRowPitch;
		m_GIConstants.reservoirBufferParams.reservoirArrayPitch = reservoirsPerArray;

		// Secondary surface textures (written by PT, read by ReSTIR GI)
		{
			nvrhi::TextureDesc desc;
			desc.width = width;
			desc.height = height;
			desc.format = nvrhi::Format::RGBA32_FLOAT;
			desc.isUAV = true;
			desc.keepInitialState = true;
			desc.initialState = nvrhi::ResourceStates::UnorderedAccess;
			desc.debugName = "SecondarySurfacePositionNormal";
			m_SecondarySurfacePositionNormal = device->createTexture(desc);
		}

		{
			nvrhi::TextureDesc desc;
			desc.width = width;
			desc.height = height;
			desc.format = nvrhi::Format::RGBA32_FLOAT;
			desc.isUAV = true;
			desc.keepInitialState = true;
			desc.initialState = nvrhi::ResourceStates::UnorderedAccess;
			desc.debugName = "SecondarySurfaceRadiance";
			m_SecondarySurfaceRadiance = device->createTexture(desc);
		}

		// Output texture
		{
			nvrhi::TextureDesc desc;
			desc.width = width;
			desc.height = height;
			desc.format = nvrhi::Format::RGBA16_FLOAT;
			desc.isUAV = true;
			desc.keepInitialState = true;
			desc.initialState = nvrhi::ResourceStates::UnorderedAccess;
			desc.debugName = "ReSTIR GI Output";
			m_RestirGIOutput = device->createTexture(desc);
		}
	}

	void ReSTIRGI::CreatePipeline()
	{
		auto device = GetRenderer()->GetDevice();
		eastl::vector<DxcDefine> defines = {};

		// --- Temporal Resampling Pass ---
		{
			nvrhi::BindingLayoutDesc layoutDesc;
			layoutDesc.visibility = nvrhi::ShaderType::Compute;
			layoutDesc.bindings = {
				nvrhi::BindingLayoutItem::VolatileConstantBuffer(0),  // Camera
				nvrhi::BindingLayoutItem::VolatileConstantBuffer(1),  // GIConst
				nvrhi::BindingLayoutItem::Texture_SRV(0),             // DepthTexture
				nvrhi::BindingLayoutItem::Texture_SRV(1),             // NormalRoughnessTexture
				nvrhi::BindingLayoutItem::Texture_SRV(2),             // MotionVectorsTexture
				nvrhi::BindingLayoutItem::StructuredBuffer_UAV(0),    // GIReservoirBuffer
				nvrhi::BindingLayoutItem::Texture_UAV(10),            // SecondarySurfacePositionNormal
				nvrhi::BindingLayoutItem::Texture_UAV(11),            // SecondarySurfaceRadiance
			};
			m_TemporalPass.bindingLayout = device->createBindingLayout(layoutDesc);

			winrt::com_ptr<IDxcBlob> blob;
			ShaderUtils::CompileShader(blob, L"data/shaders/raytracing/RTXDI/GITemporalResampling.hlsl", defines, L"cs_6_5");
			m_TemporalPass.shader = device->createShader({ nvrhi::ShaderType::Compute, "", "Main" }, blob->GetBufferPointer(), blob->GetBufferSize());

			m_TemporalPass.pipeline = device->createComputePipeline(
				nvrhi::ComputePipelineDesc()
				.setComputeShader(m_TemporalPass.shader)
				.addBindingLayout(m_TemporalPass.bindingLayout));
		}

		// --- Spatial Resampling Pass ---
		{
			nvrhi::BindingLayoutDesc layoutDesc;
			layoutDesc.visibility = nvrhi::ShaderType::Compute;
			layoutDesc.bindings = {
				nvrhi::BindingLayoutItem::VolatileConstantBuffer(0),  // Camera
				nvrhi::BindingLayoutItem::VolatileConstantBuffer(1),  // GIConst
				nvrhi::BindingLayoutItem::Texture_SRV(0),             // DepthTexture
				nvrhi::BindingLayoutItem::Texture_SRV(1),             // NormalRoughnessTexture
				nvrhi::BindingLayoutItem::StructuredBuffer_UAV(0),    // GIReservoirBuffer
			};
			m_SpatialPass.bindingLayout = device->createBindingLayout(layoutDesc);

			winrt::com_ptr<IDxcBlob> blob;
			ShaderUtils::CompileShader(blob, L"data/shaders/raytracing/RTXDI/GISpatialResampling.hlsl", defines, L"cs_6_5");
			m_SpatialPass.shader = device->createShader({ nvrhi::ShaderType::Compute, "", "Main" }, blob->GetBufferPointer(), blob->GetBufferSize());

			m_SpatialPass.pipeline = device->createComputePipeline(
				nvrhi::ComputePipelineDesc()
				.setComputeShader(m_SpatialPass.shader)
				.addBindingLayout(m_SpatialPass.bindingLayout));
		}

		// --- Final Shading Pass ---
		{
			nvrhi::BindingLayoutDesc layoutDesc;
			layoutDesc.visibility = nvrhi::ShaderType::Compute;
			layoutDesc.bindings = {
				nvrhi::BindingLayoutItem::VolatileConstantBuffer(0),  // Camera
				nvrhi::BindingLayoutItem::VolatileConstantBuffer(1),  // GIConst
				nvrhi::BindingLayoutItem::Texture_SRV(0),             // DepthTexture
				nvrhi::BindingLayoutItem::Texture_SRV(1),             // NormalRoughnessTexture
				nvrhi::BindingLayoutItem::Texture_SRV(2),             // DiffuseAlbedoTexture
				nvrhi::BindingLayoutItem::RayTracingAccelStruct(3),   // SceneBVH
				nvrhi::BindingLayoutItem::StructuredBuffer_UAV(0),    // GIReservoirBuffer
				nvrhi::BindingLayoutItem::Texture_UAV(1),             // OutputColor
				nvrhi::BindingLayoutItem::Texture_UAV(5),             // StablePlanesHeader
				nvrhi::BindingLayoutItem::StructuredBuffer_UAV(6),    // StablePlanesBuffer
				nvrhi::BindingLayoutItem::Texture_UAV(7),             // StableRadiance
				nvrhi::BindingLayoutItem::Texture_UAV(10),            // SecondarySurfacePositionNormal (for MIS)
				nvrhi::BindingLayoutItem::Texture_UAV(11),            // SecondarySurfaceRadiance (for MIS)
			};
			m_FinalShadingPass.bindingLayout = device->createBindingLayout(layoutDesc);

			winrt::com_ptr<IDxcBlob> blob;
			ShaderUtils::CompileShader(blob, L"data/shaders/raytracing/RTXDI/GIFinalShading.hlsl", defines, L"cs_6_5");
			m_FinalShadingPass.shader = device->createShader({ nvrhi::ShaderType::Compute, "", "Main" }, blob->GetBufferPointer(), blob->GetBufferSize());

			m_FinalShadingPass.pipeline = device->createComputePipeline(
				nvrhi::ComputePipelineDesc()
				.setComputeShader(m_FinalShadingPass.shader)
				.addBindingLayout(m_FinalShadingPass.bindingLayout));
		}
	}

	void ReSTIRGI::ResolutionChanged(uint2 resolution)
	{
		CreateResources(resolution);
		m_DirtyBindings = true;
	}

	void ReSTIRGI::SettingsChanged(const Settings& settings)
	{
		auto& gi = settings.ReSTIRGISettings;
		m_Enabled = gi.Enabled;
		m_EnableTemporalResampling = gi.EnableTemporalResampling;
		m_EnableBoilingFilter = gi.EnableBoilingFilter;
		m_BoilingFilterStrength = gi.BoilingFilterStrength;
		m_MaxHistoryLength = static_cast<uint32_t>(gi.MaxHistoryLength);
		m_NumSpatialSamples = static_cast<uint32_t>(gi.NumSpatialSamples);
		m_SpatialSamplingRadius = gi.SpatialSamplingRadius;
		m_EnableFinalVisibility = gi.EnableFinalVisibility;
		m_EnableFinalMIS = gi.EnableFinalMIS;
		m_DenoisingEnabled = settings.DebugSettings.StablePlanes;
	}

	void ReSTIRGI::CheckBindings()
	{
		if (!m_DirtyBindings)
			return;

		auto* renderer = GetRenderer();
		auto* scene = Scene::GetSingleton();
		auto* rts = renderer->GetRenderTargets();
		auto* rrInput = renderer->GetRRInput();

		// --- Temporal Resampling Bindings ---
		{
			nvrhi::BindingSetDesc desc;
			desc.bindings = {
				nvrhi::BindingSetItem::ConstantBuffer(0, scene->GetCameraBuffer()),
				nvrhi::BindingSetItem::ConstantBuffer(1, m_ConstantBuffer),
				nvrhi::BindingSetItem::Texture_SRV(0, renderer->m_PTDepth),
				nvrhi::BindingSetItem::Texture_SRV(1, rts->normalRoughness),
				nvrhi::BindingSetItem::Texture_SRV(2, renderer->m_PTMotionVectors),
				nvrhi::BindingSetItem::StructuredBuffer_UAV(0, m_ReservoirBuffer),
				nvrhi::BindingSetItem::Texture_UAV(10, m_SecondarySurfacePositionNormal),
				nvrhi::BindingSetItem::Texture_UAV(11, m_SecondarySurfaceRadiance),
			};
			m_TemporalPass.bindingSet = renderer->GetDevice()->createBindingSet(desc, m_TemporalPass.bindingLayout);
		}

		// --- Spatial Resampling Bindings ---
		{
			nvrhi::BindingSetDesc desc;
			desc.bindings = {
				nvrhi::BindingSetItem::ConstantBuffer(0, scene->GetCameraBuffer()),
				nvrhi::BindingSetItem::ConstantBuffer(1, m_ConstantBuffer),
				nvrhi::BindingSetItem::Texture_SRV(0, renderer->m_PTDepth),
				nvrhi::BindingSetItem::Texture_SRV(1, rts->normalRoughness),
				nvrhi::BindingSetItem::StructuredBuffer_UAV(0, m_ReservoirBuffer),
			};
			m_SpatialPass.bindingSet = renderer->GetDevice()->createBindingSet(desc, m_SpatialPass.bindingLayout);
		}

		// --- Final Shading Bindings ---
		{
			auto* sp = renderer->GetStablePlanes();

			nvrhi::BindingSetDesc desc;
			desc.bindings = {
				nvrhi::BindingSetItem::ConstantBuffer(0, scene->GetCameraBuffer()),
				nvrhi::BindingSetItem::ConstantBuffer(1, m_ConstantBuffer),
				nvrhi::BindingSetItem::Texture_SRV(0, renderer->m_PTDepth),
				nvrhi::BindingSetItem::Texture_SRV(1, rts->normalRoughness),
				nvrhi::BindingSetItem::Texture_SRV(2, rrInput->diffuseAlbedo),
				nvrhi::BindingSetItem::RayTracingAccelStruct(3, m_SceneTLAS->GetTopLevelAS().GetHandle()),
				nvrhi::BindingSetItem::StructuredBuffer_UAV(0, m_ReservoirBuffer),
				nvrhi::BindingSetItem::Texture_UAV(1, renderer->GetMainTexture()),
				nvrhi::BindingSetItem::Texture_UAV(5, sp->header),
				nvrhi::BindingSetItem::StructuredBuffer_UAV(6, sp->buffer),
				nvrhi::BindingSetItem::Texture_UAV(7, sp->stableRadiance),
				nvrhi::BindingSetItem::Texture_UAV(10, m_SecondarySurfacePositionNormal),
				nvrhi::BindingSetItem::Texture_UAV(11, m_SecondarySurfaceRadiance),
			};
			m_FinalShadingPass.bindingSet = renderer->GetDevice()->createBindingSet(desc, m_FinalShadingPass.bindingLayout);
		}

		m_DirtyBindings = false;
	}

	void ReSTIRGI::UpdateConstants(uint2 resolution)
	{
		m_GIConstants.frameDim = resolution;
		m_GIConstants.frameIndex = m_FrameIndex;
		m_GIConstants.rayEpsilon = 1e-3f;

		// Stable planes addressing (for GI final shading write-back)
		m_GIConstants.denoisingEnabled = m_DenoisingEnabled ? 1 : 0;
		m_GIConstants.stablePlaneRenderWidth = resolution.x;

		// Temporal resampling enabled by default
		m_GIConstants.enableTemporalResampling = m_EnableTemporalResampling ? 1 : 0;
		m_GIConstants.varyAgeThreshold = 1;

		// Runtime params
		m_GIConstants.runtimeParams.activeCheckerboardField = 0;
		m_GIConstants.runtimeParams.neighborOffsetMask = 0xFF;

		// Apply user-configurable parameters
		m_GIConstants.temporalResamplingParams.maxHistoryLength = m_MaxHistoryLength;
		m_GIConstants.spatialResamplingParams.numSpatialSamples = m_NumSpatialSamples;
		m_GIConstants.spatialResamplingParams.spatialSamplingRadius = m_SpatialSamplingRadius;

		// Temporal params
		m_GIConstants.temporalResamplingParams.depthThreshold = 0.1f;
		m_GIConstants.temporalResamplingParams.normalThreshold = 0.5f;
		m_GIConstants.temporalResamplingParams.maxHistoryLength = m_MaxHistoryLength;
		m_GIConstants.temporalResamplingParams.maxReservoirAge = 50;
		m_GIConstants.temporalResamplingParams.enablePermutationSampling = 1;
		m_GIConstants.temporalResamplingParams.enableFallbackSampling = 1;
		m_GIConstants.temporalResamplingParams.enableBoilingFilter = m_EnableBoilingFilter ? 1 : 0;
		m_GIConstants.temporalResamplingParams.boilingFilterStrength = m_BoilingFilterStrength;
		m_GIConstants.temporalResamplingParams.uniformRandomNumber = m_FrameIndex;

		// Spatial params
		m_GIConstants.spatialResamplingParams.spatialDepthThreshold = 0.1f;
		m_GIConstants.spatialResamplingParams.spatialNormalThreshold = 0.5f;
		m_GIConstants.spatialResamplingParams.numSpatialSamples = m_NumSpatialSamples;
		m_GIConstants.spatialResamplingParams.spatialSamplingRadius = m_SpatialSamplingRadius;

		// Final shading
		m_GIConstants.finalShadingParams.enableFinalVisibility = m_EnableFinalVisibility ? 1 : 0;
		m_GIConstants.finalShadingParams.enableFinalMIS = m_EnableFinalMIS ? 1 : 0;

		// Buffer indices: ping-pong between frames
		// Buffer 0 = temporal output (current frame), Buffer 1 = spatial output, Buffer 2 = temporal input (previous frame)
		uint even = (m_FrameIndex & 1);
		m_GIConstants.bufferIndices.temporalResamplingInputBufferIndex = even ? 2 : 0;
		m_GIConstants.bufferIndices.temporalResamplingOutputBufferIndex = even ? 0 : 2;
		m_GIConstants.bufferIndices.spatialResamplingInputBufferIndex = even ? 0 : 2;
		m_GIConstants.bufferIndices.spatialResamplingOutputBufferIndex = 1;
		m_GIConstants.bufferIndices.finalShadingInputBufferIndex = 1;
	}

	void ReSTIRGI::Execute(nvrhi::ICommandList* commandList)
	{
		if (!m_Enabled)
			return;

		CheckBindings();

		auto resolution = Renderer::GetSingleton()->GetDynamicResolution();

		UpdateConstants(resolution);

		// Upload constants
		commandList->writeBuffer(m_ConstantBuffer, &m_GIConstants, sizeof(m_GIConstants));

		auto threadGroupSize = Util::Math::GetDispatchCount(resolution, 8);

		// --- 1. Temporal Resampling ---
		{
			nvrhi::ComputeState state;
			state.pipeline = m_TemporalPass.pipeline;
			state.bindings = { m_TemporalPass.bindingSet };
			commandList->setComputeState(state);
			commandList->dispatch(threadGroupSize.x, threadGroupSize.y);
		}

		// UAV barrier: temporal writes reservoir buffer, spatial reads it
		commandList->setBufferState(m_ReservoirBuffer, nvrhi::ResourceStates::UnorderedAccess);
		commandList->commitBarriers();

		// --- 2. Spatial Resampling ---
		{
			nvrhi::ComputeState state;
			state.pipeline = m_SpatialPass.pipeline;
			state.bindings = { m_SpatialPass.bindingSet };
			commandList->setComputeState(state);
			commandList->dispatch(threadGroupSize.x, threadGroupSize.y);
		}

		// UAV barrier: spatial writes reservoir buffer, final reads it
		commandList->setBufferState(m_ReservoirBuffer, nvrhi::ResourceStates::UnorderedAccess);
		commandList->commitBarriers();

		// --- 3. Final Shading ---
		{
			nvrhi::ComputeState state;
			state.pipeline = m_FinalShadingPass.pipeline;
			state.bindings = { m_FinalShadingPass.bindingSet };
			commandList->setComputeState(state);
			commandList->dispatch(threadGroupSize.x, threadGroupSize.y);
		}

		m_FrameIndex++;
	}
}
