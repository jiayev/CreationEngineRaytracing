#pragma once

#include <nvrhi/nvrhi.h>
#include <memory>

class BLASCompactor
{
public:
	struct Allocation;
	struct Record
	{
		nvrhi::rt::AccelStructHandle source;
		std::shared_ptr<Allocation> allocation;
		uint64_t originalBytes = 0;
		uint64_t compactedBytes = 0;
		uint64_t address = 0;
	};

private:
	struct State;
	std::unique_ptr<State> m_State;

public:
	explicit BLASCompactor(nvrhi::IDevice* device);
	~BLASCompactor();
	bool IsAvailable() const;
	uint64_t GetAddress(nvrhi::rt::IAccelStruct* blas) const;
	std::shared_ptr<Record> Request(nvrhi::rt::IAccelStruct* blas);
	void BeginFrame(nvrhi::ICommandList* commandList);
	void EndBuilds(nvrhi::ICommandList* commandList);
	void Retain(nvrhi::IResource* resource);
	void Retain(const std::shared_ptr<Record>& record);
	void Collect();
	bool ShouldReleaseBuildScratch() const;
	void Submitted(uint64_t instance);
	void LogStats() const;
};
