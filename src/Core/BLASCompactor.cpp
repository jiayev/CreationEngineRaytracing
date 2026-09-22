#include "Core/BLASCompactor.h"

#include <Windows.h>
#include <vulkan/vulkan.h>
#include <algorithm>
#include <mutex>
#include <vector>

namespace
{
	struct Context
	{
		nvrhi::DeviceHandle device;
		VkDevice nativeDevice = VK_NULL_HANDLE;
		HMODULE loader = nullptr;
		PFN_vkCreateAccelerationStructureKHR createAS = nullptr;
		PFN_vkDestroyAccelerationStructureKHR destroyAS = nullptr;
		PFN_vkGetAccelerationStructureDeviceAddressKHR getAddress = nullptr;
		PFN_vkCreateQueryPool createQueryPool = nullptr;
		PFN_vkDestroyQueryPool destroyQueryPool = nullptr;
		PFN_vkGetQueryPoolResults getQueryResults = nullptr;
		PFN_vkCmdResetQueryPool resetQueries = nullptr;
		PFN_vkCmdWriteAccelerationStructuresPropertiesKHR writeQueries = nullptr;
		PFN_vkCmdCopyAccelerationStructureKHR copyAS = nullptr;
		PFN_vkCmdPipelineBarrier barrier = nullptr;

		~Context() { if (loader) FreeLibrary(loader); }

		bool Initialize(nvrhi::IDevice* parent)
		{
			device = parent;
			nativeDevice = parent->getNativeObject(nvrhi::ObjectTypes::VK_Device);
			loader = LoadLibraryW(L"vulkan-1.dll");
			if (!loader)
				return false;
			auto getDeviceProc = reinterpret_cast<PFN_vkGetDeviceProcAddr>(GetProcAddress(loader, "vkGetDeviceProcAddr"));
			if (!getDeviceProc)
				return false;
			createAS = reinterpret_cast<PFN_vkCreateAccelerationStructureKHR>(getDeviceProc(nativeDevice, "vkCreateAccelerationStructureKHR"));
			destroyAS = reinterpret_cast<PFN_vkDestroyAccelerationStructureKHR>(getDeviceProc(nativeDevice, "vkDestroyAccelerationStructureKHR"));
			getAddress = reinterpret_cast<PFN_vkGetAccelerationStructureDeviceAddressKHR>(getDeviceProc(nativeDevice, "vkGetAccelerationStructureDeviceAddressKHR"));
			createQueryPool = reinterpret_cast<PFN_vkCreateQueryPool>(getDeviceProc(nativeDevice, "vkCreateQueryPool"));
			destroyQueryPool = reinterpret_cast<PFN_vkDestroyQueryPool>(getDeviceProc(nativeDevice, "vkDestroyQueryPool"));
			getQueryResults = reinterpret_cast<PFN_vkGetQueryPoolResults>(getDeviceProc(nativeDevice, "vkGetQueryPoolResults"));
			resetQueries = reinterpret_cast<PFN_vkCmdResetQueryPool>(getDeviceProc(nativeDevice, "vkCmdResetQueryPool"));
			writeQueries = reinterpret_cast<PFN_vkCmdWriteAccelerationStructuresPropertiesKHR>(getDeviceProc(nativeDevice, "vkCmdWriteAccelerationStructuresPropertiesKHR"));
			copyAS = reinterpret_cast<PFN_vkCmdCopyAccelerationStructureKHR>(getDeviceProc(nativeDevice, "vkCmdCopyAccelerationStructureKHR"));
			barrier = reinterpret_cast<PFN_vkCmdPipelineBarrier>(getDeviceProc(nativeDevice, "vkCmdPipelineBarrier"));
			return createAS && destroyAS && getAddress && createQueryPool && destroyQueryPool &&
				getQueryResults && resetQueries && writeQueries && copyAS && barrier;
		}

		uint64_t Address(VkAccelerationStructureKHR as) const
		{
			VkAccelerationStructureDeviceAddressInfoKHR info{ VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_DEVICE_ADDRESS_INFO_KHR };
			info.accelerationStructure = as;
			return getAddress(nativeDevice, &info);
		}
	};

