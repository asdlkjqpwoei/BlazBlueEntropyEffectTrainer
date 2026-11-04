#pragma once

#include "Config.h"

#include <cstdint>
#include <optional>
#include <vector>

namespace Trainer
{
	struct PuertsArraySnapshot
	{
		std::uintptr_t ArrayObject{};
		std::uintptr_t HighWordAddress{};
		std::uint64_t ValueOffset{};
		std::uintptr_t ExpectedArrayMapPointer{};
		std::uintptr_t ArrayMapPointer{};
		std::uint16_t ArrayInstanceType{};
		std::uint64_t ExpectedLengthRaw{};
		std::uint64_t LengthRaw{};
		std::uintptr_t ParentObject{};
		std::uintptr_t ExpectedParentMapPointer{};
		std::uintptr_t ParentMapPointer{};
		std::uint16_t ParentInstanceType{};
		std::uintptr_t ParentArrayTag{};
		std::uint64_t ExpectedParentPropertiesOrHash{};
		std::uint64_t ParentPropertiesOrHash{};
		std::uint64_t RawValue{};
	};

	struct PuertsCandidateReferenceCount
	{
		std::uintptr_t Address{};
		std::uint32_t AllocationReferenceCount{};
		bool ParentPropertiesIsSmi{};
	};

	struct RefreshValueTracker
	{
		std::uint32_t LastValue{};
		std::uint32_t DecrementEvidence{};
		bool Calibrated{};
		bool Initialized{};
	};

	std::optional<std::uint32_t> FixedPointInteger(
		std::uint64_t RawValue,
		std::uint32_t Maximum) noexcept;

	std::optional<std::uint32_t> ValidatePuertsArraySnapshot(
		const Config& Configuration,
		const PuertsArraySnapshot& Snapshot) noexcept;

	std::optional<std::uintptr_t> SelectUniquePuertsRefreshCandidate(
		const std::vector<PuertsCandidateReferenceCount>& Candidates) noexcept;

	bool ObserveRefreshValue(
		RefreshValueTracker& Tracker,
		std::uint32_t Current,
		std::uint32_t Maximum) noexcept;

	std::uint32_t ApplyRefreshOperation(
		std::uint32_t Current,
		std::uint32_t Value,
		std::uint32_t Maximum,
		RefreshSemantics Semantics,
		bool SetOperation);

	std::int32_t ApplyResourceOperation(
		std::int32_t Current,
		std::uint32_t Value,
		std::uint32_t Maximum,
		bool SetOperation);
}
