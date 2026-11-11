#pragma once

#include <Windows.h>
#include <array>
#include <cstring>
#include <stdexcept>
#include <string>

namespace Trainer::Bridge
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

}
