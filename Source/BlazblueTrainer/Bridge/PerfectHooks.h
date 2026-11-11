#pragma once

#include <cstddef>
#include <string>

namespace Trainer
{
	using PerfectHitFunction = void (*)(void*, void*, const void*);
	using PerfectResetFunction = void (*)(void*, const void*);
	using PerfectScriptFunction = void (*)(void*, void*, void*, void*, const void*);

	struct PerfectBindings
	{
		PerfectHitFunction NotifyBeHit{};
		PerfectHitFunction OnBehit{};
		PerfectResetFunction ResetKills{};
		std::size_t PlayerStatistics{};
		std::size_t PlayerComboKill{};
		std::size_t StatisticsOwner{};
		std::size_t BehitTimes{};
		std::size_t TotalTakenDamage{};
		bool (*ShouldProtect)(void*){};
		PerfectScriptFunction CallBuffScript{};
		std::size_t ComboKillOwner{};
		bool (*IsHitResetScript)(void*){};
	};

	bool InstallPerfectHooks(const PerfectBindings& Bindings, std::string& Error);
	// Only remove hooks when the host guarantees that no game callbacks are executing.
	bool RemovePerfectHooks(std::string& Error);
}
