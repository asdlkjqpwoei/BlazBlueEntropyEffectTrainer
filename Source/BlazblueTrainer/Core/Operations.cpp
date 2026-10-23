#include "Operations.h"

#include <algorithm>
#include <limits>

namespace Trainer
{
    std::optional<std::uint32_t> FixedPointInteger(
        std::uint64_t RawValue,
        std::uint32_t Maximum) noexcept
    {
        if ((RawValue & 0xffffffffull) != 0)
        {
            return std::nullopt;
        }

        const std::uint64_t Integer = RawValue >> 32;
        if (Integer > Maximum)
        {
            return std::nullopt;
        }
        return static_cast<std::uint32_t>(Integer);
    }

    std::optional<std::uint32_t> ValidatePuertsArraySnapshot(
        const Config& Configuration,
        const PuertsArraySnapshot& Snapshot) noexcept
    {
        if (!Configuration.PuertsArray ||
            Snapshot.ArrayObject < 0x10000 ||
            (Snapshot.ArrayObject & (alignof(std::uintptr_t) - 1)) != 0 ||
            Snapshot.ParentObject < 0x10000 ||
            (Snapshot.ParentObject & (alignof(std::uintptr_t) - 1)) != 0)
        {
            return std::nullopt;
        }

        const PuertsArrayConfig& Layout = *Configuration.PuertsArray;
        if (Layout.Length == 0 ||
            Layout.ResourceIndex >= Layout.Length ||
            Layout.EntryStride < sizeof(std::uint64_t) ||
            Layout.ValueHighWordOffset >
                Layout.EntryStride - sizeof(std::uint32_t) ||
            Layout.ResourceIndex >
                (std::numeric_limits<std::uint64_t>::max() - Layout.ElementsOffset) /
                    Layout.EntryStride)
        {
            return std::nullopt;
        }

        const std::uint64_t ExpectedValueOffset =
            Layout.ElementsOffset +
            static_cast<std::uint64_t>(Layout.ResourceIndex) * Layout.EntryStride;
        const std::uint64_t MaximumAddress =
            std::numeric_limits<std::uintptr_t>::max();
        if (ExpectedValueOffset > MaximumAddress - sizeof(std::uint64_t) ||
            Layout.ValueHighWordOffset >
                MaximumAddress - ExpectedValueOffset ||
            Snapshot.ValueOffset != ExpectedValueOffset ||
            Snapshot.ArrayObject >
                MaximumAddress - ExpectedValueOffset - sizeof(std::uint64_t) ||
            Snapshot.ArrayObject >
                MaximumAddress - ExpectedValueOffset - Layout.ValueHighWordOffset ||
            Snapshot.HighWordAddress != Snapshot.ArrayObject +
                ExpectedValueOffset + Layout.ValueHighWordOffset ||
            Layout.ObjectLengthOffset >
                MaximumAddress - sizeof(std::uint64_t) ||
            Snapshot.ArrayObject >
                MaximumAddress - Layout.ObjectLengthOffset - sizeof(std::uint64_t) ||
            Layout.ParentElementsOffset >
                MaximumAddress - sizeof(std::uintptr_t) ||
            Snapshot.ParentObject >
                MaximumAddress - Layout.ParentElementsOffset - sizeof(std::uintptr_t) ||
            Layout.ParentPropertiesOffset >
                MaximumAddress - sizeof(std::uint64_t) ||
            Snapshot.ParentObject >
                MaximumAddress - Layout.ParentPropertiesOffset - sizeof(std::uint64_t))
        {
            return std::nullopt;
        }

        const std::uint64_t ExpectedLength =
            static_cast<std::uint64_t>(Layout.Length) << 32;
        if (Snapshot.ExpectedArrayMapPointer < 0x10000 ||
            (Snapshot.ExpectedArrayMapPointer & 1) == 0 ||
            Snapshot.ArrayMapPointer != Snapshot.ExpectedArrayMapPointer ||
            Snapshot.ArrayInstanceType != Layout.FixedArrayInstanceType ||
            Snapshot.ExpectedLengthRaw != ExpectedLength ||
            Snapshot.LengthRaw != ExpectedLength ||
            Snapshot.ExpectedParentMapPointer < 0x10000 ||
            (Snapshot.ExpectedParentMapPointer & 1) == 0 ||
            Snapshot.ParentMapPointer != Snapshot.ExpectedParentMapPointer ||
            Snapshot.ParentInstanceType != Layout.ParentInstanceType ||
            Snapshot.ParentPropertiesOrHash !=
                Snapshot.ExpectedParentPropertiesOrHash ||
            Snapshot.ArrayObject == std::numeric_limits<std::uintptr_t>::max() ||
            Snapshot.ParentArrayTag != Snapshot.ArrayObject + 1)
        {
            return std::nullopt;
        }

        return FixedPointInteger(
            Snapshot.RawValue,
            Configuration.Monitor.MaxRefreshValue);
    }

