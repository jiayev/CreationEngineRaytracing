#include "SceneDiagnostics.h"
#include "Constants.h"

#include <atomic>
#include <cstring>
#include <Windows.h>

namespace SceneDiagnostics
{
	namespace
	{
		constexpr uint32_t eventCapacity = 8192;
		struct Header
		{
			char magic[8];
			uint32_t version;
			uint32_t recordSize;
			uint32_t events;
			uint32_t geometries;
			uint32_t process;
			uint32_t reserved;
			uint64_t sequence;
			uint64_t dropped;
			uint64_t started;
			uint64_t reserved2;
		};
		static_assert(sizeof(Header) == 64);

		struct Storage
		{
			HANDLE file = INVALID_HANDLE_VALUE;
			HANDLE mapping = nullptr;
			Header* header = nullptr;
			std::atomic_flag writing = ATOMIC_FLAG_INIT;
			uint64_t eventIndex = 0;

			~Storage()
			{
				auto* view = header;
				header = nullptr;
				if (view)
					UnmapViewOfFile(view);
				if (mapping)
					CloseHandle(mapping);
				if (file != INVALID_HANDLE_VALUE)
					CloseHandle(file);
			}
		} storage;
	}

	void Initialize()
	{
		if (storage.header)
			return;
		auto directory = logger::log_directory();
		if (!directory)
			return;
		const auto path = *directory / L"CreationEngineRaytracing-flight.bin";
		const auto previous = *directory / L"CreationEngineRaytracing-flight.previous.bin";
		if (!MoveFileExW(path.c_str(), previous.c_str(), MOVEFILE_REPLACE_EXISTING) && GetLastError() != ERROR_FILE_NOT_FOUND) {
			logger::warn("[SceneDiagnostics] Cannot rotate flight record ({})", GetLastError());
			return;
		}
		storage.file = CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ,
			nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
		if (storage.file == INVALID_HANDLE_VALUE) {
			logger::warn("[SceneDiagnostics] Cannot create flight record ({})", GetLastError());
			return;
		}
		constexpr DWORD bytes = static_cast<DWORD>(sizeof(Header) +
			(eventCapacity + Constants::NUM_MESHES_MAX) * sizeof(Record));
		storage.mapping = CreateFileMappingW(storage.file, nullptr, PAGE_READWRITE, 0, bytes, nullptr);
		if (storage.mapping)
			storage.header = static_cast<Header*>(MapViewOfFile(storage.mapping, FILE_MAP_WRITE, 0, 0, bytes));
		if (!storage.header) {
			logger::warn("[SceneDiagnostics] Cannot map flight record ({})", GetLastError());
			return;
		}
		Header header{};
		std::memcpy(header.magic, "CERFLT1", 8);
		header.version = 1;
		header.recordSize = static_cast<uint32_t>(sizeof(Record));
		header.events = eventCapacity;
		header.geometries = Constants::NUM_MESHES_MAX;
		header.process = GetCurrentProcessId();
		FILETIME started;
		GetSystemTimeAsFileTime(&started);
		header.started = (uint64_t(started.dwHighDateTime) << 32) | started.dwLowDateTime;
		std::memcpy(storage.header, &header, sizeof(header));
		logger::info("[SceneDiagnostics] Flight record: {} ({} bytes)", path.string(), bytes);
	}

	bool Enabled()
	{
		return storage.header != nullptr;
	}

	void Write(const Record& record)
	{
		if (!storage.header)
			return;
		if (storage.writing.test_and_set(std::memory_order_acquire)) {
			InterlockedIncrement64(reinterpret_cast<volatile LONG64*>(&storage.header->dropped));
			return;
		}
		if (record.event != Event::Geometry || record.index < Constants::NUM_MESHES_MAX) {
			auto* records = reinterpret_cast<Record*>(storage.header + 1);
			const uint64_t index = record.event == Event::Geometry
				? eventCapacity + record.index : storage.eventIndex++ % eventCapacity;
			auto& destination = records[index];
			const auto sequence = ++storage.header->sequence;
			InterlockedExchange64(reinterpret_cast<volatile LONG64*>(&destination.sequence), 0);
			destination.committedSequence = 0;
			std::memcpy(reinterpret_cast<char*>(&destination) + sizeof(uint64_t),
				reinterpret_cast<const char*>(&record) + sizeof(uint64_t), sizeof(Record) - 2 * sizeof(uint64_t));
			InterlockedExchange64(reinterpret_cast<volatile LONG64*>(&destination.committedSequence), static_cast<LONG64>(sequence));
			InterlockedExchange64(reinterpret_cast<volatile LONG64*>(&destination.sequence), static_cast<LONG64>(sequence));
		}
		storage.writing.clear(std::memory_order_release);
	}

	void Note(Event event, uint64_t frame, uint64_t object, uint64_t value, uint64_t extra, const char* name)
	{
		if (!Enabled())
			return;
		Record record;
		record.event = event;
		record.frame = frame;
		record.data[0] = object;
		record.data[1] = value;
		record.data[2] = extra;
		record.data[8] = GetCurrentThreadId();
		record.data[9] = GetTickCount64();
		if (name)
			strncpy_s(record.name, name, _TRUNCATE);
		Write(record);
	}
}
