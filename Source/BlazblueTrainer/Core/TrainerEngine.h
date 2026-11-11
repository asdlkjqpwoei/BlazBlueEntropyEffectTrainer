#pragma once

#include "Config.h"
#include "EventAmplification.h"
#include "Il2CppScanner.h"
#include "Operations.h"
#include "PerfectGuard.h"
#include "PuertsRuntime.h"
#include "RemoteProcess.h"

#include "TrainerApi.h"

#include <atomic>
#include <cstddef>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <stop_token>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace Trainer
{
	enum class ValueKind
	{
		Unsigned32,
		Signed32,
		FixedPoint64
	};

	struct CandidatePointerGuard
	{
		std::uintptr_t Owner{};
		std::uint64_t Offset{};
		std::uintptr_t ExpectedTarget{};
		std::uintptr_t ExpectedClass{};
	};

	struct CandidateValueGuard
	{
		std::uintptr_t Address{};
		std::uint64_t Expected{};
		std::uint8_t Width{};
	};

	struct CandidateLowerBoundGuard
	{
		std::uintptr_t Address{};
		std::uint64_t Minimum{};
		std::uint8_t Width{};
	};

	struct Candidate
	{
		std::uintptr_t Object{};
		std::uintptr_t ClassPointer{};
		std::uintptr_t RootObject{};
		std::uintptr_t RootClassPointer{};
		std::uint64_t ValueOffset{};
		ValueKind Kind{};
		RefreshSemantics Semantics{ RefreshSemantics::Remaining };
		std::optional<std::uint64_t> DiscriminatorOffset;
		std::optional<std::uint32_t> DiscriminatorValue;
		std::optional<std::uint64_t> IdOffset;
		std::optional<std::uint32_t> IdValue;
		std::uint32_t Maximum{};
		std::uint32_t DynamicExpectedValue{};
		std::uintptr_t DynamicAddress{};
		std::uintptr_t PuertsArrayObject{};
		std::uintptr_t PuertsMapPointer{};
		std::uintptr_t PuertsParentObject{};
		std::uintptr_t PuertsParentMapPointer{};
		std::uint64_t PuertsParentPropertiesOrHash{};
		std::uint64_t PuertsLengthRaw{};
		bool RootValidated{};
		bool DynamicValidated{};
		bool FixedPointHighWord{};
		bool PuertsArrayValidated{};
		std::optional<PuertsResourceTarget> PuertsResource;
		std::vector<CandidatePointerGuard> PointerGuards;
		std::vector<CandidateValueGuard> ValueGuards;
		std::vector<CandidateLowerBoundGuard> LowerBoundGuards;
	};

	class Engine final
	{
	public:
		explicit Engine(Config Configuration);
		Engine(const Engine&) = delete;
		Engine& operator=(const Engine&) = delete;
		~Engine();

		bool Start(std::string& Error);
		void Stop();
		TrainerSnapshot Snapshot() const;
		bool Apply(
			TrainerFeature Feature,
			TrainerOperation Operation,
			std::int64_t Value,
			std::int32_t& Writes,
			std::string& Error);
		bool GrantAllPotentials(TrainerPotentialResult& Result, std::string& Error);
		bool SetPerfectGuard(bool Enabled, std::string& Error);
		bool SetEventAmplification(bool Enabled, std::string& Error);
		std::string Text(const std::string& Key) const;
		std::string Translate(const std::string& Message) const;
		void SetLanguage(TrainerLanguage Language);
		std::int64_t Default(TrainerFeature Feature, TrainerOperation Operation) const;

	private:
		void Run(std::stop_token StopToken);
		bool PerformScan(
			const std::shared_ptr<RemoteProcess>& Process,
			std::stop_token StopToken);
		bool PerformPuertsRuntimeScan(
			const std::shared_ptr<RemoteProcess>& Process,
			std::stop_token StopToken);
		void UpdatePuertsRuntimeCandidates(
			const std::shared_ptr<RemoteProcess>& Process,
			bool CaptureRefresh, bool CaptureExchange, bool CaptureAp);
		void UpdateDynamicRefreshCandidates(
			const std::shared_ptr<RemoteProcess>& Process);
		bool RefreshSnapshot(const std::shared_ptr<RemoteProcess>& Process);
		std::vector<Candidate> BuildRefreshCandidates(
			const ClassPointerMap& Classes,
			const InstanceMap& Instances,
			const std::shared_ptr<RemoteProcess>& Process) const;
		std::vector<Candidate> BuildResourceCandidates(
			const ResourceTargetConfig& Target,
			const ClassPointerMap& Classes,
			const InstanceMap& Instances,
			const std::shared_ptr<RemoteProcess>& Process) const;
		std::vector<Candidate> BuildRootedResourceCandidates(
			const ResourceTargetConfig& Target,
			const ClassPointerMap& Classes,
			const InstanceMap& Instances,
			const std::shared_ptr<RemoteProcess>& Process) const;
		bool ReadCandidate(
			const Candidate& Candidate,
			const std::shared_ptr<RemoteProcess>& Process,
			std::int64_t& Value) const;
		void SetStatus(
			TrainerState State,
			const std::string& StatusText,
			const std::string& Detail,
			bool ProcessDetected,
			bool BuildVerified,
			bool ScanInProgress);

		Config Settings;
		std::atomic<TrainerLanguage> CurrentLanguage{ TrainerLanguageChinese };
		std::atomic<bool> IsRunning{ false };
		PerfectGuard PerfectProtection;
		EventAmplification AmplificationEvents;
		std::jthread MonitorThread;
		mutable std::mutex StateMutex;
		std::condition_variable_any WakeEvent;
		std::shared_ptr<RemoteProcess> ConnectedProcess;
		std::vector<std::uintptr_t> PuertsModelConstructors;
		std::vector<std::uintptr_t> PuertsApModelConstructors;
		bool PuertsModelRescanRequested{};
		std::vector<Candidate> RefreshCandidates;
		std::vector<Candidate> DynamicRefreshCandidates;
		std::vector<Candidate> ExchangeCandidates;
		std::vector<Candidate> ApCandidates;
		std::vector<Candidate> PendingRefreshCandidates;
		std::vector<Candidate> PendingExchangeCandidates;
		InstanceMap PendingInstances;
		std::vector<FixedPointValueHit> PendingFixedPointHits;
		std::unordered_map<std::uintptr_t, RefreshValueTracker> DynamicRefreshTrackers;
		std::unordered_map<std::uintptr_t, FixedPointValueHit> PuertsRefreshArrays;
		std::uintptr_t SelectedPuertsRefreshAddress{};
		std::vector<std::uintptr_t> DynamicRefreshAddresses;
		std::size_t DynamicRefreshPollCursor{};
		bool PendingFixedPointHitsTruncated{};
		bool DynamicRefreshScanTruncated{};
		std::uintptr_t MemoryScanCursor{};
		TrainerSnapshot CachedSnapshot{};
	};
}
