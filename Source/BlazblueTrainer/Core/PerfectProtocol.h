#pragma once

#include <Windows.h>
#include <cstdint>
#include <cwchar>
#include <type_traits>

namespace Trainer
{
	inline constexpr wchar_t PerfectMessageName[] = L"BlazblueTrainer.PerfectGuard.v1";
	inline constexpr std::uint32_t PerfectCommandMagic = 0x42545046;
	inline constexpr std::uint32_t PerfectCommandVersion = 1;
	inline constexpr DWORD PerfectRequestTimeoutMs = 5000;
	inline constexpr ULONGLONG PerfectHeartbeatTimeoutMs = 5000;

	enum class PerfectCommandState : LONG { Pending, Executing, Completed, Failed, Cancelled };

	struct PerfectCommand
	{
		std::uint32_t Magic{PerfectCommandMagic};
		std::uint32_t Version{PerfectCommandVersion};
		std::uint32_t Size{sizeof(PerfectCommand)};
		DWORD RequesterProcessId{};
		DWORD TargetProcessId{};
		std::uint64_t Nonce{};
		volatile LONG State{static_cast<LONG>(PerfectCommandState::Pending)};
		volatile LONG Enabled{};
		alignas(8) volatile LONG64 Heartbeat{};
		DWORD ExecutionThreadId{};
		char Error[1024]{};
	};
	static_assert(std::is_standard_layout_v<PerfectCommand>);
	static_assert(std::is_trivially_copyable_v<PerfectCommand>);

	inline std::wstring PerfectMappingName(DWORD Requester, std::uint64_t Nonce)
	{
		wchar_t Name[96]{};
		swprintf_s(Name, L"Local\\BlazblueTrainer.Perfect.%lu.%016llx", Requester,
			static_cast<unsigned long long>(Nonce));
		return Name;
	}

	inline bool ValidPerfectCommand(const PerfectCommand& Command, DWORD Requester, DWORD Target, std::uint64_t Nonce)
	{
		return Command.Magic == PerfectCommandMagic && Command.Version == PerfectCommandVersion &&
			Command.Size == sizeof(PerfectCommand) && Requester && Target && Nonce &&
			Command.RequesterProcessId == Requester && Command.TargetProcessId == Target && Command.Nonce == Nonce;
	}

	inline bool PerfectHeartbeatAlive(ULONGLONG Now, ULONGLONG Last)
	{
		return Now >= Last && Now - Last < PerfectHeartbeatTimeoutMs;
	}
}
