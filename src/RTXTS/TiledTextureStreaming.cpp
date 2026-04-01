#include "TiledTextureStreaming.h"

#include <d3d12.h>
#include <nvrhi/d3d12.h>
#include <algorithm>
#include <cassert>
#include <array>
#include <map>

namespace RTXTS
{
	TiledTextureStreaming::TiledTextureStreaming() = default;

	TiledTextureStreaming::~TiledTextureStreaming()
	{
		Shutdown();
	}

	void TiledTextureStreaming::Initialize(nvrhi::IDevice* device, const TiledTextureStreamingConfig& config)
	{
		if (m_Initialized)
			return;

		m_Device = device;
		m_Config = config;

		// Create the TTM instance
		rtxts::TiledTextureManagerDesc ttmDesc = {};
		ttmDesc.heapTilesCapacity = config.heapSizeInTiles;
		m_TTM = std::shared_ptr<rtxts::TiledTextureManager>(
			CreateTiledTextureManager(ttmDesc));

		// Create the tile upload buffer (enough for maxTileUploadsPerFrame tiles)
		{
			nvrhi::BufferDesc bufferDesc = {};
			bufferDesc.byteSize = static_cast<uint64_t>(config.maxTileUploadsPerFrame) * D3D12_TILED_RESOURCE_TILE_SIZE_IN_BYTES;
			bufferDesc.cpuAccess = nvrhi::CpuAccessMode::Write;
			bufferDesc.initialState = nvrhi::ResourceStates::CopySource;
			bufferDesc.keepInitialState = true;
			bufferDesc.debugName = "RTXTS Tile Upload Buffer";
			m_TileUploadBuffer = device->createBuffer(bufferDesc);
		}

		uint32_t mipReqBufferSize = Constants::NUM_TEXTURES_MAX * sizeof(uint32_t);

		// Create software mip request buffer (one uint32 per possible texture)
		{
			nvrhi::BufferDesc bufferDesc = {};
			bufferDesc.byteSize = mipReqBufferSize;
			bufferDesc.canHaveRawViews = true;
			bufferDesc.canHaveUAVs = true;
			bufferDesc.initialState = nvrhi::ResourceStates::UnorderedAccess;
			bufferDesc.keepInitialState = true;
			bufferDesc.debugName = "RTXTS Mip Request Buffer";
			m_MipRequestBuffer = device->createBuffer(bufferDesc);
		}

		// Create clear buffer (filled with 0xFF = "no mip requested")
		{
			nvrhi::BufferDesc bufferDesc = {};
			bufferDesc.byteSize = mipReqBufferSize;
			bufferDesc.cpuAccess = nvrhi::CpuAccessMode::Write;
			bufferDesc.initialState = nvrhi::ResourceStates::CopySource;
			bufferDesc.keepInitialState = true;
			bufferDesc.debugName = "RTXTS Mip Request Clear Buffer";
			m_MipRequestBufferClear = device->createBuffer(bufferDesc);

			uint32_t* clearData = static_cast<uint32_t*>(device->mapBuffer(m_MipRequestBufferClear, nvrhi::CpuAccessMode::Write));
			if (clearData) {
				std::memset(clearData, 0xFF, mipReqBufferSize);
				device->unmapBuffer(m_MipRequestBufferClear);
			}
		}

		// Create readback buffer
		{
			nvrhi::BufferDesc bufferDesc = {};
			bufferDesc.byteSize = mipReqBufferSize;
			bufferDesc.cpuAccess = nvrhi::CpuAccessMode::Read;
			bufferDesc.initialState = nvrhi::ResourceStates::CopyDest;
			bufferDesc.keepInitialState = true;
			bufferDesc.debugName = "RTXTS Mip Request Readback";
			m_MipRequestReadback = device->createBuffer(bufferDesc);
		}

		// Create MinMip flat buffer (StructuredBuffer<float>, one per texture slot)
		{
			m_MinMipData.resize(Constants::NUM_TEXTURES_MAX, 0.0f);

			nvrhi::BufferDesc bufferDesc = {};
			bufferDesc.byteSize = Constants::NUM_TEXTURES_MAX * sizeof(float);
			bufferDesc.structStride = sizeof(float);
			bufferDesc.initialState = nvrhi::ResourceStates::ShaderResource;
			bufferDesc.keepInitialState = true;
			bufferDesc.debugName = "RTXTS MinMip Buffer";
			m_MinMipBuffer = device->createBuffer(bufferDesc);

			// Upload buffer for MinMip data
			nvrhi::BufferDesc uploadDesc = {};
			uploadDesc.byteSize = Constants::NUM_TEXTURES_MAX * sizeof(float);
			uploadDesc.cpuAccess = nvrhi::CpuAccessMode::Write;
			uploadDesc.initialState = nvrhi::ResourceStates::CopySource;
			uploadDesc.keepInitialState = true;
			uploadDesc.debugName = "RTXTS MinMip Upload Buffer";
			m_MinMipUploadBuffer = device->createBuffer(uploadDesc);
		}

		// Scratch buffer for TTM MinMip data
		m_TileMinMipScratch.resize(4096, 0xFF);

		// Create binding layout for space 6
		{
			nvrhi::BindingLayoutDesc layoutDesc;
			layoutDesc.visibility = nvrhi::ShaderType::All;
			layoutDesc.registerSpace = RTXTS_REGISTER_SPACE;
			layoutDesc.bindings = {
				nvrhi::BindingLayoutItem::StructuredBuffer_SRV(0),  // t0, space6: MinMipValues
				nvrhi::BindingLayoutItem::RawBuffer_UAV(0)          // u0, space6: MipRequestBuffer
			};
			m_BindingLayout = device->createBindingLayout(layoutDesc);
		}

		m_DirtyBindings = true;
		m_Initialized = true;

		logger::info("[RTXTS] TiledTextureStreaming initialized - heapTiles={}, maxUploads={}/frame",
			config.heapSizeInTiles, config.maxTileUploadsPerFrame);
	}

