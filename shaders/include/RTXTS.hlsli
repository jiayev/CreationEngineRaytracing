#ifndef RTXTS_HLSLI
#define RTXTS_HLSLI

#ifdef RTXTS_ENABLED

// MinMip values: one float per texture slot (global minimum resident mip)
// Non-streaming textures have 0.0 (no clamping)
StructuredBuffer<float>     RTXTSMinMipValues       : register(t0, space6);

// Software mip request buffer: one uint per texture slot
// Shaders write the minimum requested mip via InterlockedMin
RWByteAddressBuffer         RTXTSMipRequestBuffer   : register(u0, space6);

// Clamp a computed mip level to the minimum resident mip for a streaming texture.
// For non-streaming textures (MinMip == 0), this is a no-op.
float RTXTSClampMipLevel(float mipLevel, uint textureIndex)
{
    float minMip = RTXTSMinMipValues[textureIndex];
    return max(mipLevel, minMip);
}

// Record a mip request for a texture.
// Uses InterlockedMin on uint representation of positive floats
// (IEEE 754 positive floats have monotonically increasing bit patterns).
void RTXTSRecordMipRequest(uint textureIndex, float mipLevel)
{
    uint mipBits = asuint(max(0.0f, mipLevel));
    uint byteOffset = textureIndex * 4;
    RTXTSMipRequestBuffer.InterlockedMin(byteOffset, mipBits);
}

#else

// No-op stubs when RTXTS is disabled
float RTXTSClampMipLevel(float mipLevel, uint textureIndex)
{
    return mipLevel;
}

void RTXTSRecordMipRequest(uint textureIndex, float mipLevel)
{
}

#endif // RTXTS_ENABLED

#endif // RTXTS_HLSLI
