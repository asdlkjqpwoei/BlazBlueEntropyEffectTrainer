#include "Operations.h"
#include "Config.h"
#include "Paths.h"

#include "TrainerApi.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <iostream>
#include <string>

int RunCoreTests()
{
    using Trainer::RefreshSemantics;

    const auto Require = [](bool Condition, const char* Message)
    {
        if (!Condition)
        {
            std::cerr << Message << '\n';
            return false;
        }
        return true;
    };

    if (!Require(
            Trainer::ApplyRefreshOperation(5, 99, 10000, RefreshSemantics::Remaining, true) == 99,
            "Setting remaining refreshes failed.") ||
        !Require(
            Trainer::ApplyRefreshOperation(5, 99, 10000, RefreshSemantics::Config, true) == 99,
            "Setting a refresh configuration failed.") ||
        !Require(
            Trainer::ApplyRefreshOperation(5, 99, 10000, RefreshSemantics::Used, true) == 0,
            "Resetting used refreshes failed.") ||
        !Require(
            Trainer::ApplyRefreshOperation(5, 10, 10000, RefreshSemantics::Remaining, false) == 15,
            "Adding remaining refreshes failed.") ||
        !Require(
            Trainer::ApplyRefreshOperation(5, 10, 10000, RefreshSemantics::Used, false) == 0,
            "Reducing used refreshes failed.") ||
        !Require(
            Trainer::ApplyRefreshOperation(9999, 10, 10000, RefreshSemantics::Remaining, false) == 10000,
            "Refresh count saturation failed.") ||
        !Require(
            Trainer::ApplyRefreshOperation(1, 10, 10000, RefreshSemantics::Used, false) == 0,
            "Used refresh count underflowed.") ||
        !Require(
            Trainer::ApplyResourceOperation(5, 100, 1000000, true) == 100,
            "Setting a resource count failed.") ||
        !Require(
            Trainer::ApplyResourceOperation(5, 10, 1000000, false) == 15,
            "Adding a resource count failed.") ||
        !Require(
            Trainer::ApplyResourceOperation(-10, 10, 1000000, false) == 10,
            "Negative resource value handling failed.") ||
        !Require(
            Trainer::ApplyResourceOperation(
                (std::numeric_limits<std::int32_t>::max)(),
                1,
                1000000,
                false) == 1000000,
            "Resource count saturation failed."))
    {
        return 1;
    }

    Trainer::RefreshValueTracker RefreshTracker{ 15, 0, false, true };
    if (!Require(
            Trainer::FixedPointInteger(std::uint64_t{ 15 } << 32, 10000) == 15,
            "Fixed-point integer decoding failed.") ||
        !Require(
            Trainer::FixedPointInteger(std::uint64_t{ 181 } << 32, 10000) == 181,
            "The captured Puerts refresh value did not decode as a 64-bit fixed-point integer.") ||
        !Require(
            !Trainer::FixedPointInteger((std::uint64_t{ 15 } << 32) | 1, 10000).has_value(),
            "Fractional fixed-point values must not be treated as counters.") ||
        !Require(
            !Trainer::ObserveRefreshValue(RefreshTracker, 15, 10000),
            "The refresh target was verified without an observed change.") ||
        !Require(
            !Trainer::ObserveRefreshValue(RefreshTracker, 14, 10000),
            "One decrement must not verify a refresh target.") ||
        !Require(
            Trainer::ObserveRefreshValue(RefreshTracker, 13, 10000),
            "Two consecutive decrements did not verify the refresh target.") ||
        !Require(
            Trainer::ObserveRefreshValue(RefreshTracker, 12, 10000),
            "A verified refresh target lost its calibration state."))
    {
        return 1;
    }

    Trainer::RefreshValueTracker ResetTracker{ 15, 0, false, true };
    if (!Require(
            !Trainer::ObserveRefreshValue(ResetTracker, 14, 10000),
            "The first decrement must not verify a refresh target.") ||
        !Require(
            !Trainer::ObserveRefreshValue(ResetTracker, 15, 10000),
            "An upward reset must restart refresh-target calibration.") ||
        !Require(
            !Trainer::ObserveRefreshValue(ResetTracker, 14, 10000),
            "A reset refresh target must require a new first decrement.") ||
        !Require(
            Trainer::ObserveRefreshValue(ResetTracker, 13, 10000),
            "A reset refresh target must verify after two new decrements."))
    {
        return 1;
    }

    std::array<char, 512> Error{};
    const auto ConfigPath = Trainer::DefaultConfigPath();
    const Trainer::Config Configuration = Trainer::LoadConfig(ConfigPath);
    const bool PuertsRefreshLayoutValid =
        Configuration.RefreshRoots.empty() &&
        Configuration.PuertsArray &&
        Configuration.PuertsArray->Length == 19 &&
        Configuration.PuertsArray->ResourceIndex == 14 &&
        Configuration.PuertsArray->ObjectLengthOffset == 0x8 &&
        Configuration.PuertsArray->ElementsOffset == 0x10 &&
        Configuration.PuertsArray->EntryStride == 0x8 &&
        Configuration.PuertsArray->ValueHighWordOffset == 0x4 &&
        Configuration.PuertsArray->ElementsOffset +
                static_cast<std::uint64_t>(Configuration.PuertsArray->ResourceIndex) *
                    Configuration.PuertsArray->EntryStride ==
            0x80 &&
        Configuration.PuertsArray->MapInstanceTypeOffset == 0xc &&
        Configuration.PuertsArray->ParentPropertiesOffset == 0x8 &&
        Configuration.PuertsArray->ParentElementsOffset == 0x10 &&
        Configuration.PuertsArray->FixedArrayInstanceType == 0x77 &&
        Configuration.PuertsArray->ParentInstanceType == 0x421;
    if (Configuration.Module != "GameAssembly.dll" ||
        Configuration.IdentityMinTypeMatches != 3 ||
        Configuration.TypeProfile.size() < Configuration.IdentityMinTypeMatches ||
        Configuration.Monitor.MaxWriteCandidates != 16 ||
        Configuration.Monitor.MaxDynamicRefreshCandidates != 100000 ||
        Configuration.RefreshScanSeed.has_value() ||
        !Configuration.PuertsRuntime ||
        Configuration.PuertsRuntime->ModelScript != "Data/Js/Server/Model/BreedDungeon.js" ||
        Configuration.PuertsRuntime->ModelMapField != "ForceMapName" ||
        Configuration.PuertsRuntime->SingletonField != "_instance" ||
        Configuration.ExchangePuertsIndex != 5 ||
        !Configuration.PuertsAp ||
        Configuration.PuertsAp->ModelScript != "Data/Js/Server/Model/PlayerSplitStruct.js" ||
        Configuration.PuertsAp->PrototypeMethod != "BuildCurrencyPack" ||
        Configuration.PuertsAp->CurrencyId != 1 ||
        !Configuration.Exchange.Type.empty() ||
        Configuration.TypeProfile.find("DungeonActiveInfo") == Configuration.TypeProfile.end() ||
        Configuration.TypeProfile.find("STBreedDungeonResourceInfo") == Configuration.TypeProfile.end() ||
        !PuertsRefreshLayoutValid)
    {
        std::cerr << "The path-independent process profile or rooted safety configuration is invalid.\n";
        return 1;
    }

    Trainer::PuertsArraySnapshot PuertsSnapshot;
    PuertsSnapshot.ArrayObject = 0x100000;
    PuertsSnapshot.HighWordAddress = 0x100000 + 0x84;
    PuertsSnapshot.ValueOffset = 0x80;
    PuertsSnapshot.ExpectedArrayMapPointer = 0x300001;
    PuertsSnapshot.ArrayMapPointer = 0x300001;
    PuertsSnapshot.ArrayInstanceType = 0x77;
    PuertsSnapshot.ExpectedLengthRaw = std::uint64_t{ 19 } << 32;
    PuertsSnapshot.LengthRaw = PuertsSnapshot.ExpectedLengthRaw;
    PuertsSnapshot.ParentObject = 0x200000;
    PuertsSnapshot.ExpectedParentMapPointer = 0x400001;
    PuertsSnapshot.ParentMapPointer = 0x400001;
    PuertsSnapshot.ParentInstanceType = 0x421;
    PuertsSnapshot.ParentArrayTag = PuertsSnapshot.ArrayObject + 1;
    PuertsSnapshot.ExpectedParentPropertiesOrHash = 0x1419dd00000000;
    PuertsSnapshot.ParentPropertiesOrHash =
        PuertsSnapshot.ExpectedParentPropertiesOrHash;
    PuertsSnapshot.RawValue = std::uint64_t{ 181 } << 32;

    const auto DecodedPuertsValue =
        Trainer::ValidatePuertsArraySnapshot(Configuration, PuertsSnapshot);
    auto InvalidParent = PuertsSnapshot;
    InvalidParent.ParentArrayTag += sizeof(std::uintptr_t);
    auto InvalidFraction = PuertsSnapshot;
    InvalidFraction.RawValue |= 1;
    auto InvalidLength = PuertsSnapshot;
    InvalidLength.LengthRaw = std::uint64_t{ 18 } << 32;
    auto InvalidType = PuertsSnapshot;
    InvalidType.ArrayInstanceType = 0x421;
    auto InvalidArrayMap = PuertsSnapshot;
    InvalidArrayMap.ArrayMapPointer += 2;
    auto InvalidParentMap = PuertsSnapshot;
    InvalidParentMap.ParentMapPointer += 2;
    auto InvalidParentProperties = PuertsSnapshot;
    InvalidParentProperties.ParentPropertiesOrHash |= 1;
    auto InvalidRange = PuertsSnapshot;
    InvalidRange.RawValue = std::uint64_t{ 10001 } << 32;
    auto InvalidAddress = PuertsSnapshot;
    ++InvalidAddress.HighWordAddress;
    if (!Require(
            DecodedPuertsValue && *DecodedPuertsValue == 181,
            "A structurally valid Puerts array snapshot did not decode.") ||
        !Require(
            !Trainer::ValidatePuertsArraySnapshot(Configuration, InvalidParent),
            "A mismatched Puerts parent pointer must be rejected.") ||
        !Require(
            !Trainer::ValidatePuertsArraySnapshot(Configuration, InvalidFraction),
            "A fractional Puerts fixed-point value must be rejected.") ||
        !Require(
            !Trainer::ValidatePuertsArraySnapshot(Configuration, InvalidLength),
            "A Puerts array with a mismatched length must be rejected.") ||
        !Require(
            !Trainer::ValidatePuertsArraySnapshot(Configuration, InvalidType),
            "A Puerts array with a mismatched instance type must be rejected.") ||
        !Require(
            !Trainer::ValidatePuertsArraySnapshot(Configuration, InvalidArrayMap),
            "A changed Puerts array map pointer must be rejected.") ||
        !Require(
            !Trainer::ValidatePuertsArraySnapshot(Configuration, InvalidParentMap),
            "A changed Puerts parent map pointer must be rejected.") ||
        !Require(
            !Trainer::ValidatePuertsArraySnapshot(Configuration, InvalidParentProperties),
            "A changed Puerts parent properties field must be rejected.") ||
        !Require(
            !Trainer::ValidatePuertsArraySnapshot(Configuration, InvalidRange),
            "A Puerts value above the configured range must be rejected.") ||
        !Require(
            !Trainer::ValidatePuertsArraySnapshot(Configuration, InvalidAddress),
            "A mismatched Puerts slot address must be rejected."))
    {
        return 1;
    }

    const std::vector<Trainer::PuertsCandidateReferenceCount> RankedCandidates{
        { 0x100000, 1, false },
        { 0x200000, 2, true },
        { 0x300000, 1, false }
    };
    const std::vector<Trainer::PuertsCandidateReferenceCount> TiedCandidates{
        { 0x100000, 2, true },
        { 0x200000, 2, true }
    };
    const std::vector<Trainer::PuertsCandidateReferenceCount> WeakCandidates{
        { 0x100000, 2, false }
    };
    if (!Require(
            Trainer::SelectUniquePuertsRefreshCandidate(RankedCandidates) == 0x200000,
            "A uniquely referenced Puerts candidate was not selected.") ||
        !Require(
            !Trainer::SelectUniquePuertsRefreshCandidate(TiedCandidates),
            "Ambiguous Puerts candidates must remain disabled.") ||
        !Require(
            !Trainer::SelectUniquePuertsRefreshCandidate(WeakCandidates),
            "A single allocation reference is insufficient to select a Puerts candidate."))
    {
        return 1;
    }

    TrainerEngine* Engine = TrainerCreate(ConfigPath.c_str(), Error.data(), Error.size());
    if (Engine == nullptr)
    {
        std::cerr << "Could not load defaults.yaml: " << Error.data() << '\n';
        return 1;
    }

    std::array<char, 256> ApError{};
    std::int32_t ApWrites = -1;
    if (TrainerApply(
            Engine,
            TrainerFeatureAp,
            TrainerOperationSet,
            1,
            &ApWrites,
            ApError.data(),
            ApError.size()) != 0 ||
        ApWrites != 0 ||
        ApError[0] == '\0')
    {
        std::cerr << "An AP write without a connected game must be rejected safely.\n";
        TrainerDestroy(Engine);
        return 1;
    }

    if (TrainerGetDefault(Engine, TrainerFeatureRefresh, TrainerOperationSet) != 99 ||
        TrainerGetDefault(Engine, TrainerFeatureRefresh, TrainerOperationAdd) != 10 ||
        TrainerGetDefault(Engine, TrainerFeatureExchange, TrainerOperationSet) != 9999 ||
        TrainerGetDefault(Engine, TrainerFeatureAp, TrainerOperationAdd) != 1000)
    {
        std::cerr << "Parsed feature defaults do not match defaults.yaml.\n";
        TrainerDestroy(Engine);
        return 1;
    }

    std::array<char, 256> Title{};
    if (!TrainerGetText(Engine, "title", Title.data(), Title.size()) || Title[0] == '\0')
    {
        std::cerr << "Could not read the localized application title.\n";
        TrainerDestroy(Engine);
        return 1;
    }

    TrainerDestroy(Engine);
    return 0;
}