	void TiledTextureStreaming::Shutdown()
	{
		if (!m_Initialized)
			return;

		m_StreamingTextures.clear();
		m_TextureRingBuffer.clear();
		while (!m_PendingTiles.empty()) m_PendingTiles.pop();

		m_BindingSet = nullptr;
		m_BindingLayout = nullptr;
		m_TileUploadBuffer = nullptr;
		m_MipRequestBuffer = nullptr;
		m_MipRequestBufferClear = nullptr;
		m_MipRequestReadback = nullptr;
		m_MinMipBuffer = nullptr;
		m_MinMipUploadBuffer = nullptr;
		m_MinMipData.clear();
		m_TileMinMipScratch.clear();

		m_Heaps.clear();
		m_FreeHeapIds.clear();
		m_NumHeaps = 0;
		m_TotalHeapBytes = 0;

		m_TTM.reset();
		m_Device = nullptr;
		m_Initialized = false;
	}

	void TiledTextureStreaming::UpdateBindings()
	{
		if (!m_DirtyBindings || !m_BindingLayout)
			return;

		nvrhi::BindingSetDesc setDesc;
		setDesc.bindings = {
			nvrhi::BindingSetItem::StructuredBuffer_SRV(0, m_MinMipBuffer),
			nvrhi::BindingSetItem::RawBuffer_UAV(0, m_MipRequestBuffer)
		};

		m_BindingSet = m_Device->createBindingSet(setDesc, m_BindingLayout);
		m_DirtyBindings = false;
	}

