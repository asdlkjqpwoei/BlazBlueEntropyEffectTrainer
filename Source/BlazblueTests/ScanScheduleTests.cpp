#include "ScanSchedule.h"

#include <chrono>
#include <stdexcept>

namespace
{
	void Require(bool Value, const char* Message)
	{
		if (!Value) throw std::runtime_error(Message);
	}

	TrainerSnapshot Captured()
	{
		TrainerSnapshot Snapshot{};
		Snapshot.ProcessDetected = 1;
		Snapshot.BuildVerified = 1;
		Snapshot.State = TrainerStateReady;
		Snapshot.Refresh.AddressCount = Snapshot.Refresh.WriteEnabled = 1;
		Snapshot.Exchange.AddressCount = Snapshot.Exchange.WriteEnabled = 1;
		Snapshot.Ap.AddressCount = Snapshot.Ap.WriteEnabled = 1;
		return Snapshot;
	}
}

int RunScanScheduleTests()
{
	using namespace std::chrono_literals;
	using Trainer::AllResourcesCaptured;
	using Trainer::ScanSchedule;
	const auto Start = ScanSchedule::TimePoint{};
	{
		auto Snapshot = Captured();
		Require(AllResourcesCaptured(Snapshot, true), "All configured resources should finish capture.");
		Snapshot.Ap = {};
		Require(!AllResourcesCaptured(Snapshot, true), "A missing AP target must not suspend discovery.");
		Require(AllResourcesCaptured(Snapshot, false), "An unsupported AP feature must not prevent suspension.");
		Snapshot.Exchange.WriteEnabled = 0;
		Require(!AllResourcesCaptured(Snapshot, false), "Unwritable or ambiguous targets must not complete capture.");
		Snapshot = Captured();
		Snapshot.Refresh.AddressCount = 0;
		Require(!AllResourcesCaptured(Snapshot, true), "A missing address must not count as captured.");
		Snapshot = Captured();
		Snapshot.ScanInProgress = 1;
		Require(!AllResourcesCaptured(Snapshot, true), "An incomplete bounded scan must finish before suspension.");
		Snapshot = Captured();
		Snapshot.BuildVerified = 0;
		Require(!AllResourcesCaptured(Snapshot, true), "An unverified build must not count as captured.");
		Snapshot = Captured();
		Snapshot.ProcessDetected = 0;
		Require(!AllResourcesCaptured(Snapshot, true), "A disconnected game must not count as captured.");
	}
	{
		ScanSchedule Schedule;
		Require(Schedule.Due(Start), "A fresh connection must start discovery.");
		unsigned Scans = 0;
		if (Schedule.Due(Start))
		{
			++Scans;
			Schedule.ScanFinished(Start, true, 60s);
		}
		// Repeated validity/value checks must not restart capture, even beyond the old 60-second timer.
		for (auto Elapsed = 750ms; Elapsed <= 24h; Elapsed += 750ms)
		{
			Schedule.Observe(Start + Elapsed, true, false);
			if (Schedule.Due(Start + Elapsed)) ++Scans;
		}
		Require(Scans == 1 && Schedule.Suspended() && !Schedule.Due(ScanSchedule::TimePoint::max()),
			"Captured targets must not be periodically rescanned while still valid.");
		Schedule.Observe(Start + 24h, false, true);
		Require(Schedule.Due(Start + 24h), "A lost or invalidated address must restart discovery immediately.");
		++Scans;
		Schedule.ScanFinished(Start + 24h, true, 15s);
		Schedule.Observe(Start + 48h, true, false);
		Require(Scans == 2 && Schedule.Suspended(), "Recovered targets must suspend capture again.");
		Schedule.Reset();
		Require(Schedule.Due(Start + 48h), "A replacement game process must start a fresh capture.");
	}
	{
		ScanSchedule Schedule;
		Schedule.ScanFinished(Start, false, 15s);
		for (auto Elapsed = 750ms; Elapsed < 15s; Elapsed += 750ms)
		{
			Schedule.Observe(Start + Elapsed, false, false);
			Require(!Schedule.Due(Start + Elapsed), "Incomplete capture must respect its retry interval.");
		}
		Require(Schedule.Due(Start + 15s), "A missing feature must remain scheduled for discovery.");
		Schedule.ScanFinished(Start + 15s, false, 60s);
		Schedule.Observe(Start + 16s, true, false);
		Require(Schedule.Suspended(), "Targets found through a cached owner must cancel a pending retry.");
		Schedule.Observe(Start + 17s, true, true);
		Require(Schedule.Due(Start + 17s), "An invalid model identity must override apparently captured values.");
	}
	{
		ScanSchedule Schedule;
		Schedule.ScanFinished(Start, true, 60s);
		Schedule.Observe(Start + 1s, false, false);
		Require(Schedule.Due(Start + 1s), "Loss of completeness must resume a previously suspended scan.");
	}
	return 0;
}
