#include "PerfectHooks.h"
#include "PerfectProtocol.h"
#include "Text.h"

#include <atomic>
#include <cstring>
#include <memory>
#include <stdexcept>
#include <string>

namespace
{
	struct HandleCloser { void operator()(void* Handle) const { CloseHandle(Handle); } };
	struct ViewCloser { void operator()(Trainer::PerfectCommand* View) const { UnmapViewOfFile(View); } };
	using Handle = std::unique_ptr<void, HandleCloser>;
	struct Session
	{
		Handle Mapping;
		Handle Owner;
		std::unique_ptr<Trainer::PerfectCommand, ViewCloser> Command;
		bool Alive() const
		{
			return Command && Owner && WaitForSingleObject(Owner.get(), 0) == WAIT_TIMEOUT &&
				InterlockedCompareExchange(&Command->Enabled, 0, 0) == 1 &&
				Trainer::PerfectHeartbeatAlive(GetTickCount64(),
					static_cast<ULONGLONG>(InterlockedCompareExchange64(&Command->Heartbeat, 0, 0)));
		}
	};
	std::atomic<std::shared_ptr<Session>> Current;
	Handle Instance;
	bool Installed{};
	DWORD GameThread{};
	using CurrentFunction = void* (*)(const void*);
	using OnlineFunction = bool (*)(const void*);
	CurrentFunction CurrentBattle{};
	OnlineFunction OnlineMode{};
	const void* CurrentMethod{};
	const void* OnlineMethod{};
	std::size_t SelfOffset{}, CoopOffset{};

	[[noreturn]] void Incompatible()
	{ throw std::runtime_error("The current game does not expose a compatible perfect-guard API."); }

	template<class Function> Function Export(HMODULE Module, const char* Name)
	{
		const auto Address = GetProcAddress(Module, Name);
		if (!Address) Incompatible();
		return reinterpret_cast<Function>(Address);
	}