	StreamingTexture* TiledTextureStreaming::CreateStreamingTexture(
		const nvrhi::TextureDesc& desc,
		std::shared_ptr<TextureSourceData> sourceData)
	{
		if (!m_Initialized || !sourceData)
			return nullptr;

		auto streamingTex = eastl::make_unique<StreamingTexture>();

		// Create the reserved (tiled) texture
		{
			nvrhi::TextureDesc tiledDesc = desc;
			tiledDesc.isTiled = true;
			tiledDesc.initialState = nvrhi::ResourceStates::ShaderResource;
			tiledDesc.keepInitialState = true;
			tiledDesc.debugName = desc.debugName.empty() ? "RTXTS Reserved Texture" : desc.debugName;
			streamingTex->reservedTexture = m_Device->createTexture(tiledDesc);

			if (!streamingTex->reservedTexture) {
				logger::error("[RTXTS] Failed to create reserved texture {}x{}", desc.width, desc.height);
				return nullptr;
			}
		}

		// Query tiling info
		{
			uint32_t mipLevels = desc.mipLevels;
			std::array<nvrhi::SubresourceTiling, 16> tilingsInfo;
			m_Device->getTextureTiling(
				streamingTex->reservedTexture,
				&streamingTex->numTiles,
				&streamingTex->packedMipDesc,
				&streamingTex->tileShape,
				&mipLevels,
				tilingsInfo.data());

			// Register with TTM
			rtxts::TiledLevelDesc tiledLevelDescs[16];
			rtxts::TiledTextureDesc ttmDesc = {};
			ttmDesc.textureWidth = desc.width;
			ttmDesc.textureHeight = desc.height;
			ttmDesc.tiledLevelDescs = tiledLevelDescs;
			ttmDesc.regularMipLevelsNum = streamingTex->packedMipDesc.numStandardMips;
			ttmDesc.packedMipLevelsNum = streamingTex->packedMipDesc.numPackedMips;
			ttmDesc.packedTilesNum = streamingTex->packedMipDesc.numTilesForPackedMips;
			ttmDesc.tileWidth = streamingTex->tileShape.widthInTexels;
			ttmDesc.tileHeight = streamingTex->tileShape.heightInTexels;

			for (uint32_t i = 0; i < ttmDesc.regularMipLevelsNum; ++i) {
				tiledLevelDescs[i].widthInTiles = tilingsInfo[i].widthInTiles;
				tiledLevelDescs[i].heightInTiles = tilingsInfo[i].heightInTiles;
			}

			m_TTM->AddTiledTexture(ttmDesc, streamingTex->ttmTextureId);
		}

		streamingTex->sourceData = std::move(sourceData);

		StreamingTexture* result = streamingTex.get();
		m_StreamingTextures.push_back(std::move(streamingTex));
		m_TextureRingBuffer.push_back(result);

		// --- Pre-load packed mips + coarsest standard mip immediately ---
		// This ensures the texture is never completely empty on first use.
		{
			float timeStamp = static_cast<float>(GetTickCount64()) / 1000.0f;

			// Request the coarsest standard mip level via feedback
			uint32_t coarsestStandardMip = result->packedMipDesc.numStandardMips > 0
				? result->packedMipDesc.numStandardMips - 1
				: 0;

			rtxts::TextureDesc feedbackDesc = m_TTM->GetTextureDesc(
				result->ttmTextureId, rtxts::eFeedbackTexture);

			uint32_t feedbackW = feedbackDesc.textureOrMipRegionWidth;
			uint32_t feedbackH = feedbackDesc.textureOrMipRegionHeight;

			std::vector<uint8_t> feedbackData(feedbackW * feedbackH,
				static_cast<uint8_t>(coarsestStandardMip));

			rtxts::SamplerFeedbackDesc samplerFeedback = {};
			samplerFeedback.pMinMipData = feedbackData.data();
			m_TTM->UpdateWithSamplerFeedback(
				result->ttmTextureId, samplerFeedback, timeStamp, m_Config.tileTimeoutSeconds);

			// Allocate and collect the tiles
			uint32_t numRequired = m_TTM->GetNumDesiredHeaps();
			while (m_NumHeaps < numRequired) {
				uint32_t heapId;
				AllocateHeap(heapId);
				m_TTM->AddHeap(heapId);
			}
			m_TTM->AllocateRequestedTiles();

			std::vector<uint32_t> tilesToMap;
			m_TTM->GetTilesToMap(result->ttmTextureId, tilesToMap);
			for (uint32_t tileIndex : tilesToMap) {
				m_PendingTiles.push({ result, tileIndex });
			}
		}

		// Set initial globalMinMip to coarsest loaded mip (will be refined each frame)
		result->globalMinMip = static_cast<float>(desc.mipLevels);

		return result;
	}

	void TiledTextureStreaming::RemoveStreamingTexture(StreamingTexture* texture)
	{
		if (!texture)
			return;

		// Clear MinMip entry
		if (texture->textureDescriptorIndex >= 0 &&
			texture->textureDescriptorIndex < static_cast<int32_t>(m_MinMipData.size()))
		{
			m_MinMipData[texture->textureDescriptorIndex] = 0.0f;
		}

		// Drain pending tiles for this texture
		{
			std::queue<PendingTile> cleaned;
			while (!m_PendingTiles.empty()) {
				auto tile = m_PendingTiles.front();
				m_PendingTiles.pop();
				if (tile.texture != texture)
					cleaned.push(tile);
			}
			m_PendingTiles = std::move(cleaned);
		}

		// Remove from ring buffer
		m_TextureRingBuffer.remove(texture);

		// Remove from TTM
		if (texture->ttmTextureId != UINT32_MAX && m_TTM) {
			m_TTM->RemoveTiledTexture(texture->ttmTextureId);
		}

		// Remove from storage
		auto it = std::find_if(m_StreamingTextures.begin(), m_StreamingTextures.end(),
			[texture](const auto& ptr) { return ptr.get() == texture; });
		if (it != m_StreamingTextures.end())
			m_StreamingTextures.erase(it);
	}

