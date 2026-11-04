#pragma once

#include "TrainerApi.h"
#include <Windows.h>
#include <atomic>
#include <memory>
#include <mutex>
#include <stop_token>
#include <string>
#include <thread>

namespace Trainer
{
	inline bool KeepPerfectGuardForStatus(TrainerState State, bool ProcessDetected)
	{
		return ProcessDetected && (State == TrainerStateChecking || State == TrainerStateScanning || State == TrainerStateReady);
	}

	class PerfectGuard final
	{
	public:
		PerfectGuard() = default;
		~PerfectGuard();
		PerfectGuard(const PerfectGuard&) = delete;
		PerfectGuard& operator=(const PerfectGuard&) = delete;
		bool Enable(DWORD ProcessId, std::string& Error, std::stop_token StopToken = {});
		void Disable();
		bool Enabled(DWORD ProcessId) const;

	private:
		struct Session;
		std::atomic<std::shared_ptr<Session>> Current;
		std::mutex OperationMutex;
	};
}
