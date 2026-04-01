#pragma once

#include <nvrhi/nvrhi.h>
#include <rtxts-ttm/TiledTextureManager.h>
#include <vector>
#include <memory>

namespace RTXTS
{
	// CPU-side storage of texture mip data for tile filling
	struct TextureSourceData
	{
		uint32_t width = 0;
		uint32_t height = 0;
		uint32_t mipLevels = 0;
		nvrhi::Format format = nvrhi::Format::UNKNOWN;

		struct MipLevel
		{
			std::vector<uint8_t> data;
			uint32_t rowPitch = 0;
			uint32_t width = 0;
			uint32_t height = 0;
		};

		std::vector<MipLevel> mips;

		bool IsBlockCompressed() const
		{
			return format >= nvrhi::Format::BC1_UNORM && format <= nvrhi::Format::BC7_UNORM_SRGB;
		}
	};

	// Represents a single tiled/streaming texture managed by RTXTS
	struct StreamingTexture
	{
		// NVRHI resources
		nvrhi::TextureHandle reservedTexture;       // D3D12 Reserved Resource (tiled)

		// TTM tracking
		uint32_t ttmTextureId = UINT32_MAX;

		// Tiling info
		uint32_t numTiles = 0;
		nvrhi::TileShape tileShape = {};
		nvrhi::PackedMipDesc packedMipDesc = {};

		// Source data for tile filling
		std::shared_ptr<TextureSourceData> sourceData;

		// Descriptor index in the main texture bindless table (same slot for reserved texture)
		int32_t textureDescriptorIndex = -1;

		// Global minimum resident mip level (computed from TTM per-tile data)
		float globalMinMip = 0.0f;

		bool IsValid() const { return reservedTexture != nullptr && ttmTextureId != UINT32_MAX; }
	};
}
