#pragma once

#include <Windows.h>

#include <cstdint>
#include <cwchar>
#include <string>
#include <type_traits>

namespace Trainer
{
	inline constexpr wchar_t PotentialMessageName[] = L"BlazblueTrainer.GrantAllPotentials.v1";
	inline constexpr std::uint32_t PotentialCommandMagic = 0x42545047;
	inline constexpr std::uint32_t PotentialCommandVersion = 1;
	inline constexpr DWORD PotentialRequestTimeoutMs = 10000;

	enum class PotentialCommandState : LONG
	{
		Pending,
		Executing,
		Completed,
		Failed,
		Cancelled
	};

	struct PotentialCommand
	{
		std::uint32_t Magic{PotentialCommandMagic};
		std::uint32_t Version{PotentialCommandVersion};
		std::uint32_t Size{sizeof(PotentialCommand)};
		DWORD RequesterProcessId{};
		DWORD TargetProcessId{};
		std::uint64_t Nonce{};
		volatile LONG State{static_cast<LONG>(PotentialCommandState::Pending)};
		DWORD ExecutionThreadId{};
		char Response[4096]{};
	};

	static_assert(std::is_standard_layout_v<PotentialCommand>);
	static_assert(std::is_trivially_copyable_v<PotentialCommand>);

	inline std::wstring PotentialMappingName(DWORD Requester, std::uint64_t Nonce)
	{
		wchar_t Name[96]{};
		swprintf_s(Name, L"Local\\BlazblueTrainer.Potentials.%lu.%016llx",
			Requester, static_cast<unsigned long long>(Nonce));
		return Name;
	}

	inline bool ValidPotentialCommand(
		const PotentialCommand& Command, DWORD Requester, DWORD Target, std::uint64_t Nonce)
	{
		return Command.Magic == PotentialCommandMagic &&
			Command.Version == PotentialCommandVersion && Command.Size == sizeof(PotentialCommand) &&
			Requester != 0 && Target != 0 && Nonce != 0 &&
			Command.RequesterProcessId == Requester && Command.TargetProcessId == Target &&
			Command.Nonce == Nonce;
	}
}
