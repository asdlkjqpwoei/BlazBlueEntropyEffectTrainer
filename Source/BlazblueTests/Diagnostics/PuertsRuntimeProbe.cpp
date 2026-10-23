#include "Config.h"
#include "Paths.h"
#include "PuertsRuntime.h"

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace
{
    using Clock = std::chrono::steady_clock;

    template<typename T>
    bool Parse(std::string_view Text, T& Value, int Base)
    {
        if (Base == 16 && (Text.starts_with("0x") || Text.starts_with("0X")))
        {
            Text.remove_prefix(2);
        }
        const auto Result = std::from_chars(Text.data(), Text.data() + Text.size(), Value, Base);
        return !Text.empty() && Result.ec == std::errc{} &&
            Result.ptr == Text.data() + Text.size();
    }
}

int RunRuntimeProbe(int ArgumentCount, char** Arguments)
{
    std::uint32_t WatchSeconds = 0;
    std::optional<std::uintptr_t> ExpectedExchange;
    std::optional<std::uintptr_t> ExpectedAp;
    for (int Index = 1; Index < ArgumentCount; ++Index)
    {
        const std::string_view Option(Arguments[Index]);
        if (Option == "--watch-seconds" && Index + 1 < ArgumentCount)
        {
            if (!Parse(std::string_view(Arguments[++Index]), WatchSeconds, 10) ||
                WatchSeconds < 600 || WatchSeconds > 86400)
            {
                std::cerr << "Watch duration must be between 600 and 86400 seconds.\n";
                return 1;
            }
        }
        else if ((Option == "--expected-exchange-high-word" ||
            Option == "--expected-ap-high-word") && Index + 1 < ArgumentCount)
        {
            std::uintptr_t Address = 0;
            if (!Parse(std::string_view(Arguments[++Index]), Address, 16) ||
                Address < 0x10004 || (Address & 7) != 4)
            {
                std::cerr << "The expected CE high-word address must be a valid aligned hexadecimal address.\n";
                return 1;
            }
            if (Option == "--expected-ap-high-word")
            {
                ExpectedAp = Address;
            }
            else
            {
                ExpectedExchange = Address;
            }
        }
        else
        {
            std::cerr << "Usage: puerts_runtime_probe [--watch-seconds 600] [--expected-exchange-high-word <hex>] [--expected-ap-high-word <hex>]\n";
            return 1;
        }
    }

    try
    {
        const Trainer::Config Configuration = Trainer::LoadConfig(
            Trainer::DefaultConfigPath());
        if (!Configuration.PuertsRuntime || !Configuration.PuertsArray || !Configuration.ExchangePuertsIndex)
        {
            std::cerr << "A shared Puerts runtime profile is required.\n";
            return 1;
        }
        if (ExpectedAp && !Configuration.PuertsAp)
        {
            std::cerr << "An AP model profile is required to compare an AP address.\n";
            return 1;
        }
        const auto Started = Clock::now();
        const auto Deadline = Started + std::chrono::seconds(WatchSeconds);
        std::shared_ptr<Trainer::RemoteProcess> Process;
        std::vector<std::uintptr_t> Constructors;
        std::vector<std::uintptr_t> ApConstructors;
        std::string Previous;
        auto NextScan = Started;
        auto NextHeartbeat = Started + std::chrono::seconds(30);
        bool ObservedExchange = false;
        bool ObservedAp = false;
        bool ExpectedMatched = !ExpectedExchange;
        bool ExpectedApMatched = !ExpectedAp;
        std::cout << "read_only=true";
        if (WatchSeconds != 0)
        {
            std::cout << " Monitoring is active; duration_seconds=" << WatchSeconds
                << "; game focus is not required";
        }
        std::cout << '\n' << std::flush;

        do
        {
            if (Process && !Process->IsAlive())
            {
                std::cout << "game_process_exited pid=" << Process->ProcessId() << '\n' << std::flush;
                Process.reset();
                Constructors.clear();
                ApConstructors.clear();
                NextScan = Clock::now();
            }
            if (!Process && Clock::now() >= NextScan)
            {
                const auto Attach = Trainer::RemoteProcess::FindCompatibleGameProcess(Configuration);
                if (Attach.Status == Trainer::AttachStatus::Attached && Attach.Process)
                {
                    Process = Attach.Process;
                    std::cout << "attached pid=" << Process->ProcessId() << '\n' << std::flush;
                }
                else if (WatchSeconds == 0)
                {
                    std::cerr << "Attach failed: " << Attach.Message << '\n';
                    return 2;
                }
                NextScan = Clock::now() + std::chrono::seconds(2);
            }
            if (Process)
            {
                Trainer::PuertsRuntimeReader Reader(Configuration, [Process](auto Address, auto Output, auto Size)
                {
                    return Process->Read(Address, Output, Size);
                });
                const bool Valid = !Constructors.empty() &&
                    std::all_of(Constructors.begin(), Constructors.end(), [&](auto Constructor)
                    {
                        return Reader.IsModelConstructor(Constructor);
                    }) && (!Configuration.PuertsAp ||
                        (!ApConstructors.empty() &&
                            std::all_of(ApConstructors.begin(), ApConstructors.end(), [&](auto Constructor)
                            {
                                return Reader.IsApModelConstructor(Constructor);
                            })));
                if (!Valid && (WatchSeconds == 0 || Clock::now() >= NextScan))
                {
                    const auto Scan = Trainer::ScanPuertsModelConstructors(
                        Configuration, Process, std::stop_token{});
                    std::cout << "scan_complete=" << Scan.Complete
                        << " class_maps=" << Scan.ClassMaps
                        << " constructors=" << Scan.Constructors.size()
                        << " ap_constructors=" << Scan.ApConstructors.size()
                        << " bytes=" << Scan.BytesScanned
                        << " changed_regions=" << Scan.ChangedRegions
                        << " error=" << (Scan.Error.empty() ? "none" : Scan.Error) << '\n' << std::flush;
                    Constructors = Scan.Complete ? Scan.Constructors : std::vector<std::uintptr_t>{};
                    ApConstructors = Scan.Complete ? Scan.ApConstructors : std::vector<std::uintptr_t>{};
                    NextScan = Clock::now() + std::chrono::seconds(2);
                    if (!Scan.Complete && WatchSeconds == 0)
                    {
                        return 2;
                    }
                }

                std::ostringstream State;
                State << "pid=" << Process->ProcessId() << " model_constructors=" << Constructors.size()
                    << " ap_model_constructors=" << ApConstructors.size();
                const auto Describe = [&](const char* Name, const auto& Target)
                {
                    State << ' ' << Name << '=';
                    if (!Target)
                    {
                        State << "inactive_or_invalid";
                        return;
                    }
                    State << std::dec << Target->IntegerValue
                        << " high_word=0x" << std::hex
                        << Target->ValueAddress + Configuration.PuertsArray->ValueHighWordOffset
                        << " array=0x" << Target->ArrayObject
                        << " model=0x" << Target->ModelObject;
                };
                for (const auto Constructor : Constructors)
                {
                    const auto Refresh = Reader.ResolveResource(
                        Constructor, Configuration.PuertsArray->ResourceIndex, Configuration.Monitor.MaxRefreshValue);
                    const auto Exchange = Reader.ResolveResource(
                        Constructor, *Configuration.ExchangePuertsIndex, Configuration.Monitor.MaxResourceValue);
                    State << " constructor=0x" << std::hex << Constructor;
                    Describe("refresh", Refresh);
                    Describe("exchange", Exchange);
                    if (Exchange)
                    {
                        ObservedExchange = true;
                        const auto Address = Exchange->ValueAddress +
                            Configuration.PuertsArray->ValueHighWordOffset;
                        if (ExpectedExchange && Address == *ExpectedExchange)
                        {
                            ExpectedMatched = true;
                            State << " matches_ce=true";
                        }
                    }
                }
                for (const auto ApConstructor : ApConstructors)
                {
                    State << " ap_constructor=0x" << std::hex << ApConstructor;
                    const auto Ap = Reader.ResolveAp(ApConstructor, Configuration.Monitor.MaxResourceValue);
                    Describe("ap", Ap);
                    if (Ap)
                    {
                        const bool ImmutableBoolean = Process->IsReadOnlyRange(
                            Ap->SaveDirtyTrueValue - 1, 0x30);
                        ObservedAp = ObservedAp || ImmutableBoolean;
                        State << " save_dirty_address=0x" << Ap->SaveDirtyAddress
                            << " canonical_true=0x" << Ap->SaveDirtyTrueValue
                            << " canonical_true_read_only=" << ImmutableBoolean;
                        std::uint64_t DirtyValue = 0;
                        if (Process->Read(Ap->SaveDirtyAddress, &DirtyValue, sizeof(DirtyValue)))
                        {
                            State << " save_dirty_value=0x" << DirtyValue;
                        }
                        else
                        {
                            State << " save_dirty_value=unreadable";
                        }
                        if (ExpectedAp && ImmutableBoolean &&
                            Ap->ValueAddress + Configuration.PuertsArray->ValueHighWordOffset == *ExpectedAp)
                        {
                            ExpectedApMatched = true;
                            State << " matches_ap_ce=true";
                        }
                    }
                }
                if (State.str() != Previous)
                {
                    std::cout << State.str() << '\n' << std::flush;
                    Previous = State.str();
                }
            }
            if (WatchSeconds == 0)
            {
                break;
            }
            if (Clock::now() >= NextHeartbeat)
            {
                std::cout << "monitor_alive elapsed_seconds="
                    << std::chrono::duration_cast<std::chrono::seconds>(Clock::now() - Started).count()
                    << '\n' << std::flush;
                NextHeartbeat = Clock::now() + std::chrono::seconds(30);
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(Configuration.Monitor.ValueCheckMs));
        } while (Clock::now() < Deadline);

        std::cout << "monitor_complete elapsed_seconds="
            << std::chrono::duration_cast<std::chrono::seconds>(Clock::now() - Started).count()
            << " observed_exchange=" << ObservedExchange
            << " observed_ap=" << ObservedAp
            << " expected_address_matched=" << ExpectedMatched
            << " expected_ap_address_matched=" << ExpectedApMatched << '\n';
        return (ObservedExchange || ObservedAp) &&
            (!ExpectedExchange || ObservedExchange) && ExpectedMatched &&
            (!ExpectedAp || ObservedAp) && ExpectedApMatched ? 0 : 3;
    }
    catch (const std::exception& Exception)
    {
        std::cerr << "Puerts runtime probe failed: " << Exception.what() << '\n';
        return 1;
    }
}
