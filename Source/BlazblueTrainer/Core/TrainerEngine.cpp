#include "TrainerEngine.h"
#include "Text.h"

#include "Operations.h"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <sstream>
#include <stdexcept>
#include <unordered_set>

namespace Trainer
{
    namespace
    {
        using Clock = std::chrono::steady_clock;

        bool ReadUnsigned(
            const std::shared_ptr<RemoteProcess>& Process,
            std::uintptr_t Address,
            std::uint32_t& Value)
        {
            return Process->Read(Address, &Value, sizeof(Value));
        }

        bool ReadBool(
            const std::shared_ptr<RemoteProcess>& Process,
            std::uintptr_t Address,
            bool& Value)
        {
            std::uint8_t RawValue = 0;
            if (!Process->Read(Address, &RawValue, sizeof(RawValue)) || RawValue > 1)
            {
                return false;
            }
            Value = RawValue != 0;
            return true;
        }

        bool ReadPointer(
            const std::shared_ptr<RemoteProcess>& Process,
            std::uintptr_t Address,
            std::uintptr_t& Value)
        {
            return Process->Read(Address, &Value, sizeof(Value)) &&
                Value >= 0x10000 &&
                (Value & (alignof(std::uintptr_t) - 1)) == 0;
        }

        bool IsPlausibleObject(
            const std::shared_ptr<RemoteProcess>& Process,
            std::uintptr_t Object,
            std::uintptr_t ExpectedClass)
        {
            if (Object < 0x10000 ||
                (Object & (alignof(std::uintptr_t) - 1)) != 0)
            {
                return false;
            }

            std::uintptr_t ActualClass = 0;
            std::uintptr_t Monitor = 0;
            return Process->Read(Object, &ActualClass, sizeof(ActualClass)) &&
                ActualClass == ExpectedClass &&
                Process->Read(Object + sizeof(std::uintptr_t), &Monitor, sizeof(Monitor)) &&
                (Monitor == 0 ||
                 (Monitor >= 0x10000 && (Monitor & (alignof(std::uintptr_t) - 1)) == 0));
        }

        bool ReadGuardValue(
            const std::shared_ptr<RemoteProcess>& Process,
            std::uintptr_t Address,
            std::uint8_t Width,
            std::uint64_t& Value)
        {
            switch (Width)
            {
            case sizeof(std::uint8_t):
            {
                std::uint8_t Result = 0;
                if (!Process->Read(Address, &Result, sizeof(Result)))
                {
                    return false;
                }
                Value = Result;
                return true;
            }
            case sizeof(std::uint32_t):
            {
                std::uint32_t Result = 0;
                if (!Process->Read(Address, &Result, sizeof(Result)))
                {
                    return false;
                }
                Value = Result;
                return true;
            }
            case sizeof(std::uint64_t):
                return Process->Read(Address, &Value, sizeof(Value));
            default:
                return false;
            }
        }

        void AppendUniqueCandidates(
            std::vector<Candidate>& Destination,
            const std::vector<Candidate>& Source)
        {
            std::unordered_set<std::uintptr_t> Seen;
            for (const Candidate& Candidate : Destination)
            {
                Seen.insert(Candidate.Object + static_cast<std::uintptr_t>(Candidate.ValueOffset));
            }
            for (const Candidate& Candidate : Source)
            {
                const auto Address =
                    Candidate.Object + static_cast<std::uintptr_t>(Candidate.ValueOffset);
                if (Seen.insert(Address).second)
                {
                    Destination.push_back(Candidate);
                }
            }
        }

        void AppendUniqueFixedPointHits(
            std::vector<FixedPointValueHit>& Destination,
            const std::vector<FixedPointValueHit>& Source)
        {
            std::unordered_map<std::uintptr_t, std::size_t> Seen;
            for (std::size_t Index = 0; Index < Destination.size(); ++Index)
            {
                Seen.emplace(Destination[Index].HighWordAddress, Index);
            }
            for (const FixedPointValueHit& Hit : Source)
            {
                const auto [Found, inserted] = Seen.emplace(
                    Hit.HighWordAddress,
                    Destination.size());
                if (inserted)
                {
                    Destination.push_back(Hit);
                }
                else if (Hit.IsPuertsArray() &&
                    !Destination[Found->second].IsPuertsArray())
                {
                    Destination[Found->second] = Hit;
                }
            }
        }

        void AppendUniqueInstances(
            InstanceMap& Destination,
            const InstanceMap& Source,
            std::size_t MaximumPerType)
        {
            for (const auto& [Type, Objects] : Source)
            {
                auto& Stored = Destination[Type];
                std::unordered_set<std::uintptr_t> Seen(
                    Stored.begin(),
                    Stored.end());
                for (const std::uintptr_t Object : Objects)
                {
                    if (Stored.size() >= MaximumPerType)
                    {
                        break;
                    }
                    if (Seen.insert(Object).second)
                    {
                        Stored.push_back(Object);
                    }
                }
            }
        }

        TrainerFeatureSnapshot Summarize(
            const std::vector<std::int64_t>& Values,
            const std::vector<Candidate>& Candidates,
            std::uint32_t MaximumWriteCandidates,
            bool RequireSingleCandidate)
        {
            TrainerFeatureSnapshot Result{};
            if (Values.empty())
            {
                return Result;
            }

            Result.AddressCount = static_cast<std::uint32_t>(Values.size());
            const auto [Minimum, Maximum] = std::minmax_element(Values.begin(), Values.end());
            Result.Minimum = *Minimum;
            Result.Maximum = *Maximum;
            Result.WriteEnabled =
                Values.size() == Candidates.size() &&
                Candidates.size() <= MaximumWriteCandidates &&
                (!RequireSingleCandidate || Candidates.size() == 1) &&
                std::all_of(Candidates.begin(), Candidates.end(), [](const Candidate& Candidate)
                {
                    return Candidate.RootValidated || Candidate.PuertsArrayValidated ||
                        Candidate.PuertsResource.has_value();
                })
                    ? 1u
                    : 0u;
            return Result;
        }

        std::string JoinNames(const std::vector<std::string>& Names, std::size_t Maximum)
        {
            std::ostringstream Output;
            const std::size_t Count = std::min(Names.size(), Maximum);
            for (std::size_t I = 0; I < Count; ++I)
            {
                if (I != 0)
                {
                    Output << ", ";
                }
                Output << Names[I];
            }
            if (Names.size() > Maximum)
            {
                Output << " ...";
            }
            return Output.str();
        }
    }

    Engine::Engine(Config Configuration)
        : Settings(std::move(Configuration)), CurrentLanguage(Settings.Texts.DefaultLanguage)
    {
        CachedSnapshot.State = TrainerStateStopped;
        CopyUtf8(CachedSnapshot.StatusMessage, sizeof(CachedSnapshot.StatusMessage), Text("waiting"));
    }

    Engine::~Engine()
    {
        Stop();
    }

    bool Engine::Start(std::string& Error)
    {
        Error.clear();
        bool Expected = false;
        if (!IsRunning.compare_exchange_strong(Expected, true))
        {
            return true;
        }

        try
        {
            MonitorThread = std::jthread([this](std::stop_token StopToken)
            {
                Run(StopToken);
            });
            return true;
        }
        catch (const std::system_error& Exception)
        {
            IsRunning.store(false);
            Error = std::string("Could not start the monitor thread: ") + Exception.what();
            return false;
        }
    }

    void Engine::Stop()
    {
        IsRunning.store(false);

        if (MonitorThread.joinable())
        {
            MonitorThread.request_stop();
            WakeEvent.notify_all();
            MonitorThread.join();
        }

        std::lock_guard Lock(StateMutex);
        ConnectedProcess.reset();
        PuertsModelConstructors.clear();
        PuertsApModelConstructors.clear();
        PuertsModelRescanRequested = false;
        RefreshCandidates.clear();
        DynamicRefreshCandidates.clear();
        ExchangeCandidates.clear();
        ApCandidates.clear();
        PendingRefreshCandidates.clear();
        PendingExchangeCandidates.clear();
        PendingInstances.clear();
        PendingFixedPointHits.clear();
        DynamicRefreshTrackers.clear();
        PuertsRefreshArrays.clear();
        SelectedPuertsRefreshAddress = 0;
        DynamicRefreshAddresses.clear();
        DynamicRefreshPollCursor = 0;
        PendingFixedPointHitsTruncated = false;
        DynamicRefreshScanTruncated = false;
        MemoryScanCursor = 0;
        CachedSnapshot = {};
        CachedSnapshot.State = TrainerStateStopped;
        CopyUtf8(CachedSnapshot.StatusMessage, sizeof(CachedSnapshot.StatusMessage), Text("waiting"));
    }

    TrainerSnapshot Engine::Snapshot() const
    {
        std::lock_guard Lock(StateMutex);
        return CachedSnapshot;
    }

    std::string Engine::Text(const std::string& Key) const
    {
        return Settings.Text(Key, CurrentLanguage.load());
    }

    std::string Engine::Translate(const std::string& Message) const
    {
        return Settings.Texts.Translate(Message, CurrentLanguage.load());
    }

    void Engine::SetLanguage(TrainerLanguage Language)
    {
        if (Language != TrainerLanguageChinese && Language != TrainerLanguageEnglish)
            throw std::invalid_argument("Unsupported interface language.");
        std::lock_guard Lock(StateMutex);
        CurrentLanguage.store(Language);
        CopyUtf8(CachedSnapshot.StatusMessage, sizeof(CachedSnapshot.StatusMessage),
            Settings.Texts.Translate(CachedSnapshot.StatusMessage, Language));
        CopyUtf8(CachedSnapshot.DetailMessage, sizeof(CachedSnapshot.DetailMessage),
            Settings.Texts.Translate(CachedSnapshot.DetailMessage, Language));
    }

    std::int64_t Engine::Default(TrainerFeature Feature, TrainerOperation Operation) const
    {
        const bool SetOperation = Operation == TrainerOperationSet;
        switch (Feature)
        {
        case TrainerFeatureRefresh:
            return SetOperation ? Settings.RefreshDefaults.Set : Settings.RefreshDefaults.Add;
        case TrainerFeatureExchange:
            return SetOperation ? Settings.ExchangeDefaults.Set : Settings.ExchangeDefaults.Add;
        case TrainerFeatureAp:
            return SetOperation ? Settings.ApDefaults.Set : Settings.ApDefaults.Add;
        default:
            return 0;
        }
    }

    void Engine::SetStatus(
        TrainerState State,
        const std::string& StatusText,
        const std::string& Detail,
        bool ProcessDetected,
        bool BuildVerified,
        bool ScanInProgress)
    {
        std::lock_guard Lock(StateMutex);
        CachedSnapshot.State = State;
        CachedSnapshot.ProcessDetected = ProcessDetected ? 1u : 0u;
        CachedSnapshot.BuildVerified = BuildVerified ? 1u : 0u;
        CachedSnapshot.ScanInProgress = ScanInProgress ? 1u : 0u;
        CachedSnapshot.ProcessId = ProcessDetected && ConnectedProcess
            ? ConnectedProcess->ProcessId() : 0;
        CopyUtf8(CachedSnapshot.StatusMessage, sizeof(CachedSnapshot.StatusMessage), Translate(StatusText));
        CopyUtf8(CachedSnapshot.DetailMessage, sizeof(CachedSnapshot.DetailMessage), Translate(Detail));
    }

