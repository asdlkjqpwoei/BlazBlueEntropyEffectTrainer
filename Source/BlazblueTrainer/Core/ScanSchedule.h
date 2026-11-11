#pragma once

#include "TrainerApi.h"

#include <chrono>
#include <optional>

namespace Trainer
{
	inline bool AllResourcesCaptured(const TrainerSnapshot& Snapshot, bool RequireAp) noexcept
	{
		const auto Ready = [](const TrainerFeatureSnapshot& Feature) {
			return Feature.AddressCount != 0 && Feature.WriteEnabled != 0;
		};
		return Snapshot.ProcessDetected && Snapshot.BuildVerified && !Snapshot.ScanInProgress &&
			Ready(Snapshot.Refresh) && Ready(Snapshot.Exchange) && (!RequireAp || Ready(Snapshot.Ap));
	}

	class ScanSchedule final
	{
	public:
		using Clock = std::chrono::steady_clock;
		using TimePoint = Clock::time_point;

		void Reset() noexcept { NextScan = TimePoint::min(); }
		bool Due(TimePoint Now) const noexcept { return NextScan && Now >= *NextScan; }
		bool Suspended() const noexcept { return !NextScan; }

		void ScanFinished(TimePoint Now, bool Complete, std::chrono::milliseconds RetryDelay) noexcept
		{
			if (Complete) NextScan.reset();
			else NextScan = Now + RetryDelay;
		}

		void Observe(TimePoint Now, bool Complete, bool Invalidated) noexcept
		{
			if (Complete && !Invalidated) NextScan.reset();
			else if (Invalidated || !NextScan) NextScan = Now;
		}

	private:
		std::optional<TimePoint> NextScan{TimePoint::min()};
	};
}