	void TiledTextureStreaming::RemoveStreamingTextureByDescriptorIndex(int32_t descriptorIndex)
	{
		if (descriptorIndex < 0)
			return;

		for (auto& tex : m_StreamingTextures) {
			if (tex->textureDescriptorIndex == descriptorIndex) {
				RemoveStreamingTexture(tex.get());
				return;
			}
		}
	}

	// =========================================================================
	// Frame Pipeline
	// =========================================================================

	void TiledTextureStreaming::BeginFrame(nvrhi::ICommandList* /*commandList*/)
	{
		if (!m_Initialized || m_StreamingTextures.empty())
			return;

		UpdateBindings();

		m_TileUploadCount = 0;

		// --- Read back mip request data from previous frame ---
		{
			uint32_t* readbackData = static_cast<uint32_t*>(
				m_Device->mapBuffer(m_MipRequestReadback, nvrhi::CpuAccessMode::Read));

			if (readbackData) {
				float timeStamp = static_cast<float>(GetTickCount64()) / 1000.0f;

				for (auto& tex : m_StreamingTextures) {
					if (tex->textureDescriptorIndex < 0)
						continue;

					uint32_t requestedMipBits = readbackData[tex->textureDescriptorIndex];

					if (requestedMipBits == 0xFFFFFFFF)
						continue;  // No request for this texture

					// Convert from asuint(float) back to float
					float requestedMipFloat;
					std::memcpy(&requestedMipFloat, &requestedMipBits, sizeof(float));

					// Convert per-texture mip request to per-tile feedback
					rtxts::TextureDesc feedbackDesc = m_TTM->GetTextureDesc(
						tex->ttmTextureId, rtxts::eFeedbackTexture);

					uint32_t feedbackW = feedbackDesc.textureOrMipRegionWidth;
					uint32_t feedbackH = feedbackDesc.textureOrMipRegionHeight;

					std::vector<uint8_t> feedbackData(feedbackW * feedbackH, 0xFF);

					uint8_t mipValue = static_cast<uint8_t>(
						std::clamp(static_cast<int>(requestedMipFloat), 0, 254));
					std::fill(feedbackData.begin(), feedbackData.end(), mipValue);

					rtxts::SamplerFeedbackDesc samplerFeedback = {};
					samplerFeedback.pMinMipData = feedbackData.data();
					m_TTM->UpdateWithSamplerFeedback(
						tex->ttmTextureId, samplerFeedback, timeStamp, m_Config.tileTimeoutSeconds);
				}

				m_Device->unmapBuffer(m_MipRequestReadback);
			}
		}

		// --- Check heap requirements ---
		{
			uint32_t numRequired = m_TTM->GetNumDesiredHeaps();
			while (m_NumHeaps < numRequired) {
				uint32_t heapId;
				AllocateHeap(heapId);
				m_TTM->AddHeap(heapId);
			}
		}

		// --- Allocate requested tiles ---
		m_TTM->AllocateRequestedTiles();

		// --- Process tile unmappings and collect tiles to map ---
		std::vector<uint32_t> tilesToUnmap;
		std::vector<uint32_t> tilesToMap;

		for (auto& tex : m_StreamingTextures) {
			// Unmap evicted tiles
			m_TTM->GetTilesToUnmap(tex->ttmTextureId, tilesToUnmap);
			if (!tilesToUnmap.empty()) {
				const auto& tileCoords = m_TTM->GetTileCoordinates(tex->ttmTextureId);

				eastl::vector<nvrhi::TiledTextureCoordinate> coords(tilesToUnmap.size());
				eastl::vector<nvrhi::TiledTextureRegion> regions(tilesToUnmap.size());

				for (uint32_t i = 0; i < tilesToUnmap.size(); ++i) {
					coords[i].mipLevel = tileCoords[tilesToUnmap[i]].mipLevel;
					coords[i].arrayLevel = 0;
					coords[i].x = tileCoords[tilesToUnmap[i]].x;
					coords[i].y = tileCoords[tilesToUnmap[i]].y;
					coords[i].z = 0;

					regions[i].tilesNum = 1;
				}

				nvrhi::TextureTilesMapping mapping = {};
				mapping.numTextureRegions = static_cast<uint32_t>(tilesToUnmap.size());
				mapping.tiledTextureCoordinates = coords.data();
				mapping.tiledTextureRegions = regions.data();

				m_Device->updateTextureTileMappings(tex->reservedTexture, &mapping, 1);
			}

			// Collect tiles to map
			m_TTM->GetTilesToMap(tex->ttmTextureId, tilesToMap);
			for (uint32_t tileIndex : tilesToMap) {
				m_PendingTiles.push({ tex.get(), tileIndex });
			}
		}
	}

