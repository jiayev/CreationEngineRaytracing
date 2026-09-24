#pragma once

#include "Core/Mesh/BaseMesh.h"

#include <array>
#include <memory>
#include <unordered_map>
#include <vector>

class BLASCluster;

struct BLASSharingBudget
{
	size_t bytes = 0;
};

struct BLASSharingData
{
	std::shared_ptr<BLASSharingBudget> budget;
	std::vector<uint8_t> bytes;
	std::vector<size_t> transformOffsets;
	~BLASSharingData() { budget->bytes -= bytes.size(); }
};

struct SharedBLASEntry
{
	std::shared_ptr<BLASSharingData> data;
	nvrhi::rt::AccelStructHandle blas;
	uint64_t uncompactedBytes = 0;
};

struct SharedBLASRequest
{
	struct Geometry
	{
		nvrhi::rt::GeometryDesc desc;
		nvrhi::BufferHandle vertices;
		nvrhi::BufferHandle indices;
		winrt::com_ptr<ID3D11Buffer> vertexSource;
		winrt::com_ptr<ID3D11Buffer> indexSource;
		std::array<uint8_t, 48> transform{};
		uint32_t positionBytes = 0;
	};

	std::vector<Geometry> geometries;
	nvrhi::rt::AccelStructHandle original;
	uint64_t uncompactedBytes = 0;
	std::shared_ptr<BLASSharingData> data;
	std::shared_ptr<SharedBLASEntry> entry;
	size_t transformsReady = 0;
	size_t transfersRemaining = 0;
	bool queued = false;
};

class BLASSharing
{
	enum class TransferType { Transform, Vertices, Indices };
	struct Transfer
	{
		std::weak_ptr<SharedBLASRequest> request;
		TransferType type;
		size_t geometry;
		uint64_t offset;
		size_t keyOffset;
	};
	struct Batch
	{
		nvrhi::BufferHandle buffer;
		nvrhi::EventQueryHandle query;
		uint64_t frame = 0;
		uint64_t used = 0;
		bool submitted = false;
		std::vector<Transfer> transfers;
		std::vector<winrt::com_ptr<ID3D11Buffer>> sources;
		std::vector<nvrhi::BufferHandle> buffers;
	};

	std::vector<std::unique_ptr<Batch>> m_Batches;
	std::vector<std::weak_ptr<SharedBLASRequest>> m_Waiting;
	std::unordered_map<uint64_t, std::vector<std::weak_ptr<SharedBLASEntry>>> m_Entries;
	std::unordered_map<uint64_t, std::vector<std::weak_ptr<SharedBLASEntry>>> m_GeometryEntries;
	std::shared_ptr<BLASSharingBudget> m_Budget = std::make_shared<BLASSharingBudget>();
	Batch* m_Transforms = nullptr;
	uint64_t m_Verified = 0;
	uint64_t m_Matched = 0;
	uint64_t m_TransformMismatches = 0;
	uint64_t m_Skipped = 0;

	Batch* CreateBatch(uint64_t bytes);
	void Finish(const std::shared_ptr<SharedBLASRequest>& request);

public:
	void BeginFrame();
	void Capture(BLASCluster& cluster, nvrhi::ICommandList* commandList);
	void EndFrame(nvrhi::ICommandList* commandList);
	void OnSubmitted(uint64_t frame, uint64_t fence);
	void LogStats() const;
};
