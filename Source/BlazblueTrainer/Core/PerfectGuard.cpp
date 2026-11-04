#include "PerfectGuard.h"
#include "Paths.h"
#include "PerfectProtocol.h"

#include <bcrypt.h>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <new>
#include <stdexcept>

namespace Trainer
{
	namespace
	{
		struct HandleCloser { void operator()(void* Handle) const { CloseHandle(Handle); } };
		struct ViewCloser { void operator()(PerfectCommand* View) const { UnmapViewOfFile(View); } };
		using Handle = std::unique_ptr<void, HandleCloser>;
		struct GameWindow { DWORD ProcessId{}; HWND Window{}; bool Ambiguous{}; };
		BOOL CALLBACK FindWindow(HWND Window, LPARAM Parameter)
		{
			auto& Target = *reinterpret_cast<GameWindow*>(Parameter);
			DWORD Process{};
			GetWindowThreadProcessId(Window, &Process);
			wchar_t Class[128]{};
			if (Process == Target.ProcessId && IsWindowVisible(Window) && !GetWindow(Window, GW_OWNER) &&
				GetClassNameW(Window, Class, static_cast<int>(std::size(Class))) &&
				std::wcscmp(Class, L"UnityWndClass") == 0)
			{
				if (Target.Window) Target.Ambiguous = true;
				else Target.Window = Window;
			}
			return TRUE;
		}
		std::runtime_error WindowsError(const char* Operation)
		{
			return std::runtime_error(std::string("Perfect guard: ") + Operation +
				" failed with Windows error " + std::to_string(GetLastError()) + ".");
		}
	}

	struct PerfectGuard::Session
	{
		Handle Mapping;
		Handle Game;
		std::unique_ptr<PerfectCommand, ViewCloser> Command;
		std::jthread Heartbeat;

		~Session()
		{
			if (Command) InterlockedExchange(&Command->Enabled, 0);
			if (Heartbeat.joinable()) Heartbeat.request_stop();
		}
		bool Alive(DWORD ProcessId) const
		{
			return Command && Game && Command->TargetProcessId == ProcessId &&
				WaitForSingleObject(Game.get(), 0) == WAIT_TIMEOUT &&
				InterlockedCompareExchange(&Command->Enabled, 0, 0) == 1 &&
				InterlockedCompareExchange(&Command->State, 0, 0) == static_cast<LONG>(PerfectCommandState::Completed) &&
				PerfectHeartbeatAlive(GetTickCount64(), static_cast<ULONGLONG>(InterlockedCompareExchange64(&Command->Heartbeat, 0, 0)));
		}
	};

	PerfectGuard::~PerfectGuard() { Disable(); }

	void PerfectGuard::Disable()
	{
		const auto Session = Current.exchange({});
		if (Session && Session->Command) InterlockedExchange(&Session->Command->Enabled, 0);
	}

	bool PerfectGuard::Enabled(DWORD ProcessId) const
	{
		const auto Session = Current.load();
		return Session && Session->Alive(ProcessId);
	}