	void TiledTextureStreaming::UpdateTileMappings(nvrhi::ICommandList* commandList)
	{
		if (!m_Initialized || m_PendingTiles.empty())
			return;

		uint32_t tilesThisFrame = std::min(
			static_cast<uint32_t>(m_PendingTiles.size()),
			m_Config.maxTileUploadsPerFrame);

		// Group tiles by texture
		struct TextureTilesBatch
		{
			StreamingTexture* texture;
			eastl::vector<uint32_t> tileIndices;
		};

		eastl::vector<TextureTilesBatch> batches;

		for (uint32_t i = 0; i < tilesThisFrame; ++i) {
			PendingTile pending = m_PendingTiles.front();
			m_PendingTiles.pop();

			TextureTilesBatch* batch = nullptr;
			for (auto& b : batches) {
				if (b.texture == pending.texture) {
					batch = &b;
					break;
				}
			}
			if (!batch) {
				batches.push_back({ pending.texture, {} });
				batch = &batches.back();
			}
			batch->tileIndices.push_back(pending.tileIndex);
		}

		// Process each batch
		for (auto& batch : batches) {
			StreamingTexture* tex = batch.texture;

			std::vector<uint32_t> mappedTiles(batch.tileIndices.begin(), batch.tileIndices.end());
			m_TTM->UpdateTilesMapping(tex->ttmTextureId, mappedTiles);

			const auto& tileCoords = m_TTM->GetTileCoordinates(tex->ttmTextureId);
			const auto& tileAllocs = m_TTM->GetTileAllocations(tex->ttmTextureId);

			// Group by heap for efficient mapping
			std::map<uint32_t, eastl::vector<uint32_t>> heapTiles;
			for (uint32_t tileIndex : batch.tileIndices) {
				heapTiles[tileAllocs[tileIndex].heapId].push_back(tileIndex);
			}

			for (auto& [heapId, tiles] : heapTiles) {
				uint32_t numTiles = static_cast<uint32_t>(tiles.size());
				nvrhi::HeapHandle heap = m_Heaps[heapId].heap;

				eastl::vector<nvrhi::TiledTextureCoordinate> coords;
				eastl::vector<nvrhi::TiledTextureRegion> regions;
				eastl::vector<uint64_t> offsets;

				for (uint32_t tileIndex : tiles) {
					nvrhi::TiledTextureCoordinate coord = {};
					coord.mipLevel = tileCoords[tileIndex].mipLevel;
					coord.x = tileCoords[tileIndex].x;
					coord.y = tileCoords[tileIndex].y;
					coord.z = 0;
					coords.push_back(coord);

					nvrhi::TiledTextureRegion region = {};
					region.tilesNum = 1;
					regions.push_back(region);

					offsets.push_back(
						static_cast<uint64_t>(tileAllocs[tileIndex].heapTileIndex) * D3D12_TILED_RESOURCE_TILE_SIZE_IN_BYTES);
				}

				nvrhi::TextureTilesMapping mapping = {};
				mapping.numTextureRegions = numTiles;
				mapping.tiledTextureCoordinates = coords.data();
				mapping.tiledTextureRegions = regions.data();
				mapping.byteOffsets = offsets.data();
				mapping.heap = heap;

				m_Device->updateTextureTileMappings(tex->reservedTexture, &mapping, 1);
			}

			// Upload tile data
			for (uint32_t tileIndex : batch.tileIndices) {
				UploadTileData(commandList, tex, tileIndex);
			}
		}

		// Update MinMip buffer
		UpdateMinMipBuffer(commandList);
	}

