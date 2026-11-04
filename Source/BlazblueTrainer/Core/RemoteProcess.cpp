#include "RemoteProcess.h"

#include "Il2CppScanner.h"
#include "GameData.h"

#include <tlhelp32.h>

#include <algorithm>
#include <sstream>
#include <stdexcept>
#include <utility>
#include <map>
#include <mutex>

namespace Trainer
{
	namespace
	{
		constexpr DWORD ProcessRights =
			PROCESS_QUERY_INFORMATION |
			PROCESS_VM_OPERATION |
			PROCESS_VM_READ |
			PROCESS_VM_WRITE |
			SYNCHRONIZE;

		std::wstring Utf8ToWide(const std::string& Value)
		{
			if (Value.empty())
			{
				return {};
			}

			const int Size = MultiByteToWideChar(
				CP_UTF8,
				MB_ERR_INVALID_CHARS,
				Value.data(),
				static_cast<int>(Value.size()),
				nullptr,
				0);
			if (Size <= 0)
			{
				throw std::runtime_error("A configured module name is not valid UTF-8.");
			}

			std::wstring Result(static_cast<std::size_t>(Size), L'\0');
			if (MultiByteToWideChar(
					CP_UTF8,
					MB_ERR_INVALID_CHARS,
					Value.data(),
					static_cast<int>(Value.size()),
					Result.data(),
					Size) != Size)
			{
				throw std::runtime_error("Could not convert the configured module name to UTF-16.");
			}
			return Result;
		}

		std::string WideToUtf8(const std::wstring& Value)
		{
			if (Value.empty())
			{
				return {};
			}

			const int Size = WideCharToMultiByte(
				CP_UTF8,
				WC_ERR_INVALID_CHARS,
				Value.data(),
				static_cast<int>(Value.size()),
				nullptr,
				0,
				nullptr,
				nullptr);
			if (Size <= 0)
			{
				return "Windows returned an unrepresentable error message.";
			}

			std::string Result(static_cast<std::size_t>(Size), '\0');
			if (WideCharToMultiByte(
					CP_UTF8,
					WC_ERR_INVALID_CHARS,
					Value.data(),
					static_cast<int>(Value.size()),
					Result.data(),
					Size,
					nullptr,
					nullptr) != Size)
			{
				return "Windows returned an unrepresentable error message.";
			}
			return Result;
		}

		std::string Win32Error(const char* Operation, DWORD Error)
		{
			wchar_t* Buffer = nullptr;
			const DWORD Length = FormatMessageW(
				FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
				nullptr,
				Error,
				MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
				reinterpret_cast<wchar_t*>(&Buffer),
				0,
				nullptr);

			std::wstring Description;
			if (Length != 0 && Buffer != nullptr)
			{
				Description.assign(Buffer, Length);
				LocalFree(Buffer);
			}

			while (!Description.empty() &&
				   (Description.back() == L'\r' || Description.back() == L'\n' || Description.back() == L' '))
			{
				Description.pop_back();
			}

			std::ostringstream Message;
			Message << Operation << " failed with Windows error " << Error;
			if (!Description.empty())
			{
				Message << ": " << WideToUtf8(Description);
			}
			return Message.str();
		}

		struct ModuleDetails
		{
			std::uintptr_t Base{};
			std::size_t Size{};
		};

		bool FindModule(DWORD TargetProcessId, const std::wstring& ModuleName, ModuleDetails& Details)
		{
			HANDLE Snapshot = CreateToolhelp32Snapshot(
				TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32,
				TargetProcessId);
			if (Snapshot == INVALID_HANDLE_VALUE)
			{
				return false;
			}

			MODULEENTRY32W Entry{};
			Entry.dwSize = sizeof(Entry);
			bool Found = false;
			if (Module32FirstW(Snapshot, &Entry))
			{
				do
				{
					if (CompareStringOrdinal(
							Entry.szModule,
							-1,
							ModuleName.c_str(),
							-1,
							TRUE) == CSTR_EQUAL)
					{
						Details.Base = reinterpret_cast<std::uintptr_t>(Entry.modBaseAddr);
						Details.Size = Entry.modBaseSize;
						Found = true;
						break;
					}
				} while (Module32NextW(Snapshot, &Entry));
			}
			CloseHandle(Snapshot);
			return Found;
		}