    std::optional<std::uintptr_t> SelectUniquePuertsRefreshCandidate(
        const std::vector<PuertsCandidateReferenceCount>& Candidates) noexcept
    {
        std::uintptr_t SelectedAddress = 0;
        std::uint32_t HighestReferenceCount = 0;
        bool Tied = false;
        for (const PuertsCandidateReferenceCount& Candidate : Candidates)
        {
            if (Candidate.Address == 0 ||
                !Candidate.ParentPropertiesIsSmi ||
                Candidate.AllocationReferenceCount == 0)
            {
                continue;
            }
            if (Candidate.AllocationReferenceCount > HighestReferenceCount)
            {
                SelectedAddress = Candidate.Address;
                HighestReferenceCount = Candidate.AllocationReferenceCount;
                Tied = false;
            }
            else if (Candidate.AllocationReferenceCount == HighestReferenceCount)
            {
                Tied = true;
            }
        }
        if (SelectedAddress == 0 || Tied)
        {
            return std::nullopt;
        }
        return SelectedAddress;
    }

    bool ObserveRefreshValue(
        RefreshValueTracker& Tracker,
        std::uint32_t Current,
        std::uint32_t Maximum) noexcept
    {
        if (!Tracker.Initialized)
        {
            Tracker.LastValue = Current;
            Tracker.DecrementEvidence = 0;
            Tracker.Calibrated = false;
            Tracker.Initialized = true;
            return false;
        }
        if (Current > Maximum)
        {
            Tracker.LastValue = Current;
            Tracker.DecrementEvidence = 0;
            Tracker.Calibrated = false;
            Tracker.Initialized = false;
            return false;
        }

        if (Tracker.Calibrated)
        {
            if (Current == Tracker.LastValue)
            {
                return true;
            }
            if (Current < Tracker.LastValue &&
                Tracker.LastValue - Current <= 2)
            {
                Tracker.LastValue = Current;
                return true;
            }
            Tracker.LastValue = Current;
            Tracker.DecrementEvidence = 0;
            Tracker.Calibrated = false;
            return false;
        }

        if (Current < Tracker.LastValue)
        {
            const std::uint32_t Decrement = Tracker.LastValue - Current;
            Tracker.DecrementEvidence =
                Decrement <= 2
                    ? Tracker.DecrementEvidence + Decrement
                    : 0;
        }
        else if (Current > Tracker.LastValue)
        {
            Tracker.DecrementEvidence = 0;
        }

        Tracker.LastValue = Current;
        Tracker.Calibrated = Tracker.DecrementEvidence >= 2;
        return Tracker.Calibrated;
    }

    std::uint32_t ApplyRefreshOperation(
        std::uint32_t Current,
        std::uint32_t Value,
        std::uint32_t Maximum,
        RefreshSemantics Semantics,
        bool SetOperation)
    {
        if (Semantics == RefreshSemantics::Used)
        {
            if (SetOperation)
            {
                return 0;
            }
            return Current > Value ? Current - Value : 0;
        }

        if (SetOperation)
        {
            return std::min(Value, Maximum);
        }

        const std::uint64_t Sum = static_cast<std::uint64_t>(Current) + Value;
        return static_cast<std::uint32_t>(std::min<std::uint64_t>(Sum, Maximum));
    }

    std::int32_t ApplyResourceOperation(
        std::int32_t Current,
        std::uint32_t Value,
        std::uint32_t Maximum,
        bool SetOperation)
    {
        const std::uint32_t NonNegative = Current < 0 ? 0u : static_cast<std::uint32_t>(Current);
        const std::uint64_t Result = SetOperation
            ? Value
            : static_cast<std::uint64_t>(NonNegative) + Value;
        const std::uint64_t Bounded = std::min<std::uint64_t>(
            Result,
            std::min<std::uint64_t>(Maximum, static_cast<std::uint64_t>(std::numeric_limits<std::int32_t>::max())));
        return static_cast<std::int32_t>(Bounded);
    }
}
