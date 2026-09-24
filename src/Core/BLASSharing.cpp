#include "Core/BLASSharing.h"
#include "Core/BLASCluster.h"
#include "Renderer.h"

#include <algorithm>
#include <cstring>

namespace
{
	constexpr uint64_t ContentBytesPerFrame = 8ull * 1024 * 1024;
	constexpr size_t MaxKeyBytes = 256ull * 1024 * 1024;
	constexpr size_t MaxBatches = 8;
	constexpr size_t MaxRequestsPerFrame = 64;
	constexpr uint64_t TransformBytes = 48;

	uint64_t Align4(uint64_t value) { return (value + 3) & ~3ull; }

	bool CanCopy(nvrhi::IBuffer* buffer, uint64_t offset, uint64_t bytes)
	{
		if (!buffer || !bytes || offset > buffer->getDesc().byteSize)
			return false;
		return bytes <= buffer->getDesc().byteSize - offset;
	}

	uint64_t VertexBytes(const SharedBLASRequest::Geometry& geometry)
	{
		const auto& triangles = geometry.desc.geometryData.triangles;
		return uint64_t(triangles.vertexCount - 1) * triangles.vertexStride + geometry.positionBytes;
	}

	uint64_t IndexBytes(const SharedBLASRequest::Geometry& geometry)
	{
		return uint64_t(geometry.desc.geometryData.triangles.indexCount) * sizeof(uint16_t);
	}

	void WriteU32(std::vector<uint8_t>& bytes, size_t& offset, uint32_t value)
	{
		std::memcpy(bytes.data() + offset, &value, sizeof(value));
		offset += sizeof(value);
	}

	bool SameGeometry(const BLASSharingData& a, const BLASSharingData& b)
	{
		if (a.bytes.size() != b.bytes.size() || a.transformOffsets != b.transformOffsets)
			return false;
		size_t start = 0;
		for (size_t offset : a.transformOffsets) {
			if (std::memcmp(a.bytes.data() + start, b.bytes.data() + start, offset - start) != 0)
				return false;
			start = offset + TransformBytes;
		}
		return std::memcmp(a.bytes.data() + start, b.bytes.data() + start, a.bytes.size() - start) == 0;
	}
}

BLASSharing::Batch* BLASSharing::CreateBatch(uint64_t bytes)
{
	if (m_Batches.size() >= MaxBatches)
		return nullptr;
	auto* renderer = Renderer::GetSingleton();
	auto* device = renderer->GetDevice();
	auto batch = std::make_unique<Batch>();
	batch->buffer = device->createBuffer(nvrhi::BufferDesc()
		.setByteSize(bytes)
		.setCpuAccess(nvrhi::CpuAccessMode::Read)
		.setInitialState(nvrhi::ResourceStates::CopyDest)
		.setKeepInitialState(true)
		.setDebugName("BLAS sharing readback"));
	batch->query = device->createEventQuery();
	if (!batch->buffer || !batch->query)
		return nullptr;
	batch->frame = renderer->GetFrameIndex();
	auto* result = batch.get();
	m_Batches.push_back(std::move(batch));
	return result;
}

