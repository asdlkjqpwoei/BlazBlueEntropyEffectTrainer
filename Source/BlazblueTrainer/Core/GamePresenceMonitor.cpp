#include "GamePresenceMonitor.h"

#include <stdexcept>
#include <utility>

namespace Trainer
{
	GamePresenceMonitor::GamePresenceMonitor(const Config &Configuration)
		: GamePresenceMonitor([Configuration] { return RemoteProcess::ObserveGamePresence(Configuration); },
							  std::chrono::milliseconds(500))
	{
	}

	GamePresenceMonitor::GamePresenceMonitor(Observer Observe, std::chrono::milliseconds Interval)
	{
		if (!Observe || Interval.count() <= 0)
			throw std::invalid_argument("A game-presence observer and positive interval are required.");
		Worker = std::jthread([this, Observe = std::move(Observe), Interval](std::stop_token Stop) {
			while (!Stop.stop_requested())
			{
				const auto NextCheck = std::chrono::steady_clock::now() + Interval;
				GamePresence Next;
				try
				{
					Next = Observe();
				}
				catch (const std::exception &Error)
				{
					Next = {GameRunState::Unknown, Error.what()};
				}
				{
					std::lock_guard Lock(StateMutex);
					Current = std::move(Next);
				}
				std::unique_lock Lock(StateMutex);
				Wake.wait_until(Lock, Stop, NextCheck, [] { return false; });
			}
		});
	}

	GamePresenceMonitor::~GamePresenceMonitor()
	{
		Worker.request_stop();
		Wake.notify_all();
		if (Worker.joinable())
			Worker.join();
	}

	GamePresence GamePresenceMonitor::Snapshot() const
	{
		std::lock_guard Lock(StateMutex);
		return Current;
	}
} // namespace Trainer