    void Engine::Run(std::stop_token StopToken)
    {
        auto NextProcessCheck = Clock::now();
        auto NextScan = Clock::now();
        auto NextValueCheck = Clock::now();
        bool ScanRequested = true;
        std::string AttachDetail;

        try
        {
            while (!StopToken.stop_requested())
            {
                std::shared_ptr<RemoteProcess> Process;
                {
                    std::lock_guard Lock(StateMutex);
                    Process = ConnectedProcess;
                }

                if (Process && !Process->IsAlive())
                {
                    {
                        std::lock_guard Lock(StateMutex);
                        if (ConnectedProcess == Process)
                        {
                            ConnectedProcess.reset();
                            PuertsModelConstructors.clear();
                            PuertsApModelConstructors.clear();
                            PuertsModelRescanRequested = false;
                            RefreshCandidates.clear();
                            DynamicRefreshCandidates.clear();
                            ExchangeCandidates.clear();
                            ApCandidates.clear();
                            PendingRefreshCandidates.clear();
                            PendingExchangeCandidates.clear();
                            PendingInstances.clear();
                            PendingFixedPointHits.clear();
                            DynamicRefreshTrackers.clear();
                            PuertsRefreshArrays.clear();
                            SelectedPuertsRefreshAddress = 0;
                            DynamicRefreshAddresses.clear();
                            DynamicRefreshPollCursor = 0;
                            PendingFixedPointHitsTruncated = false;
                            DynamicRefreshScanTruncated = false;
                            MemoryScanCursor = 0;
                            CachedSnapshot.Refresh = {};
                            CachedSnapshot.Exchange = {};
                            CachedSnapshot.Ap = {};
                        }
                    }
                    Process.reset();
                    ScanRequested = true;
                    SetStatus(
                        TrainerStateWaiting,
                        Text("waiting"),
                        "The compatible game process exited.",
                        false,
                        false,
                        false);
                    NextProcessCheck = Clock::now();
                }

                const auto Now = Clock::now();
                if (!Process && Now >= NextProcessCheck)
                {
                    const AttachResult Attach = RemoteProcess::FindCompatibleGameProcess(Settings);
                    NextProcessCheck = Clock::now() +
                        std::chrono::milliseconds(Settings.Monitor.ProcessCheckMs);

                    if (Attach.Status == AttachStatus::Attached)
                    {
                        {
                            std::lock_guard Lock(StateMutex);
                            ConnectedProcess = Attach.Process;
                            PuertsModelConstructors.clear();
                            PuertsApModelConstructors.clear();
                            PuertsModelRescanRequested = false;
                            RefreshCandidates.clear();
                            DynamicRefreshCandidates.clear();
                            ExchangeCandidates.clear();
                            ApCandidates.clear();
                            PendingRefreshCandidates.clear();
                            PendingExchangeCandidates.clear();
                            PendingInstances.clear();
                            PendingFixedPointHits.clear();
                            DynamicRefreshTrackers.clear();
                            PuertsRefreshArrays.clear();
                            SelectedPuertsRefreshAddress = 0;
                            DynamicRefreshAddresses.clear();
                            DynamicRefreshPollCursor = 0;
                            PendingFixedPointHitsTruncated = false;
                            DynamicRefreshScanTruncated = false;
                            MemoryScanCursor = 0;
                            CachedSnapshot.Refresh = {};
                            CachedSnapshot.Exchange = {};
                            CachedSnapshot.Ap = {};
                        }
                        Process = Attach.Process;
                        AttachDetail = Attach.Message;
                        ScanRequested = true;
                        NextScan = Now;
                        SetStatus(
                            TrainerStateChecking,
                            Text("checking"),
                            AttachDetail,
                            true,
                            !Attach.RuntimeVerificationRequired,
                            false);
                    }
                    else if (Attach.Status == AttachStatus::UnsupportedBuild)
                    {
                        SetStatus(
                            TrainerStateUnsupportedBuild,
                            Text("unsupported_build"),
                            Attach.Message,
                            true,
                            false,
                            false);
                    }
                    else if (Attach.Status == AttachStatus::AccessDenied)
                    {
                        SetStatus(
                            TrainerStateAccessDenied,
                            Text("access_denied"),
                            Attach.Message,
                            true,
                            false,
                            false);
                    }
                    else if (Attach.Status == AttachStatus::Error)
                    {
                        SetStatus(
                            TrainerStateError,
                            Text("process_error"),
                            Attach.Message,
                            false,
                            false,
                            false);
                    }
                    else
                    {
                        SetStatus(
                            TrainerStateWaiting,
                            Text("waiting"),
                            Attach.Message,
                            false,
                            false,
                            false);
                    }
                }

                if (Process && Process->IsAlive())
                {
                    const auto Current = Clock::now();
                    bool RuntimeRescanRequested = false;
                    {
                        std::lock_guard Lock(StateMutex);
                        RuntimeRescanRequested = PuertsModelRescanRequested;
                    }
                    if (ScanRequested || RuntimeRescanRequested || Current >= NextScan)
                    {
                        const bool NeedsRetry = PerformScan(Process, StopToken);
                        ScanRequested = false;
                        const std::uint32_t Delay = NeedsRetry
                            ? Settings.Monitor.RescanMs
                            : Settings.Monitor.StableRescanMs;
                        NextScan = Clock::now() + std::chrono::milliseconds(Delay);
                        NextValueCheck = Clock::now();
                    }

                    if (Clock::now() >= NextValueCheck)
                    {
                        const bool CandidatesLost = RefreshSnapshot(Process);
                        if (CandidatesLost)
                        {
                            const auto RetryTime = Settings.PuertsRuntime
                                ? Clock::now()
                                : Clock::now() + std::chrono::milliseconds(Settings.Monitor.RescanMs);
                            NextScan = std::min(NextScan, RetryTime);
                        }
                        NextValueCheck = Clock::now() +
                            std::chrono::milliseconds(Settings.Monitor.ValueCheckMs);
                    }
                }

                std::unique_lock Lock(StateMutex);
                WakeEvent.wait_for(
                    Lock,
                    StopToken,
                    std::chrono::milliseconds(150),
                    [] { return false; });
            }
        }
        catch (const std::exception& Exception)
        {
            bool ProcessDetected = false;
            {
                std::lock_guard Lock(StateMutex);
                ProcessDetected = ConnectedProcess != nullptr;
            }
            IsRunning.store(false);
            SetStatus(
                TrainerStateError,
                Text("process_error"),
                Exception.what(),
                ProcessDetected,
                ProcessDetected,
                false);
        }
    }