	void TiledTextureStreaming::ProcessMipRequests(nvrhi::ICommandList* commandList)
	{
		if (!m_Initialized || m_StreamingTextures.empty())
			return;

		// Copy the mip request buffer to readback
		commandList->copyBuffer(
			m_MipRequestReadback, 0,
			m_MipRequestBuffer, 0,
			Constants::NUM_TEXTURES_MAX * sizeof(uint32_t));

		// Clear the mip request buffer for next frame
		commandList->copyBuffer(
			m_MipRequestBuffer, 0,
			m_MipRequestBufferClear, 0,
			Constants::NUM_TEXTURES_MAX * sizeof(uint32_t));
	}

	void TiledTextureStreaming::EndFrame()
	{
		if (!m_Initialized)
			return;

		m_FrameIndex++;

		// Rotate ring buffer
		if (!m_TextureRingBuffer.empty() && m_Config.maxTexturesToUpdatePerFrame > 0) {
			uint32_t count = std::min(
				m_Config.maxTexturesToUpdatePerFrame,
				static_cast<uint32_t>(m_TextureRingBuffer.size()));
			for (uint32_t i = 0; i < count; ++i) {
				auto* tex = m_TextureRingBuffer.front();
				m_TextureRingBuffer.pop_front();
				m_TextureRingBuffer.push_back(tex);
			}
		}
	}

	TiledTextureStreamingStats TiledTextureStreaming::GetStats() const
	{
		TiledTextureStreamingStats stats = {};
		if (!m_Initialized)
			return stats;

		stats.heapAllocationInBytes = m_TotalHeapBytes;
		stats.streamingTextureCount = static_cast<uint32_t>(m_StreamingTextures.size());

		rtxts::Statistics ttmStats = m_TTM->GetStatistics();
		stats.tilesAllocated = ttmStats.allocatedTilesNum;
		stats.tilesTotal = ttmStats.totalTilesNum;
		stats.heapTilesFree = ttmStats.heapFreeTilesNum;
		stats.tilesStandby = ttmStats.standbyTilesNum;

		return stats;
	}

	// =========================================================================
	// Heap Management
	// =========================================================================

	void TiledTextureStreaming::AllocateHeap(uint32_t& heapId)
	{
		uint64_t heapSize = static_cast<uint64_t>(m_Config.heapSizeInTiles) * D3D12_TILED_RESOURCE_TILE_SIZE_IN_BYTES;

		nvrhi::HeapDesc heapDesc = {};
		heapDesc.capacity = heapSize;
		heapDesc.type = nvrhi::HeapType::DeviceLocal;
		nvrhi::HeapHandle heap = m_Device->createHeap(heapDesc);

		nvrhi::BufferDesc bufferDesc = {};
		bufferDesc.byteSize = heapSize;
		bufferDesc.isVirtual = true;
		bufferDesc.initialState = nvrhi::ResourceStates::CopySource;
		bufferDesc.keepInitialState = true;
		nvrhi::BufferHandle buffer = m_Device->createBuffer(bufferDesc);

		m_Device->bindBufferMemory(buffer, heap, 0);

		if (m_FreeHeapIds.empty()) {
			heapId = static_cast<uint32_t>(m_Heaps.size());
			m_Heaps.push_back({ heap, buffer });
		} else {
			heapId = m_FreeHeapIds.back();
			m_FreeHeapIds.pop_back();
			m_Heaps[heapId] = { heap, buffer };
		}

		m_TotalHeapBytes += heapSize;
		m_NumHeaps++;
	}

	void TiledTextureStreaming::ReleaseHeap(uint32_t heapId)
	{
		uint64_t heapSize = static_cast<uint64_t>(m_Config.heapSizeInTiles) * D3D12_TILED_RESOURCE_TILE_SIZE_IN_BYTES;

		m_FreeHeapIds.push_back(heapId);
		m_Heaps[heapId].heap = nullptr;
		m_Heaps[heapId].buffer = nullptr;
		m_TotalHeapBytes -= heapSize;
		m_NumHeaps--;
	}

	// =========================================================================
	// Tile Data Upload
	// =========================================================================

