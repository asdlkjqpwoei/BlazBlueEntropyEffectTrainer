#pragma once

#include "Config.h"
#include "Operations.h"
#include "RemoteProcess.h"

#include <Windows.h>

#include <cstdint>
#include <memory>
#include <optional>
#include <stop_token>
#include <string>
#include <unordered_map>
#include <vector>

namespace Trainer
{
	using ClassPointerMap = std::unordered_map<std::string, std::uintptr_t>;
	using InstanceMap = std::unordered_map<std::string, std::vector<std::uintptr_t>>;
	using ClassPointerHitMap = std::unordered_map<std::string, std::size_t>;

	struct FixedPointValueHit
	{
		std::uintptr_t HighWordAddress{};
		std::uint32_t IntegerValue{};
		std::uintptr_t PuertsArrayObject{};
		std::uintptr_t PuertsMapPointer{};
		std::uintptr_t PuertsAllocationBase{};
		std::uintptr_t PuertsParentObject{};
		std::uintptr_t PuertsParentMapPointer{};
		std::uint64_t PuertsParentPropertiesOrHash{};
		std::uint32_t PuertsParentAllocationReferenceCount{};
		std::uint64_t PuertsLengthRaw{};
		std::uint64_t PuertsValueOffset{};

		bool IsPuertsArray() const noexcept
		{
			return PuertsArrayObject != 0 &&
				PuertsParentObject != 0 &&
				PuertsParentMapPointer != 0;
		}
	};

	std::optional<std::uint16_t> ReadV8MapInstanceType(
		const std::shared_ptr<RemoteProcess>& Process,
		std::uintptr_t TaggedMapPointer,
		const PuertsArrayConfig& Layout);

	std::optional<std::uint32_t> ReadPuertsArrayInteger(
		const Config& Configuration,
		const std::shared_ptr<RemoteProcess>& Process,
		const FixedPointValueHit& Hit);

	class Il2CppScanner final
	{
	public:
		Il2CppScanner(const Config& Configuration, std::shared_ptr<RemoteProcess> Process);

		ClassPointerMap ResolveClasses(std::vector<std::string>& UnresolvedTypes) const;
		InstanceMap ScanInstances(
			const ClassPointerMap& Classes,
			std::stop_token StopToken,
			std::uintptr_t& MemoryCursor,
			ClassPointerHitMap& ClassPointerHits,
			std::vector<FixedPointValueHit>& FixedPointHits,
			bool& FixedPointHitsTruncated,
			bool& Truncated,
			std::string& Error) const;

		bool IsObjectOfClass(std::uintptr_t Object, std::uintptr_t ClassPointer) const;
		bool ReadClassName(std::uintptr_t ClassPointer, std::string& Name) const;

	private:
		const Config& Settings;
		std::shared_ptr<RemoteProcess> ConnectedProcess;
	};
}