   bool Engine::PerformScan(
        const std::shared_ptr<RemoteProcess>& Process,
        std::stop_token StopToken)
    {
        if (Settings.PuertsRuntime)
        {
            return PerformPuertsRuntimeScan(Process, StopToken);
        }
        const std::uintptr_t ScanStart = MemoryScanCursor;
        SetStatus(
            TrainerStateScanning,
            Text("scanning"),
            "Resolving IL2CPP type information and scanning committed private memory.",
            true,
            true,
            true);

        Il2CppScanner Scanner(Settings, Process);
        std::vector<std::string> UnresolvedTypes;
        ClassPointerMap Classes = Scanner.ResolveClasses(UnresolvedTypes);
        bool Truncated = false;
        std::string ScanError;
        ClassPointerHitMap ClassPointerHits;
        std::vector<FixedPointValueHit> FixedPointHits;
        bool FixedPointHitsTruncated = false;
        InstanceMap Instances = Scanner.ScanInstances(
            Classes,
            StopToken,
            MemoryScanCursor,
            ClassPointerHits,
            FixedPointHits,
            FixedPointHitsTruncated,
            Truncated,
            ScanError);
        if (StopToken.stop_requested())
        {
            return false;
        }

        const bool ScanComplete = !Truncated &&
            ScanError.empty() &&
            MemoryScanCursor == 0;
        std::vector<Candidate> Refresh;
        std::vector<Candidate> Exchange;
        {
            std::lock_guard Lock(StateMutex);
            if (ConnectedProcess != Process)
            {
                return false;
            }
            if (ScanStart == 0)
            {
                PendingInstances.clear();
            }
            AppendUniqueInstances(
                PendingInstances,
                Instances,
                Settings.Monitor.MaxObjectsPerType);
            if (ScanComplete)
            {
                Instances = PendingInstances;
                PendingInstances.clear();
            }
        }

        if (ScanComplete)
        {
            Refresh = BuildRefreshCandidates(Classes, Instances, Process);
            Exchange = BuildRootedResourceCandidates(
                Settings.Exchange,
                Classes,
                Instances,
                Process);
        }

        bool RequiredTypesMissing = false;
        const auto HasType = [&](const std::string& Type)
        {
            if (Classes.find(Type) == Classes.end())
            {
                RequiredTypesMissing = true;
            }
        };
        for (const RefreshRootConfig& Root : Settings.RefreshRoots)
        {
            HasType(Root.Type);
            HasType(Root.ActiveInfoType);
            for (const RefreshFieldConfig& Field : Root.Fields)
            {
                for (const RefreshFieldConfig::PathStep& Step : Field.Path)
                {
                    HasType(Step.Type);
                }
            }
            if (Root.ResourceDictionary)
            {
                HasType(Root.ResourceDictionary->Type);
            }
        }
        HasType(Settings.Exchange.Type);
        if (Settings.Exchange.RootType)
        {
            HasType(*Settings.Exchange.RootType);
        }

        std::size_t RefreshCount = 0;
        std::size_t ExchangeCount = 0;
        std::size_t PendingRefreshCount = 0;
        std::size_t PendingExchangeCount = 0;
        std::size_t DynamicRefreshSeedCount = 0;
        std::size_t DynamicRefreshCount = 0;
        std::size_t PuertsArrayCount = 0;
        bool ScanWasTruncated = false;

        {
            std::lock_guard Lock(StateMutex);
            if (ConnectedProcess != Process)
            {
                return false;
            }

            if (ScanStart == 0)
            {
                PendingRefreshCandidates.clear();
                PendingExchangeCandidates.clear();
                PendingFixedPointHits.clear();
                PendingFixedPointHitsTruncated = false;
            }
            AppendUniqueCandidates(PendingRefreshCandidates, Refresh);
            AppendUniqueCandidates(PendingExchangeCandidates, Exchange);
            AppendUniqueFixedPointHits(PendingFixedPointHits, FixedPointHits);
            PendingFixedPointHitsTruncated =
                PendingFixedPointHitsTruncated || FixedPointHitsTruncated;

            if (ScanComplete)
            {
                RefreshCandidates = std::move(PendingRefreshCandidates);
                ExchangeCandidates = std::move(PendingExchangeCandidates);
                PendingRefreshCandidates.clear();
                PendingExchangeCandidates.clear();

                DynamicRefreshScanTruncated = PendingFixedPointHitsTruncated;
                if (!DynamicRefreshScanTruncated)
                {
                    std::vector<PuertsCandidateReferenceCount> ReferenceCounts;
                    for (const FixedPointValueHit& Hit : PendingFixedPointHits)
                    {
                        if (Hit.IsPuertsArray())
                        {
                            ReferenceCounts.push_back({
                                Hit.HighWordAddress,
                                Hit.PuertsParentAllocationReferenceCount,
                                Hit.PuertsParentPropertiesOrHash != 0 &&
                                    (Hit.PuertsParentPropertiesOrHash & 1) == 0
                            });
                        }
                    }
                    const auto SelectedPuertsRefresh =
                        SelectUniquePuertsRefreshCandidate(ReferenceCounts);
                    SelectedPuertsRefreshAddress =
                        SelectedPuertsRefresh.value_or(0);

                    std::unordered_map<std::uintptr_t, FixedPointValueHit> CurrentPuertsArrays;
                    for (const FixedPointValueHit& Hit : PendingFixedPointHits)
                    {
                        if (DynamicRefreshTrackers.size() >=
                            Settings.Monitor.MaxDynamicRefreshCandidates)
                        {
                            DynamicRefreshScanTruncated = true;
                            break;
                        }

                        auto [Tracker, inserted] = DynamicRefreshTrackers.try_emplace(
                            Hit.HighWordAddress,
                            RefreshValueTracker{
                                Hit.IntegerValue,
                                0,
                                false,
                                true
                            });
                        static_cast<void>(inserted);
                        if (Hit.IsPuertsArray())
                        {
                            Tracker->second.LastValue = Hit.IntegerValue;
                            Tracker->second.Initialized = true;
                            if (Hit.HighWordAddress == SelectedPuertsRefreshAddress)
                            {
                                Tracker->second.DecrementEvidence = 2;
                                Tracker->second.Calibrated = true;
                            }
                            else
                            {
                                Tracker->second.DecrementEvidence = 0;
                                Tracker->second.Calibrated = false;
                            }
                            CurrentPuertsArrays.insert_or_assign(
                                Hit.HighWordAddress,
                                Hit);
                        }
                    }
                    for (const auto& [Address, Hit] : PuertsRefreshArrays)
                    {
                        static_cast<void>(Hit);
                        if (CurrentPuertsArrays.find(Address) ==
                            CurrentPuertsArrays.end())
                        {
                            DynamicRefreshTrackers.erase(Address);
                        }
                    }
                    PuertsRefreshArrays = std::move(CurrentPuertsArrays);
                }
                if (DynamicRefreshScanTruncated)
                {
                    DynamicRefreshTrackers.clear();
                    PuertsRefreshArrays.clear();
                    SelectedPuertsRefreshAddress = 0;
                    DynamicRefreshAddresses.clear();
                    DynamicRefreshCandidates.clear();
                }
                else
                {
                    DynamicRefreshAddresses.clear();
                    DynamicRefreshAddresses.reserve(DynamicRefreshTrackers.size());
                    for (const auto& [Address, State] : DynamicRefreshTrackers)
                    {
                        static_cast<void>(State);
                        DynamicRefreshAddresses.push_back(Address);
                    }
                    std::sort(
                        DynamicRefreshAddresses.begin(),
                        DynamicRefreshAddresses.end());
                }
                PendingFixedPointHits.clear();
                PendingFixedPointHitsTruncated = false;
            }

            RefreshCount = RefreshCandidates.size();
            ExchangeCount = ExchangeCandidates.size();
            PendingRefreshCount = PendingRefreshCandidates.size();
            PendingExchangeCount = PendingExchangeCandidates.size();
            DynamicRefreshSeedCount = DynamicRefreshTrackers.size();
            DynamicRefreshCount = DynamicRefreshCandidates.size();
            PuertsArrayCount = PuertsRefreshArrays.size();
            ScanWasTruncated = DynamicRefreshScanTruncated;
        }

        const bool NeedsRetry =
            !ScanComplete ||
            RequiredTypesMissing ||
            !ScanError.empty() ||
            (RefreshCount == 0 && DynamicRefreshSeedCount == 0);

        std::ostringstream Detail;
        Detail << "Refresh candidates: " << RefreshCount
            << ", exchange candidates: " << ExchangeCount
            << ", fixed-point/Puerts refresh candidates: " << DynamicRefreshSeedCount
            << " (" << PuertsArrayCount << " validated arrays)"
            << ", verified dynamic refresh candidates: " << DynamicRefreshCount
            << ". " << Settings.Text("ap_unavailable", TrainerLanguageEnglish);
        if (ScanWasTruncated)
        {
            Detail << ". Dynamic refresh scan exceeded its configured candidate limit; writes remain disabled.";
        }
        if (!ScanComplete)
        {
            Detail << ". Memory scan is continuing in bounded chunks; next cursor: 0x"
                << std::hex << MemoryScanCursor << std::dec
                << ". Pending this pass: refresh " << PendingRefreshCount
                << ", exchange " << PendingExchangeCount;
        }
        if (RefreshCount > Settings.Monitor.MaxWriteCandidates ||
            ExchangeCount > Settings.Monitor.MaxWriteCandidates)
        {
            Detail << ". Safety lock: a feature has too many candidate addresses; writes are disabled.";
        }
        if (ScanComplete && (RefreshCount == 0 || ExchangeCount == 0))
        {
            const auto InstanceCount = [&Instances](const std::string& Type)
            {
                const auto Found = Instances.find(Type);
                return Found == Instances.end() ? std::size_t{ 0 } : Found->second.size();
            };
            const auto ClassPointerHitCount = [&ClassPointerHits](const std::string& Type)
            {
                const auto Found = ClassPointerHits.find(Type);
                return Found == ClassPointerHits.end() ? std::size_t{ 0 } : Found->second;
            };
            Detail << ". Read-only object counts: resolved types " << Classes.size()
                << "/" << Settings.TypeProfile.size()
                << ", STBreedDungeon " << InstanceCount("STBreedDungeon")
                << ", STExploreDungeon " << InstanceCount("STExploreDungeon")
                << ", DungeonActiveInfo " << InstanceCount("DungeonActiveInfo")
                << ", BreedRewardInfo " << InstanceCount("STBreedDungeonRewardInfo")
                << ", BreedRandomInfo " << InstanceCount("STBreedDungeonRandomInfo")
                << ", BreedBaseInfo " << InstanceCount("STBreedDungeonBaseInfo")
                << ", BreedPhase " << InstanceCount("STBreedDungeonPhase")
                << ", BreedRoom " << InstanceCount("STBreedDungeonRoom")
                << ", ExploreRewardInfo " << InstanceCount("STExploreDungeonRewardInfo")
                << ", FesActor " << InstanceCount("STFesActor")
                << ", DataPoint objects " << InstanceCount("ExploreDungeonResource")
                << "; raw class-pointer hits: STBreedDungeon "
                << ClassPointerHitCount("STBreedDungeon")
                << ", STExploreDungeon " << ClassPointerHitCount("STExploreDungeon")
                << ", DungeonActiveInfo " << ClassPointerHitCount("DungeonActiveInfo");
        }
        if (!UnresolvedTypes.empty())
        {
            Detail << ". Types not initialized yet: " << JoinNames(UnresolvedTypes, 8);
        }
        if (Truncated)
        {
            Detail << ". The per-pass scan limit was reached; the next pass resumes at the saved cursor.";
        }
        if (!ScanError.empty())
        {
            Detail << ". " << ScanError;
        }

        const bool HasCandidates =
            RefreshCount != 0 || DynamicRefreshCount != 0 || ExchangeCount != 0;
        SetStatus(
            HasCandidates && ScanComplete ? TrainerStateReady : TrainerStateScanning,
            HasCandidates && ScanComplete
                ? Text("ready")
                : Text("scanning"),
            Detail.str(),
            true,
            true,
            !ScanComplete);
        RefreshSnapshot(Process);
        return NeedsRetry;
    }

    bool Engine::PerformPuertsRuntimeScan(
        const std::shared_ptr<RemoteProcess>& Process,
        std::stop_token StopToken)
    {
        SetStatus(
            TrainerStateScanning, Text("scanning"),
            "Resolving the named Puerts dungeon and currency model singletons.",
            true, false, true);
        std::vector<std::uintptr_t> Constructors;
        std::vector<std::uintptr_t> ApConstructors;
        {
            std::lock_guard Lock(StateMutex);
            if (ConnectedProcess != Process)
            {
                return false;
            }
            Constructors = PuertsModelConstructors;
            ApConstructors = PuertsApModelConstructors;
        }
        PuertsRuntimeReader Reader(Settings, [Process](auto Address, auto Output, auto Size)
        {
            return Process->Read(Address, Output, Size);
        });
        const bool Cached = !Constructors.empty() &&
            std::all_of(Constructors.begin(), Constructors.end(), [&](auto Constructor)
            {
                return Reader.IsModelConstructor(Constructor);
            }) && (!Settings.PuertsAp ||
                (!ApConstructors.empty() &&
                    std::all_of(ApConstructors.begin(), ApConstructors.end(), [&](auto Constructor)
                    {
                        return Reader.IsApModelConstructor(Constructor);
                    })));
        PuertsModelScanResult Scan;
        if (!Cached)
        {
            Scan = ScanPuertsModelConstructors(Settings, Process, StopToken);
            if (StopToken.stop_requested())
            {
                return false;
            }
            Constructors = Scan.Complete ? Scan.Constructors : std::vector<std::uintptr_t>{};
            ApConstructors = Scan.Complete ? Scan.ApConstructors : std::vector<std::uintptr_t>{};
        }
        {
            std::lock_guard Lock(StateMutex);
            if (ConnectedProcess != Process)
            {
                return false;
            }
            PuertsModelConstructors = Constructors;
            PuertsApModelConstructors = ApConstructors;
            PuertsModelRescanRequested = false;
        }
        std::ostringstream Detail;
        Detail << "Puerts model constructors: " << Constructors.size()
            << (Cached ? " (cached identity revalidated)" :
                Scan.Complete ? " (complete named-class scan)" : " (incomplete named-class scan)")
            << "; authoritative path: _instance.originData.resourceInfo.count"
            << "; refresh index " << Settings.PuertsArray->ResourceIndex
            << ", exchange index " << *Settings.ExchangePuertsIndex
            << "; AP currency constructors: " << ApConstructors.size()
            << "; changed memory regions handled: " << Scan.ChangedRegions;
        if (Settings.PuertsAp)
        {
            Detail << "; AP path: _instance.originData.currencyPack.dict["
                << Settings.PuertsAp->CurrencyId << "].count";
        }
        else
        {
            Detail << ". " << Settings.Text("ap_unavailable", TrainerLanguageEnglish);
        }
        if (!Scan.Error.empty())
        {
            Detail << ". " << Scan.Error;
        }
        SetStatus(
            TrainerStateScanning, Text("scanning"),
            Detail.str(), true, !Constructors.empty() || !ApConstructors.empty(), false);
        RefreshSnapshot(Process);
        return Constructors.empty() || (Settings.PuertsAp && ApConstructors.empty());
    }

    void Engine::UpdatePuertsRuntimeCandidates(
        const std::shared_ptr<RemoteProcess>& Process)
    {
        std::vector<std::uintptr_t> Constructors;
        std::vector<std::uintptr_t> ApConstructors;
        {
            std::lock_guard Lock(StateMutex);
            if (ConnectedProcess != Process)
            {
                return;
            }
            Constructors = PuertsModelConstructors;
            ApConstructors = PuertsApModelConstructors;
        }
        PuertsRuntimeReader Reader(Settings, [Process](auto Address, auto Output, auto Size)
        {
            return Process->Read(Address, Output, Size);
        });
        std::vector<Candidate> Refresh;
        std::vector<Candidate> Exchange;
        std::vector<Candidate> Ap;
        bool ConstructorLost = false;
        const auto Append = [&](std::vector<Candidate>& Candidates,
                                std::optional<PuertsResourceTarget> Target,
                                std::uint32_t Maximum)
        {
            if (!Target || std::any_of(Candidates.begin(), Candidates.end(), [&](const auto& Other)
                {
                    return Other.PuertsResource->ValueAddress == Target->ValueAddress;
                }))
            {
                return;
            }
            Candidate Candidate;
            Candidate.Object = Target->ValueObject;
            Candidate.ValueOffset = Target->ValueAddress - Target->ValueObject;
            Candidate.Kind = ValueKind::FixedPoint64;
            Candidate.Maximum = Maximum;
            Candidate.PuertsResource = std::move(Target);
            Candidates.push_back(std::move(Candidate));
        };
        for (const auto Constructor : Constructors)
        {
            if (!Reader.IsModelConstructor(Constructor))
            {
                ConstructorLost = true;
                continue;
            }
            Append(Refresh, Reader.ResolveResource(
                Constructor, Settings.PuertsArray->ResourceIndex, Settings.Monitor.MaxRefreshValue),
                Settings.Monitor.MaxRefreshValue);
            Append(Exchange, Reader.ResolveResource(
                Constructor, *Settings.ExchangePuertsIndex, Settings.Monitor.MaxResourceValue),
                Settings.Monitor.MaxResourceValue);
        }
        for (const auto Constructor : ApConstructors)
        {
            if (!Reader.IsApModelConstructor(Constructor))
            {
                ConstructorLost = true;
                continue;
            }
            Append(Ap, Reader.ResolveAp(Constructor, Settings.Monitor.MaxResourceValue),
                Settings.Monitor.MaxResourceValue);
        }
        std::lock_guard Lock(StateMutex);
        if (ConnectedProcess == Process)
        {
            RefreshCandidates = std::move(Refresh);
            ExchangeCandidates = std::move(Exchange);
            ApCandidates = std::move(Ap);
            PuertsModelRescanRequested = ConstructorLost;
        }
    }