	void TiledTextureStreaming::UploadTileData(
		nvrhi::ICommandList* commandList,
		StreamingTexture* texture,
		uint32_t tileIndex)
	{
		if (!texture || !texture->sourceData || m_TileUploadCount >= m_Config.maxTileUploadsPerFrame)
			return;

		eastl::vector<TileRegion> regions;
		GetTileRegion(texture, tileIndex, regions);

		for (const auto& region : regions) {
			if (region.mip >= texture->sourceData->mips.size())
				continue;

			const auto& mipData = texture->sourceData->mips[region.mip];

			if (IsTilePacked(texture, tileIndex)) {
				// For packed mips, use writeTexture
				commandList->writeTexture(
					texture->reservedTexture,
					0, region.mip,
					mipData.data.data(),
					mipData.rowPitch,
					0);
			} else {
				// For standard tiles, use upload buffer + D3D12 CopyTextureRegion
				uint32_t bufferOffset = m_TileUploadCount * D3D12_TILED_RESOURCE_TILE_SIZE_IN_BYTES;

				uint8_t* mappedData = static_cast<uint8_t*>(
					m_Device->mapBuffer(m_TileUploadBuffer, nvrhi::CpuAccessMode::Write));
				if (!mappedData)
					return;

				mappedData += bufferOffset;

				bool isBC = texture->sourceData->IsBlockCompressed();
				uint32_t blockDiv = isBC ? 4 : 1;
				uint32_t shapeBlocksWidth = texture->tileShape.widthInTexels / blockDiv;
				uint32_t shapeBlocksHeight = texture->tileShape.heightInTexels / blockDiv;
				uint32_t bytesPerBlock = D3D12_TILED_RESOURCE_TILE_SIZE_IN_BYTES / (shapeBlocksWidth * shapeBlocksHeight);
				uint32_t tileBlocksWidth = region.widthInTexels / blockDiv;
				uint32_t tileBlocksHeight = region.heightInTexels / blockDiv;
				uint32_t sourceBlockX = region.xInTexels / blockDiv;
				uint32_t sourceBlockY = region.yInTexels / blockDiv;
				uint32_t rowPitchTile = tileBlocksWidth * bytesPerBlock;
				uint32_t rowPitchSource = mipData.rowPitch;

				for (uint32_t blockRow = 0; blockRow < tileBlocksHeight; ++blockRow) {
					uint32_t readOffset = (sourceBlockY + blockRow) * rowPitchSource + sourceBlockX * bytesPerBlock;
					uint32_t writeOffset = blockRow * rowPitchTile;

					if (readOffset + rowPitchTile <= mipData.data.size())
						std::memcpy(mappedData + writeOffset, mipData.data.data() + readOffset, rowPitchTile);
				}

				m_Device->unmapBuffer(m_TileUploadBuffer);

				// Copy from upload buffer to reserved texture via D3D12 native command
				ID3D12GraphicsCommandList* d3d12CmdList = commandList->getNativeObject(nvrhi::ObjectTypes::D3D12_GraphicsCommandList);
				ID3D12Resource* d3d12Resource = texture->reservedTexture->getNativeObject(nvrhi::ObjectTypes::D3D12_Resource);

				if (d3d12CmdList && d3d12Resource) {
					D3D12_TEXTURE_COPY_LOCATION srcLocation = {};
					srcLocation.pResource = m_TileUploadBuffer->getNativeObject(nvrhi::ObjectTypes::D3D12_Resource);
					srcLocation.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
					srcLocation.PlacedFootprint.Offset = bufferOffset;
					srcLocation.PlacedFootprint.Footprint.Format = nvrhi::d3d12::convertFormat(texture->sourceData->format);
					srcLocation.PlacedFootprint.Footprint.Width = region.widthInTexels;
					srcLocation.PlacedFootprint.Footprint.Height = region.heightInTexels;
					srcLocation.PlacedFootprint.Footprint.Depth = 1;
					srcLocation.PlacedFootprint.Footprint.RowPitch = rowPitchTile;

					D3D12_TEXTURE_COPY_LOCATION dstLocation = {};
					dstLocation.pResource = d3d12Resource;
					dstLocation.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
					dstLocation.SubresourceIndex = region.mip;

					D3D12_BOX sourceBox = {};
					sourceBox.right = region.widthInTexels;
					sourceBox.bottom = region.heightInTexels;
					sourceBox.back = 1;

					d3d12CmdList->CopyTextureRegion(&dstLocation, region.xInTexels, region.yInTexels, 0, &srcLocation, &sourceBox);
				}

				m_TileUploadCount++;
			}
		}
	}