void BLASSharing::BeginFrame()
{
	m_Transforms = nullptr;
	auto* device = Renderer::GetSingleton()->GetDevice();
	for (auto it = m_Batches.begin(); it != m_Batches.end();) {
		auto& batch = **it;
		if (!batch.submitted || !device->pollEventQuery(batch.query)) {
			++it;
			continue;
		}
		const auto* bytes = static_cast<const uint8_t*>(device->mapBuffer(batch.buffer, nvrhi::CpuAccessMode::Read));
		if (bytes) {
			for (const auto& transfer : batch.transfers) {
				auto request = transfer.request.lock();
				if (!request)
					continue;
				auto& geometry = request->geometries[transfer.geometry];
				const auto& triangles = geometry.desc.geometryData.triangles;
				const auto* source = bytes + transfer.offset;
				if (transfer.type == TransferType::Transform) {
					std::memcpy(geometry.transform.data(), source, TransformBytes);
					++request->transformsReady;
					continue;
				}
				auto* destination = request->data->bytes.data() + transfer.keyOffset;
				if (transfer.type == TransferType::Vertices) {
					for (uint32_t vertex = 0; vertex < triangles.vertexCount; ++vertex)
						std::memcpy(destination + size_t(vertex) * geometry.positionBytes,
							source + size_t(vertex) * triangles.vertexStride, geometry.positionBytes);
				} else {
					std::memcpy(destination, source, static_cast<size_t>(IndexBytes(geometry)));
				}
				if (--request->transfersRemaining == 0)
					Finish(request);
			}
			device->unmapBuffer(batch.buffer);
		} else {
			++m_Skipped;
			for (const auto& transfer : batch.transfers) {
				if (auto request = transfer.request.lock()) {
					request->queued = true;
					request->transfersRemaining = 0;
					request->data.reset();
				}
			}
		}
		it = m_Batches.erase(it);
	}
	std::erase_if(m_Waiting, [](const auto& weak) {
		auto request = weak.lock();
		return !request || request->queued;
	});
	for (auto* cache : { &m_Entries, &m_GeometryEntries }) {
		for (auto it = cache->begin(); it != cache->end();) {
			std::erase_if(it->second, [](const auto& entry) { return entry.expired(); });
			if (it->second.empty())
				it = cache->erase(it);
			else
				++it;
		}
	}
}

void BLASSharing::Capture(BLASCluster& cluster, nvrhi::ICommandList* commandList)
{
	if (!CER_VULKAN_BLAS_SHARING || !Renderer::GetSingleton()->IsVulkan() || !cluster.Valid() ||
		!cluster.m_BLAS || cluster.m_SharingRequest || cluster.m_SharedBLAS || cluster.m_RequiresUpdate || cluster.IsPlayer() ||
		(cluster.m_BLAS->getDesc().buildFlags & nvrhi::rt::AccelStructBuildFlags::AllowCompaction) == 0)
		return;

	auto request = std::make_shared<SharedBLASRequest>();
	request->original = cluster.m_BLAS;
	request->uncompactedBytes = cluster.m_UncompactedBytes;
	for (const auto* mesh : cluster.m_Members) {
		if (mesh->IsHidden())
			continue;
		const BaseMesh::BufferDescriptor* vertices = nullptr;
		const BaseMesh::BufferDescriptor* indices = nullptr;
		if (mesh->IsUpdatable() || !mesh->GetStaticBuffers(vertices, indices) ||
			!vertices || !indices || !vertices->m_SourceBuffer || !indices->m_SourceBuffer)
			return;
		for ([[maybe_unused]] const auto& entry : mesh->GetGeometryEntries()) {
			if (request->geometries.size() >= cluster.m_GeometryDescs.size())
				return;
			SharedBLASRequest::Geometry geometry;
			geometry.desc = cluster.m_GeometryDescs[request->geometries.size()];
			if (geometry.desc.geometryType != nvrhi::rt::GeometryType::Triangles || !geometry.desc.useTransform)
				return;
			const auto& triangles = geometry.desc.geometryData.triangles;
			if (triangles.vertexFormat == nvrhi::Format::RGB32_FLOAT)
				geometry.positionBytes = 12;
			else if (triangles.vertexFormat == nvrhi::Format::RGBA16_FLOAT)
				geometry.positionBytes = 6;
			else
				return;
			if (!triangles.vertexCount || !triangles.indexCount || triangles.indexFormat != nvrhi::Format::R16_UINT ||
				triangles.opacityMicromap || triangles.ommIndexBuffer || triangles.numOmmUsageCounts ||
				triangles.vertexStride < geometry.positionBytes ||
				triangles.vertexBuffer != vertices->m_Buffer || triangles.indexBuffer != indices->m_Buffer)
				return;
			geometry.vertices = vertices->m_Buffer;
			geometry.indices = indices->m_Buffer;
			geometry.vertexSource = vertices->m_SourceBuffer;
			geometry.indexSource = indices->m_SourceBuffer;
			if (!CanCopy(geometry.vertices, triangles.vertexOffset, VertexBytes(geometry)) ||
				!CanCopy(geometry.indices, triangles.indexOffset, IndexBytes(geometry)) ||
				!CanCopy(geometry.desc.transformBuffer, geometry.desc.transformBufferOffset, TransformBytes) ||
				(geometry.desc.transformBufferOffset & 3) != 0)
				return;
			request->geometries.push_back(std::move(geometry));
		}
	}
	if (request->geometries.empty() || request->geometries.size() != cluster.m_GeometryDescs.size())
		return;
	auto* transformBuffer = request->geometries.front().desc.transformBuffer;
	for (const auto& geometry : request->geometries) {
		if (geometry.desc.transformBuffer != transformBuffer)
			return;
	}
	if (!m_Transforms) {
		const uint64_t bytes = transformBuffer->getDesc().byteSize;
		if (bytes > ContentBytesPerFrame || (bytes & 3) != 0) {
			++m_Skipped;
			return;
		}
		m_Transforms = CreateBatch(bytes);
		if (m_Transforms) {
			commandList->copyBuffer(m_Transforms->buffer, 0, transformBuffer, 0, bytes);
			m_Transforms->buffers.push_back(transformBuffer);
			m_Transforms->used = bytes;
		}
	}
	if (!m_Transforms || m_Transforms->buffers.front() != transformBuffer) {
		++m_Skipped;
		return;
	}
	for (size_t i = 0; i < request->geometries.size(); ++i) {
		const auto& desc = request->geometries[i].desc;
		m_Transforms->transfers.push_back({ request, TransferType::Transform, i, desc.transformBufferOffset, 0 });
	}
	cluster.m_SharingRequest = request;
	m_Waiting.push_back(request);
}

