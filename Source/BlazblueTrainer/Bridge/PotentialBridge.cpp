#include "PotentialProtocol.h"
#include "ManagedRuntime.h"
#include "PotentialScript.generated.h"
#include "Text.h"

#include <array>
#include <cstring>
#include <memory>
#include <stdexcept>
#include <string>

namespace
{
	using Trainer::Bridge::Export;
	using Trainer::Bridge::ManagedApi;

	void Execute(Trainer::PotentialCommand& Command)
	{
		const auto Managed = GetModuleHandleW(L"GameAssembly.dll");
		const auto Puerts = GetModuleHandleW(L"puerts.dll");
		if (!Managed || !Puerts)
			throw std::runtime_error("The game scripting runtime is not available.");
		const auto Eval = Export<void* (*)(void*, const char*, const char*)>(Puerts, "Eval");
		const auto ResultType = Export<int (*)(void*)>(Puerts, "GetResultType");
		const auto ResultString = Export<const char* (*)(void*, int*)>(Puerts, "GetStringFromResult");
		const auto Reset = Export<void (*)(void*)>(Puerts, "ResetResult");
		const auto LastException = Export<const char* (*)(void*, int*)>(Puerts, "GetLastExceptionInfo");
		const auto Engine = ManagedApi(Managed).ActiveEngine();
		const auto Value = Eval(Engine, GrantAllPotentialsScript, "BlazblueTrainer/GrantAllPotentials");
		if (!Value)
		{
			int Length{};
			const auto Detail = LastException(Engine, &Length);
			throw std::runtime_error(std::string("The potential script failed: ") +
				(Detail && Length > 0 && Length < 4096 ? std::string(Detail, Length) : "No exception details."));
		}
		struct ResultOwner
		{
			void* Value;
			void (*Reset)(void*);
			~ResultOwner() { Reset(Value); }
		} Owner{Value, Reset};
		if (ResultType(Value) != 8)
			throw std::runtime_error("The game returned an invalid potential result.");
		int Length{};
		const auto Text = ResultString(Value, &Length);
		if (!Text || Length <= 0 || static_cast<std::size_t>(Length) >= sizeof(Command.Response))
			throw std::runtime_error("The game returned an invalid potential result.");
		std::memcpy(Command.Response, Text, static_cast<std::size_t>(Length));
		Command.Response[Length] = '\0';
	}

	struct HandleCloser
	{
		void operator()(void* Handle) const { CloseHandle(Handle); }
	};
	struct ViewCloser
	{
		void operator()(Trainer::PotentialCommand* View) const { UnmapViewOfFile(View); }
	};

	void Receive(const CWPSTRUCT& Message)
	{
		if (Message.wParam == 0 || Message.wParam > MAXDWORD || !Message.lParam)
			return;
		const auto Requester = static_cast<DWORD>(Message.wParam);
		const auto Nonce = static_cast<std::uint64_t>(Message.lParam);
		const auto Name = Trainer::PotentialMappingName(Requester, Nonce);
		std::unique_ptr<void, HandleCloser> Mapping(
			OpenFileMappingW(FILE_MAP_READ | FILE_MAP_WRITE, FALSE, Name.c_str()));
		if (!Mapping)
			return;
		std::unique_ptr<Trainer::PotentialCommand, ViewCloser> Command(
			static_cast<Trainer::PotentialCommand*>(MapViewOfFile(
				Mapping.get(), FILE_MAP_READ | FILE_MAP_WRITE, 0, 0, sizeof(Trainer::PotentialCommand))));
		if (!Command || !Trainer::ValidPotentialCommand(*Command, Requester, GetCurrentProcessId(), Nonce) ||
			InterlockedCompareExchange(&Command->State, static_cast<LONG>(Trainer::PotentialCommandState::Executing),
				static_cast<LONG>(Trainer::PotentialCommandState::Pending)) !=
				static_cast<LONG>(Trainer::PotentialCommandState::Pending))
			return;
		Command->ExecutionThreadId = GetCurrentThreadId();
		try
		{
			Execute(*Command);
			InterlockedExchange(&Command->State, static_cast<LONG>(Trainer::PotentialCommandState::Completed));
		}
		catch (const std::exception& Error)
		{
			Trainer::CopyUtf8(Command->Response, sizeof(Command->Response), Error.what());
			InterlockedExchange(&Command->State, static_cast<LONG>(Trainer::PotentialCommandState::Failed));
		}
	}
}

extern "C" __declspec(dllexport) LRESULT CALLBACK TrainerPotentialHook(int Code, WPARAM Word, LPARAM Parameter)
{
	if (Code >= 0 && Parameter)
	{
		const auto Message = reinterpret_cast<const CWPSTRUCT*>(Parameter);
		const auto Expected = RegisterWindowMessageW(Trainer::PotentialMessageName);
		if (Expected && Message->message == Expected)
		{
			try
			{
				Receive(*Message);
			}
			catch (const std::exception& Error)
			{
				OutputDebugStringA("Blazblue potential request could not be opened: ");
				OutputDebugStringA(Error.what());
			}
		}
	}
	return CallNextHookEx(nullptr, Code, Word, Parameter);
}
