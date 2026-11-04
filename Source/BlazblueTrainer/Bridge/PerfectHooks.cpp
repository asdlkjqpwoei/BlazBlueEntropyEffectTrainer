#include "PerfectHooks.h"
#include <MinHook.h>
#include <array>
#include <cstdint>
#include <cstring>

namespace Trainer
{
	namespace
	{
		PerfectBindings Bound;
		PerfectHitFunction OriginalNotify{};
		PerfectHitFunction OriginalStatistics{};
		PerfectResetFunction OriginalReset{};
		bool Installed{};
		bool Initialized{};
		struct Context { void* Statistics{}; void* Kills{}; };
		thread_local Context Active;

		template<class T> T Read(void* Object, std::size_t Offset)
		{
			T Value{};
			if (Object) std::memcpy(&Value, static_cast<std::byte*>(Object) + Offset, sizeof(Value));
			return Value;
		}
		template<class T> void Write(void* Object, std::size_t Offset, T Value)
		{
			std::memcpy(static_cast<std::byte*>(Object) + Offset, &Value, sizeof(Value));
		}

		void Notify(void* Player, void* Hit, const void* Method)
		{
			struct Scope
			{
				Context Previous{Active};
				~Scope() { Active = Previous; }
			} Restore;
			// Nested notifications establish their own scope, including unprotected enemies.
			Active = {};
			if (Player && Bound.ShouldProtect(Player))
			{
				Active = {Read<void*>(Player, Bound.PlayerStatistics), Read<void*>(Player, Bound.PlayerComboKill)};
			}
			OriginalNotify(Player, Hit, Method);
		}

		void Statistics(void* Object, void* Hit, const void* Method)
		{
			const auto Owner = Read<void*>(Object, Bound.StatisticsOwner);
			const bool Protect = Object && ((Active.Statistics == Object) ||
				(Owner && Bound.ShouldProtect(Owner) && Read<void*>(Owner, Bound.PlayerStatistics) == Object));
			if (!Protect) { OriginalStatistics(Object, Hit, Method); return; }
			struct RestoreStatistics
			{
				void* Object;
				std::int32_t Hits;
				std::int64_t Damage;
				~RestoreStatistics()
				{
					Write(Object, Bound.BehitTimes, Hits);
					Write(Object, Bound.TotalTakenDamage, Damage);
				}
			} Restore{Object, Read<std::int32_t>(Object, Bound.BehitTimes), Read<std::int64_t>(Object, Bound.TotalTakenDamage)};
			// Preserve other statistics, action-point processing, and hit effects.
			OriginalStatistics(Object, Hit, Method);
		}

		void Reset(void* Object, const void* Method)
		{
			if (Object && Active.Kills == Object) return;
			OriginalReset(Object, Method);
		}

		bool Check(MH_STATUS Status, std::string& Error)
		{
			if (Status == MH_OK) return true;
			Error = std::string("Perfect guard hook: ") + MH_StatusToString(Status);
			return false;
		}
	}

	bool InstallPerfectHooks(const PerfectBindings& Bindings, std::string& Error)
	{
		Error.clear();
		if (Installed) return true;
		if (!Bindings.NotifyBeHit || !Bindings.OnBehit || !Bindings.ResetKills || !Bindings.ShouldProtect)
		{ Error = "The current game does not expose a compatible perfect-guard API."; return false; }
		if (!Initialized)
		{
			if (!Check(MH_Initialize(), Error)) return false;
			Initialized = true;
		}
		Bound = Bindings;
		const std::array<void*, 3> Targets{reinterpret_cast<void*>(Bound.NotifyBeHit),
			reinterpret_cast<void*>(Bound.OnBehit), reinterpret_cast<void*>(Bound.ResetKills)};
		const std::array<void*, 3> Detours{reinterpret_cast<void*>(Notify), reinterpret_cast<void*>(Statistics), reinterpret_cast<void*>(Reset)};
		std::array<void**, 3> Originals{reinterpret_cast<void**>(&OriginalNotify),
			reinterpret_cast<void**>(&OriginalStatistics), reinterpret_cast<void**>(&OriginalReset)};
		std::size_t Created{};
		for (; Created < Targets.size(); ++Created)
		{
			if (!Check(MH_CreateHook(Targets[Created], Detours[Created], Originals[Created]), Error)) break;
		}
		if (Created == Targets.size())
		{
			bool Queued = true;
			for (auto Target : Targets) if (!Check(MH_QueueEnableHook(Target), Error)) { Queued = false; break; }
			if (Queued && Check(MH_ApplyQueued(), Error)) { Installed = true; return true; }
		}
		// Creation/activation failure must not leave a partial feature armed.
		for (std::size_t i = 0; i < Created; ++i) MH_DisableHook(Targets[i]);
		for (std::size_t i = 0; i < Created; ++i) MH_RemoveHook(Targets[i]);
		return false;
	}

	bool RemovePerfectHooks(std::string& Error)
	{
		Error.clear();
		if (!Installed) return true;
		for (void* Target : {reinterpret_cast<void*>(Bound.NotifyBeHit), reinterpret_cast<void*>(Bound.OnBehit), reinterpret_cast<void*>(Bound.ResetKills)})
		{
			if (!Check(MH_DisableHook(Target), Error) || !Check(MH_RemoveHook(Target), Error)) return false;
		}
		Installed = false;
		return true;
	}
}