void BLASSharing::Finish(const std::shared_ptr<SharedBLASRequest>& request)
{
	uint64_t hash = 14695981039346656037ull;
	uint64_t geometryHash = hash;
	size_t transformIndex = 0;
	const auto& data = *request->data;
	for (size_t i = 0; i < data.bytes.size(); ++i) {
		const uint8_t byte = data.bytes[i];
		hash ^= byte;
		hash *= 1099511628211ull;
		while (transformIndex < data.transformOffsets.size() && i >= data.transformOffsets[transformIndex] + TransformBytes)
			++transformIndex;
		if (transformIndex == data.transformOffsets.size() || i < data.transformOffsets[transformIndex]) {
			geometryHash ^= byte;
			geometryHash *= 1099511628211ull;
		}
	}
	++m_Verified;
	auto& entries = m_Entries[hash];
	for (const auto& weak : entries) {
		if (auto entry = weak.lock(); entry && entry->data->bytes == request->data->bytes) {
			request->entry = std::move(entry);
			++m_Matched;
			break;
		}
	}
	if (!request->entry) {
		auto& geometryEntries = m_GeometryEntries[geometryHash];
		for (const auto& weak : geometryEntries) {
			if (auto entry = weak.lock(); entry && SameGeometry(*entry->data, data)) {
				++m_TransformMismatches;
				break;
			}
		}
		request->entry = std::make_shared<SharedBLASEntry>();
		request->entry->data = request->data;
		request->entry->blas = request->original;
		request->entry->uncompactedBytes = request->uncompactedBytes;
		entries.push_back(request->entry);
		geometryEntries.push_back(request->entry);
	}
	request->data.reset();
}