    void Engine::UpdateDynamicRefreshCandidates(
        const std::shared_ptr<RemoteProcess>& Process)
    {
        if (Settings.PuertsRuntime)
        {
            UpdatePuertsRuntimeCandidates(Process);
            return;
        }
        static constexpr std::size_t PollBatchSize = 4096;
        std::vector<std::uintptr_t> Addresses;
        std::unordered_map<std::uintptr_t, FixedPointValueHit> PuertsArrays;
        std::uintptr_t SelectedAddress = 0;
        {
            std::lock_guard Lock(StateMutex);
            if (ConnectedProcess != Process ||
                DynamicRefreshScanTruncated ||
                DynamicRefreshAddresses.empty())
            {
                DynamicRefreshCandidates.clear();
                return;
            }

            PuertsArrays = PuertsRefreshArrays;
            SelectedAddress = SelectedPuertsRefreshAddress;
            const std::size_t Count = std::min(
                PollBatchSize,
                DynamicRefreshAddresses.size());
            Addresses.reserve(Count);
            for (std::size_t Index = 0; Index < Count; ++Index)
            {
                Addresses.push_back(DynamicRefreshAddresses[DynamicRefreshPollCursor]);
                DynamicRefreshPollCursor =
                    (DynamicRefreshPollCursor + 1) % DynamicRefreshAddresses.size();
            }
        }

        struct Observation
        {
            std::uintptr_t Address{};
            std::optional<std::uint32_t> Value;
        };
        std::vector<Observation> Observations;
        Observations.reserve(Addresses.size());
        for (const std::uintptr_t Address : Addresses)
        {
            std::optional<std::uint32_t> Value;
            const auto PuertsArray = PuertsArrays.find(Address);
            if (PuertsArray != PuertsArrays.end())
            {
                Value = ReadPuertsArrayInteger(
                    Settings,
                    Process,
                    PuertsArray->second);
            }
            else if (Address >= sizeof(std::uint32_t))
            {
                std::uint64_t RawValue = 0;
                if (Process->Read(
                        Address - sizeof(std::uint32_t),
                        &RawValue,
                        sizeof(RawValue)))
                {
                    Value = FixedPointInteger(
                        RawValue,
                        Settings.Monitor.MaxRefreshValue);
                }
            }
            Observations.push_back({ Address, Value });
        }

        std::lock_guard Lock(StateMutex);
        if (ConnectedProcess != Process || DynamicRefreshScanTruncated)
        {
            return;
        }

        for (const Observation& Observation : Observations)
        {
            const auto State = DynamicRefreshTrackers.find(Observation.Address);
            if (State == DynamicRefreshTrackers.end())
            {
                continue;
            }
            if (!Observation.Value)
            {
                State->second.Initialized = false;
                State->second.DecrementEvidence = 0;
                State->second.Calibrated = false;
                continue;
            }
            if (PuertsRefreshArrays.find(Observation.Address) !=
                PuertsRefreshArrays.end())
            {
                State->second.LastValue = *Observation.Value;
                State->second.Initialized = true;
                State->second.DecrementEvidence =
                    Observation.Address == SelectedAddress ? 2 : 0;
                State->second.Calibrated =
                    Observation.Address == SelectedAddress;
                continue;
            }
            if (State->second.Calibrated &&
                Settings.RefreshScanSeed &&
                *Observation.Value == *Settings.RefreshScanSeed)
            {
                State->second.LastValue = *Observation.Value;
                State->second.Initialized = true;
                continue;
            }
            ObserveRefreshValue(
                State->second,
                *Observation.Value,
                Settings.Monitor.MaxRefreshValue);
        }

        std::vector<Candidate> Verified;
        for (const std::uintptr_t Address : DynamicRefreshAddresses)
        {
            const auto State = DynamicRefreshTrackers.find(Address);
            if (State == DynamicRefreshTrackers.end() ||
                !State->second.Initialized ||
                !State->second.Calibrated)
            {
                continue;
            }
            if (PuertsRefreshArrays.find(Address) != PuertsRefreshArrays.end() &&
                Address != SelectedAddress)
            {
                continue;
            }

            Candidate Candidate;
            Candidate.DynamicAddress = Address;
            const auto PuertsArray = PuertsRefreshArrays.find(Address);
            if (PuertsArray != PuertsRefreshArrays.end() &&
                Settings.PuertsArray)
            {
                const FixedPointValueHit& Hit = PuertsArray->second;
                Candidate.Object = Hit.PuertsArrayObject;
                Candidate.ValueOffset = Hit.PuertsValueOffset;
                Candidate.Kind = ValueKind::FixedPoint64;
                Candidate.PuertsArrayObject = Hit.PuertsArrayObject;
                Candidate.PuertsMapPointer = Hit.PuertsMapPointer;
                Candidate.PuertsParentObject = Hit.PuertsParentObject;
                Candidate.PuertsParentMapPointer = Hit.PuertsParentMapPointer;
                Candidate.PuertsParentPropertiesOrHash =
                    Hit.PuertsParentPropertiesOrHash;
                Candidate.PuertsLengthRaw = Hit.PuertsLengthRaw;
                Candidate.PuertsArrayValidated = true;
            }
            else
            {
                Candidate.Object = Address;
                Candidate.Kind = ValueKind::Unsigned32;
            }
            Candidate.Semantics = RefreshSemantics::Remaining;
            Candidate.Maximum = Settings.Monitor.MaxRefreshValue;
            Candidate.DynamicExpectedValue = State->second.LastValue;
            Candidate.DynamicValidated = true;
            Candidate.FixedPointHighWord = true;
            Verified.push_back(std::move(Candidate));
            if (Verified.size() > 1)
            {
                break;
            }
        }
        DynamicRefreshCandidates = Verified.size() == 1
            ? std::move(Verified)
            : std::vector<Candidate>{};
    }

