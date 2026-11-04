#pragma once

#include "RemoteProcess.h"

#include <chrono>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <thread>

namespace Trainer
{
	class GamePresenceMonitor final
	{
	  public:
		using Observer = std::function<GamePresence()>;

		explicit GamePresenceMonitor(const Config &Configuration);
		GamePresenceMonitor(Observer Observe, std::chrono::milliseconds Interval);
		GamePresenceMonitor(const GamePresenceMonitor &) = delete;
		GamePresenceMonitor &operator=(const GamePresenceMonitor &) = delete;
		~GamePresenceMonitor();

		GamePresence Snapshot() const;

	  private:
		mutable std::mutex StateMutex;
		std::condition_variable_any Wake;
		GamePresence Current;
		std::jthread Worker;
	};
} // namespace Trainer
