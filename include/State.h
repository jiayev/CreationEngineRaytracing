#pragma once

#include "Model.h"

struct MessageCallback : public nvrhi::IMessageCallback
{
	static MessageCallback& GetInstance()
	{
		static MessageCallback instance;
		return instance;
	}

	void message(nvrhi::MessageSeverity severity, const char* messageText) override
	{
		switch (severity) {
		case nvrhi::MessageSeverity::Fatal:
			logger::critical("{}", messageText);
			break;
		case nvrhi::MessageSeverity::Error:
			logger::error("{}", messageText);
			break;
		case nvrhi::MessageSeverity::Warning:
			logger::warn("{}", messageText);
			break;
		case nvrhi::MessageSeverity::Info:
			logger::info("{}", messageText);
			break;
		}
	}
};

struct State
{
	nvrhi::DeviceHandle nvrhiDevice;

	eastl::unordered_map<eastl::string, eastl::unique_ptr<Model*>> models;

	static State* GetSingleton()
	{
		static State singleton;
		return &singleton;
	}

	void Initialize(ID3D12Device5* device, ID3D12CommandQueue* commandQueue);

	void InitializeLog(spdlog::level::level_enum a_level);

	void AttachModel(RE::TESForm* form, RE::NiAVObject* root);
};