	struct Api
	{
		void* (*Domain)();
		const void** (*Assemblies)(const void*, std::size_t*);
		const void* (*Image)(const void*);
		const char* (*ImageName)(const void*);
		void* (*Class)(const void*, const char*, const char*);
		const void* (*Method)(void*, const char*, int);
		void* (*Field)(void*, const char*);
		std::size_t (*Offset)(void*);
		int (*Flags)(void*);
		const void* (*FieldType)(void*);
		int (*TypeKind)(const void*);
		void* (*TypeClass)(const void*);
		std::int32_t (*ValueSize)(void*, std::uint32_t*);
		std::uint32_t (*InstanceSize)(void*);
		void* (*CurrentThread)();
		HMODULE Module;
		const void* GameImage{};
		explicit Api(HMODULE Game) : Module(Game)
		{
#define LOAD(Member, Name) Member = Export<decltype(Member)>(Game, Name)
			LOAD(Domain, "il2cpp_domain_get"); LOAD(Assemblies, "il2cpp_domain_get_assemblies");
			LOAD(Image, "il2cpp_assembly_get_image"); LOAD(ImageName, "il2cpp_image_get_name");
			LOAD(Class, "il2cpp_class_from_name"); LOAD(Method, "il2cpp_class_get_method_from_name");
			LOAD(Field, "il2cpp_class_get_field_from_name"); LOAD(Offset, "il2cpp_field_get_offset");
			LOAD(Flags, "il2cpp_field_get_flags"); LOAD(FieldType, "il2cpp_field_get_type");
			LOAD(TypeKind, "il2cpp_type_get_type"); LOAD(TypeClass, "il2cpp_class_from_type");
			LOAD(ValueSize, "il2cpp_class_value_size"); LOAD(InstanceSize, "il2cpp_class_instance_size");
			LOAD(CurrentThread, "il2cpp_thread_current");
#undef LOAD
			if (!CurrentThread()) Incompatible();
			const auto Root = Domain();
			if (!Root) Incompatible();
			std::size_t Count{};
			const auto Loaded = Assemblies(Root, &Count);
			if (!Loaded || !Count || Count > 4096) Incompatible();
			for (std::size_t i = 0; i < Count; ++i)
			{
				const auto Candidate = Image(Loaded[i]);
				const auto Name = Candidate ? ImageName(Candidate) : nullptr;
				if (Name && std::strcmp(Name, "Assembly-CSharp.dll") == 0)
				{
					if (GameImage) Incompatible();
					GameImage = Candidate;
				}
			}
			if (!GameImage) Incompatible();
		}
		void* FindClass(const char* Name, const char* Space = "GamePlay") const
		{
			const auto Result = Class(GameImage, Space, Name);
			if (!Result) Incompatible();
			return Result;
		}
		std::size_t FieldOffset(void* Owner, const char* Name, std::size_t Width, int Kind) const
		{
			const auto Info = Field(Owner, Name);
			if (!Info || (Flags(Info) & 0x10)) Incompatible();
			const auto Type = FieldType(Info);
			if (!Type || TypeKind(Type) != Kind) Incompatible();
			if (Kind == 0x11)
			{
				const auto Value = TypeClass(Type);
				std::uint32_t Alignment{};
				if (!Value || ValueSize(Value, &Alignment) != static_cast<std::int32_t>(Width)) Incompatible();
			}
			const auto Result = Offset(Info);
			const auto Size = InstanceSize(Owner);
			if (Result < 16 || Result > 65536 || Result + Width > Size) Incompatible();
			return Result;
		}
		template<class Function> Function Native(void* Owner, const char* Name, int Count, const void** Info = nullptr) const
		{
			const auto MethodInfo = Method(Owner, Name, Count);
			if (!MethodInfo) Incompatible();
			void* Pointer{};
			std::memcpy(&Pointer, MethodInfo, sizeof(Pointer));
			MEMORY_BASIC_INFORMATION Region{};
			HMODULE OwnerModule{};
			if (!Pointer || !VirtualQuery(Pointer, &Region, sizeof(Region)) || Region.State != MEM_COMMIT ||
				(Region.Protect & (PAGE_GUARD | PAGE_NOACCESS)) ||
				!(Region.Protect & (PAGE_EXECUTE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY)) ||
				!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
					reinterpret_cast<LPCWSTR>(Pointer), &OwnerModule) || OwnerModule != Module) Incompatible();
			if (Info) *Info = MethodInfo;
			return reinterpret_cast<Function>(Pointer);
		}
	};

	bool Protect(void* Player)
	{
		const auto Session = Current.load();
		if (!Session || !Session->Alive() || GetCurrentThreadId() != GameThread || OnlineMode(OnlineMethod)) return false;
		const auto Battle = CurrentBattle(CurrentMethod);
		if (!Battle) return false;
		bool Coop{};
		void* Self{};
		std::memcpy(&Coop, static_cast<std::byte*>(Battle) + CoopOffset, sizeof(Coop));
		std::memcpy(&Self, static_cast<std::byte*>(Battle) + SelfOffset, sizeof(Self));
		return !Coop && Self == Player;
	}

	void Initialize()
	{
		if (Installed) return;
		const auto Module = GetModuleHandleW(L"GameAssembly.dll");
		if (!Module) throw std::runtime_error("The game runtime is not available for perfect guard.");
		Api Managed(Module);
		const auto Player = Managed.FindClass("PlayerObj"), Statistics = Managed.FindClass("BattlePlayerStatistics");
		const auto Kills = Managed.FindClass("PlayerComboKill"), Battle = Managed.FindClass("BattleBase");
		const auto Pvp = Managed.FindClass("PvpDefine", "");
		Trainer::PerfectBindings Bindings;
		Bindings.NotifyBeHit = Managed.Native<Trainer::PerfectHitFunction>(Player, "NotifyBeHit", 1);
		Bindings.OnBehit = Managed.Native<Trainer::PerfectHitFunction>(Statistics, "OnBehit", 1);
		Bindings.ResetKills = Managed.Native<Trainer::PerfectResetFunction>(Kills, "Reset", 0);
		Bindings.PlayerStatistics = Managed.FieldOffset(Player, "<Statistics>k__BackingField", 8, 0x12);
		Bindings.PlayerComboKill = Managed.FieldOffset(Player, "ComboKill", 8, 0x12);
		Bindings.StatisticsOwner = Managed.FieldOffset(Statistics, "Owner", 8, 0x12);
		Bindings.BehitTimes = Managed.FieldOffset(Statistics, "<BehitTimes>k__BackingField", 4, 0x08);
		Bindings.TotalTakenDamage = Managed.FieldOffset(Statistics, "<TotalTakenDamage>k__BackingField", 8, 0x11);
		SelfOffset = Managed.FieldOffset(Battle, "PlayerSelf", 8, 0x12);
		CoopOffset = Managed.FieldOffset(Battle, "IsLocalCoopMode", 1, 0x02);
		CurrentBattle = Managed.Native<CurrentFunction>(Battle, "get_Cur", 0, &CurrentMethod);
		OnlineMode = Managed.Native<OnlineFunction>(Pvp, "IsOnlineMode", 0, &OnlineMethod);
		// Initialize managed static getters before they are used inside an intercepted call.
		CurrentBattle(CurrentMethod);
		OnlineMode(OnlineMethod);
		GameThread = GetCurrentThreadId();
		Bindings.ShouldProtect = Protect;

		wchar_t Name[80]{};
		swprintf_s(Name, L"Local\\BlazblueTrainer.PerfectBridge.%lu", GetCurrentProcessId());
		Instance.reset(CreateMutexW(nullptr, FALSE, Name));
		if (!Instance || GetLastError() == ERROR_ALREADY_EXISTS)
			throw std::runtime_error("Another perfect-guard bridge is already loaded; restart the game to use this build.");
		HMODULE Resident{};
		if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
			reinterpret_cast<LPCWSTR>(&Initialize), &Resident)) Incompatible();
		std::string Error;
		if (!Trainer::InstallPerfectHooks(Bindings, Error)) { Instance.reset(); throw std::runtime_error(Error); }
		Installed = true;
	}

	void Receive(const CWPSTRUCT& Message)
	{
		if (!Message.wParam || Message.wParam > MAXDWORD || !Message.lParam) return;
		const auto Requester = static_cast<DWORD>(Message.wParam);
		const auto Nonce = static_cast<std::uint64_t>(Message.lParam);
		const auto Name = Trainer::PerfectMappingName(Requester, Nonce);
		const auto Next = std::make_shared<Session>();
		Next->Mapping.reset(OpenFileMappingW(FILE_MAP_READ | FILE_MAP_WRITE, FALSE, Name.c_str()));
		if (!Next->Mapping) return;
		Next->Command.reset(static_cast<Trainer::PerfectCommand*>(MapViewOfFile(Next->Mapping.get(),
			FILE_MAP_READ | FILE_MAP_WRITE, 0, 0, sizeof(Trainer::PerfectCommand))));
		if (!Next->Command || !Trainer::ValidPerfectCommand(*Next->Command, Requester, GetCurrentProcessId(), Nonce) ||
			InterlockedCompareExchange(&Next->Command->State, static_cast<LONG>(Trainer::PerfectCommandState::Executing),
				static_cast<LONG>(Trainer::PerfectCommandState::Pending)) != static_cast<LONG>(Trainer::PerfectCommandState::Pending)) return;
		auto& Command = *Next->Command;
		Command.ExecutionThreadId = GetCurrentThreadId();
		try
		{
			Next->Owner.reset(OpenProcess(SYNCHRONIZE, FALSE, Requester));
			if (!Next->Alive()) throw std::runtime_error("The perfect-guard request was cancelled.");
			const auto Previous = Current.load();
			if (Previous && Previous->Alive()) throw std::runtime_error("Another trainer is already maintaining perfect guard.");
			Initialize();
			if (!Next->Alive()) throw std::runtime_error("The perfect-guard request was cancelled.");
			Current.store(Next);
			InterlockedExchange(&Command.State, static_cast<LONG>(Trainer::PerfectCommandState::Completed));
		}
		catch (const std::exception& Failure)
		{
			InterlockedExchange(&Command.Enabled, 0);
			Trainer::CopyUtf8(Command.Error, sizeof(Command.Error), Failure.what());
			InterlockedExchange(&Command.State, static_cast<LONG>(Trainer::PerfectCommandState::Failed));
		}
	}
}

extern "C" __declspec(dllexport) LRESULT CALLBACK TrainerPerfectHook(int Code, WPARAM Word, LPARAM Parameter)
{
	if (Code >= 0 && Parameter)
	{
		const auto Message = reinterpret_cast<const CWPSTRUCT*>(Parameter);
		const auto Expected = RegisterWindowMessageW(Trainer::PerfectMessageName);
		if (Expected && Message->message == Expected)
		{
			try { Receive(*Message); }
			catch (const std::exception& Failure) { OutputDebugStringA(Failure.what()); }
		}
	}
	return CallNextHookEx(nullptr, Code, Word, Parameter);
}
