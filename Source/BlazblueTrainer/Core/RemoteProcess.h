#pragma once

#include "Config.h"

#include <Windows.h>

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace Trainer
{
	enum class AttachStatus
	{
		Attached,
		NotRunning,
		UnsupportedBuild,
		AccessDenied,
		Error
	};

	class RemoteProcess;

	struct AttachResult
	{
		AttachStatus Status{ AttachStatus::NotRunning };
		std::shared_ptr<RemoteProcess> Process;
		std::string Message;
		bool RuntimeVerificationRequired{};
	};

	enum class GameRunState
	{
		Stopped,
		Running,
		Unknown
	};

	struct GamePresence
	{
		GameRunState State{ GameRunState::Unknown };
		std::string Error;
		bool operator==(const GamePresence&) const = default;
	};

	struct MemoryRegion
	{
		std::uintptr_t Base{};
		std::size_t Size{};
		std::uintptr_t AllocationBase{};
	};

	class RemoteProcess final
	{
	public:
		RemoteProcess(const RemoteProcess&) = delete;
		RemoteProcess& operator=(const RemoteProcess&) = delete;
		~RemoteProcess();

		static AttachResult FindCompatibleGameProcess(const Config& Configuration);
		static GamePresence ObserveGamePresence(const Config& Configuration);
		static bool CanProbeUpdatedRuntime(
			const Config& Configuration, std::wstring_view Executable);
		static bool HasGameMetadataIdentity(const std::filesystem::path& Metadata);

		bool IsAlive() const;
		DWORD ProcessId() const;
		std::uintptr_t ModuleBase() const;
		std::size_t ModuleSize() const;

		bool Read(std::uintptr_t Address, void* Output, std::size_t Size) const;
		bool IsReadOnlyRange(std::uintptr_t Address, std::size_t Size) const;
		bool Write(std::uintptr_t Address, const void* Input, std::size_t Size, std::string& Error) const;
		std::optional<MemoryRegion> WritablePrivateRegionAt(
			std::uintptr_t Address,
			std::string& Error) const;
		std::vector<MemoryRegion> WritablePrivateRegions(
			std::uint64_t MaximumBytes,
			std::uintptr_t& Cursor,
			bool& Truncated,
			std::string& Error) const;
		std::vector<MemoryRegion> WritablePrivateAllocationRegions(
			std::uintptr_t AllocationBase,
			std::uint64_t MaximumBytes,
			bool& Truncated,
			std::string& Error) const;

	private:
		RemoteProcess(
			HANDLE Process,
			DWORD TargetProcessId,
			std::uintptr_t ModuleBase,
			std::size_t ModuleSize);

		HANDLE ConnectedProcess{};
		DWORD ProcessIdentifier{};
		std::uintptr_t ImageBase{};
		std::size_t ImageSize{};
	};
}