	void TiledTextureStreaming::GetTileRegion(
		const StreamingTexture* texture,
		uint32_t tileIndex,
		eastl::vector<TileRegion>& regions) const
	{
		regions.clear();

		if (IsTilePacked(texture, tileIndex)) {
			const auto& packedMip = texture->packedMipDesc;
			const auto& texDesc = texture->reservedTexture->getDesc();
			bool isBC = texture->sourceData->IsBlockCompressed();

			for (uint32_t mip = packedMip.numStandardMips;
				mip < static_cast<uint32_t>(packedMip.numStandardMips + packedMip.numPackedMips); ++mip)
			{
				uint32_t w = std::max(texDesc.width >> mip, 1u);
				uint32_t h = std::max(texDesc.height >> mip, 1u);
				if (isBC) {
					w = ((w + 3) / 4) * 4;
					h = ((h + 3) / 4) * 4;
				}

				TileRegion region;
				region.mip = mip;
				region.xInTexels = 0;
				region.yInTexels = 0;
				region.widthInTexels = w;
				region.heightInTexels = h;
				regions.push_back(region);
			}
		} else {
			const auto& tileCoords = m_TTM->GetTileCoordinates(texture->ttmTextureId);
			const auto& coord = tileCoords[tileIndex];
			const auto& texDesc = texture->reservedTexture->getDesc();
			bool isBC = texture->sourceData->IsBlockCompressed();

			uint32_t w = texture->tileShape.widthInTexels;
			uint32_t h = texture->tileShape.heightInTexels;

			uint32_t subresW = std::max(texDesc.width >> coord.mipLevel, 1u);
			uint32_t subresH = std::max(texDesc.height >> coord.mipLevel, 1u);
			if (isBC) {
				subresW = ((subresW + 3) / 4) * 4;
				subresH = ((subresH + 3) / 4) * 4;
			}

			uint32_t x = coord.x * texture->tileShape.widthInTexels;
			uint32_t y = coord.y * texture->tileShape.heightInTexels;

			if (x + w > subresW) w = subresW - x;
			if (y + h > subresH) h = subresH - y;

			TileRegion region;
			region.mip = coord.mipLevel;
			region.xInTexels = x;
			region.yInTexels = y;
			region.widthInTexels = w;
			region.heightInTexels = h;
			regions.push_back(region);
		}
	}

	bool TiledTextureStreaming::IsTilePacked(const StreamingTexture* texture, uint32_t tileIndex) const
	{
		return tileIndex >= texture->packedMipDesc.startTileIndexInOverallResource;
	}

	// =========================================================================
	// MinMip Buffer Update
	// =========================================================================

	void TiledTextureStreaming::UpdateMinMipBuffer(nvrhi::ICommandList* commandList)
	{
		bool anyDirty = false;

		for (auto& tex : m_StreamingTextures) {
			if (tex->textureDescriptorIndex < 0)
				continue;

			// Read per-tile MinMip data from TTM
			rtxts::TextureDesc minMipDesc = m_TTM->GetTextureDesc(
				tex->ttmTextureId, rtxts::eMinMipTexture);

			uint32_t numMinMipEntries = minMipDesc.textureOrMipRegionWidth * minMipDesc.textureOrMipRegionHeight;

			if (m_TileMinMipScratch.size() < numMinMipEntries)
				m_TileMinMipScratch.resize(numMinMipEntries, 0xFF);

			m_TTM->WriteMinMipData(tex->ttmTextureId, m_TileMinMipScratch.data());

			// Compute global MinMip = max of all per-tile values
			// (highest mip number = coarsest mip = most conservative)
			uint8_t maxMinMip = 0;
			for (uint32_t i = 0; i < numMinMipEntries; ++i) {
				if (m_TileMinMipScratch[i] != 0xFF)
					maxMinMip = std::max(maxMinMip, m_TileMinMipScratch[i]);
			}

			float newMinMip = (maxMinMip == 0xFF)
				? static_cast<float>(tex->reservedTexture->getDesc().mipLevels)
				: static_cast<float>(maxMinMip);

			if (newMinMip != tex->globalMinMip) {
				tex->globalMinMip = newMinMip;
				m_MinMipData[tex->textureDescriptorIndex] = newMinMip;
				anyDirty = true;
			}
		}

		if (anyDirty) {
			// Upload the full buffer
			float* mapped = static_cast<float*>(
				m_Device->mapBuffer(m_MinMipUploadBuffer, nvrhi::CpuAccessMode::Write));
			if (mapped) {
				std::memcpy(mapped, m_MinMipData.data(), m_MinMipData.size() * sizeof(float));
				m_Device->unmapBuffer(m_MinMipUploadBuffer);
			}

			commandList->copyBuffer(
				m_MinMipBuffer, 0,
				m_MinMipUploadBuffer, 0,
				m_MinMipData.size() * sizeof(float));
		}
	}
}
