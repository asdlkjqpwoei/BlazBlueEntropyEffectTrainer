#include "EventAmplificationCallback.h"
#include "EventAmplificationProtocol.h"
#include "EventAmplificationScript.h"
#include "ManagedRuntime.h"
#include "Text.h"

#include <atomic>
#include <cstring>
#include <memory>
#include <stdexcept>
#include <string>

namespace
{
	struct HandleCloser { void operator()(void* Value) const { CloseHandle(Value); } };
	struct ViewCloser { void operator()(Trainer::EventAmplificationCommand* Value) const { UnmapViewOfFile(Value); } };
	using Handle = std::unique_ptr<void, HandleCloser>;
	struct Session
	{
		Handle Mapping;
		Handle Owner;
		std::unique_ptr<Trainer::EventAmplificationCommand, ViewCloser> Command;
		bool Alive() const
		{
			return Command && Owner && WaitForSingleObject(Owner.get(), 0) == WAIT_TIMEOUT &&
				InterlockedCompareExchange(&Command->Enabled, 0, 0) == 1 &&
				Trainer::EventAmplificationHeartbeatAlive(GetTickCount64(),
					static_cast<ULONGLONG>(InterlockedCompareExchange64(&Command->Heartbeat, 0, 0)));
		}
	};
	std::atomic<std::shared_ptr<Session>> Current;
	Handle Instance;
	std::unique_ptr<Trainer::Bridge::EventAmplificationCallback> Script;

	bool SessionAlive(void*) noexcept
	{
		const auto Session = Current.load();
		return Session && Session->Alive();
	}

	void Initialize()
	{
		const auto Managed = GetModuleHandleW(L"GameAssembly.dll"), Puerts = GetModuleHandleW(L"puerts.dll");
		if (!Managed || !Puerts) throw std::runtime_error("The game scripting runtime is not available.");
		const auto Engine = Trainer::Bridge::ManagedApi(Managed).ActiveEngine();
		if (!Instance)
		{
			wchar_t Name[96]{};
			swprintf_s(Name, L"Local\\BlazblueTrainer.EventAmplificationBridge.%lu", GetCurrentProcessId());
			Instance.reset(CreateMutexW(nullptr, FALSE, Name));
			if (!Instance || GetLastError() == ERROR_ALREADY_EXISTS)
			{
				Instance.reset();
				throw std::runtime_error("Another event-amplification bridge is already loaded; restart the game to use this build.");
			}
			HMODULE Resident{};
			if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
				reinterpret_cast<LPCWSTR>(&Initialize), &Resident))
			{
				Instance.reset();
				throw std::runtime_error("The event-amplification bridge could not remain loaded.");
			}
		}
		if (!Script) Script = std::make_unique<Trainer::Bridge::EventAmplificationCallback>(Puerts, SessionAlive, nullptr);
		Script->Bind(Engine);
		const auto Eval = Trainer::Bridge::Export<void* (*)(void*, const char*, const char*)>(Puerts, "Eval");
		const auto Type = Trainer::Bridge::Export<int (*)(void*)>(Puerts, "GetResultType");
		const auto String = Trainer::Bridge::Export<const char* (*)(void*, int*)>(Puerts, "GetStringFromResult");
		const auto Reset = Trainer::Bridge::Export<void (*)(void*)>(Puerts, "ResetResult");
		const auto Exception = Trainer::Bridge::Export<const char* (*)(void*, int*)>(Puerts, "GetLastExceptionInfo");
		const auto Value = Eval(Engine, Trainer::Bridge::EventAmplificationScript, "BlazblueTrainer/EventAmplification");
		if (!Value)
		{
			int Length{};
			const auto Detail = Exception(Engine, &Length);
			throw std::runtime_error(std::string("The event-amplification adapter failed: ") +
				(Detail && Length > 0 && Length < 4096 ? std::string(Detail, Length) : "No exception details."));
		}
		struct ResultOwner { void* Value; void (*Reset)(void*); ~ResultOwner() { Reset(Value); } } Owner{Value, Reset};
		int Length{};
		const auto Text = Type(Value) == 8 ? String(Value, &Length) : nullptr;
		if (!Text || Length != 9 || std::memcmp(Text, "installed", 9) != 0)
			throw std::runtime_error("The current game does not expose a compatible event-amplification API.");
	}

	void Receive(const CWPSTRUCT& Message)
	{
		if (!Message.wParam || Message.wParam > MAXDWORD || !Message.lParam) return;
		const auto Requester = static_cast<DWORD>(Message.wParam);
		const auto Nonce = static_cast<std::uint64_t>(Message.lParam);
		const auto Next = std::make_shared<Session>();
		const auto Name = Trainer::EventAmplificationMappingName(Requester, Nonce);
		Next->Mapping.reset(OpenFileMappingW(FILE_MAP_READ | FILE_MAP_WRITE, FALSE, Name.c_str()));
		if (!Next->Mapping) return;
		Next->Command.reset(static_cast<Trainer::EventAmplificationCommand*>(MapViewOfFile(Next->Mapping.get(),
			FILE_MAP_READ | FILE_MAP_WRITE, 0, 0, sizeof(Trainer::EventAmplificationCommand))));
		if (!Next->Command || !Trainer::ValidEventAmplificationCommand(*Next->Command, Requester, GetCurrentProcessId(), Nonce) ||
			InterlockedCompareExchange(&Next->Command->State, static_cast<LONG>(Trainer::EventAmplificationCommandState::Executing),
				static_cast<LONG>(Trainer::EventAmplificationCommandState::Pending)) != static_cast<LONG>(Trainer::EventAmplificationCommandState::Pending)) return;
		auto& Command = *Next->Command;
		Command.ExecutionThreadId = GetCurrentThreadId();
		try
		{
			Next->Owner.reset(OpenProcess(SYNCHRONIZE, FALSE, Requester));
			if (!Next->Alive()) throw std::runtime_error("The event-amplification request was cancelled.");
			const auto Previous = Current.load();
			if (Previous && Previous->Alive()) throw std::runtime_error("Another trainer is already maintaining event amplification.");
			Current.store(Next);
			Initialize();
			if (!Next->Alive()) throw std::runtime_error("The event-amplification request was cancelled.");
			InterlockedExchange(&Command.State, static_cast<LONG>(Trainer::EventAmplificationCommandState::Completed));
		}
		catch (const std::exception& Failure)
		{
			InterlockedExchange(&Command.Enabled, 0);
			Trainer::CopyUtf8(Command.Error, sizeof(Command.Error), Failure.what());
			InterlockedExchange(&Command.State, static_cast<LONG>(Trainer::EventAmplificationCommandState::Failed));
		}
	}
}

extern "C" __declspec(dllexport) LRESULT CALLBACK TrainerEventAmplificationHook(int Code, WPARAM Word, LPARAM Parameter)
{
	if (Code >= 0 && Parameter)
	{
		const auto Message = reinterpret_cast<const CWPSTRUCT*>(Parameter);
		const auto Expected = RegisterWindowMessageW(Trainer::EventAmplificationMessageName);
		if (Expected && Message->message == Expected)
		{
			try { Receive(*Message); }
			catch (const std::exception& Failure) { OutputDebugStringA(Failure.what()); }
		}
	}
	return CallNextHookEx(nullptr, Code, Word, Parameter);
}
