#pragma once

#include <cstdint>

namespace SceneDiagnostics
{
	enum class Event : uint32_t
	{
		Geometry = 1, Build, Submitted, Completed, PassBegin, PassEnd,
		WaitBegin, WaitEnd, MeshRetired, MeshReleased, ClusterRetired, FrameStart
	};

	struct Record
	{
		uint64_t sequence = 0;
		uint64_t frame = 0;
		Event event = Event::Geometry;
		uint32_t index = 0;
		uint64_t data[10] = {};
		float transforms[24] = {};
		char name[48] = {};
		uint64_t committedSequence = 0;
	};
	static_assert(sizeof(Record) == 256);

	void Initialize();
	bool Enabled();
	void Write(const Record& record);
	void Note(Event event, uint64_t frame, uint64_t object = 0, uint64_t value = 0,
		uint64_t extra = 0, const char* name = nullptr);
}