	struct Block
	{
		struct Range { uint64_t offset; uint64_t size; };
		nvrhi::BufferHandle buffer;
		std::vector<Range> freeRanges;
		std::mutex mutex;

		bool Allocate(uint64_t size, uint64_t& offset)
		{
			std::scoped_lock lock(mutex);
			for (auto it = freeRanges.begin(); it != freeRanges.end(); ++it) {
				if (it->size < size)
					continue;
				offset = it->offset;
				it->offset += size;
				it->size -= size;
				if (!it->size)
					freeRanges.erase(it);
				return true;
			}
			return false;
		}

		void Free(uint64_t offset, uint64_t size)
		{
			std::scoped_lock lock(mutex);
			auto it = std::lower_bound(freeRanges.begin(), freeRanges.end(), offset,
				[](const Range& range, uint64_t value) { return range.offset < value; });
			it = freeRanges.insert(it, { offset, size });
			if (it != freeRanges.begin() && (it - 1)->offset + (it - 1)->size == it->offset) {
				(it - 1)->size += it->size;
				it = freeRanges.erase(it) - 1;
			}
			if (it + 1 != freeRanges.end() && it->offset + it->size == (it + 1)->offset) {
				it->size += (it + 1)->size;
				freeRanges.erase(it + 1);
			}
		}
	};
}

struct BLASCompactor::Allocation
{
	std::shared_ptr<Context> context;
	std::shared_ptr<Block> block;
	VkAccelerationStructureKHR handle = VK_NULL_HANDLE;
	uint64_t offset = 0;
	uint64_t size = 0;

	~Allocation()
	{
		if (handle)
			context->destroyAS(context->nativeDevice, handle, nullptr);
		if (block)
			block->Free(offset, size);
	}
};

struct BLASCompactor::State
{
	struct Batch
	{
		std::shared_ptr<Context> context;
		VkQueryPool queries = VK_NULL_HANDLE;
		nvrhi::EventQueryHandle completion;
		std::vector<std::shared_ptr<Record>> builds;
		std::vector<std::shared_ptr<Record>> copies;
		std::vector<std::shared_ptr<Record>> uses;
		std::vector<nvrhi::RefCountPtr<nvrhi::IResource>> resources;
		~Batch() { if (queries) context->destroyQueryPool(context->nativeDevice, queries, nullptr); }
	};

	std::shared_ptr<Context> context = std::make_shared<Context>();
	std::vector<std::weak_ptr<Block>> blocks;
	std::vector<std::weak_ptr<Record>> pending;
	std::vector<std::unique_ptr<Batch>> submitted;
	std::unique_ptr<Batch> current;
	bool available = false;

	Batch& Current()
	{
		if (!current) {
			current = std::make_unique<Batch>();
			current->context = context;
		}
		return *current;
	}

	std::shared_ptr<Allocation> Allocate(uint64_t size)
	{
		auto allocation = std::make_shared<Allocation>();
		allocation->context = context;
		allocation->size = (size + 255) & ~uint64_t(255);
		std::erase_if(blocks, [](const auto& block) { return block.expired(); });
		for (const auto& entry : blocks) {
			auto block = entry.lock();
			if (block && block->Allocate(allocation->size, allocation->offset)) {
				allocation->block = std::move(block);
				break;
			}
		}
		if (!allocation->block) {
			auto block = std::make_shared<Block>();
			const uint64_t capacity = std::max(8ull * 1024 * 1024, allocation->size);
			auto desc = nvrhi::BufferDesc().setByteSize(capacity).setIsAccelStructStorage(true)
				.setDebugName("Compacted BLAS Pool");
			block->buffer = context->device->createBuffer(desc);
			if (!block->buffer)
				return nullptr;
			block->freeRanges.push_back({ 0, capacity });
			block->Allocate(allocation->size, allocation->offset);
			allocation->block = block;
			blocks.push_back(block);
		}

		VkAccelerationStructureCreateInfoKHR info{ VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_CREATE_INFO_KHR };
		info.buffer = allocation->block->buffer->getNativeObject(nvrhi::ObjectTypes::VK_Buffer);
		info.offset = allocation->offset;
		info.size = size;
		info.type = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR;
		if (context->createAS(context->nativeDevice, &info, nullptr, &allocation->handle) != VK_SUCCESS) {
			allocation->handle = VK_NULL_HANDLE;
			return nullptr;
		}
		return allocation;
	}