    std::vector<Candidate> Engine::BuildRefreshCandidates(
        const ClassPointerMap& Classes,
        const InstanceMap& Instances,
        const std::shared_ptr<RemoteProcess>& Process) const
    {
        struct PathTarget
        {
            std::uintptr_t Object{};
            std::uintptr_t ClassPointer{};
            std::vector<CandidatePointerGuard> PointerGuards;
            std::vector<CandidateValueGuard> ValueGuards;
            std::vector<CandidateLowerBoundGuard> LowerBoundGuards;
        };

        std::vector<Candidate> Candidates;
        std::unordered_set<std::uintptr_t> SeenFields;
        Il2CppScanner ClassScanner(Settings, Process);

        for (const RefreshRootConfig& RootConfig : Settings.RefreshRoots)
        {
            const auto RootClass = Classes.find(RootConfig.Type);
            const auto ActiveClass = Classes.find(RootConfig.ActiveInfoType);
            const auto RootInstances = Instances.find(RootConfig.Type);
            if (RootClass == Classes.end() ||
                ActiveClass == Classes.end() ||
                RootInstances == Instances.end())
            {
                continue;
            }

            for (const std::uintptr_t Root : RootInstances->second)
            {
                std::uintptr_t ActualRootClass = 0;
                if (!Process->Read(Root, &ActualRootClass, sizeof(ActualRootClass)) ||
                    ActualRootClass != RootClass->second)
                {
                    continue;
                }

                PathTarget RootTarget;
                RootTarget.Object = Root;
                RootTarget.ClassPointer = RootClass->second;
                if (RootConfig.FinishedFlagOffset)
                {
                    bool Finished = false;
                    if (!ReadBool(
                            Process,
                            Root + static_cast<std::uintptr_t>(*RootConfig.FinishedFlagOffset),
                            Finished) ||
                        Finished)
                    {
                        continue;
                    }
                    RootTarget.ValueGuards.push_back({
                        Root + static_cast<std::uintptr_t>(*RootConfig.FinishedFlagOffset),
                        0,
                        sizeof(std::uint8_t)
                    });
                }

                std::uintptr_t ActiveInfo = 0;
                std::uintptr_t ActualActiveClass = 0;
                if (!ReadPointer(
                        Process,
                        Root + static_cast<std::uintptr_t>(RootConfig.ActiveInfoOffset),
                        ActiveInfo) ||
                    !Process->Read(ActiveInfo, &ActualActiveClass, sizeof(ActualActiveClass)) ||
                    ActualActiveClass != ActiveClass->second)
                {
                    continue;
                }
                RootTarget.PointerGuards.push_back({
                    Root,
                    RootConfig.ActiveInfoOffset,
                    ActiveInfo,
                    ActiveClass->second
                });

                if (RootConfig.ResourceDictionary)
                {
                    const RefreshRootConfig::ResourceMap& MapInfo = *RootConfig.ResourceDictionary;
                    const auto ResourceInfoClass = Classes.find(MapInfo.Type);
                    std::uintptr_t ResourceInfo = 0;
                    if (ResourceInfoClass != Classes.end() &&
                        ReadPointer(
                            Process,
                            Root + static_cast<std::uintptr_t>(MapInfo.InfoOffset),
                            ResourceInfo) &&
                        IsPlausibleObject(
                            Process,
                            ResourceInfo,
                            ResourceInfoClass->second))
                    {
                        std::uintptr_t MapField = 0;
                        std::uintptr_t MapFieldClass = 0;
                        std::string MapFieldName;
                        if (ReadPointer(
                                Process,
                                ResourceInfo + static_cast<std::uintptr_t>(MapInfo.MapFieldOffset),
                                MapField) &&
                            Process->Read(MapField, &MapFieldClass, sizeof(MapFieldClass)) &&
                            IsPlausibleObject(Process, MapField, MapFieldClass) &&
                            ClassScanner.ReadClassName(MapFieldClass, MapFieldName) &&
                            MapFieldName.find("MapField") == 0)
                        {
                            std::uintptr_t LinkedList = 0;
                            std::uintptr_t LinkedListClass = 0;
                            std::string LinkedListName;
                            if (ReadPointer(
                                    Process,
                                    MapField + static_cast<std::uintptr_t>(MapInfo.MapListOffset),
                                    LinkedList) &&
                                Process->Read(
                                    LinkedList,
                                    &LinkedListClass,
                                    sizeof(LinkedListClass)) &&
                                IsPlausibleObject(
                                    Process,
                                    LinkedList,
                                    LinkedListClass) &&
                                ClassScanner.ReadClassName(
                                    LinkedListClass,
                                    LinkedListName) &&
                                LinkedListName.find("LinkedList") != std::string::npos)
                            {
                            std::uintptr_t Dictionary = 0;
                            std::uintptr_t DictionaryClass = 0;
                            std::string DictionaryName;
                            if (ReadPointer(
                                    Process,
                                    MapField + static_cast<std::uintptr_t>(MapInfo.DictionaryOffset),
                                    Dictionary) &&
                                Process->Read(
                                    Dictionary,
                                    &DictionaryClass,
                                    sizeof(DictionaryClass)) &&
                                IsPlausibleObject(Process, Dictionary, DictionaryClass) &&
                                ClassScanner.ReadClassName(DictionaryClass, DictionaryName) &&
                                DictionaryName.find("Dictionary") != std::string::npos)
                            {
                                std::uintptr_t Entries = 0;
                                std::uintptr_t EntriesClass = 0;
                                std::int32_t DictionaryCount = 0;
                                std::uint64_t EntriesLength = 0;
                                if (ReadPointer(
                                        Process,
                                        Dictionary + static_cast<std::uintptr_t>(
                                            MapInfo.DictionaryEntriesOffset),
                                        Entries) &&
                                    Process->Read(
                                        Entries,
                                        &EntriesClass,
                                        sizeof(EntriesClass)) &&
                                    IsPlausibleObject(Process, Entries, EntriesClass) &&
                                    Process->Read(
                                        Dictionary + static_cast<std::uintptr_t>(
                                            MapInfo.DictionaryCountOffset),
                                        &DictionaryCount,
                                        sizeof(DictionaryCount)) &&
                                    DictionaryCount > 0 &&
                                    static_cast<std::uint32_t>(DictionaryCount) <=
                                        Settings.Monitor.MaxObjectsPerType &&
                                    Process->Read(
                                        Entries + static_cast<std::uintptr_t>(
                                            MapInfo.ArrayLengthOffset),
                                        &EntriesLength,
                                        sizeof(EntriesLength)) &&
                                    EntriesLength >= static_cast<std::uint64_t>(DictionaryCount) &&
                                    EntriesLength <= Settings.Monitor.MaxObjectsPerType)
                                {
                                    for (std::int32_t Index = 0;
                                         Index < DictionaryCount;
                                         ++Index)
                                    {
                                        const std::uint64_t EntryOffset =
                                            MapInfo.ArrayDataOffset +
                                            static_cast<std::uint64_t>(Index) * MapInfo.EntryStride;
                                        std::int32_t HashCode = -1;
                                        std::uint32_t Key = 0;
                                        std::uintptr_t Node = 0;
                                        std::uintptr_t NodeClass = 0;
                                        std::uintptr_t NodeList = 0;
                                        std::uint32_t NodeKey = 0;
                                        std::string NodeClassName;
                                        std::int32_t ResourceValue = -1;
                                        if (!Process->Read(
                                                Entries + static_cast<std::uintptr_t>(
                                                    EntryOffset + MapInfo.EntryHashOffset),
                                                &HashCode,
                                                sizeof(HashCode)) ||
                                            HashCode < 0 ||
                                            !ReadUnsigned(
                                                Process,
                                                Entries + static_cast<std::uintptr_t>(
                                                    EntryOffset + MapInfo.EntryKeyOffset),
                                                Key) ||
                                            Key != MapInfo.EntryKeyValue ||
                                            static_cast<std::uint32_t>(HashCode) != Key ||
                                            !ReadPointer(
                                                Process,
                                                Entries + static_cast<std::uintptr_t>(
                                                    EntryOffset + MapInfo.EntryNodeOffset),
                                                Node) ||
                                            !Process->Read(
                                                Node,
                                                &NodeClass,
                                                sizeof(NodeClass)) ||
                                            !IsPlausibleObject(
                                                Process,
                                                Node,
                                                NodeClass) ||
                                            !ClassScanner.ReadClassName(
                                                NodeClass,
                                                NodeClassName) ||
                                            NodeClassName.find("LinkedListNode") ==
                                                std::string::npos ||
                                            !ReadPointer(
                                                Process,
                                                Node + static_cast<std::uintptr_t>(
                                                    MapInfo.NodeListOffset),
                                                NodeList) ||
                                            NodeList != LinkedList ||
                                            !ReadUnsigned(
                                                Process,
                                                Node + static_cast<std::uintptr_t>(
                                                    MapInfo.NodeKeyOffset),
                                                NodeKey) ||
                                            NodeKey != MapInfo.EntryKeyValue ||
                                            !Process->Read(
                                                Node + static_cast<std::uintptr_t>(
                                                    MapInfo.NodeValueOffset),
                                                &ResourceValue,
                                                sizeof(ResourceValue)) ||
                                            ResourceValue < 0 ||
                                            static_cast<std::uint32_t>(ResourceValue) >
                                                Settings.Monitor.MaxRefreshValue)
                                        {
                                            continue;
                                        }

                                        Candidate Candidate;
                                        Candidate.Object = Node;
                                        Candidate.ClassPointer = NodeClass;
                                        Candidate.RootObject = Root;
                                        Candidate.RootClassPointer = RootClass->second;
                                        Candidate.ValueOffset = MapInfo.NodeValueOffset;
                                        Candidate.Kind = ValueKind::Signed32;
                                        Candidate.Semantics = RefreshSemantics::Remaining;
                                        Candidate.Maximum = Settings.Monitor.MaxRefreshValue;
                                        Candidate.RootValidated = true;
                                        Candidate.PointerGuards = RootTarget.PointerGuards;
                                        Candidate.PointerGuards.push_back({
                                            Root,
                                            MapInfo.InfoOffset,
                                            ResourceInfo,
                                            ResourceInfoClass->second
                                        });
                                        Candidate.PointerGuards.push_back({
                                            ResourceInfo,
                                            MapInfo.MapFieldOffset,
                                            MapField,
                                            MapFieldClass
                                        });
                                        Candidate.PointerGuards.push_back({
                                            MapField,
                                            MapInfo.DictionaryOffset,
                                            Dictionary,
                                            DictionaryClass
                                        });
                                        Candidate.PointerGuards.push_back({
                                            Dictionary,
                                            MapInfo.DictionaryEntriesOffset,
                                            Entries,
                                            EntriesClass
                                        });
                                        Candidate.PointerGuards.push_back({
                                            MapField,
                                            MapInfo.MapListOffset,
                                            LinkedList,
                                            LinkedListClass
                                        });
                                        Candidate.PointerGuards.push_back({
                                            Entries,
                                            EntryOffset + MapInfo.EntryNodeOffset,
                                            Node,
                                            NodeClass
                                        });
                                        Candidate.PointerGuards.push_back({
                                            Node,
                                            MapInfo.NodeListOffset,
                                            LinkedList,
                                            LinkedListClass
                                        });
                                        Candidate.ValueGuards = RootTarget.ValueGuards;
                                        Candidate.ValueGuards.push_back({
                                            Dictionary + static_cast<std::uintptr_t>(
                                                MapInfo.DictionaryCountOffset),
                                            static_cast<std::uint32_t>(DictionaryCount),
                                            sizeof(std::uint32_t)
                                        });
                                        Candidate.ValueGuards.push_back({
                                            Entries + static_cast<std::uintptr_t>(
                                                MapInfo.ArrayLengthOffset),
                                            EntriesLength,
                                            sizeof(std::uint64_t)
                                        });
                                        Candidate.ValueGuards.push_back({
                                            Entries + static_cast<std::uintptr_t>(
                                                EntryOffset + MapInfo.EntryHashOffset),
                                            static_cast<std::uint32_t>(HashCode),
                                            sizeof(std::uint32_t)
                                        });
                                        Candidate.ValueGuards.push_back({
                                            Entries + static_cast<std::uintptr_t>(
                                                EntryOffset + MapInfo.EntryKeyOffset),
                                            Key,
                                            sizeof(std::uint32_t)
                                        });
                                        Candidate.ValueGuards.push_back({
                                            Node + static_cast<std::uintptr_t>(
                                                MapInfo.NodeKeyOffset),
                                            NodeKey,
                                            sizeof(std::uint32_t)
                                        });
                                        const std::uintptr_t ValueAddress =
                                            Node + static_cast<std::uintptr_t>(
                                                Candidate.ValueOffset);
                                        std::int64_t ValidatedValue = 0;
                                        if (SeenFields.insert(ValueAddress).second &&
                                            ReadCandidate(Candidate, Process, ValidatedValue))
                                        {
                                            Candidates.push_back(std::move(Candidate));
                                        }
                                    }
                                }
                                }
                            }
                        }
                    }
                }

                for (const RefreshFieldConfig& Field : RootConfig.Fields)
                {
                    std::vector<PathTarget> Targets{ RootTarget };
                    for (const RefreshFieldConfig::PathStep& Step : Field.Path)
                    {
                        const auto ExpectedClass = Classes.find(Step.Type);
                        if (ExpectedClass == Classes.end())
                        {
                            Targets.clear();
                            break;
                        }

                        std::vector<PathTarget> NextTargets;
                        for (const PathTarget& Source : Targets)
                        {
                            std::uintptr_t ChildOrCollection = 0;
                            if (!ReadPointer(
                                    Process,
                                    Source.Object + static_cast<std::uintptr_t>(Step.Offset),
                                    ChildOrCollection))
                            {
                                continue;
                            }

                            if (!Step.Repeated)
                            {
                                std::uintptr_t ActualChildClass = 0;
                                if (!Process->Read(
                                        ChildOrCollection,
                                        &ActualChildClass,
                                        sizeof(ActualChildClass)) ||
                                    ActualChildClass != ExpectedClass->second)
                                {
                                    continue;
                                }

                                PathTarget Child = Source;
                                Child.Object = ChildOrCollection;
                                Child.ClassPointer = ExpectedClass->second;
                                Child.PointerGuards.push_back({
                                    Source.Object,
                                    Step.Offset,
                                    ChildOrCollection,
                                    ExpectedClass->second
                                });
                                if (Step.UnfinishedFlagOffset)
                                {
                                    bool Unfinished = false;
                                    const std::uintptr_t FlagAddress = Child.Object +
                                        static_cast<std::uintptr_t>(*Step.UnfinishedFlagOffset);
                                    if (!ReadBool(Process, FlagAddress, Unfinished) || Unfinished)
                                    {
                                        continue;
                                    }
                                    Child.ValueGuards.push_back({
                                        FlagAddress,
                                        0,
                                        sizeof(std::uint8_t)
                                    });
                                }
                                NextTargets.push_back(std::move(Child));
                                continue;
                            }

                            std::uintptr_t Array = 0;
                            std::uint32_t Count = 0;
                            const auto& Layout = Settings.Exchange;
                            if (!ReadPointer(
                                    Process,
                                    ChildOrCollection +
                                        static_cast<std::uintptr_t>(Layout.RepeatedArrayOffset),
                                    Array) ||
                                !ReadUnsigned(
                                    Process,
                                    ChildOrCollection +
                                        static_cast<std::uintptr_t>(Layout.RepeatedCountOffset),
                                    Count) ||
                                Count == 0 ||
                                Count > Settings.Monitor.MaxObjectsPerType)
                            {
                                continue;
                            }

                            std::uint64_t Length = 0;
                            if (!Process->Read(
                                    Array + static_cast<std::uintptr_t>(Layout.ArrayLengthOffset),
                                    &Length,
                                    sizeof(Length)) ||
                                Length < Count ||
                                Length > Settings.Monitor.MaxObjectsPerType)
                            {
                                continue;
                            }

                            for (std::uint32_t Index = 0; Index < Count; ++Index)
                            {
                                const std::uintptr_t ElementOffset =
                                    static_cast<std::uintptr_t>(Layout.ArrayDataOffset) +
                                    static_cast<std::uintptr_t>(Index) * sizeof(std::uintptr_t);
                                std::uintptr_t Child = 0;
                                std::uintptr_t ActualChildClass = 0;
                                if (!ReadPointer(
                                        Process,
                                        Array + ElementOffset,
                                        Child) ||
                                    !Process->Read(
                                        Child,
                                        &ActualChildClass,
                                        sizeof(ActualChildClass)) ||
                                    ActualChildClass != ExpectedClass->second)
                                {
                                    continue;
                                }

                                PathTarget Target = Source;
                                Target.Object = Child;
                                Target.ClassPointer = ExpectedClass->second;
                                Target.PointerGuards.push_back({
                                    Source.Object,
                                    Step.Offset,
                                    ChildOrCollection,
                                    0
                                });
                                Target.PointerGuards.push_back({
                                    ChildOrCollection,
                                    Layout.RepeatedArrayOffset,
                                    Array,
                                    0
                                });
                                Target.PointerGuards.push_back({
                                    Array,
                                    ElementOffset,
                                    Child,
                                    ExpectedClass->second
                                });
                                Target.LowerBoundGuards.push_back({
                                    ChildOrCollection +
                                        static_cast<std::uintptr_t>(Layout.RepeatedCountOffset),
                                    static_cast<std::uint64_t>(Index) + 1,
                                    sizeof(std::uint32_t)
                                });
                                Target.LowerBoundGuards.push_back({
                                    Array + static_cast<std::uintptr_t>(Layout.ArrayLengthOffset),
                                    static_cast<std::uint64_t>(Index) + 1,
                                    sizeof(std::uint64_t)
                                });
                                if (Step.UnfinishedFlagOffset)
                                {
                                    bool Unfinished = false;
                                    const std::uintptr_t FlagAddress = Child +
                                        static_cast<std::uintptr_t>(*Step.UnfinishedFlagOffset);
                                    if (!ReadBool(Process, FlagAddress, Unfinished) || Unfinished)
                                    {
                                        continue;
                                    }
                                    Target.ValueGuards.push_back({
                                        FlagAddress,
                                        0,
                                        sizeof(std::uint8_t)
                                    });
                                }
                                NextTargets.push_back(std::move(Target));
                            }
                        }
                        Targets = std::move(NextTargets);
                        if (Targets.empty())
                        {
                            break;
                        }
                    }

                    for (const PathTarget& Target : Targets)
                    {
                        Candidate Candidate;
                        Candidate.Object = Target.Object;
                        Candidate.ClassPointer = Target.ClassPointer;
                        Candidate.RootObject = Root;
                        Candidate.RootClassPointer = RootClass->second;
                        Candidate.ValueOffset = Field.Offset;
                        Candidate.Kind = ValueKind::Unsigned32;
                        Candidate.Semantics = Field.Semantics;
                        Candidate.Maximum = Settings.Monitor.MaxRefreshValue;
                        Candidate.RootValidated = true;
                        Candidate.PointerGuards = Target.PointerGuards;
                        Candidate.ValueGuards = Target.ValueGuards;
                        Candidate.LowerBoundGuards = Target.LowerBoundGuards;

                        const std::uintptr_t ValueAddress = Candidate.Object +
                            static_cast<std::uintptr_t>(Candidate.ValueOffset);
                        std::int64_t Value = 0;
                        if (ReadCandidate(Candidate, Process, Value) &&
                            (Field.Semantics != RefreshSemantics::Remaining || Value != 0) &&
                            SeenFields.insert(ValueAddress).second)
                        {
                            Candidates.push_back(std::move(Candidate));
                        }
                    }
                }
            }
        }
        return Candidates;
    }

