#include "Config.h"
#include "Paths.h"
#include "Il2CppScanner.h"
#include "RemoteProcess.h"

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <limits>
#include <memory>
#include <optional>
#include <sstream>
#include <stop_token>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace
{
    using Clock = std::chrono::steady_clock;

    constexpr std::size_t MaximumScanPasses = 8;
    constexpr std::uint32_t MinimumWatchSeconds = 600;
    constexpr std::uint32_t MaximumWatchSeconds = 24 * 60 * 60;

    std::string HexAddress(std::uintptr_t Address)
    {
        std::ostringstream Value;
        Value << "0x" << std::hex << Address;
        return Value.str();
    }

    std::optional<std::uint32_t> ReadCurrentValue(
        const Trainer::Config& Configuration,
        const std::shared_ptr<Trainer::RemoteProcess>& Process,
        const Trainer::FixedPointValueHit& Hit)
    {
        return Trainer::ReadPuertsArrayInteger(Configuration, Process, Hit);
    }

    std::optional<std::uint16_t> ReadV8HeapObjectInstanceType(
        const Trainer::Config& Configuration,
        const std::shared_ptr<Trainer::RemoteProcess>& Process,
        std::uintptr_t TaggedObject)
    {
        if (!Configuration.PuertsArray ||
            TaggedObject < 0x10000 ||
            (TaggedObject & 1) == 0)
        {
            return std::nullopt;
        }

        std::uintptr_t MapPointer = 0;
        if (!Process->Read(
                TaggedObject - 1,
                &MapPointer,
                sizeof(MapPointer)))
        {
            return std::nullopt;
        }
        return Trainer::ReadV8MapInstanceType(
            Process,
            MapPointer,
            *Configuration.PuertsArray);
    }

    struct ScanResult
    {
        std::unordered_map<std::uintptr_t, Trainer::FixedPointValueHit> Hits;
        std::unordered_map<std::string, std::size_t> InstanceCounts;
        std::size_t ResolvedClassCount{};
        std::size_t Passes{};
        std::uintptr_t MemoryCursor{};
        bool Complete{};
        bool HitsTruncated{};
        bool MemoryTruncated{};
        std::string Error;
    };

    ScanResult ScanMemory(
        const Trainer::Config& Configuration,
        const std::shared_ptr<Trainer::RemoteProcess>& Process)
    {
        ScanResult Result;
        Trainer::Il2CppScanner Scanner(Configuration, Process);
        std::vector<std::string> UnresolvedTypes;
        const Trainer::ClassPointerMap Classes = Scanner.ResolveClasses(UnresolvedTypes);
        Result.ResolvedClassCount = Classes.size();
        if (Classes.size() < Configuration.IdentityMinTypeMatches)
        {
            Result.Error = "The game process no longer matches the configured IL2CPP profile.";
            return Result;
        }

        std::uintptr_t MemoryCursor = 0;
        Trainer::ClassPointerHitMap ClassPointerHits;
        std::unordered_map<std::string, std::unordered_set<std::uintptr_t>>
            InstancesByType;
        do
        {
            bool PassTruncated = false;
            bool PassHitsTruncated = false;
            std::vector<Trainer::FixedPointValueHit> PassHits;
            const Trainer::InstanceMap PassInstances = Scanner.ScanInstances(
                Classes,
                std::stop_token{},
                MemoryCursor,
                ClassPointerHits,
                PassHits,
                PassHitsTruncated,
                PassTruncated,
                Result.Error);
            for (const auto& [Type, Objects] : PassInstances)
            {
                auto& UniqueObjects = InstancesByType[Type];
                UniqueObjects.insert(Objects.begin(), Objects.end());
            }
            for (const Trainer::FixedPointValueHit& Hit : PassHits)
            {
                if (!Hit.IsPuertsArray())
                {
                    continue;
                }
                auto& Existing = Result.Hits[Hit.HighWordAddress];
                if (Existing.HighWordAddress == 0)
                {
                    Existing = Hit;
                }
            }
            Result.HitsTruncated = Result.HitsTruncated || PassHitsTruncated;
            Result.MemoryTruncated = PassTruncated;
            Result.MemoryCursor = MemoryCursor;
            ++Result.Passes;
            if (!Result.Error.empty() || (!PassTruncated && MemoryCursor == 0))
            {
                break;
            }
        } while (MemoryCursor != 0 && Result.Passes < MaximumScanPasses);

        Result.Complete = Result.Error.empty() &&
            !Result.HitsTruncated &&
            !Result.MemoryTruncated &&
            Result.MemoryCursor == 0;
        if (Result.Error.empty() && !Result.Complete)
        {
            Result.Error = "The bounded memory scan did not complete; no result is considered final.";
        }
        for (const auto& [Type, Objects] : InstancesByType)
        {
            Result.InstanceCounts.emplace(Type, Objects.size());
        }
        return Result;
    }

    void PrintScanResult(
        const Trainer::Config& Configuration,
        const ScanResult& Result,
        const std::shared_ptr<Trainer::RemoteProcess>& Process)
    {
        std::cout << "PID: " << Process->ProcessId()
            << ", resolved IL2CPP classes: " << Result.ResolvedClassCount
            << ", scan passes: " << Result.Passes
            << ", incomplete: " << (!Result.Complete)
            << ", hit list truncated: " << Result.HitsTruncated
            << ", read error: " << (Result.Error.empty() ? "none" : Result.Error) << '\n';

        for (const auto& [Address, Hit] : Result.Hits)
        {
            std::cout << "Puerts refresh candidate: high-word=0x" << std::hex << Address
                << ", count=" << std::dec << Hit.IntegerValue
                << ", array=0x" << std::hex << Hit.PuertsArrayObject
                << ", array-map=0x" << Hit.PuertsMapPointer
                << ", allocation=0x" << Hit.PuertsAllocationBase
                << ", parent=0x" << Hit.PuertsParentObject
                << ", parent-map=0x" << Hit.PuertsParentMapPointer
                << ", parent-properties=0x" << Hit.PuertsParentPropertiesOrHash
                << ", parent-allocation-references="
                << Hit.PuertsParentAllocationReferenceCount
                << ", length=0x" << Hit.PuertsLengthRaw
                << ", slot-offset=0x" << Hit.PuertsValueOffset
                << '\n';

            if (Configuration.PuertsArray)
            {
                const Trainer::PuertsArrayConfig& Layout = *Configuration.PuertsArray;
                std::cout << "  slots:";
                for (std::uint32_t Index = 0; Index < Layout.Length; ++Index)
                {
                    const std::uintptr_t Slot = Hit.PuertsArrayObject +
                        static_cast<std::uintptr_t>(Layout.ElementsOffset) +
                        static_cast<std::uintptr_t>(Index) *
                            static_cast<std::uintptr_t>(Layout.EntryStride);
                    std::uint64_t RawValue = 0;
                    if (!Process->Read(Slot, &RawValue, sizeof(RawValue)))
                    {
                        std::cout << " [" << std::dec << Index << "]=unreadable";
                        continue;
                    }
                    std::cout << " [" << std::dec << Index << "]=0x"
                        << std::hex << RawValue;
                    const auto ObjectType = ReadV8HeapObjectInstanceType(
                        Configuration,
                        Process,
                        static_cast<std::uintptr_t>(RawValue));
                    if (ObjectType)
                    {
                        std::cout << "(type=0x" << std::hex << *ObjectType << ")";
                    }
                    const auto Integer = Trainer::FixedPointInteger(
                        RawValue,
                        Configuration.Monitor.MaxRefreshValue);
                    if (Integer)
                    {
                        std::cout << "(" << std::dec << *Integer << ")";
                    }
                }
                std::cout << '\n' << "  parent:";
                for (std::uintptr_t Offset = 0; Offset < 0x40; Offset += sizeof(std::uintptr_t))
                {
                    std::uint64_t RawValue = 0;
                    if (!Process->Read(
                            Hit.PuertsParentObject + Offset,
                            &RawValue,
                            sizeof(RawValue)))
                    {
                        std::cout << " [+0x" << std::hex << Offset << "]=unreadable";
                        continue;
                    }
                    std::cout << " [+0x" << std::hex << Offset << "]=0x" << RawValue;
                    const auto FieldType = ReadV8HeapObjectInstanceType(
                        Configuration,
                        Process,
                        RawValue);
                    if (FieldType)
                    {
                        std::cout << "(type=0x" << *FieldType << ")";
                    }
                }
                std::cout << std::dec << '\n';
            }
        }

        std::cout << "Structurally validated Puerts arrays: "
            << Result.Hits.size() << '\n';
        std::cout << "IL2CPP instance counts:";
        for (const std::string& Type : {
                 "STBreedDungeon",
                 "STExploreDungeon",
                 "DungeonActiveInfo",
                 "ExploreDungeonResource"})
        {
            const auto Found = Result.InstanceCounts.find(Type);
            std::cout << ' ' << Type << '='
                << (Found == Result.InstanceCounts.end() ? 0 : Found->second);
        }
        std::cout << '\n';
        std::vector<Trainer::PuertsCandidateReferenceCount> ReferenceCounts;
        for (const auto& [Address, Hit] : Result.Hits)
        {
            ReferenceCounts.push_back({
                Address,
                Hit.PuertsParentAllocationReferenceCount,
                Hit.PuertsParentPropertiesOrHash != 0 &&
                    (Hit.PuertsParentPropertiesOrHash & 1) == 0
            });
        }
        const auto Selected =
            Trainer::SelectUniquePuertsRefreshCandidate(ReferenceCounts);
        if (Selected)
        {
            const auto Candidate = Result.Hits.find(*Selected);
            std::cout << "Unique immediate refresh target: high-word="
                << HexAddress(*Selected)
                << ", allocation-parent-references="
                << (Candidate == Result.Hits.end()
                    ? 0
                    : Candidate->second.PuertsParentAllocationReferenceCount)
                << '\n';
        }
        else
        {
            std::cout << "No unique immediate refresh target; writes must remain disabled.\n";
        }
    }

    void PrintElapsed(std::string_view Event, Clock::time_point Start)
    {
        const auto Elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            Clock::now() - Start).count();
        std::cout << Event << " elapsed_ms=" << Elapsed << '\n' << std::flush;
    }

    void PrintParentReferences(
        const Trainer::Config& Configuration,
        const std::shared_ptr<Trainer::RemoteProcess>& Process,
        const ScanResult& Scan)
    {
        if (!Configuration.PuertsArray || Scan.Hits.empty())
        {
            return;
        }

        std::unordered_map<
            std::uintptr_t,
            std::vector<std::uintptr_t>> ArraysByParentTag;
        for (const auto& [Address, Hit] : Scan.Hits)
        {
            static_cast<void>(Address);
            if (Hit.PuertsParentObject <
                std::numeric_limits<std::uintptr_t>::max())
            {
                ArraysByParentTag[Hit.PuertsParentObject + 1].push_back(
                    Hit.PuertsArrayObject);
            }
        }

        std::unordered_map<std::uintptr_t, std::vector<std::uintptr_t>> References;
        std::uintptr_t Cursor = 0;
        std::size_t Passes = 0;
        std::uint64_t BytesScanned = 0;
        bool Truncated = false;
        std::string Error;
        std::vector<std::byte> Buffer(Configuration.Monitor.ScanChunkBytes);

        do
        {
            bool PassTruncated = false;
            std::string PassError;
            const auto Regions = Process->WritablePrivateRegions(
                Configuration.Monitor.MaxScanBytes,
                Cursor,
                PassTruncated,
                PassError);
            if (!PassError.empty())
            {
                Error = std::move(PassError);
                break;
            }

            for (const Trainer::MemoryRegion& Region : Regions)
            {
                std::size_t Consumed = 0;
                while (Consumed < Region.Size)
                {
                    const std::size_t Request = std::min(
                        Buffer.size(),
                        Region.Size - Consumed);
                    if (Process->Read(
                            Region.Base + Consumed,
                            Buffer.data(),
                            Request))
                    {
                        const std::size_t Usable = Request & ~std::size_t{ 7 };
                        for (std::size_t Offset = 0;
                             Offset + sizeof(std::uintptr_t) <= Usable;
                             Offset += sizeof(std::uintptr_t))
                        {
                            std::uintptr_t Value = 0;
                            std::memcpy(
                                &Value,
                                Buffer.data() + Offset,
                                sizeof(Value));
                            const auto Found = ArraysByParentTag.find(Value);
                            if (Found == ArraysByParentTag.end())
                            {
                                continue;
                            }
                            auto& Slots = References[Value];
                            if (Slots.size() < 20)
                            {
                                Slots.push_back(Region.Base + Consumed + Offset);
                            }
                        }
                    }
                    Consumed += Request;
                }
                BytesScanned += Region.Size;
            }
            Truncated = PassTruncated;
            ++Passes;
        } while (Cursor != 0 && Passes < MaximumScanPasses);

        std::cout << "Parent references: passes=" << Passes
            << " bytes=" << BytesScanned
            << " incomplete=" << (Cursor != 0 || Truncated)
            << " error=" << (Error.empty() ? "none" : Error) << '\n';
        for (const auto& [ParentTag, arrays] : ArraysByParentTag)
        {
            const auto Found = References.find(ParentTag);
            std::cout << "  parent-tag=" << HexAddress(ParentTag)
                << " arrays=" << arrays.size()
                << " reference-slots="
                << (Found == References.end() ? 0 : Found->second.size()) << '\n';
            if (Found == References.end())
            {
                continue;
            }

            std::unordered_set<std::uintptr_t> PrintedOwners;
            for (const std::uintptr_t Slot : Found->second)
            {
                std::cout << "    reference-slot=" << HexAddress(Slot) << '\n';
                for (std::uintptr_t OwnerOffset = 0;
                     OwnerOffset <= 0x40;
                     OwnerOffset += sizeof(std::uintptr_t))
                {
                    if (Slot < OwnerOffset)
                    {
                        break;
                    }
                    const std::uintptr_t Owner = Slot - OwnerOffset;
                    std::uintptr_t MapPointer = 0;
                    if (!Process->Read(Owner, &MapPointer, sizeof(MapPointer)))
                    {
                        continue;
                    }
                    const auto InstanceType = Trainer::ReadV8MapInstanceType(
                        Process,
                        MapPointer,
                        *Configuration.PuertsArray);
                    if (!InstanceType)
                    {
                        continue;
                    }
                    std::cout << "      owner=" << HexAddress(Owner)
                        << " offset=0x" << std::hex << OwnerOffset
                        << " map=" << HexAddress(MapPointer)
                        << " instance-type=0x" << *InstanceType
                        << std::dec << '\n';
                    if (*InstanceType == Configuration.PuertsArray->ParentInstanceType &&
                        PrintedOwners.insert(Owner).second)
                    {
                        std::cout << "        owner-fields:";
                        for (std::uintptr_t FieldOffset = 0;
                             FieldOffset < 0x50;
                             FieldOffset += sizeof(std::uintptr_t))
                        {
                            std::uintptr_t Field = 0;
                            if (Process->Read(
                                    Owner + FieldOffset,
                                    &Field,
                                    sizeof(Field)))
                            {
                                std::cout << " [+0x" << std::hex << FieldOffset
                                    << "]=0x" << Field;
                                const auto FieldType = ReadV8HeapObjectInstanceType(
                                    Configuration,
                                    Process,
                                    Field);
                                if (FieldType)
                                {
                                    std::cout << "(type=0x" << *FieldType << ")";
                                }
                            }
                        }
                        std::cout << std::dec << '\n';
                    }
                }
            }
        }
    }

    int RunWatch(const Trainer::Config& Configuration, std::uint32_t Seconds)
    {
        const auto Started = Clock::now();
        const auto Deadline = Started + std::chrono::seconds(Seconds);
        const auto RetryScanDelay = std::chrono::milliseconds(
            std::max(Configuration.Monitor.RescanMs, 60'000u));
        const auto StableScanDelay = std::chrono::milliseconds(
            std::max(Configuration.Monitor.StableRescanMs, 300'000u));
        auto NextAttach = Started;
        auto NextScan = Started;
        auto LastWaitMessage = Started - std::chrono::seconds(10);
        std::shared_ptr<Trainer::RemoteProcess> Process;
        std::unordered_map<std::uintptr_t, Trainer::FixedPointValueHit> Hits;
        std::unordered_map<std::uintptr_t, std::uint32_t> LastValues;
        std::size_t ObservedChanges = 0;
        std::size_t ObservedProcessExits = 0;
        std::size_t ObservedInvalidations = 0;

        std::cout << "WATCH_STARTED seconds=" << Seconds
            << " read_only=true" << std::endl;

        while (Clock::now() < Deadline)
        {
            const auto Now = Clock::now();
            if (Process && !Process->IsAlive())
            {
                ++ObservedProcessExits;
                PrintElapsed(
                    "PROCESS_EXITED pid=" + std::to_string(Process->ProcessId()),
                    Started);
                Process.reset();
                Hits.clear();
                LastValues.clear();
                NextAttach = Now;
                NextScan = Now;
            }

            if (!Process && Now >= NextAttach)
            {
                const Trainer::AttachResult Attach =
                    Trainer::RemoteProcess::FindCompatibleGameProcess(Configuration);
                NextAttach = Clock::now() +
                    std::chrono::milliseconds(Configuration.Monitor.ProcessCheckMs);
                if (Attach.Status == Trainer::AttachStatus::Attached && Attach.Process)
                {
                    Process = Attach.Process;
                    PrintElapsed(
                        "PROCESS_ATTACHED pid=" + std::to_string(Process->ProcessId()),
                        Started);
                    NextScan = Clock::now();
                }
                else if (Now - LastWaitMessage >= std::chrono::seconds(10))
                {
                    PrintElapsed("WAITING_FOR_GAME " + Attach.Message, Started);
                    LastWaitMessage = Now;
                }
            }

            if (Process && Process->IsAlive() && Clock::now() >= NextScan)
            {
                const ScanResult Scan = ScanMemory(Configuration, Process);
                std::vector<Trainer::PuertsCandidateReferenceCount> ReferenceCounts;
                ReferenceCounts.reserve(Scan.Hits.size());
                for (const auto& [Address, Hit] : Scan.Hits)
                {
                    ReferenceCounts.push_back({
                        Address,
                        Hit.PuertsParentAllocationReferenceCount,
                        Hit.PuertsParentPropertiesOrHash != 0 &&
                            (Hit.PuertsParentPropertiesOrHash & 1) == 0
                    });
                }
                const auto Selected =
                    Trainer::SelectUniquePuertsRefreshCandidate(ReferenceCounts);
                PrintElapsed(
                    "SCAN_COMPLETE candidates=" + std::to_string(Scan.Hits.size()) +
                        " complete=" + (Scan.Complete ? "true" : "false") +
                        (Scan.Error.empty() ? "" : " error=" + Scan.Error),
                    Started);

                if (Scan.Complete)
                {
                    if (Selected)
                    {
                        const auto Candidate = Scan.Hits.find(*Selected);
                        PrintElapsed(
                            "SELECTED_REFRESH_TARGET high_word=" +
                                HexAddress(*Selected) +
                                " count=" + std::to_string(
                                    Candidate == Scan.Hits.end()
                                        ? 0
                                        : Candidate->second.IntegerValue),
                            Started);
                    }
                    else
                    {
                        PrintElapsed("NO_UNIQUE_REFRESH_TARGET", Started);
                    }

                    std::unordered_set<std::uintptr_t> Seen;
                    for (const auto& [Address, Hit] : Scan.Hits)
                    {
                        Seen.insert(Address);
                        const auto Existing = Hits.find(Address);
                        const bool Replaced = Existing != Hits.end() &&
                            (Existing->second.PuertsArrayObject != Hit.PuertsArrayObject ||
                             Existing->second.PuertsMapPointer != Hit.PuertsMapPointer ||
                             Existing->second.PuertsParentObject != Hit.PuertsParentObject ||
                             Existing->second.PuertsParentMapPointer != Hit.PuertsParentMapPointer);
                        if (Existing == Hits.end() || Replaced)
                        {
                            LastValues.insert_or_assign(Address, Hit.IntegerValue);
                            PrintElapsed(
                                std::string(Replaced ? "CANDIDATE_REPLACED" : "BASELINE") +
                                    " high_word=" + HexAddress(Address) +
                                    " count=" + std::to_string(Hit.IntegerValue),
                                Started);
                        }
                        Hits.insert_or_assign(Address, Hit);
                    }

                    for (auto Iterator = Hits.begin(); Iterator != Hits.end();)
                    {
                        if (Seen.find(Iterator->first) == Seen.end())
                        {
                            PrintElapsed(
                                "CANDIDATE_REMOVED high_word=" +
                                    HexAddress(Iterator->first),
                                Started);
                            LastValues.erase(Iterator->first);
                            Iterator = Hits.erase(Iterator);
                        }
                        else
                        {
                            ++Iterator;
                        }
                    }
                    PrintElapsed(
                        "WATCHING_CANDIDATES count=" + std::to_string(Hits.size()),
                        Started);
                    NextScan = Clock::now() +
                        (Hits.empty() ? RetryScanDelay : StableScanDelay);
                }
                else
                {
                    NextScan = Clock::now() +
                        RetryScanDelay;
                }
            }

            bool CandidateInvalidated = false;
            if (Process && Process->IsAlive())
            {
                for (const auto& [Address, Hit] : Hits)
                {
                    const std::optional<std::uint32_t> Current =
                        ReadCurrentValue(Configuration, Process, Hit);
                    if (!Current)
                    {
                        if (LastValues.erase(Address) != 0)
                        {
                            ++ObservedInvalidations;
                            PrintElapsed(
                                "CANDIDATE_INVALID high_word=" + HexAddress(Address),
                                Started);
                        }
                        CandidateInvalidated = true;
                        continue;
                    }

                    const auto Previous = LastValues.find(Address);
                    if (Previous == LastValues.end())
                    {
                        LastValues.emplace(Address, *Current);
                        continue;
                    }
                    if (Previous->second != *Current)
                    {
                        const std::uint32_t OldValue = Previous->second;
                        const bool Decrement = *Current < OldValue;
                        PrintElapsed(
                            std::string(Decrement ? "VALUE_DECREASE" : "VALUE_INCREASE") +
                                " high_word=" + HexAddress(Address) +
                                " old=" + std::to_string(OldValue) +
                                " new=" + std::to_string(*Current) +
                                " delta=" + std::to_string(
                                    Decrement ? OldValue - *Current : *Current - OldValue),
                            Started);
                        ++ObservedChanges;
                        Previous->second = *Current;
                    }
                }
            }

            if (CandidateInvalidated &&
                (NextScan == Clock::time_point::max() ||
                 NextScan > Clock::now() + RetryScanDelay))
            {
                NextScan = Clock::now() + RetryScanDelay;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(250));
        }

        PrintElapsed(
            "WATCH_FINISHED observed_changes=" + std::to_string(ObservedChanges) +
                " process_exits=" + std::to_string(ObservedProcessExits) +
                " candidate_invalidations=" + std::to_string(ObservedInvalidations),
            Started);
        return ObservedChanges != 0 ||
                ObservedProcessExits != 0 ||
                ObservedInvalidations != 0
            ? 0
            : 3;
    }

    void PrintUsage()
    {
        std::cerr
            << "Usage: puerts_scan_probe [--watch-seconds N] [--dump-parent-references]\n"
            << "The watch mode is read-only and requires N >= 600.\n";
    }
}

int RunScanProbe(int ArgumentCount, char** Arguments)
{
    std::optional<std::uint32_t> WatchSeconds;
    bool DumpParentReferences = false;
    for (int Index = 1; Index < ArgumentCount; ++Index)
    {
        const std::string_view Argument(Arguments[Index]);
        if (Argument == "--help")
        {
            PrintUsage();
            return 0;
        }
        if (Argument == "--dump-parent-references")
        {
            DumpParentReferences = true;
            continue;
        }
        if (Argument != "--watch-seconds" || Index + 1 >= ArgumentCount)
        {
            PrintUsage();
            return 64;
        }

        std::uint32_t Seconds = 0;
        const std::string_view Value(Arguments[++Index]);
        const auto Parsed = std::from_chars(Value.data(), Value.data() + Value.size(), Seconds);
        if (Parsed.ec != std::errc{} ||
            Parsed.ptr != Value.data() + Value.size() ||
            Seconds < MinimumWatchSeconds ||
            Seconds > MaximumWatchSeconds)
        {
            PrintUsage();
            return 64;
        }
        WatchSeconds = Seconds;
    }

    try
    {
        const Trainer::Config Configuration = Trainer::LoadConfig(
            Trainer::DefaultConfigPath());
        if (WatchSeconds)
        {
            return RunWatch(Configuration, *WatchSeconds);
        }

        const Trainer::AttachResult Attach =
            Trainer::RemoteProcess::FindCompatibleGameProcess(Configuration);
        if (Attach.Status != Trainer::AttachStatus::Attached || !Attach.Process)
        {
            std::cerr << "Game process attach failed: " << Attach.Message << '\n';
            return 1;
        }

        const ScanResult Result = ScanMemory(Configuration, Attach.Process);
        PrintScanResult(Configuration, Result, Attach.Process);
        if (DumpParentReferences && Result.Complete)
        {
            PrintParentReferences(Configuration, Attach.Process, Result);
        }
        return Result.Complete ? 0 : 2;
    }
    catch (const std::exception& Exception)
    {
        std::cerr << "Probe failed: " << Exception.what() << '\n';
        return 1;
    }
}