	void Collect()
	{
		for (auto it = submitted.begin(); it != submitted.end();) {
			auto& batch = **it;
			if (!context->device->pollEventQuery(batch.completion)) {
				++it;
				continue;
			}
			for (auto& record : batch.copies)
				record->source = nullptr;
			if (batch.queries) {
				std::vector<uint64_t> sizes(batch.builds.size());
				const auto result = context->getQueryResults(context->nativeDevice, batch.queries, 0,
					static_cast<uint32_t>(sizes.size()), sizes.size() * sizeof(uint64_t), sizes.data(), sizeof(uint64_t), VK_QUERY_RESULT_64_BIT);
				if (result == VK_SUCCESS) {
					for (size_t i = 0; i < sizes.size(); ++i) {
						auto& record = batch.builds[i];
						if (sizes[i] && sizes[i] < record->originalBytes) {
							record->compactedBytes = sizes[i];
							pending.push_back(record);
						}
					}
				} else {
					logger::error("BLAS compaction query failed: {}", static_cast<int>(result));
				}
			}
			it = submitted.erase(it);
		}
	}
};

BLASCompactor::BLASCompactor(nvrhi::IDevice* device) : m_State(std::make_unique<State>())
{
	m_State->available = m_State->context->Initialize(device);
	if (!m_State->available)
		logger::warn("Vulkan BLAS compaction unavailable");
}

BLASCompactor::~BLASCompactor()
{
	for (const auto& batch : m_State->submitted)
		m_State->context->device->waitEventQuery(batch->completion);
}

bool BLASCompactor::IsAvailable() const { return m_State->available; }

uint64_t BLASCompactor::GetAddress(nvrhi::rt::IAccelStruct* blas) const
{
	return m_State->context->Address(blas->getNativeObject(nvrhi::ObjectTypes::VK_AccelerationStructureKHR));
}

std::shared_ptr<BLASCompactor::Record> BLASCompactor::Request(nvrhi::rt::IAccelStruct* blas)
{
	auto record = std::make_shared<Record>();
	record->source = blas;
	record->originalBytes = blas->getBufferSize();
	record->address = GetAddress(blas);
	m_State->Current().builds.push_back(record);
	return record;
}

void BLASCompactor::BeginFrame(nvrhi::ICommandList* commandList)
{
	m_State->Collect();
	if (m_State->pending.empty())
		return;
	commandList->clearState();
	commandList->commitBarriers();
	VkCommandBuffer cmd = commandList->getNativeObject(nvrhi::ObjectTypes::VK_CommandBuffer);
	auto& context = *m_State->context;
	VkMemoryBarrier barrier{ VK_STRUCTURE_TYPE_MEMORY_BARRIER };
	barrier.srcAccessMask = VK_ACCESS_ACCELERATION_STRUCTURE_WRITE_BIT_KHR;
	barrier.dstAccessMask = VK_ACCESS_ACCELERATION_STRUCTURE_READ_BIT_KHR;
	context.barrier(cmd, VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR, VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR,
		0, 1, &barrier, 0, nullptr, 0, nullptr);

	uint64_t bytes = 0;
	size_t consumed = 0;
	for (const auto& entry : m_State->pending) {
		auto record = entry.lock();
		if (!record) {
			++consumed;
			continue;
		}
		if (bytes && bytes + record->compactedBytes > 64ull * 1024 * 1024)
			break;
		++consumed;
		auto allocation = m_State->Allocate(record->compactedBytes);
		if (!allocation) {
			logger::warn("BLAS compaction allocation failed; retaining original BLAS");
			continue;
		}
		VkCopyAccelerationStructureInfoKHR info{ VK_STRUCTURE_TYPE_COPY_ACCELERATION_STRUCTURE_INFO_KHR };
		info.src = record->source->getNativeObject(nvrhi::ObjectTypes::VK_AccelerationStructureKHR);
		info.dst = allocation->handle;
		info.mode = VK_COPY_ACCELERATION_STRUCTURE_MODE_COMPACT_KHR;
		context.copyAS(cmd, &info);
		record->address = context.Address(allocation->handle);
		record->allocation = std::move(allocation);
		bytes += record->compactedBytes;
		m_State->Current().copies.push_back(record);
	}
	m_State->pending.erase(m_State->pending.begin(), m_State->pending.begin() + consumed);
	context.barrier(cmd, VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR,
		VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
		0, 1, &barrier, 0, nullptr, 0, nullptr);
}

