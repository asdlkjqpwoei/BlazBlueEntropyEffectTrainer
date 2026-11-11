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
	inline bool KeepEventAmplificationForStatus(TrainerState State, bool ProcessDetected)
	{
		return ProcessDetected && (State == TrainerStateChecking || State == TrainerStateScanning || State == TrainerStateReady);
	}

	class EventAmplification final
	{
	public:
		EventAmplification() = default;
		~EventAmplification();
		EventAmplification(const EventAmplification&) = delete;
		EventAmplification& operator=(const EventAmplification&) = delete;
		bool Enable(DWORD ProcessId, std::string& Error, std::stop_token StopToken = {});
		void Disable();
		bool Enabled(DWORD ProcessId) const;

	private:
		struct Session;
		std::atomic<std::shared_ptr<Session>> Current;
		std::atomic<std::uint64_t> Revision{};
		std::mutex OperationMutex;
	};
}