    std::vector<Candidate> Engine::BuildResourceCandidates(
        const ResourceTargetConfig& Target,
        const ClassPointerMap& Classes,
        const InstanceMap& Instances,
        const std::shared_ptr<RemoteProcess>& Process) const
    {
        std::vector<Candidate> Candidates;
        const auto ClassFound = Classes.find(Target.Type);
        const auto InstanceFound = Instances.find(Target.Type);
        if (ClassFound == Classes.end() || InstanceFound == Instances.end())
        {
            return Candidates;
        }

        std::unordered_set<std::uintptr_t> Seen;
        for (const std::uintptr_t Object : InstanceFound->second)
        {
            std::uintptr_t ActualClass = 0;
            if (!Process->Read(Object, &ActualClass, sizeof(ActualClass)) ||
                ActualClass != ClassFound->second)
            {
                continue;
            }

            std::uint32_t Discriminator = 0;
            if (Target.DiscriminatorOffset &&
                Target.DiscriminatorValue &&
                (!ReadUnsigned(
                     Process,
                     Object + static_cast<std::uintptr_t>(*Target.DiscriminatorOffset),
                     Discriminator) ||
                 Discriminator != *Target.DiscriminatorValue))
            {
                continue;
            }

            if (Target.IdOffset && Target.IdValue)
            {
                std::uint32_t Id = 0;
                if (!ReadUnsigned(Process, Object + static_cast<std::uintptr_t>(*Target.IdOffset), Id) ||
                    Id != *Target.IdValue)
                {
                    continue;
                }
            }

            Candidate Candidate;
            Candidate.Object = Object;
            Candidate.ClassPointer = ClassFound->second;
            Candidate.ValueOffset = Target.CountOffset;
            Candidate.Kind = Target.SignedCount
                ? ValueKind::Signed32
                : ValueKind::Unsigned32;
            Candidate.Maximum = Settings.Monitor.MaxResourceValue;
            Candidate.DiscriminatorOffset = Target.DiscriminatorOffset;
            Candidate.DiscriminatorValue = Target.DiscriminatorValue;
            Candidate.IdOffset = Target.IdOffset;
            Candidate.IdValue = Target.IdValue;

            if (Seen.insert(Object + static_cast<std::uintptr_t>(Target.CountOffset)).second)
            {
                if (Candidate.Kind == ValueKind::Signed32)
                {
                    std::int32_t Value = 0;
                    if (Process->Read(
                            Object + static_cast<std::uintptr_t>(Target.CountOffset),
                            &Value,
                            sizeof(Value)) &&
                        Value >= 0 &&
                        static_cast<std::uint32_t>(Value) <= Candidate.Maximum)
                    {
                        Candidates.push_back(std::move(Candidate));
                    }
                }
                else
                {
                    std::uint32_t Value = 0;
                    if (ReadUnsigned(
                            Process,
                            Object + static_cast<std::uintptr_t>(Target.CountOffset),
                            Value) &&
                        Value <= Candidate.Maximum)
                    {
                        Candidates.push_back(std::move(Candidate));
                    }
                }
            }
        }
        return Candidates;
    }

    std::vector<Candidate> Engine::BuildRootedResourceCandidates(
        const ResourceTargetConfig& Target,
        const ClassPointerMap& Classes,
        const InstanceMap& Instances,
        const std::shared_ptr<RemoteProcess>& Process) const
    {
        if (!Target.RootType)
        {
            return BuildResourceCandidates(Target, Classes, Instances, Process);
        }

        std::vector<Candidate> Candidates;
        const auto ValidateRequiredPointers = [&](Candidate& Candidate, std::uintptr_t Object)
        {
            const auto& Layout = Settings.Exchange;
            for (const std::uint64_t Offset : Target.RequiredPointerOffsets)
            {
                std::uintptr_t Collection = 0;
                std::uintptr_t Array = 0;
                std::uint32_t Count = 0;
                std::uint64_t Length = 0;
                if (!ReadPointer(
                        Process,
                        Object + static_cast<std::uintptr_t>(Offset),
                        Collection) ||
                    !ReadPointer(
                        Process,
                        Collection +
                            static_cast<std::uintptr_t>(Layout.RepeatedArrayOffset),
                        Array) ||
                    !ReadUnsigned(
                        Process,
                        Collection +
                            static_cast<std::uintptr_t>(Layout.RepeatedCountOffset),
                        Count) ||
                    Count > Settings.Monitor.MaxObjectsPerType ||
                    !Process->Read(
                        Array + static_cast<std::uintptr_t>(Layout.ArrayLengthOffset),
                        &Length,
                        sizeof(Length)) ||
                    Length < Count ||
                    Length > Settings.Monitor.MaxObjectsPerType)
                {
                    return false;
                }

                Candidate.PointerGuards.push_back({
                    Object,
                    Offset,
                    Collection,
                    0
                });
                Candidate.PointerGuards.push_back({
                    Collection,
                    Layout.RepeatedArrayOffset,
                    Array,
                    0
                });
                Candidate.LowerBoundGuards.push_back({
                    Collection +
                        static_cast<std::uintptr_t>(Layout.RepeatedCountOffset),
                    Count,
                    sizeof(std::uint32_t)
                });
                Candidate.LowerBoundGuards.push_back({
                    Array + static_cast<std::uintptr_t>(Layout.ArrayLengthOffset),
                    Count,
                    sizeof(std::uint64_t)
                });
            }
            return true;
        };

        if (Target.RootIsSelf)
        {
            const auto PlayerClass = Classes.find(Target.Type);
            const auto Players = Instances.find(Target.Type);
            if (PlayerClass == Classes.end() || Players == Instances.end())
            {
                return Candidates;
            }

            std::unordered_set<std::uintptr_t> Seen;
            for (const std::uintptr_t Object : Players->second)
            {
                Candidate Candidate;
                Candidate.Object = Object;
                Candidate.ClassPointer = PlayerClass->second;
                Candidate.RootObject = Object;
                Candidate.RootClassPointer = PlayerClass->second;
                Candidate.ValueOffset = Target.CountOffset;
                Candidate.Kind = Target.SignedCount
                    ? ValueKind::Signed32
                    : ValueKind::Unsigned32;
                Candidate.Maximum = Settings.Monitor.MaxResourceValue;
                Candidate.DiscriminatorOffset = Target.DiscriminatorOffset;
                Candidate.DiscriminatorValue = Target.DiscriminatorValue;
                Candidate.IdOffset = Target.IdOffset;
                Candidate.IdValue = Target.IdValue;
                Candidate.RootValidated = true;
                if (!ValidateRequiredPointers(Candidate, Object))
                {
                    continue;
                }

                const std::uintptr_t Address =
                    Object + static_cast<std::uintptr_t>(Target.CountOffset);
                std::int64_t Value = 0;
                if (Seen.insert(Address).second && ReadCandidate(Candidate, Process, Value))
                {
                    Candidates.push_back(std::move(Candidate));
                }
            }
            return Candidates;
        }

        if (Target.RootPointerOffset)
        {
            const auto RootClass = Classes.find(*Target.RootType);
            const auto ResourceClass = Classes.find(Target.Type);
            const auto Roots = Instances.find(*Target.RootType);
            if (RootClass == Classes.end() ||
                ResourceClass == Classes.end() ||
                Roots == Instances.end())
            {
                return Candidates;
            }

            std::unordered_set<std::uintptr_t> Seen;
            for (const std::uintptr_t Root : Roots->second)
            {
                if (!IsPlausibleObject(Process, Root, RootClass->second))
                {
                    continue;
                }

                std::uintptr_t Object = 0;
                if (!ReadPointer(
                        Process,
                        Root + static_cast<std::uintptr_t>(*Target.RootPointerOffset),
                        Object) ||
                    !IsPlausibleObject(Process, Object, ResourceClass->second))
                {
                    continue;
                }

                Candidate Candidate;
                Candidate.Object = Object;
                Candidate.ClassPointer = ResourceClass->second;
                Candidate.RootObject = Root;
                Candidate.RootClassPointer = RootClass->second;
                Candidate.ValueOffset = Target.CountOffset;
                Candidate.Kind = Target.SignedCount
                    ? ValueKind::Signed32
                    : ValueKind::Unsigned32;
                Candidate.Maximum = Settings.Monitor.MaxResourceValue;
                Candidate.DiscriminatorOffset = Target.DiscriminatorOffset;
                Candidate.DiscriminatorValue = Target.DiscriminatorValue;
                Candidate.IdOffset = Target.IdOffset;
                Candidate.IdValue = Target.IdValue;
                Candidate.RootValidated = true;
                Candidate.PointerGuards.push_back({
                    Root,
                    *Target.RootPointerOffset,
                    Object,
                    ResourceClass->second
                });
                if (!ValidateRequiredPointers(Candidate, Object))
                {
                    continue;
                }

                const std::uintptr_t Address =
                    Object + static_cast<std::uintptr_t>(Target.CountOffset);
                std::int64_t Value = 0;
                if (Seen.insert(Address).second && ReadCandidate(Candidate, Process, Value))
                {
                    Candidates.push_back(std::move(Candidate));
                }
            }
            return Candidates;
        }

        if (!Target.RootCollectionOffset)
        {
            return Candidates;
        }

        const auto RootClass = Classes.find(*Target.RootType);
        const auto ResourceClass = Classes.find(Target.Type);
        const auto Roots = Instances.find(*Target.RootType);
        if (RootClass == Classes.end() ||
            ResourceClass == Classes.end() ||
            Roots == Instances.end())
        {
            return Candidates;
        }

        std::unordered_set<std::uintptr_t> Seen;
        for (const std::uintptr_t Root : Roots->second)
        {
            std::uintptr_t ActualRootClass = 0;
            if (!Process->Read(Root, &ActualRootClass, sizeof(ActualRootClass)) ||
                ActualRootClass != RootClass->second)
            {
                continue;
            }

            std::uintptr_t RepeatedField = 0;
            if (!ReadPointer(
                    Process,
                    Root + static_cast<std::uintptr_t>(*Target.RootCollectionOffset),
                    RepeatedField))
            {
                continue;
            }

            std::uintptr_t Array = 0;
            std::uint32_t Count = 0;
            if (!ReadPointer(
                    Process,
                    RepeatedField + static_cast<std::uintptr_t>(Target.RepeatedArrayOffset),
                    Array) ||
                !ReadUnsigned(
                    Process,
                    RepeatedField + static_cast<std::uintptr_t>(Target.RepeatedCountOffset),
                    Count) ||
                Count == 0 ||
                Count > 2048)
            {
                continue;
            }

            std::uint64_t Length = 0;
            if (!Process->Read(
                    Array + static_cast<std::uintptr_t>(Target.ArrayLengthOffset),
                    &Length,
                    sizeof(Length)) ||
                Length < Count ||
                Length > 4096)
            {
                continue;
            }

            for (std::uint32_t Index = 0; Index < Count; ++Index)
            {
                std::uintptr_t Object = 0;
                std::uintptr_t ActualClass = 0;
                const std::uintptr_t ElementAddress = Array +
                    static_cast<std::uintptr_t>(Target.ArrayDataOffset) +
                    static_cast<std::uintptr_t>(Index) * sizeof(std::uintptr_t);
                if (!ReadPointer(Process, ElementAddress, Object) ||
                    !Process->Read(Object, &ActualClass, sizeof(ActualClass)) ||
                    ActualClass != ResourceClass->second)
                {
                    continue;
                }

                std::uint32_t Discriminator = 0;
                if (Target.DiscriminatorOffset &&
                    Target.DiscriminatorValue &&
                    (!ReadUnsigned(
                         Process,
                         Object + static_cast<std::uintptr_t>(*Target.DiscriminatorOffset),
                         Discriminator) ||
                     Discriminator != *Target.DiscriminatorValue))
                {
                    continue;
                }

                Candidate Candidate;
                Candidate.Object = Object;
                Candidate.ClassPointer = ResourceClass->second;
                Candidate.RootObject = Root;
                Candidate.RootClassPointer = RootClass->second;
                Candidate.ValueOffset = Target.CountOffset;
                Candidate.Kind = Target.SignedCount
                    ? ValueKind::Signed32
                    : ValueKind::Unsigned32;
                Candidate.Maximum = Settings.Monitor.MaxResourceValue;
                Candidate.DiscriminatorOffset = Target.DiscriminatorOffset;
                Candidate.DiscriminatorValue = Target.DiscriminatorValue;
                Candidate.RootValidated = true;
                Candidate.PointerGuards = {
                    {
                        Root,
                        *Target.RootCollectionOffset,
                        RepeatedField,
                        0
                    },
                    {
                        RepeatedField,
                        Target.RepeatedArrayOffset,
                        Array,
                        0
                    },
                    {
                        Array,
                        ElementAddress - Array,
                        Object,
                        ResourceClass->second
                    }
                };
                Candidate.LowerBoundGuards = {
                    {
                        RepeatedField +
                            static_cast<std::uintptr_t>(Target.RepeatedCountOffset),
                        static_cast<std::uint64_t>(Index) + 1,
                        sizeof(std::uint32_t)
                    },
                    {
                        Array + static_cast<std::uintptr_t>(Target.ArrayLengthOffset),
                        static_cast<std::uint64_t>(Index) + 1,
                        sizeof(std::uint64_t)
                    }
                };
                if (Seen.insert(Object + static_cast<std::uintptr_t>(Target.CountOffset)).second)
                {
                    std::int64_t Value = 0;
                    if (ReadCandidate(Candidate, Process, Value))
                    {
                        Candidates.push_back(Candidate);
                    }
                }
            }
        }
        return Candidates;
    }

