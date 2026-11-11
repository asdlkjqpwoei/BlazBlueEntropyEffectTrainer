#pragma once

#include <Windows.h>
#include <cstdint>
#include <cwchar>
#include <string>
#include <type_traits>

namespace Trainer
{
	inline constexpr wchar_t EventAmplificationMessageName[] = L"BlazblueTrainer.EventAmplification.v1";
	inline constexpr std::uint32_t EventAmplificationCommandMagic = 0x42544541;
	inline constexpr std::uint32_t EventAmplificationCommandVersion = 1;
	inline constexpr DWORD EventAmplificationRequestTimeoutMs = 5000;
	inline constexpr ULONGLONG EventAmplificationHeartbeatTimeoutMs = 5000;

	enum class EventAmplificationCommandState : LONG { Pending, Executing, Completed, Failed, Cancelled };

	struct EventAmplificationCommand
	{
		std::uint32_t Magic{EventAmplificationCommandMagic};
		std::uint32_t Version{EventAmplificationCommandVersion};
		std::uint32_t Size{sizeof(EventAmplificationCommand)};
		DWORD RequesterProcessId{};
		DWORD TargetProcessId{};
		std::uint64_t Nonce{};
		volatile LONG State{static_cast<LONG>(EventAmplificationCommandState::Pending)};
		volatile LONG Enabled{};
		alignas(8) volatile LONG64 Heartbeat{};
		DWORD ExecutionThreadId{};
		char Error[1024]{};
	};
	static_assert(std::is_standard_layout_v<EventAmplificationCommand>);
	static_assert(std::is_trivially_copyable_v<EventAmplificationCommand>);

	inline std::wstring EventAmplificationMappingName(DWORD Requester, std::uint64_t Nonce)
	{
		wchar_t Name[96]{};
		swprintf_s(Name, L"Local\\BlazblueTrainer.EventAmplification.%lu.%016llx", Requester,
			static_cast<unsigned long long>(Nonce));
		return Name;
	}

	inline bool ValidEventAmplificationCommand(const EventAmplificationCommand& Command, DWORD Requester, DWORD Target, std::uint64_t Nonce)
	{
		return Command.Magic == EventAmplificationCommandMagic && Command.Version == EventAmplificationCommandVersion &&
			Command.Size == sizeof(EventAmplificationCommand) && Requester && Target && Nonce &&
			Command.RequesterProcessId == Requester && Command.TargetProcessId == Target && Command.Nonce == Nonce;
	}

	inline bool EventAmplificationHeartbeatAlive(ULONGLONG Now, ULONGLONG Last)
	{
		return Now >= Last && Now - Last < EventAmplificationHeartbeatTimeoutMs;
	}
}
