#pragma once

#include "EventAmplificationPolicy.h"
#include "ManagedRuntime.h"

#include <array>
#include <cmath>
#include <cstdint>
#include <limits>

namespace Trainer::Bridge
{
	// The bridge owns this object for as long as its JavaScript callback is reachable.
	// Keeping the binding separate also permits verification with a real Puerts engine.
	class EventAmplificationCallback final
	{
	public:
		using SessionCheck = bool (*)(void*) noexcept;
		EventAmplificationCallback(HMODULE Module, SessionCheck Check, void* Context)
			: SetGlobalFunction(Export<decltype(SetGlobalFunction)>(Module, "SetGlobalFunction")),
			  Argument(Export<decltype(Argument)>(Module, "GetArgumentValue")),
			  ValueType(Export<decltype(ValueType)>(Module, "GetJsValueType")),
			  Number(Export<decltype(Number)>(Module, "GetNumberFromValue")),
			  ReturnBoolean(Export<decltype(ReturnBoolean)>(Module, "ReturnBoolean")),
			  Check(Check), Context(Context) {}
		EventAmplificationCallback(const EventAmplificationCallback&) = delete;
		EventAmplificationCallback& operator=(const EventAmplificationCallback&) = delete;

		void Bind(void* Engine)
		{
			OwnerThread = GetCurrentThreadId();
			SetGlobalFunction(Engine, "__BlazblueTrainerEventAmplificationV1", Dispatch,
				reinterpret_cast<std::int64_t>(this));
		}

	private:
		using Callback = void (*)(void*, void*, void*, int, std::int64_t);
		void (*SetGlobalFunction)(void*, const char*, Callback, std::int64_t);
		void* (*Argument)(void*, void*, int);
		int (*ValueType)(void*, void*, bool);
		double (*Number)(void*, void*, bool);
		void (*ReturnBoolean)(void*, void*, bool);
		SessionCheck Check;
		void* Context;
		DWORD OwnerThread{};

		static void Dispatch(void* Isolate, void* Info, void*, int Count, std::int64_t Data) noexcept
		{
			auto& Self = *reinterpret_cast<EventAmplificationCallback*>(Data);
			bool Result = false;
			if (GetCurrentThreadId() == Self.OwnerThread && Self.Check && Self.Check(Self.Context))
			{
				if (Count == 0) Result = true;
				else if (Count == 4)
				{
					std::array<std::int32_t, 4> Values{};
					bool Valid = true;
					for (int Index = 0; Index < Count; ++Index)
					{
						const auto Value = Self.Argument(Isolate, Info, Index);
						// Puerts JsValueType.Number = 4. Reject coercion of strings and booleans.
						if (!Value || Self.ValueType(Isolate, Value, false) != 4) { Valid = false; break; }
						const auto Number = Self.Number(Isolate, Value, false);
						if (!std::isfinite(Number) || std::trunc(Number) != Number ||
							Number < INT32_MIN || Number > INT32_MAX) { Valid = false; break; }
						Values[static_cast<std::size_t>(Index)] = static_cast<std::int32_t>(Number);
					}
					Result = Valid && IsGuaranteedAmplificationReward(Values[0], Values[1], Values[2], Values[3]);
				}
			}
			Self.ReturnBoolean(Isolate, Info, Result);
		}
	};
}