	bool PerfectGuard::Enable(DWORD ProcessId, std::string& Error, std::stop_token StopToken)
	{
		Error.clear();
		std::unique_lock Lock(OperationMutex, std::try_to_lock);
		if (!Lock.owns_lock()) { Error = "A perfect-guard request is already in progress."; return false; }
		Disable();
		if (!ProcessId || StopToken.stop_requested()) { Error = "The perfect-guard request was cancelled."; return false; }
		try
		{
			const auto Session = std::make_shared<PerfectGuard::Session>();
			Session->Game.reset(OpenProcess(SYNCHRONIZE, FALSE, ProcessId));
			if (!Session->Game) throw WindowsError("OpenProcess");
			GameWindow Target{ProcessId};
			if (!EnumWindows(FindWindow, reinterpret_cast<LPARAM>(&Target))) throw WindowsError("EnumWindows");
			if (!Target.Window || Target.Ambiguous)
				throw std::runtime_error("A unique active game window could not be verified.");
			DWORD WindowProcess{};
			const auto Thread = GetWindowThreadProcessId(Target.Window, &WindowProcess);
			if (!Thread || WindowProcess != ProcessId) throw std::runtime_error("A unique active game window could not be verified.");
			const auto Message = RegisterWindowMessageW(PerfectMessageName);
			if (!Message) throw WindowsError("RegisterWindowMessage");
			std::uint64_t Nonce{};
			if (BCryptGenRandom(nullptr, reinterpret_cast<PUCHAR>(&Nonce), sizeof(Nonce), BCRYPT_USE_SYSTEM_PREFERRED_RNG) < 0 || !Nonce)
				throw std::runtime_error("Cannot create a unique perfect-guard request.");
			const auto Requester = GetCurrentProcessId();
			const auto Name = PerfectMappingName(Requester, Nonce);
			Session->Mapping.reset(CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, sizeof(PerfectCommand), Name.c_str()));
			if (!Session->Mapping || GetLastError() == ERROR_ALREADY_EXISTS) throw WindowsError("CreateFileMapping");
			Session->Command.reset(static_cast<PerfectCommand*>(MapViewOfFile(Session->Mapping.get(), FILE_MAP_READ | FILE_MAP_WRITE, 0, 0, sizeof(PerfectCommand))));
			if (!Session->Command) throw WindowsError("MapViewOfFile");
			auto& Command = *Session->Command;
			new (&Command) PerfectCommand;
			Command.RequesterProcessId = Requester;
			Command.TargetProcessId = ProcessId;
			Command.Nonce = Nonce;
			InterlockedExchange64(&Command.Heartbeat, static_cast<LONG64>(GetTickCount64()));
			InterlockedExchange(&Command.Enabled, 1);
			const auto Path = ExecutableDirectory() / RuntimeBridgeFileName;
			const auto Module = LoadLibraryExW(Path.c_str(), nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
			if (!Module) throw WindowsError("LoadLibrary");
			const auto Callback = reinterpret_cast<HOOKPROC>(GetProcAddress(Module, "TrainerPerfectHook"));
			const auto Hook = Callback ? SetWindowsHookExW(WH_CALLWNDPROC, Callback, Module, Thread) : nullptr;
			if (!Hook)
			{
				const auto Failure = WindowsError(Callback ? "SetWindowsHookEx" : "GetProcAddress");
				FreeLibrary(Module);
				throw Failure;
			}
			std::stop_callback Cancel(StopToken, [&] { InterlockedExchange(&Command.Enabled, 0); });
			DWORD_PTR Result{};
			SendMessageTimeoutW(Target.Window, Message, Requester, static_cast<LPARAM>(Nonce),
				SMTO_ABORTIFHUNG | SMTO_BLOCK | SMTO_ERRORONEXIT, PerfectRequestTimeoutMs, &Result);
			const auto State = static_cast<PerfectCommandState>(InterlockedCompareExchange(&Command.State,
				static_cast<LONG>(PerfectCommandState::Cancelled), static_cast<LONG>(PerfectCommandState::Pending)));
			const bool Detached = UnhookWindowsHookEx(Hook) != 0;
			if (Detached) FreeLibrary(Module); // Keep a still-referenced hook module loaded on failure.
			if (!Detached) throw WindowsError("UnhookWindowsHookEx");
			if (State != PerfectCommandState::Completed || StopToken.stop_requested() || !Session->Alive(ProcessId))
			{
				if (State == PerfectCommandState::Failed && std::memchr(Command.Error, '\0', sizeof(Command.Error)))
					Error = Command.Error;
				if (Error.empty()) Error = "The game did not enable perfect guard.";
				return false;
			}
			Session->Heartbeat = std::jthread([View = Session->Command.get(), Game = Session->Game.get()](std::stop_token Stop)
			{
				std::mutex Mutex;
				std::condition_variable_any Wake;
				std::unique_lock Wait(Mutex);
				while (!Stop.stop_requested() && WaitForSingleObject(Game, 0) == WAIT_TIMEOUT)
				{
					InterlockedExchange64(&View->Heartbeat, static_cast<LONG64>(GetTickCount64()));
					Wake.wait_for(Wait, Stop, std::chrono::seconds(1), [] { return false; });
				}
				InterlockedExchange(&View->Enabled, 0);
			});
			Current.store(Session);
			if (StopToken.stop_requested()) { Disable(); Error = "The perfect-guard request was cancelled."; return false; }
			return true;
		}
		catch (const std::exception& Failure) { Error = Failure.what(); return false; }
	}
}