    bool Engine::ReadCandidate(
        const Candidate& Candidate,
        const std::shared_ptr<RemoteProcess>& Process,
        std::int64_t& Value) const
    {
        if (Candidate.PuertsResource)
        {
            const PuertsResourceTarget& Target = *Candidate.PuertsResource;
            if (Candidate.Kind != ValueKind::FixedPoint64 ||
                Candidate.Object != Target.ValueObject ||
                Target.ValueAddress < Target.ValueObject ||
                Candidate.ValueOffset != Target.ValueAddress - Target.ValueObject ||
                (Target.Kind == PuertsResourceKind::Ap &&
                    !Process->IsReadOnlyRange(Target.SaveDirtyTrueValue - 1, 0x30)))
            {
                return false;
            }
            PuertsRuntimeReader Reader(Settings, [Process](auto Address, auto Output, auto Size)
            {
                return Process->Read(Address, Output, Size);
            });
            const auto Integer = Reader.ReadResource(Target, Candidate.Maximum);
            if (!Integer)
            {
                return false;
            }
            Value = *Integer;
            return true;
        }
        if (Candidate.FixedPointHighWord)
        {
            const std::uintptr_t DynamicAddress = Candidate.DynamicAddress != 0
                ? Candidate.DynamicAddress
                : Candidate.Object;
            if (!Candidate.DynamicValidated ||
                DynamicAddress < 0x10000 + sizeof(std::uint32_t) ||
                (DynamicAddress & (alignof(std::uint32_t) - 1)) != 0)
            {
                return false;
            }

            {
                std::lock_guard Lock(StateMutex);
                const auto State = DynamicRefreshTrackers.find(DynamicAddress);
                if (State == DynamicRefreshTrackers.end() ||
                    !State->second.Initialized ||
                    !State->second.Calibrated ||
                    State->second.LastValue != Candidate.DynamicExpectedValue)
                {
                    return false;
                }
            }

            if (Candidate.PuertsArrayValidated)
            {
                if (!Settings.PuertsArray ||
                    Candidate.Object != Candidate.PuertsArrayObject ||
                    Candidate.Object < 0x10000 ||
                    (Candidate.Object & (alignof(std::uintptr_t) - 1)) != 0)
                {
                    return false;
                }

                FixedPointValueHit Hit;
                Hit.HighWordAddress = DynamicAddress;
                Hit.PuertsArrayObject = Candidate.PuertsArrayObject;
                Hit.PuertsMapPointer = Candidate.PuertsMapPointer;
                Hit.PuertsParentObject = Candidate.PuertsParentObject;
                Hit.PuertsParentMapPointer = Candidate.PuertsParentMapPointer;
                Hit.PuertsParentPropertiesOrHash =
                    Candidate.PuertsParentPropertiesOrHash;
                Hit.PuertsLengthRaw = Candidate.PuertsLengthRaw;
                Hit.PuertsValueOffset = Candidate.ValueOffset;
                const std::optional<std::uint32_t> Integer =
                    ReadPuertsArrayInteger(Settings, Process, Hit);
                if (!Integer || *Integer != Candidate.DynamicExpectedValue)
                {
                    return false;
                }
                Value = *Integer;
                return true;
            }

            std::uint64_t RawValue = 0;
            if (DynamicAddress < sizeof(std::uint32_t) ||
                !Process->Read(
                    DynamicAddress - sizeof(std::uint32_t),
                    &RawValue,
                    sizeof(RawValue)))
            {
                return false;
            }
            const std::optional<std::uint32_t> Integer = FixedPointInteger(
                RawValue,
                Candidate.Maximum);
            if (!Integer)
            {
                return false;
            }
            if (*Integer != Candidate.DynamicExpectedValue)
            {
                return false;
            }
            Value = *Integer;
            return true;
        }

        if (!Candidate.RootValidated ||
            Candidate.RootObject < 0x10000 ||
            Candidate.RootClassPointer < 0x10000)
        {
            return false;
        }

        if (!IsPlausibleObject(
                Process,
                Candidate.RootObject,
                Candidate.RootClassPointer))
        {
            return false;
        }

        for (const CandidatePointerGuard& Guard : Candidate.PointerGuards)
        {
            std::uintptr_t ActualTarget = 0;
            if (!ReadPointer(
                    Process,
                    Guard.Owner + static_cast<std::uintptr_t>(Guard.Offset),
                    ActualTarget) ||
                ActualTarget != Guard.ExpectedTarget)
            {
                return false;
            }
            if (Guard.ExpectedClass != 0)
            {
                if (!IsPlausibleObject(Process, ActualTarget, Guard.ExpectedClass))
                {
                    return false;
                }
            }
        }

        for (const CandidateValueGuard& Guard : Candidate.ValueGuards)
        {
            std::uint64_t ActualValue = 0;
            if (!ReadGuardValue(Process, Guard.Address, Guard.Width, ActualValue) ||
                ActualValue != Guard.Expected)
            {
                return false;
            }
        }

        for (const CandidateLowerBoundGuard& Guard : Candidate.LowerBoundGuards)
        {
            std::uint64_t ActualValue = 0;
            if (!ReadGuardValue(Process, Guard.Address, Guard.Width, ActualValue) ||
                ActualValue < Guard.Minimum)
            {
                return false;
            }
        }

        if (!IsPlausibleObject(Process, Candidate.Object, Candidate.ClassPointer))
        {
            return false;
        }

        if (Candidate.DiscriminatorOffset && Candidate.DiscriminatorValue)
        {
            std::uint32_t Discriminator = 0;
            if (!ReadUnsigned(
                    Process,
                    Candidate.Object + static_cast<std::uintptr_t>(*Candidate.DiscriminatorOffset),
                    Discriminator) ||
                Discriminator != *Candidate.DiscriminatorValue)
            {
                return false;
            }
        }

        if (Candidate.IdOffset && Candidate.IdValue)
        {
            std::uint32_t Id = 0;
            if (!ReadUnsigned(
                    Process,
                    Candidate.Object + static_cast<std::uintptr_t>(*Candidate.IdOffset),
                    Id) ||
                Id != *Candidate.IdValue)
            {
                return false;
            }
        }

        if (Candidate.Kind == ValueKind::Signed32)
        {
            std::int32_t SignedValue = 0;
            if (!Process->Read(
                    Candidate.Object + static_cast<std::uintptr_t>(Candidate.ValueOffset),
                    &SignedValue,
                    sizeof(SignedValue)) ||
                SignedValue < 0 ||
                static_cast<std::uint32_t>(SignedValue) > Candidate.Maximum)
            {
                return false;
            }
            Value = SignedValue;
            return true;
        }

        std::uint32_t UnsignedValue = 0;
        if (!ReadUnsigned(
                Process,
                Candidate.Object + static_cast<std::uintptr_t>(Candidate.ValueOffset),
                UnsignedValue) ||
            UnsignedValue > Candidate.Maximum)
        {
            return false;
        }
        Value = UnsignedValue;
        return true;
    }

