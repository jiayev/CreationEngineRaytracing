#pragma once

#include "StreamingTexture.h"
#include "Constants.h"

#include <nvrhi/nvrhi.h>
#include <rtxts-ttm/TiledTextureManager.h>

#include <eastl/vector.h>
#include <eastl/unique_ptr.h>

#include <queue>
#include <list>

namespace RTXTS
{
	struct TiledTextureStreamingConfig
	{
		uint32_t numFramesInFlight = 3;
		uint32_t heapSizeInTiles = 512;           // Tiles per heap (each tile = 64KB)
		uint32_t maxTileUploadsPerFrame = 64;      // Max tiles to upload per frame  
		uint32_t maxTexturesToUpdatePerFrame = 32;  // Max textures to process feedback for per frame
		float tileTimeoutSeconds = 5.0f;           // How long before unused tiles are evicted
	};

	struct TiledTextureStreamingStats
	{
		uint64_t heapAllocationInBytes = 0;
		uint32_t heapTilesFree = 0;
		uint32_t tilesTotal = 0;
		uint32_t tilesAllocated = 0;
		uint32_t tilesStandby = 0;
		uint32_t streamingTextureCount = 0;
	};

	// Pending tile to fill and upload
	struct PendingTile
	{
		StreamingTexture* texture;
		uint32_t tileIndex;
	};

	// Tile region info for upload
	struct TileRegion
	{
		uint32_t mip;
		uint32_t xInTexels;
		uint32_t yInTexels;
		uint32_t widthInTexels;
		uint32_t heightInTexels;
	};

	// Register space used by RTXTS shader resources
	static constexpr uint32_t RTXTS_REGISTER_SPACE = 6;

	class TiledTextureStreaming
	{
	public:
		TiledTextureStreaming();
		~TiledTextureStreaming();

		// Initialize with device and config
		void Initialize(nvrhi::IDevice* device, const TiledTextureStreamingConfig& config = {});

		// Shutdown and release all resources
		void Shutdown();

		// Check if the system is initialized
		bool IsInitialized() const { return m_Initialized; }

		// Create a streaming texture from a texture description and source data
		// Returns a StreamingTexture that can be used for bindless descriptor registration
		StreamingTexture* CreateStreamingTexture(const nvrhi::TextureDesc& desc, std::shared_ptr<TextureSourceData> sourceData);

		// Remove a streaming texture
		void RemoveStreamingTexture(StreamingTexture* texture);

		// Remove a streaming texture by its bindless descriptor index
		void RemoveStreamingTextureByDescriptorIndex(int32_t descriptorIndex);

		// Get the RTXTS binding layout (space 6: MinMip SRV + MipRequest UAV)
		nvrhi::IBindingLayout* GetBindingLayout() const { return m_BindingLayout; }

		// Get the RTXTS binding set
		nvrhi::IBindingSet* GetBindingSet() const { return m_BindingSet; }

		// Mark binding set as dirty (e.g., after buffer recreation)
		void MarkBindingsDirty() { m_DirtyBindings = true; }

		// --- Per-frame pipeline ---

		// Step 1: Called at the beginning of the frame, before rendering.
		// Reads back feedback from N frames ago, determines which tiles to map/unmap.
		void BeginFrame(nvrhi::ICommandList* commandList);

		// Step 2: Called after BeginFrame. 
		// Uploads tile data and updates tile mappings for tiles that are ready.
		void UpdateTileMappings(nvrhi::ICommandList* commandList);

		// Step 3: Called after rendering.
		// Copies mip request buffer to readback and clears it for next frame.
		void ProcessMipRequests(nvrhi::ICommandList* commandList);

		// Step 4: End of frame cleanup.
		void EndFrame();

		// Get statistics
		TiledTextureStreamingStats GetStats() const;

	private:
		bool m_Initialized = false;

		nvrhi::DeviceHandle m_Device;
		TiledTextureStreamingConfig m_Config;
		uint32_t m_FrameIndex = 0;

		// RTXTS Tiled Texture Manager
		std::shared_ptr<rtxts::TiledTextureManager> m_TTM;

		// Heap management
		struct HeapEntry
		{
			nvrhi::HeapHandle heap;
			nvrhi::BufferHandle buffer;  // Virtual buffer bound to heap (for tile data upload)
		};
		eastl::vector<HeapEntry> m_Heaps;
		eastl::vector<uint32_t> m_FreeHeapIds;
		uint32_t m_NumHeaps = 0;
		uint64_t m_TotalHeapBytes = 0;

		void AllocateHeap(uint32_t& heapId);
		void ReleaseHeap(uint32_t heapId);

		// Streaming textures
		eastl::vector<eastl::unique_ptr<StreamingTexture>> m_StreamingTextures;
		std::list<StreamingTexture*> m_TextureRingBuffer;  // Round-robin for feedback processing

		// Pending tiles queue (tiles waiting for data upload)
		std::queue<PendingTile> m_PendingTiles;

		// Tile upload buffer
		nvrhi::BufferHandle m_TileUploadBuffer;
		uint32_t m_TileUploadCount = 0;

		// Software mip request system
		nvrhi::BufferHandle m_MipRequestBuffer;      // GPU RWByteAddressBuffer: one uint per texture
		nvrhi::BufferHandle m_MipRequestBufferClear;  // Staging buffer with 0xFF values for clearing
		nvrhi::BufferHandle m_MipRequestReadback;     // CPU-readable readback buffer

		// MinMip flat buffer (one float per texture slot = global min resident mip)
		nvrhi::BufferHandle m_MinMipBuffer;           // GPU StructuredBuffer<float>
		nvrhi::BufferHandle m_MinMipUploadBuffer;     // CPU-writable upload buffer
		eastl::vector<float> m_MinMipData;            // CPU shadow copy

		// Binding layout and set for space 6
		nvrhi::BindingLayoutHandle m_BindingLayout;
		nvrhi::BindingSetHandle m_BindingSet;
		bool m_DirtyBindings = true;

		void UpdateBindings();

		// Per-tile MinMip CPU data (for TTM, not uploaded to GPU per-tile)
		eastl::vector<uint8_t> m_TileMinMipScratch;

		// Helpers
		void UpdateMinMipBuffer(nvrhi::ICommandList* commandList);
		void UploadTileData(nvrhi::ICommandList* commandList, StreamingTexture* texture, uint32_t tileIndex);
		void GetTileRegion(const StreamingTexture* texture, uint32_t tileIndex, eastl::vector<TileRegion>& regions) const;
		bool IsTilePacked(const StreamingTexture* texture, uint32_t tileIndex) const;
	};
}
