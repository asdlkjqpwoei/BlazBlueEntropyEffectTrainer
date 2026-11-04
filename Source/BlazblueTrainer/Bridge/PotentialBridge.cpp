#include "PotentialProtocol.h"
#include "PotentialScript.generated.h"
#include "Text.h"

#include <array>
#include <cstring>
#include <memory>
#include <stdexcept>
#include <string>

namespace
{
	struct Il2CppDomain;
	struct Il2CppAssembly;
	struct Il2CppImage;
	struct Il2CppClass;
	struct Il2CppObject;
	struct Il2CppException;
	struct Il2CppThread;
	struct Il2CppType;
	struct FieldInfo;
	struct MethodInfo;

	template<typename Function>
	Function Export(HMODULE Module, const char* Name)
	{
		const auto Address = GetProcAddress(Module, Name);
		if (!Address)
			throw std::runtime_error(std::string("A required game scripting export is missing: ") + Name);
		return reinterpret_cast<Function>(Address);
	}

	struct ManagedApi
	{
		Il2CppDomain* (*Domain)();
		const Il2CppAssembly** (*Assemblies)(const Il2CppDomain*, std::size_t*);
		const Il2CppImage* (*Image)(const Il2CppAssembly*);
		Il2CppClass* (*Class)(const Il2CppImage*, const char*, const char*);
		Il2CppClass* (*ObjectClass)(Il2CppObject*);
		FieldInfo* (*Field)(Il2CppClass*, const char*);
		int (*FieldFlags)(FieldInfo*);
		void (*StaticValue)(FieldInfo*, void*);
		const MethodInfo* (*Method)(Il2CppClass*, const char*, int);
		const Il2CppType* (*ReturnType)(const MethodInfo*);
		int (*TypeKind)(const Il2CppType*);
		Il2CppObject* (*Invoke)(const MethodInfo*, void*, void**, Il2CppException**);
		void* (*Unbox)(Il2CppObject*);
		Il2CppThread* (*CurrentThread)();
		void (*FormatException)(const Il2CppException*, char*, int);

		explicit ManagedApi(HMODULE Module)
			: Domain(Export<decltype(Domain)>(Module, "il2cpp_domain_get")),
			  Assemblies(Export<decltype(Assemblies)>(Module, "il2cpp_domain_get_assemblies")),
			  Image(Export<decltype(Image)>(Module, "il2cpp_assembly_get_image")),
			  Class(Export<decltype(Class)>(Module, "il2cpp_class_from_name")),
			  ObjectClass(Export<decltype(ObjectClass)>(Module, "il2cpp_object_get_class")),
			  Field(Export<decltype(Field)>(Module, "il2cpp_class_get_field_from_name")),
			  FieldFlags(Export<decltype(FieldFlags)>(Module, "il2cpp_field_get_flags")),
			  StaticValue(Export<decltype(StaticValue)>(Module, "il2cpp_field_static_get_value")),
			  Method(Export<decltype(Method)>(Module, "il2cpp_class_get_method_from_name")),
			  ReturnType(Export<decltype(ReturnType)>(Module, "il2cpp_method_get_return_type")),
			  TypeKind(Export<decltype(TypeKind)>(Module, "il2cpp_type_get_type")),
			  Invoke(Export<decltype(Invoke)>(Module, "il2cpp_runtime_invoke")),
			  Unbox(Export<decltype(Unbox)>(Module, "il2cpp_object_unbox")),
			  CurrentThread(Export<decltype(CurrentThread)>(Module, "il2cpp_thread_current")),
			  FormatException(Export<decltype(FormatException)>(Module, "il2cpp_format_exception"))
		{
		}

		Il2CppObject* Call(const MethodInfo* MethodInfo, Il2CppObject* Object, void** Arguments = nullptr) const
		{
			if (!MethodInfo)
				throw std::runtime_error("The managed game scripting environment is incompatible.");
			Il2CppException* Exception{};
			auto Result = Invoke(MethodInfo, Object, Arguments, &Exception);
			if (Exception)
			{
				std::array<char, 1024> Detail{};
				FormatException(Exception, Detail.data(), static_cast<int>(Detail.size()));
				Detail.back() = '\0';
				throw std::runtime_error(
					std::string("A managed game scripting call failed: ") + Detail.data());
			}
			return Result;
		}

		void* ActiveEngine() const
		{
			// A window hook must already be on an attached Unity thread, never a new worker thread.
			if (!CurrentThread())
				throw std::runtime_error("The game window thread is not an attached Unity thread.");
			const auto GameDomain = Domain();
			if (!GameDomain)
				throw std::runtime_error("The managed game scripting environment is not ready.");
			std::size_t Count{};
			const auto Loaded = Assemblies(GameDomain, &Count);
			if (!Loaded || !Count || Count > 4096)
				throw std::runtime_error("The managed game scripting environment is incompatible.");
			Il2CppClass* JsEnv{};
			for (std::size_t Index = 0; Index < Count; ++Index)
			{
				const auto AssemblyImage = Loaded[Index] ? Image(Loaded[Index]) : nullptr;
				if (!AssemblyImage)
					throw std::runtime_error("The managed game scripting environment is incompatible.");
				const auto Candidate = Class(AssemblyImage, "Puerts", "JsEnv");
				if (!Candidate)
					continue;
				if (JsEnv && JsEnv != Candidate)
					throw std::runtime_error("A unique game scripting environment could not be verified.");
				JsEnv = Candidate;
			}
			const auto Environments = JsEnv ? Field(JsEnv, "jsEnvs") : nullptr;
			const auto IsolateGetter = JsEnv ? Method(JsEnv, "get_Isolate", 0) : nullptr;
			if (!Environments || (FieldFlags(Environments) & 0x10) == 0 || !IsolateGetter ||
				TypeKind(ReturnType(IsolateGetter)) != 0x18)
				throw std::runtime_error("The managed game scripting environment is incompatible.");
			Il2CppObject* List{};
			StaticValue(Environments, &List);
			if (!List)
				throw std::runtime_error("The managed game scripting environment is not ready.");
			const auto ListClass = ObjectClass(List);
			const auto CountGetter = Method(ListClass, "get_Count", 0);
			const auto ItemGetter = Method(ListClass, "get_Item", 1);
			if (!CountGetter || !ItemGetter || TypeKind(ReturnType(CountGetter)) != 8)
				throw std::runtime_error("The managed game scripting environment is incompatible.");
			const auto BoxedCount = Call(CountGetter, List);
			const auto CountValue = BoxedCount ? static_cast<const int*>(Unbox(BoxedCount)) : nullptr;
			if (!CountValue || *CountValue < 1 || *CountValue > 16)
				throw std::runtime_error("A unique game scripting environment could not be verified.");
			const auto EnvironmentCount = *CountValue;
			void* Engine{};
			for (int Index = 0; Index < EnvironmentCount; ++Index)
			{
				void* Arguments[]{&Index};
				const auto Environment = Call(ItemGetter, List, Arguments);
				if (!Environment)
					continue;
				if (ObjectClass(Environment) != JsEnv)
					throw std::runtime_error("The managed game scripting environment is incompatible.");
				const auto BoxedEngine = Call(IsolateGetter, Environment);
				const auto Address = BoxedEngine ? static_cast<void* const*>(Unbox(BoxedEngine)) : nullptr;
				if (!Address || !*Address)
					continue;
				if (Engine && Engine != *Address)
					throw std::runtime_error("A unique game scripting environment could not be verified.");
				Engine = *Address;
			}
			if (!Engine)
				throw std::runtime_error("The managed game scripting environment is not ready.");
			return Engine;
		}
	};

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
