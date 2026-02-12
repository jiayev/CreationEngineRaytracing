#include "State.h"

void State::InitializeLog([[maybe_unused]] spdlog::level::level_enum a_level = spdlog::level::info)
{
#ifndef NDEBUG
	auto sink = std::make_shared<spdlog::sinks::msvc_sink_mt>();
#else
	auto path = logger::log_directory();

	*path /= std::format("{}.log"sv, "CreationEngineRaytracing");
	auto sink = std::make_shared<spdlog::sinks::basic_file_sink_mt>(path->string(), true);
#endif

#ifndef NDEBUG
	const auto level = spdlog::level::trace;
#else
	const auto level = a_level;
#endif

	auto log = std::make_shared<spdlog::logger>("global log"s, std::move(sink));
	log->set_level(level);
	log->flush_on(spdlog::level::info);

	spdlog::set_default_logger(std::move(log));
	spdlog::set_pattern("[%Y-%m-%d %H:%M:%S.%e] [%l] [%t] [%s:%#] %v");
}

void State::Initialize(ID3D12Device5* device, ID3D12CommandQueue* commandQueue)
{
	InitializeLog();

	nvrhi::d3d12::DeviceDesc deviceDesc;
	deviceDesc.errorCB = &MessageCallback::GetInstance();
	deviceDesc.pDevice = device;
	deviceDesc.pGraphicsCommandQueue = commandQueue;

	nvrhiDevice = nvrhi::d3d12::createDevice(deviceDesc);
}

void State::AttachModel([[maybe_unused]] RE::TESForm* form, [[maybe_unused]] RE::NiAVObject* root) {

}