    bool Engine::RefreshSnapshot(const std::shared_ptr<RemoteProcess>& Process)
    {
        UpdateDynamicRefreshCandidates(Process);

        std::vector<Candidate> Refresh;
        std::vector<Candidate> Exchange;
        std::vector<Candidate> Ap;
        std::size_t DynamicSeedCount = 0;
        bool DynamicScanTruncated = false;
        {
            std::lock_guard Lock(StateMutex);
            if (ConnectedProcess != Process)
            {
                return false;
            }
            Refresh = RefreshCandidates;
            if (Refresh.empty())
            {
                Refresh = DynamicRefreshCandidates;
            }
            Exchange = ExchangeCandidates;
            Ap = ApCandidates;
            DynamicSeedCount = DynamicRefreshTrackers.size();
            DynamicScanTruncated = DynamicRefreshScanTruncated;
        }

        auto Collect = [&](const std::vector<Candidate>& Candidates)
        {
            std::vector<std::int64_t> Values;
            Values.reserve(Candidates.size());
            for (const Candidate& Candidate : Candidates)
            {
                std::int64_t Value = 0;
                if (ReadCandidate(Candidate, Process, Value))
                {
                    Values.push_back(Value);
                }
            }
            return Values;
        };

        const std::vector<std::int64_t> RefreshValues = Collect(Refresh);
        const std::vector<std::int64_t> ExchangeValues = Collect(Exchange);
        const std::vector<std::int64_t> ApValues = Collect(Ap);
        const TrainerFeatureSnapshot RefreshSummary = Summarize(
            RefreshValues,
            Refresh,
            Settings.Monitor.MaxWriteCandidates,
            true);
        const TrainerFeatureSnapshot ExchangeSummary = Summarize(
            ExchangeValues,
            Exchange,
            Settings.Monitor.MaxWriteCandidates,
            Settings.PuertsRuntime.has_value());
        const TrainerFeatureSnapshot ApSummary = Summarize(
            ApValues, Ap, Settings.Monitor.MaxWriteCandidates, true);

        std::lock_guard Lock(StateMutex);
        if (ConnectedProcess != Process)
        {
            return false;
        }

        const bool CandidatesLost =
            (CachedSnapshot.Refresh.AddressCount != 0 && RefreshSummary.AddressCount == 0) ||
            (CachedSnapshot.Exchange.AddressCount != 0 && ExchangeSummary.AddressCount == 0) ||
            (CachedSnapshot.Ap.AddressCount != 0 && ApSummary.AddressCount == 0);
        CachedSnapshot.Refresh = RefreshSummary;
        CachedSnapshot.Exchange = ExchangeSummary;
        CachedSnapshot.Ap = ApSummary;
        if (RefreshSummary.AddressCount == 0 && ExchangeSummary.AddressCount == 0 &&
            ApSummary.AddressCount == 0)
        {
            CachedSnapshot.State = TrainerStateScanning;
            CopyUtf8(
                CachedSnapshot.StatusMessage,
                sizeof(CachedSnapshot.StatusMessage),
                DynamicSeedCount != 0 && !DynamicScanTruncated
                    ? Text("refresh_calibrating")
                    : Text("no_targets"));
        }
        else
        {
            CachedSnapshot.State = TrainerStateReady;
            CopyUtf8(
                CachedSnapshot.StatusMessage,
                sizeof(CachedSnapshot.StatusMessage),
                Text("ready"));
        }
        return CandidatesLost;
    }

    bool Engine::Apply(
        TrainerFeature Feature,
        TrainerOperation Operation,
        std::int64_t Value,
        std::int32_t& Writes,
        std::string& Error)
    {
        Writes = 0;
        Error.clear();
        if (Operation != TrainerOperationSet && Operation != TrainerOperationAdd)
        {
            Error = "Unsupported operation.";
            return false;
        }
        if (Value < 0)
        {
            Error = "A target value or increment cannot be negative.";
            return false;
        }
        if (Feature == TrainerFeatureAp && !Settings.PuertsAp)
        {
            Error = Text("ap_unavailable");
            return false;
        }

        const std::vector<Candidate>* Source = nullptr;
        std::vector<Candidate> Candidates;
        std::shared_ptr<RemoteProcess> Process;
        std::uint32_t Maximum = 0;
        {
            std::lock_guard Lock(StateMutex);
            Process = ConnectedProcess;
            if (!Process || !CachedSnapshot.BuildVerified)
            {
                Error = "A compatible game process is not connected.";
                return false;
            }

            switch (Feature)
            {
            case TrainerFeatureRefresh:
                Source = RefreshCandidates.empty()
                    ? &DynamicRefreshCandidates
                    : &RefreshCandidates;
                Maximum = Settings.Monitor.MaxRefreshValue;
                break;
            case TrainerFeatureExchange:
                Source = &ExchangeCandidates;
                Maximum = Settings.Monitor.MaxResourceValue;
                break;
            case TrainerFeatureAp:
                Source = &ApCandidates;
                Maximum = Settings.Monitor.MaxResourceValue;
                break;
            default:
                Error = "Unsupported trainer feature.";
                return false;
            }
            Candidates = *Source;
        }

        if (static_cast<std::uint64_t>(Value) > Maximum)
        {
            Error = "The requested value exceeds the configured safe limit.";
            return false;
        }
        if (Candidates.empty())
        {
            Error = Text("operation_no_address");
            return false;
        }

        if ((Feature == TrainerFeatureRefresh || Settings.PuertsRuntime) &&
            Candidates.size() != 1)
        {
            Error = Text("operation_safety_error");
            return false;
        }

        if (Candidates.size() > Settings.Monitor.MaxWriteCandidates ||
            std::any_of(Candidates.begin(), Candidates.end(), [](const Candidate& Candidate)
            {
                return !Candidate.RootValidated && !Candidate.PuertsArrayValidated &&
                    !Candidate.PuertsResource;
            }))
        {
            Error = Text("operation_safety_error");
            return false;
        }

        struct PlannedWrite
        {
            Candidate Candidate;
            std::int64_t Previous{};
            std::int64_t Next{};
        };

        std::vector<PlannedWrite> Plan;
        Plan.reserve(Candidates.size());
        for (const Candidate& Candidate : Candidates)
        {
            std::int64_t Current = 0;
            if (!ReadCandidate(Candidate, Process, Current))
            {
                Error = Text("operation_safety_error");
                return false;
            }

            const std::int64_t Next = Feature != TrainerFeatureRefresh
                ? ApplyResourceOperation(
                    static_cast<std::int32_t>(Current),
                    static_cast<std::uint32_t>(Value),
                    Candidate.Maximum,
                    Operation == TrainerOperationSet)
                : ApplyRefreshOperation(
                    static_cast<std::uint32_t>(Current),
                    static_cast<std::uint32_t>(Value),
                    Candidate.Maximum,
                    Candidate.Semantics,
                    Operation == TrainerOperationSet);
            Plan.push_back({ Candidate, Current, Next });
        }

        for (const PlannedWrite& Write : Plan)
        {
            std::int64_t Current = 0;
            if (!ReadCandidate(Write.Candidate, Process, Current) || Current != Write.Previous)
            {
                Error = Text("operation_safety_error");
                return false;
            }
        }

        for (std::size_t Index = 0; Index < Plan.size(); ++Index)
        {
            const PlannedWrite& Write = Plan[Index];
            const std::uintptr_t Address = Write.Candidate.Object +
                static_cast<std::uintptr_t>(Write.Candidate.ValueOffset);
            std::string WriteError;
            const bool CountSucceeded = Write.Candidate.Kind == ValueKind::FixedPoint64
                ? [&]
                {
                    const auto Next =
                        static_cast<std::uint64_t>(Write.Next) << 32;
                    return Process->Write(Address, &Next, sizeof(Next), WriteError);
                }()
                : Write.Candidate.Kind == ValueKind::Signed32
                ? [&]
                {
                    const auto Next = static_cast<std::int32_t>(Write.Next);
                    return Process->Write(Address, &Next, sizeof(Next), WriteError);
                }()
                : [&]
                {
                    const auto Next = static_cast<std::uint32_t>(Write.Next);
                    return Process->Write(Address, &Next, sizeof(Next), WriteError);
                }();
            bool Succeeded = CountSucceeded;
            if (CountSucceeded)
            {
                ++Writes;
                if (Feature == TrainerFeatureAp)
                {
                    std::int64_t Current = 0;
                    if (!ReadCandidate(Write.Candidate, Process, Current) ||
                        Current != Write.Next)
                    {
                        Succeeded = false;
                        WriteError = "The AP owner changed before its save-dirty flag could be set.";
                    }
                    else
                    {
                        const auto& Target = *Write.Candidate.PuertsResource;
                        const std::uint64_t TrueValue = Target.SaveDirtyTrueValue;
                        // Match DataSave.forceSetDirty without replacing a model, proxy, or save buffer.
                        Succeeded = Process->Write(
                            Target.SaveDirtyAddress, &TrueValue, sizeof(TrueValue), WriteError);
                    }
                }
            }
            if (Succeeded)
            {
                continue;
            }

            std::string RollbackError;
            for (std::size_t RollbackIndex = Index + (CountSucceeded ? 1 : 0);
                 RollbackIndex > 0; --RollbackIndex)
            {
                const PlannedWrite& Previous = Plan[RollbackIndex - 1];
                std::int64_t Current = 0;
                if (!ReadCandidate(Previous.Candidate, Process, Current) ||
                    Current != Previous.Next)
                {
                    if (RollbackError.empty())
                    {
                        RollbackError = "A previously written field changed before rollback.";
                    }
                    continue;
                }

                std::string RestoreError;
                const std::uintptr_t PreviousAddress = Previous.Candidate.Object +
                    static_cast<std::uintptr_t>(Previous.Candidate.ValueOffset);
                const bool Restored = Previous.Candidate.Kind == ValueKind::FixedPoint64
                    ? [&]
                    {
                        const auto Original =
                            static_cast<std::uint64_t>(Previous.Previous) << 32;
                        return Process->Write(
                            PreviousAddress,
                            &Original,
                            sizeof(Original),
                            RestoreError);
                    }()
                    : Previous.Candidate.Kind == ValueKind::Signed32
                    ? [&]
                    {
                        const auto Original = static_cast<std::int32_t>(Previous.Previous);
                        return Process->Write(
                            PreviousAddress,
                            &Original,
                            sizeof(Original),
                            RestoreError);
                    }()
                    : [&]
                    {
                        const auto Original = static_cast<std::uint32_t>(Previous.Previous);
                        return Process->Write(
                            PreviousAddress,
                            &Original,
                            sizeof(Original),
                            RestoreError);
                    }();
                if (!Restored && RollbackError.empty())
                {
                    RollbackError = std::move(RestoreError);
                }
                if (Restored)
                {
                    --Writes;
                }
            }

            Error = "The write was aborted: " + WriteError;
            if (!RollbackError.empty())
            {
                Error += ". Rollback could not restore every earlier field: " + RollbackError;
            }
            else
            {
                Error += ". Earlier writes were rolled back.";
            }
            return false;
        }

        {
            std::lock_guard Lock(StateMutex);
            for (const PlannedWrite& Write : Plan)
            {
                if (!Write.Candidate.FixedPointHighWord)
                {
                    continue;
                }
                const std::uintptr_t DynamicAddress =
                    Write.Candidate.DynamicAddress != 0
                        ? Write.Candidate.DynamicAddress
                        : Write.Candidate.Object;
                const auto State = DynamicRefreshTrackers.find(DynamicAddress);
                if (State != DynamicRefreshTrackers.end())
                {
                    State->second.LastValue = static_cast<std::uint32_t>(Write.Next);
                    State->second.DecrementEvidence = 2;
                    State->second.Calibrated = true;
                    State->second.Initialized = true;
                }
                for (Candidate& DynamicCandidate : DynamicRefreshCandidates)
                {
                    if (DynamicCandidate.DynamicAddress == DynamicAddress ||
                        (DynamicCandidate.DynamicAddress == 0 &&
                         DynamicCandidate.Object == DynamicAddress))
                    {
                        DynamicCandidate.DynamicExpectedValue =
                            static_cast<std::uint32_t>(Write.Next);
                    }
                }
            }
        }

        Error = Text("operation_succeeded");
        const std::size_t Placeholder = Error.find("{0}");
        if (Placeholder != std::string::npos)
        {
            Error.replace(Placeholder, 3, std::to_string(Writes));
        }

        return true;
    }
}
