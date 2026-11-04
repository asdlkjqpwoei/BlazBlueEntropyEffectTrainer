#include "GamePresenceMonitor.h"

#include <atomic>
#include <chrono>
#include <future>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <thread>

int RunResponsivenessTests()
{
	using Clock = std::chrono::steady_clock;
	int Failures = 0;
	const auto Require = [&](bool Condition, const char *Message) {
		if (!Condition)
		{
			std::cerr << Message << '\n';
			++Failures;
		}
	};
	std::atomic<unsigned> Observations{};
	std::atomic<Trainer::GameRunState> State{Trainer::GameRunState::Stopped};
	std::promise<void> StartObservation;
	const auto Start = StartObservation.get_future().share();
	auto Monitor = std::make_shared<Trainer::GamePresenceMonitor>(
		[&, Start] {
			++Observations;
			Start.wait();
			std::this_thread::sleep_for(std::chrono::milliseconds(60));
			return Trainer::GamePresence{State.load(), {}};
		},
		std::chrono::milliseconds(150));
	const auto WaitFor = [&](Trainer::GameRunState Expected) {
		const auto Deadline = Clock::now() + std::chrono::seconds(3);
		while (Clock::now() < Deadline)
		{
			if (Monitor->Snapshot().State == Expected)
				return true;
			std::this_thread::sleep_for(std::chrono::milliseconds(5));
		}
		return false;
	};
	Require(Monitor->Snapshot().State == Trainer::GameRunState::Unknown,
			"Offline controls must start blocked until a background observation completes.");
	StartObservation.set_value();
	Require(WaitFor(Trainer::GameRunState::Stopped), "The background game-state observation never completed.");
	const auto SecondWindow = Monitor;
	const auto Started = Clock::now();
	for (unsigned Index = 0; Index < 10000; ++Index)
	{
		Require(Monitor->Snapshot().State == Trainer::GameRunState::Stopped &&
					SecondWindow->Snapshot().State == Trainer::GameRunState::Stopped,
				"Both windows must share the same observed game state.");
	}
	Require(Clock::now() - Started < std::chrono::milliseconds(500),
			"UI snapshot reads must not synchronously repeat slow process scans.");
	Require(Observations.load() < 20, "Reading UI state must not cause additional process enumeration.");
	State.store(Trainer::GameRunState::Running);
	Require(WaitFor(Trainer::GameRunState::Running), "Game startup did not update the cached offline state.");
	State.store(Trainer::GameRunState::Stopped);
	Require(WaitFor(Trainer::GameRunState::Stopped), "Game exit did not restore the offline state.");

	std::atomic<bool> Fail{true};
	Trainer::GamePresenceMonitor Errors(
		[&] {
			if (Fail.load())
				throw std::runtime_error("Synthetic process observation error");
			return Trainer::GamePresence{Trainer::GameRunState::Stopped, {}};
		},
		std::chrono::milliseconds(25));
	auto Deadline = Clock::now() + std::chrono::seconds(2);
	while (Errors.Snapshot().Error.empty() && Clock::now() < Deadline)
		std::this_thread::sleep_for(std::chrono::milliseconds(5));
	Require(Errors.Snapshot().State == Trainer::GameRunState::Unknown &&
				Errors.Snapshot().Error == "Synthetic process observation error",
			"Observation errors must explicitly block offline editing, not default to stopped.");
	Fail.store(false);
	Deadline = Clock::now() + std::chrono::seconds(2);
	while (Errors.Snapshot().State != Trainer::GameRunState::Stopped && Clock::now() < Deadline)
		std::this_thread::sleep_for(std::chrono::milliseconds(5));
	Require(Errors.Snapshot().State == Trainer::GameRunState::Stopped && Errors.Snapshot().Error.empty(),
			"A recovered observation must clear its previous error.");
	bool Rejected = false;
	try
	{
		Trainer::GamePresenceMonitor Invalid({}, std::chrono::milliseconds(0));
	}
	catch (const std::invalid_argument &)
	{
		Rejected = true;
	}
	Require(Rejected, "Invalid observation configuration must not start a worker.");
	return Failures != 0;
}