void BLASCompactor::EndBuilds(nvrhi::ICommandList* commandList)
{
	if (!m_State->current || m_State->current->builds.empty())
		return;
	auto& batch = *m_State->current;
	auto& context = *m_State->context;
	VkQueryPoolCreateInfo info{ VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO };
	info.queryType = VK_QUERY_TYPE_ACCELERATION_STRUCTURE_COMPACTED_SIZE_KHR;
	info.queryCount = static_cast<uint32_t>(batch.builds.size());
	if (context.createQueryPool(context.nativeDevice, &info, nullptr, &batch.queries) != VK_SUCCESS) {
		batch.queries = VK_NULL_HANDLE;
		logger::warn("BLAS compaction query pool allocation failed; retaining original BLAS");
		return;
	}
	std::vector<VkAccelerationStructureKHR> handles;
	handles.reserve(batch.builds.size());
	for (const auto& record : batch.builds) {
		commandList->setAccelStructState(record->source, nvrhi::ResourceStates::AccelStructBuildBlas);
		handles.push_back(record->source->getNativeObject(nvrhi::ObjectTypes::VK_AccelerationStructureKHR));
	}
	commandList->commitBarriers();
	VkCommandBuffer cmd = commandList->getNativeObject(nvrhi::ObjectTypes::VK_CommandBuffer);
	context.resetQueries(cmd, batch.queries, 0, info.queryCount);
	context.writeQueries(cmd, info.queryCount, handles.data(), info.queryType, batch.queries, 0);
}

void BLASCompactor::Submitted(uint64_t instance)
{
	if (!m_State->current)
		return;
	auto& batch = *m_State->current;
	batch.completion = m_State->context->device->createEventQuery();
	m_State->context->device->setEventQuery(batch.completion, nvrhi::CommandQueue::Graphics, instance);
	m_State->submitted.push_back(std::move(m_State->current));
}

void BLASCompactor::Retain(nvrhi::IResource* resource)
{
	m_State->Current().resources.push_back(resource);
}

void BLASCompactor::Retain(const std::shared_ptr<Record>& record)
{
	m_State->Current().uses.push_back(record);
}

void BLASCompactor::Collect() { m_State->Collect(); }

bool BLASCompactor::ShouldReleaseBuildScratch() const
{
	if (!m_State->current)
		return false;
	uint64_t bytes = 0;
	for (const auto& record : m_State->current->builds)
		bytes += record->originalBytes;
	return bytes >= 64ull * 1024 * 1024;
}

void BLASCompactor::LogStats() const
{
	uint64_t capacity = 0;
	uint64_t freeBytes = 0;
	uint32_t count = 0;
	for (const auto& entry : m_State->blocks) {
		if (auto block = entry.lock()) {
			std::scoped_lock lock(block->mutex);
			capacity += block->buffer->getDesc().byteSize;
			for (const auto& range : block->freeRanges)
				freeBytes += range.size;
			++count;
		}
	}
	logger::info("[VRAM] Compacted BLAS pool: {} buffers, {:.1f} MiB capacity, {:.1f} MiB free", count, capacity / 1048576.0, freeBytes / 1048576.0);
}