void BLASSharing::EndFrame(nvrhi::ICommandList* commandList)
{
	Batch* content = nullptr;
	size_t scheduled = 0;
	for (const auto& weak : m_Waiting) {
		auto request = weak.lock();
		if (!request || request->queued || request->transformsReady != request->geometries.size())
			continue;
		if (scheduled >= MaxRequestsPerFrame)
			break;
		uint64_t copyBytes = 0;
		size_t keyBytes = 2 * sizeof(uint32_t);
		for (const auto& geometry : request->geometries) {
			const auto& triangles = geometry.desc.geometryData.triangles;
			copyBytes += Align4(VertexBytes(geometry));
			copyBytes += Align4(IndexBytes(geometry));
			keyBytes += 4 * sizeof(uint32_t) + TransformBytes + size_t(triangles.vertexCount) * geometry.positionBytes +
				static_cast<size_t>(IndexBytes(geometry));
		}
		if (copyBytes > ContentBytesPerFrame || keyBytes > MaxKeyBytes - m_Budget->bytes) {
			request->queued = true;
			++m_Skipped;
			continue;
		}
		if (!content)
			content = CreateBatch(ContentBytesPerFrame);
		if (!content)
			break;
		if (copyBytes > ContentBytesPerFrame - content->used)
			continue;

		request->data = std::make_shared<BLASSharingData>();
		request->data->budget = m_Budget;
		request->data->bytes.resize(keyBytes);
		m_Budget->bytes += keyBytes;
		auto& key = request->data->bytes;
		size_t keyOffset = 0;
		WriteU32(key, keyOffset, static_cast<uint32_t>(request->original->getDesc().buildFlags));
		WriteU32(key, keyOffset, static_cast<uint32_t>(request->geometries.size()));
		for (size_t i = 0; i < request->geometries.size(); ++i) {
			const auto& geometry = request->geometries[i];
			const auto& triangles = geometry.desc.geometryData.triangles;
			WriteU32(key, keyOffset, static_cast<uint32_t>(geometry.desc.flags));
			WriteU32(key, keyOffset, static_cast<uint32_t>(triangles.vertexFormat));
			WriteU32(key, keyOffset, triangles.vertexCount);
			WriteU32(key, keyOffset, triangles.indexCount);
			request->data->transformOffsets.push_back(keyOffset);
			std::memcpy(key.data() + keyOffset, geometry.transform.data(), TransformBytes);
			keyOffset += TransformBytes;
			const auto copy = [&](nvrhi::IBuffer* buffer, uint64_t offset, uint64_t bytes, TransferType type) {
				commandList->copyBuffer(content->buffer, content->used, buffer, offset, bytes);
				content->transfers.push_back({ request, type, i, content->used, keyOffset });
				content->used += Align4(bytes);
				++request->transfersRemaining;
			};
			copy(geometry.vertices, triangles.vertexOffset, VertexBytes(geometry), TransferType::Vertices);
			keyOffset += size_t(triangles.vertexCount) * geometry.positionBytes;
			copy(geometry.indices, triangles.indexOffset, IndexBytes(geometry), TransferType::Indices);
			keyOffset += static_cast<size_t>(IndexBytes(geometry));
			content->sources.push_back(geometry.vertexSource);
			content->sources.push_back(geometry.indexSource);
			content->buffers.push_back(geometry.vertices);
			content->buffers.push_back(geometry.indices);
		}
		request->queued = true;
		++scheduled;
	}
	for (const auto* batch : { m_Transforms, content }) {
		if (!batch)
			continue;
		for (const auto& buffer : batch->buffers)
			commandList->setBufferState(buffer, nvrhi::ResourceStates::ShaderResource);
	}
	if (m_Transforms || content)
		commandList->commitBarriers();
}

void BLASSharing::OnSubmitted(uint64_t frame, uint64_t fence)
{
	auto* device = Renderer::GetSingleton()->GetDevice();
	for (auto& batch : m_Batches) {
		if (!batch->submitted && batch->frame == frame) {
			device->setEventQuery(batch->query, nvrhi::CommandQueue::Graphics, fence);
			batch->submitted = true;
		}
	}
}

void BLASSharing::LogStats() const
{
	logger::info("[VRAM] BLAS sharing: {} verified, {} matches, {} transform mismatches, {} budget/readback skips, {:.1f} MiB CPU keys, {} pending batches",
		m_Verified, m_Matched, m_TransformMismatches, m_Skipped, m_Budget->bytes / 1048576.0, m_Batches.size());
}