		std::optional<bool> DiskGameIdentity(DWORD ProcessId, std::string& Error)
		{
			HANDLE Process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, ProcessId);
			if (!Process)
			{
				Error = Win32Error("OpenProcess", GetLastError());
				return std::nullopt;
			}
			std::wstring Image(32768, L'\0');
			DWORD Length = static_cast<DWORD>(Image.size());
			const bool Read = QueryFullProcessImageNameW(Process, 0, Image.data(), &Length) != FALSE;
			const auto ReadError = GetLastError();
			CloseHandle(Process);
			if (!Read)
			{
				Error = Win32Error("QueryFullProcessImageName", ReadError);
				return std::nullopt;
			}
			Image.resize(Length);
			const auto Metadata = std::filesystem::path(Image).parent_path() /
				L"BlazblueEntropyEffect_Data" / L"il2cpp_data" / L"Metadata" / L"global-metadata.dat";
			std::error_code FileError;
			const auto Time = std::filesystem::last_write_time(Metadata, FileError);
			if (FileError == std::errc::no_such_file_or_directory)
				return false;
			if (FileError)
			{
				Error = "Cannot inspect the candidate game's metadata: " + FileError.message();
				return std::nullopt;
			}
			struct CachedIdentity {std::filesystem::file_time_type Time; bool Matches;};
			static std::mutex CacheMutex;
			static std::map<std::filesystem::path, CachedIdentity> Cache;
			std::lock_guard Lock(CacheMutex);
			if (const auto Found = Cache.find(Metadata); Found != Cache.end() && Found->second.Time == Time)
				return Found->second.Matches;
			try
			{
				const bool Matches = RemoteProcess::HasGameMetadataIdentity(Metadata);
				if (Cache.size() > 256) Cache.clear();
				Cache.insert_or_assign(Metadata, CachedIdentity{Time, Matches});
				return Matches;
			}
			catch (const std::runtime_error& Exception)
			{
				Error = Exception.what();
				return std::nullopt;
			}
		}

	}

	RemoteProcess::RemoteProcess(
		HANDLE Process,
		DWORD TargetProcessId,
		std::uintptr_t ModuleBase,
		std::size_t ModuleSize)
		: ConnectedProcess(Process),
		  ProcessIdentifier(TargetProcessId),
		  ImageBase(ModuleBase),
		  ImageSize(ModuleSize)
	{
	}

	RemoteProcess::~RemoteProcess()
	{
		if (ConnectedProcess != nullptr)
		{
			CloseHandle(ConnectedProcess);
		}
	}

	AttachResult RemoteProcess::FindCompatibleGameProcess(const Config& Configuration)
	{
		AttachResult Result;
		const std::wstring ModuleName = Utf8ToWide(Configuration.Module);

		HANDLE Snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
		if (Snapshot == INVALID_HANDLE_VALUE)
		{
			Result.Status = AttachStatus::Error;
			Result.Message = Win32Error("CreateToolhelp32Snapshot", GetLastError());
			return Result;
		}

		PROCESSENTRY32W Entry{};
		Entry.dwSize = sizeof(Entry);
		bool ModuleCandidateSeen = false;
		bool ProfileMismatchSeen = false;
		bool AccessDenied = false;
		DWORD CurrentSessionId = 0;
		const bool HasCurrentSession =
			ProcessIdToSessionId(GetCurrentProcessId(), &CurrentSessionId) != FALSE;

		if (Process32FirstW(Snapshot, &Entry))
		{
			do
			{
				if (Entry.th32ProcessID <= 4 ||
					Entry.th32ProcessID == GetCurrentProcessId())
				{
					continue;
				}

				DWORD CandidateSessionId = 0;
				if (HasCurrentSession &&
					(!ProcessIdToSessionId(Entry.th32ProcessID, &CandidateSessionId) ||
					 CandidateSessionId != CurrentSessionId))
				{
					continue;
				}

				ModuleDetails Module;
				if (!FindModule(Entry.th32ProcessID, ModuleName, Module))
				{
					continue;
				}
				ModuleCandidateSeen = true;

				HANDLE Candidate = OpenProcess(ProcessRights, FALSE, Entry.th32ProcessID);
				if (Candidate == nullptr)
				{
					AccessDenied = true;
					continue;
				}

				auto Process = std::shared_ptr<RemoteProcess>(new RemoteProcess(
					Candidate,
					Entry.th32ProcessID,
					Module.Base,
					Module.Size));
				Il2CppScanner Scanner(Configuration, Process);
				std::vector<std::string> UnresolvedTypes;
				const ClassPointerMap MatchingTypes = Scanner.ResolveClasses(UnresolvedTypes);
				if (MatchingTypes.size() < Configuration.IdentityMinTypeMatches)
				{
					std::string IdentityError;
					const auto DiskIdentity = DiskGameIdentity(Entry.th32ProcessID, IdentityError);
					if (!CanProbeUpdatedRuntime(Configuration, Entry.szExeFile) &&
						!(Configuration.PuertsRuntime && DiskIdentity && *DiskIdentity))
					{
						ProfileMismatchSeen = true;
						continue;
					}
					Result.RuntimeVerificationRequired = true;
				}

				Result.Status = AttachStatus::Attached;
				Result.Process = std::move(Process);
				Result.Message = Result.RuntimeVerificationRequired
					? "Updated game detected; validating authoritative runtime models before enabling writes."
					: "Compatible GameAssembly process detected (PID " +
					std::to_string(Entry.th32ProcessID) + "; " +
					std::to_string(MatchingTypes.size()) + "/" +
					std::to_string(Configuration.TypeProfile.size()) + " IL2CPP type markers matched).";
				CloseHandle(Snapshot);
				return Result;
			} while (Process32NextW(Snapshot, &Entry));
		}

		CloseHandle(Snapshot);
		if (AccessDenied)
		{
			Result.Status = AttachStatus::AccessDenied;
			Result.Message = "A GameAssembly process was found, but opening it was denied. Run the trainer and game with the same privilege level.";
		}
		else if (ProfileMismatchSeen)
		{
			Result.Status = AttachStatus::UnsupportedBuild;
			Result.Message = "GameAssembly.dll was found, but its IL2CPP type profile did not match the configured game; no writes were enabled.";
		}
		else if (!ModuleCandidateSeen)
		{
			Result.Status = AttachStatus::NotRunning;
			Result.Message = "No running process with the configured GameAssembly module was found.";
		}
		return Result;
	}

	bool RemoteProcess::CanProbeUpdatedRuntime(
		const Config& Configuration, std::wstring_view Executable)
	{
		const auto Expected = Utf8ToWide(Configuration.ExecutableName);
		return Configuration.PuertsRuntime && Configuration.PuertsArray &&
			CompareStringOrdinal(Executable.data(), static_cast<int>(Executable.size()),
				Expected.data(), static_cast<int>(Expected.size()), TRUE) == CSTR_EQUAL;
	}

	bool RemoteProcess::HasGameMetadataIdentity(const std::filesystem::path& Metadata)
	{
		const auto Data = ReadBinaryFile(Metadata);
		if (Data.size() < 8 || Data[0] != 0xaf || Data[1] != 0x1b || Data[2] != 0xb1 || Data[3] != 0xfa)
			return false;
		const std::string_view Strings(reinterpret_cast<const char*>(Data.data()), Data.size());
		return Strings.find("STFesActorPack") != std::string_view::npos &&
			Strings.find("STPlayerNewFesActorPack") != std::string_view::npos &&
			Strings.find("EBreedDungeonResType") != std::string_view::npos &&
			Strings.find("Puerts") != std::string_view::npos;
	}

	GamePresence RemoteProcess::ObserveGamePresence(const Config& Configuration)
	{
		const auto Expected = Utf8ToWide(Configuration.ExecutableName);
		HANDLE Snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
		if (Snapshot == INVALID_HANDLE_VALUE)
			return {GameRunState::Unknown, Win32Error("CreateToolhelp32Snapshot", GetLastError())};
		PROCESSENTRY32W Entry{};
		Entry.dwSize = sizeof(Entry);
		if (!Process32FirstW(Snapshot, &Entry))
		{
			const auto Error = GetLastError();
			CloseHandle(Snapshot);
			return {GameRunState::Unknown, Win32Error("Process32First", Error)};
		}
		do
		{
			if (CompareStringOrdinal(Entry.szExeFile, -1, Expected.c_str(), -1, TRUE) == CSTR_EQUAL)
			{
				CloseHandle(Snapshot);
				return {GameRunState::Running, {}};
			}
			ModuleDetails Module;
			if (Entry.th32ProcessID > 4 && Entry.th32ProcessID != GetCurrentProcessId() &&
				FindModule(Entry.th32ProcessID, Utf8ToWide(Configuration.Module), Module))
			{
				std::string Error;
				const auto Matches = DiskGameIdentity(Entry.th32ProcessID, Error);
				if (!Matches)
				{
					CloseHandle(Snapshot);
					return {GameRunState::Unknown, Error};
				}
				if (*Matches)
				{
					CloseHandle(Snapshot);
					return {GameRunState::Running, {}};
				}
			}
		} while (Process32NextW(Snapshot, &Entry));
		const auto Error = GetLastError();
		CloseHandle(Snapshot);
		if (Error != ERROR_NO_MORE_FILES)
			return {GameRunState::Unknown, Win32Error("Process32Next", Error)};
		return {GameRunState::Stopped, {}};
	}

	bool RemoteProcess::IsAlive() const
	{
		if (ConnectedProcess == nullptr)
		{
			return false;
		}

		DWORD ExitCode = 0;
		return GetExitCodeProcess(ConnectedProcess, &ExitCode) && ExitCode == STILL_ACTIVE;
	}

	DWORD RemoteProcess::ProcessId() const
	{
		return ProcessIdentifier;
	}

	std::uintptr_t RemoteProcess::ModuleBase() const
	{
		return ImageBase;
	}

	std::size_t RemoteProcess::ModuleSize() const
	{
		return ImageSize;
	}

	bool RemoteProcess::Read(std::uintptr_t Address, void* Output, std::size_t Size) const
	{
		if (ConnectedProcess == nullptr || Output == nullptr || Size == 0)
		{
			return false;
		}

		SIZE_T BytesRead = 0;
		return ReadProcessMemory(
				   ConnectedProcess,
				   reinterpret_cast<LPCVOID>(Address),
				   Output,
				   Size,
				   &BytesRead) != FALSE &&
			BytesRead == Size;
	}

	bool RemoteProcess::IsReadOnlyRange(std::uintptr_t Address, std::size_t Size) const
	{
		MEMORY_BASIC_INFORMATION Information{};
		if (Size == 0 || VirtualQueryEx(
				ConnectedProcess, reinterpret_cast<LPCVOID>(Address),
				&Information, sizeof(Information)) == 0 ||
			Information.State != MEM_COMMIT ||
			(Information.Protect & (PAGE_GUARD | PAGE_NOACCESS)) != 0 ||
			(Information.Protect & 0xff) != PAGE_READONLY)
		{
			return false;
		}
		const auto Base = reinterpret_cast<std::uintptr_t>(Information.BaseAddress);
		return Address >= Base && Address - Base < Information.RegionSize &&
			Size <= Information.RegionSize - (Address - Base);
	}

	bool RemoteProcess::Write(
		std::uintptr_t Address,
		const void* Input,
		std::size_t Size,
		std::string& Error) const
	{
		if (ConnectedProcess == nullptr || Input == nullptr || Size == 0)
		{
			Error = "Invalid process memory write request.";
			return false;
		}

		MEMORY_BASIC_INFORMATION Information{};
		if (VirtualQueryEx(
				ConnectedProcess,
				reinterpret_cast<LPCVOID>(Address),
				&Information,
				sizeof(Information)) == 0 ||
			Information.State != MEM_COMMIT ||
			Information.Type != MEM_PRIVATE ||
			(Information.Protect & (PAGE_GUARD | PAGE_NOACCESS)) != 0)
		{
			Error = "The candidate address is no longer a committed private memory region.";
			return false;
		}

		const DWORD Protection = Information.Protect & 0xff;
		if (Protection != PAGE_READWRITE &&
			Protection != PAGE_WRITECOPY &&
			Protection != PAGE_EXECUTE_READWRITE &&
			Protection != PAGE_EXECUTE_WRITECOPY)
		{
			Error = "The candidate memory region is not writable; no protection changes were attempted.";
			return false;
		}

		SIZE_T BytesWritten = 0;
		if (!WriteProcessMemory(
				ConnectedProcess,
				reinterpret_cast<LPVOID>(Address),
				Input,
				Size,
				&BytesWritten) ||
			BytesWritten != Size)
		{
			Error = Win32Error("WriteProcessMemory", GetLastError());
			return false;
		}
		return true;
	}

	std::optional<MemoryRegion> RemoteProcess::WritablePrivateRegionAt(
		std::uintptr_t Address,
		std::string& Error) const
	{
		Error.clear();
		if (ConnectedProcess == nullptr)
		{
			Error = "No game process is attached.";
			return std::nullopt;
		}
		MEMORY_BASIC_INFORMATION Information{};
		if (VirtualQueryEx(
				ConnectedProcess, reinterpret_cast<LPCVOID>(Address),
				&Information, sizeof(Information)) == 0)
		{
			Error = Win32Error("VirtualQueryEx", GetLastError());
			return std::nullopt;
		}
		const DWORD Protection = Information.Protect & 0xff;
		const bool Writable = Protection == PAGE_READWRITE ||
			Protection == PAGE_WRITECOPY || Protection == PAGE_EXECUTE_READWRITE ||
			Protection == PAGE_EXECUTE_WRITECOPY;
		if (Information.State != MEM_COMMIT || Information.Type != MEM_PRIVATE ||
			!Writable || (Information.Protect & (PAGE_GUARD | PAGE_NOACCESS)) != 0)
		{
			return std::nullopt;
		}
		return MemoryRegion{
			reinterpret_cast<std::uintptr_t>(Information.BaseAddress),
			Information.RegionSize,
			reinterpret_cast<std::uintptr_t>(Information.AllocationBase)
		};
	}

	std::vector<MemoryRegion> RemoteProcess::WritablePrivateRegions(
		std::uint64_t MaximumBytes,
		std::uintptr_t& Cursor,
		bool& Truncated,
		std::string& Error) const
	{
		std::vector<MemoryRegion> Regions;
		Truncated = false;
		Error.clear();
		const std::uintptr_t StartCursor = Cursor;
		Cursor = 0;
		if (ConnectedProcess == nullptr)
		{
			Error = "No game process is attached.";
			return Regions;
		}

		SYSTEM_INFO SystemInfo{};
		GetSystemInfo(&SystemInfo);
		const auto MinimumAddress =
			reinterpret_cast<std::uintptr_t>(SystemInfo.lpMinimumApplicationAddress);
		const auto MaximumAddress = reinterpret_cast<std::uintptr_t>(SystemInfo.lpMaximumApplicationAddress);
		auto Address = StartCursor == 0 ||
				StartCursor < MinimumAddress ||
				StartCursor >= MaximumAddress
			? MinimumAddress
			: StartCursor;
		std::uint64_t ScannedBytes = 0;

		while (Address < MaximumAddress)
		{
			if (ScannedBytes >= MaximumBytes)
			{
				Truncated = true;
				Cursor = Address;
				break;
			}

			MEMORY_BASIC_INFORMATION Information{};
			const SIZE_T Queried = VirtualQueryEx(
				ConnectedProcess,
				reinterpret_cast<LPCVOID>(Address),
				&Information,
				sizeof(Information));
			if (Queried == 0)
			{
				const DWORD ErrorCode = GetLastError();
				if (ErrorCode != ERROR_INVALID_PARAMETER)
				{
					Error = Win32Error("VirtualQueryEx", ErrorCode);
				}
				break;
			}

			const DWORD Protection = Information.Protect & 0xff;
			const bool Writable =
				Protection == PAGE_READWRITE ||
				Protection == PAGE_WRITECOPY ||
				Protection == PAGE_EXECUTE_READWRITE ||
				Protection == PAGE_EXECUTE_WRITECOPY;
			const auto RegionBase = reinterpret_cast<std::uintptr_t>(Information.BaseAddress);
			const auto RegionEnd = RegionBase + Information.RegionSize;
			if (RegionEnd <= Address)
			{
				Error = "VirtualQueryEx returned a non-advancing memory region.";
				break;
			}

			if (Information.State == MEM_COMMIT &&
				Information.Type == MEM_PRIVATE &&
				Writable &&
				(Information.Protect & (PAGE_GUARD | PAGE_NOACCESS)) == 0)
			{
				const auto ClippedBase = std::max(Address, RegionBase);
				const std::uint64_t ClippedSize = RegionEnd - ClippedBase;
				if (ScannedBytes >= MaximumBytes)
				{
					Truncated = true;
					Cursor = ClippedBase;
					break;
				}

				const std::uint64_t Remaining = MaximumBytes - ScannedBytes;
				const std::uint64_t BytesToScan = std::min(ClippedSize, Remaining);
				if (BytesToScan != 0)
				{
					Regions.push_back({
						ClippedBase,
						static_cast<std::size_t>(BytesToScan),
						reinterpret_cast<std::uintptr_t>(Information.AllocationBase)
					});
					ScannedBytes += BytesToScan;
				}
				if (BytesToScan < ClippedSize)
				{
					Truncated = true;
					Cursor = ClippedBase + static_cast<std::uintptr_t>(BytesToScan);
					break;
				}
			}
			Address = RegionEnd;
		}

		return Regions;
	}

	std::vector<MemoryRegion> RemoteProcess::WritablePrivateAllocationRegions(
		std::uintptr_t AllocationBase,
		std::uint64_t MaximumBytes,
		bool& Truncated,
		std::string& Error) const
	{
		std::vector<MemoryRegion> Regions;
		Truncated = false;
		Error.clear();
		if (ConnectedProcess == nullptr)
		{
			Error = "No game process is attached.";
			return Regions;
		}
		if (AllocationBase < 0x10000 || MaximumBytes == 0)
		{
			Error = "Invalid private-allocation scan request.";
			return Regions;
		}

		SYSTEM_INFO SystemInfo{};
		GetSystemInfo(&SystemInfo);
		const auto MaximumAddress =
			reinterpret_cast<std::uintptr_t>(SystemInfo.lpMaximumApplicationAddress);
		auto Address = AllocationBase;
		std::uint64_t ScannedBytes = 0;

		while (Address < MaximumAddress)
		{
			if (ScannedBytes >= MaximumBytes)
			{
				Truncated = true;
				break;
			}

			MEMORY_BASIC_INFORMATION Information{};
			const SIZE_T Queried = VirtualQueryEx(
				ConnectedProcess,
				reinterpret_cast<LPCVOID>(Address),
				&Information,
				sizeof(Information));
			if (Queried == 0)
			{
				const DWORD ErrorCode = GetLastError();
				if (ErrorCode != ERROR_INVALID_PARAMETER)
				{
					Error = Win32Error("VirtualQueryEx", ErrorCode);
				}
				break;
			}

			if (reinterpret_cast<std::uintptr_t>(Information.AllocationBase) != AllocationBase)
			{
				break;
			}

			const DWORD Protection = Information.Protect & 0xff;
			const bool Writable =
				Protection == PAGE_READWRITE ||
				Protection == PAGE_WRITECOPY ||
				Protection == PAGE_EXECUTE_READWRITE ||
				Protection == PAGE_EXECUTE_WRITECOPY;
			const auto RegionBase = reinterpret_cast<std::uintptr_t>(Information.BaseAddress);
			const auto RegionEnd = RegionBase + Information.RegionSize;
			if (RegionEnd <= Address)
			{
				Error = "VirtualQueryEx returned a non-advancing allocation region.";
				break;
			}

			if (Information.State == MEM_COMMIT &&
				Information.Type == MEM_PRIVATE &&
				Writable &&
				(Information.Protect & (PAGE_GUARD | PAGE_NOACCESS)) == 0)
			{
				const auto ClippedBase = std::max(Address, RegionBase);
				const std::uint64_t ClippedSize = RegionEnd - ClippedBase;
				const std::uint64_t Remaining = MaximumBytes - ScannedBytes;
				const std::uint64_t BytesToScan = std::min(ClippedSize, Remaining);
				if (BytesToScan != 0)
				{
					Regions.push_back({
						ClippedBase,
						static_cast<std::size_t>(BytesToScan),
						AllocationBase
					});
					ScannedBytes += BytesToScan;
				}
				if (BytesToScan < ClippedSize)
				{
					Truncated = true;
					break;
				}
			}
			Address = RegionEnd;
		}

		return Regions;
	}
}
