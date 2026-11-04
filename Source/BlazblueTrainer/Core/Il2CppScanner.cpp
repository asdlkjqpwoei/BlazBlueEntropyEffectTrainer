#include "Il2CppScanner.h"
#include "Operations.h"

#include <algorithm>
#include <cstddef>
#include <cstring>
#include <limits>
#include <unordered_map>

namespace Trainer
{
	namespace
	{
		constexpr std::uintptr_t Il2CppClassSelfOffset = 0x78;

		bool IsV8MapOfType(
			const std::shared_ptr<RemoteProcess>& Process,
			std::uintptr_t TaggedMapPointer,
			std::uint64_t InstanceTypeOffset,
			std::uint16_t ExpectedInstanceType)
		{
			PuertsArrayConfig Layout;
			Layout.MapInstanceTypeOffset = InstanceTypeOffset;
			const auto InstanceType =
				ReadV8MapInstanceType(Process, TaggedMapPointer, Layout);
			return InstanceType && *InstanceType == ExpectedInstanceType;
		}

		std::optional<std::uint64_t> PuertsSlotOffset(
			const PuertsArrayConfig& Layout) noexcept
		{
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

			const std::uint64_t Offset =
				Layout.ElementsOffset +
				static_cast<std::uint64_t>(Layout.ResourceIndex) * Layout.EntryStride;
			const std::uint64_t MaximumAddress =
				std::numeric_limits<std::uintptr_t>::max();
			if (Offset > MaximumAddress - sizeof(std::uint64_t) ||
				Layout.ValueHighWordOffset > MaximumAddress - Offset)
			{
				return std::nullopt;
			}
			return Offset;
		}
	}

	std::optional<std::uint16_t> ReadV8MapInstanceType(
		const std::shared_ptr<RemoteProcess>& Process,
		std::uintptr_t TaggedMapPointer,
		const PuertsArrayConfig& Layout)
	{
		if (!Process ||
			TaggedMapPointer < 0x10000 ||
			(TaggedMapPointer & 1) == 0)
		{
			return std::nullopt;
		}

		const std::uintptr_t MaximumAddress =
			std::numeric_limits<std::uintptr_t>::max();
		const std::uintptr_t MapObject = TaggedMapPointer - 1;
		if ((MapObject & (alignof(std::uintptr_t) - 1)) != 0 ||
			Layout.MapInstanceTypeOffset >
				MaximumAddress - sizeof(std::uint16_t) ||
			MapObject > MaximumAddress -
				static_cast<std::uintptr_t>(Layout.MapInstanceTypeOffset) -
				sizeof(std::uint16_t))
		{
			return std::nullopt;
		}

		std::uintptr_t MetaMapPointer = 0;
		std::uint16_t InstanceType = 0;
		if (!Process->Read(MapObject, &MetaMapPointer, sizeof(MetaMapPointer)) ||
			MetaMapPointer < 0x10000 ||
			(MetaMapPointer & 1) == 0 ||
			((MetaMapPointer - 1) & (alignof(std::uintptr_t) - 1)) != 0 ||
			!Process->Read(
				MapObject + static_cast<std::uintptr_t>(Layout.MapInstanceTypeOffset),
				&InstanceType,
				sizeof(InstanceType)))
		{
			return std::nullopt;
		}
		return InstanceType;
	}

	std::optional<std::uint32_t> ReadPuertsArrayInteger(
		const Config& Configuration,
		const std::shared_ptr<RemoteProcess>& Process,
		const FixedPointValueHit& Hit)
	{
		if (!Process || !Configuration.PuertsArray || !Hit.IsPuertsArray() ||
			Hit.PuertsArrayObject < 0x10000 ||
			(Hit.PuertsArrayObject & (alignof(std::uintptr_t) - 1)) != 0 ||
			Hit.PuertsParentObject < 0x10000 ||
			(Hit.PuertsParentObject & (alignof(std::uintptr_t) - 1)) != 0)
		{
			return std::nullopt;
		}

		const PuertsArrayConfig& Layout = *Configuration.PuertsArray;
		const auto SlotOffset = PuertsSlotOffset(Layout);
		if (!SlotOffset ||
			Hit.PuertsValueOffset != *SlotOffset ||
			Hit.PuertsArrayObject >
				std::numeric_limits<std::uintptr_t>::max() -
					static_cast<std::uintptr_t>(*SlotOffset) -
					sizeof(std::uint64_t) ||
			Layout.ObjectLengthOffset >
				std::numeric_limits<std::uintptr_t>::max() -
					sizeof(std::uint64_t) ||
			Hit.PuertsArrayObject >
				std::numeric_limits<std::uintptr_t>::max() -
					static_cast<std::uintptr_t>(Layout.ObjectLengthOffset) -
					sizeof(std::uint64_t) ||
			Layout.ParentElementsOffset >
				std::numeric_limits<std::uintptr_t>::max() -
					sizeof(std::uintptr_t) ||
			Hit.PuertsParentObject >
				std::numeric_limits<std::uintptr_t>::max() -
					static_cast<std::uintptr_t>(Layout.ParentElementsOffset) -
					sizeof(std::uintptr_t) ||
			Layout.ParentPropertiesOffset >
				std::numeric_limits<std::uintptr_t>::max() -
					sizeof(std::uint64_t) ||
			Hit.PuertsParentObject >
				std::numeric_limits<std::uintptr_t>::max() -
					static_cast<std::uintptr_t>(Layout.ParentPropertiesOffset) -
					sizeof(std::uint64_t))
		{
			return std::nullopt;
		}

		const std::uintptr_t SlotAddress = Hit.PuertsArrayObject +
			static_cast<std::uintptr_t>(*SlotOffset);
		const std::uintptr_t HighWordAddress = SlotAddress +
			static_cast<std::uintptr_t>(Layout.ValueHighWordOffset);
		if (Hit.HighWordAddress != HighWordAddress)
		{
			return std::nullopt;
		}

		PuertsArraySnapshot Snapshot;
		Snapshot.ArrayObject = Hit.PuertsArrayObject;
		Snapshot.HighWordAddress = Hit.HighWordAddress;
		Snapshot.ValueOffset = Hit.PuertsValueOffset;
		Snapshot.ExpectedArrayMapPointer = Hit.PuertsMapPointer;
		Snapshot.ExpectedLengthRaw = Hit.PuertsLengthRaw;
		Snapshot.ParentObject = Hit.PuertsParentObject;
		Snapshot.ExpectedParentMapPointer = Hit.PuertsParentMapPointer;
		Snapshot.ExpectedParentPropertiesOrHash =
			Hit.PuertsParentPropertiesOrHash;
		if (!Process->Read(
				Hit.PuertsArrayObject,
				&Snapshot.ArrayMapPointer,
				sizeof(Snapshot.ArrayMapPointer)) ||
			!Process->Read(
				Hit.PuertsArrayObject + static_cast<std::uintptr_t>(
					Layout.ObjectLengthOffset),
				&Snapshot.LengthRaw,
				sizeof(Snapshot.LengthRaw)) ||
			!Process->Read(
				Hit.PuertsParentObject,
				&Snapshot.ParentMapPointer,
				sizeof(Snapshot.ParentMapPointer)) ||
			!Process->Read(
				Hit.PuertsParentObject + static_cast<std::uintptr_t>(
					Layout.ParentElementsOffset),
				&Snapshot.ParentArrayTag,
				sizeof(Snapshot.ParentArrayTag)) ||
			!Process->Read(
				Hit.PuertsParentObject + static_cast<std::uintptr_t>(
					Layout.ParentPropertiesOffset),
				&Snapshot.ParentPropertiesOrHash,
				sizeof(Snapshot.ParentPropertiesOrHash)) ||
			!Process->Read(SlotAddress, &Snapshot.RawValue, sizeof(Snapshot.RawValue)))
		{
			return std::nullopt;
		}

		const auto ArrayInstanceType = ReadV8MapInstanceType(
			Process,
			Snapshot.ArrayMapPointer,
			Layout);
		const auto ParentInstanceType = ReadV8MapInstanceType(
			Process,
			Snapshot.ParentMapPointer,
			Layout);
		if (!ArrayInstanceType || !ParentInstanceType)
		{
			return std::nullopt;
		}
		Snapshot.ArrayInstanceType = *ArrayInstanceType;
		Snapshot.ParentInstanceType = *ParentInstanceType;
		return ValidatePuertsArraySnapshot(Configuration, Snapshot);
	}

	Il2CppScanner::Il2CppScanner(const Config& Configuration, std::shared_ptr<RemoteProcess> Process)
		: Settings(Configuration),
		  ConnectedProcess(std::move(Process))
	{
	}

	bool Il2CppScanner::ReadClassName(std::uintptr_t ClassPointer, std::string& Name) const
	{
		Name.clear();
		if (ClassPointer < 0x10000)
		{
			return false;
		}

		std::uintptr_t ClassSelf = 0;
		if (!ConnectedProcess->Read(
				ClassPointer + Il2CppClassSelfOffset,
				&ClassSelf,
				sizeof(ClassSelf)) ||
			ClassSelf != ClassPointer)
		{
			return false;
		}

		std::uintptr_t NamePointer = 0;
		if (!ConnectedProcess->Read(
				ClassPointer + static_cast<std::uintptr_t>(Settings.TypeNameOffset),
				&NamePointer,
				sizeof(NamePointer)) ||
			NamePointer < 0x10000)
		{
			return false;
		}

		Name.reserve(48);
		for (std::size_t Index = 0; Index < 127; ++Index)
		{
			char Character = '\0';
			if (!ConnectedProcess->Read(NamePointer + Index, &Character, sizeof(Character)))
			{
				Name.clear();
				return false;
			}
			if (Character == '\0')
			{
				return !Name.empty();
			}
			Name.push_back(Character);
		}

		Name.clear();
		return false;
	}

	bool Il2CppScanner::IsObjectOfClass(
		std::uintptr_t Object,
		std::uintptr_t ClassPointer) const
	{
		std::uintptr_t ActualClass = 0;
		std::uintptr_t Monitor = 0;
		return (Object & (alignof(std::uintptr_t) - 1)) == 0 &&
			ConnectedProcess->Read(Object, &ActualClass, sizeof(ActualClass)) &&
			ActualClass == ClassPointer &&
			ConnectedProcess->Read(Object + sizeof(std::uintptr_t), &Monitor, sizeof(Monitor)) &&
			(Monitor == 0 ||
			 (Monitor >= 0x10000 && (Monitor & (alignof(std::uintptr_t) - 1)) == 0));
	}

	ClassPointerMap Il2CppScanner::ResolveClasses(std::vector<std::string>& UnresolvedTypes) const
	{
		ClassPointerMap Resolved;
		UnresolvedTypes.clear();

		for (const auto& [type_name, Descriptor] : Settings.TypeProfile)
		{
			if (Descriptor.Rva > ConnectedProcess->ModuleSize() ||
				ConnectedProcess->ModuleSize() - static_cast<std::size_t>(Descriptor.Rva) < sizeof(std::uintptr_t))
			{
				UnresolvedTypes.push_back(type_name);
				continue;
			}

			const std::uintptr_t Slot = ConnectedProcess->ModuleBase() +
				static_cast<std::uintptr_t>(Descriptor.Rva);
			std::uintptr_t ClassPointer = 0;
			std::string ActualName;

			if (ConnectedProcess->Read(Slot, &ClassPointer, sizeof(ClassPointer)) &&
				ReadClassName(ClassPointer, ActualName) &&
				ActualName == Descriptor.ClassName)
			{
				Resolved.emplace(type_name, ClassPointer);
				continue;
			}

			UnresolvedTypes.push_back(type_name);
		}

		return Resolved;
	}

	InstanceMap Il2CppScanner::ScanInstances(
		const ClassPointerMap& Classes,
		std::stop_token StopToken,
		std::uintptr_t& MemoryCursor,
		ClassPointerHitMap& ClassPointerHits,
		std::vector<FixedPointValueHit>& FixedPointHits,
		bool& FixedPointHitsTruncated,
		bool& Truncated,
		std::string& Error) const
	{
		InstanceMap Instances;
		ClassPointerHits.clear();
		FixedPointHits.clear();
		FixedPointHitsTruncated = false;
		Truncated = false;
		Error.clear();
		if (Classes.empty() && !Settings.PuertsArray)
		{
			return Instances;
		}

		std::unordered_map<std::uintptr_t, std::string> ClassNames;
		for (const auto& [Name, pointer] : Classes)
		{
			ClassNames.emplace(pointer, Name);
			Instances.emplace(Name, std::vector<std::uintptr_t>{});
		}

		const std::vector<MemoryRegion> Regions = ConnectedProcess->WritablePrivateRegions(
			Settings.Monitor.MaxScanBytes,
			MemoryCursor,
			Truncated,
			Error);
		std::vector<std::byte> Buffer(Settings.Monitor.ScanChunkBytes);
		std::vector<FixedPointValueHit> PuertsArrayHits;

		for (const MemoryRegion& Region : Regions)
		{
			if (StopToken.stop_requested())
			{
				return Instances;
			}

			std::size_t Consumed = 0;
			while (Consumed < Region.Size)
			{
				if (StopToken.stop_requested())
				{
					return Instances;
				}

				const std::size_t Request = std::min(
					Buffer.size(),
					Region.Size - Consumed);
				if (!ConnectedProcess->Read(
						Region.Base + Consumed,
						Buffer.data(),
						Request))
				{
					Consumed += Request;
					continue;
				}

				for (std::size_t Offset = 0;
					 Offset + 2 * sizeof(std::uint32_t) <= Request;
					 Offset += sizeof(std::uint32_t))
				{
					std::uint32_t Fractional = 0;
					std::uint32_t Integer = 0;
					std::memcpy(&Fractional, Buffer.data() + Offset, sizeof(Fractional));
					if (Fractional != 0)
					{
						continue;
					}
					std::memcpy(
						&Integer,
						Buffer.data() + Offset + sizeof(Fractional),
						sizeof(Integer));
					if (!Settings.RefreshScanSeed ||
						Integer != *Settings.RefreshScanSeed)
					{
						continue;
					}
					if (FixedPointHits.size() >=
						Settings.Monitor.MaxDynamicRefreshCandidates)
					{
						FixedPointHitsTruncated = true;
						break;
					}
					FixedPointHits.push_back({
						Region.Base + Consumed + Offset + sizeof(Fractional),
						Integer
					});
				}

				const std::size_t Usable = Request & ~std::size_t{ 7 };
				for (std::size_t Offset = 0; Offset + sizeof(std::uintptr_t) <= Usable; Offset += sizeof(std::uintptr_t))
				{
					std::uintptr_t Value = 0;
					std::memcpy(&Value, Buffer.data() + Offset, sizeof(Value));
					const auto Found = ClassNames.find(Value);
					if (Found == ClassNames.end())
					{
						continue;
					}

					++ClassPointerHits[Found->second];
					auto& Objects = Instances[Found->second];
					if (Objects.size() >= Settings.Monitor.MaxObjectsPerType)
					{
						continue;
					}

					const std::uintptr_t Candidate = Region.Base + Consumed + Offset;
					if (IsObjectOfClass(Candidate, Value))
					{
						Objects.push_back(Candidate);
					}
				}

				if (Settings.PuertsArray)
				{
					const PuertsArrayConfig& Layout = *Settings.PuertsArray;
					const std::uint64_t ExpectedLengthRaw =
						static_cast<std::uint64_t>(Layout.Length) << 32;
					const std::uint64_t ValueOffset =
						Layout.ElementsOffset +
						static_cast<std::uint64_t>(Layout.ResourceIndex) * Layout.EntryStride;
					for (std::size_t Offset = 0;
						 Offset + Layout.ObjectLengthOffset + sizeof(std::uint64_t) <= Usable;
						 Offset += alignof(std::uintptr_t))
					{
						std::uint64_t LengthRaw = 0;
						std::memcpy(
							&LengthRaw,
							Buffer.data() + Offset + Layout.ObjectLengthOffset,
							sizeof(LengthRaw));
						if (LengthRaw != ExpectedLengthRaw)
						{
							continue;
						}

						std::uintptr_t MapPointer = 0;
						std::memcpy(
							&MapPointer,
							Buffer.data() + Offset,
							sizeof(MapPointer));
						if (!IsV8MapOfType(
								ConnectedProcess,
								MapPointer,
								Layout.MapInstanceTypeOffset,
								Layout.FixedArrayInstanceType))
						{
							continue;
						}

						const std::uintptr_t ArrayObject =
							Region.Base + Consumed + Offset;
						const std::uintptr_t SlotAddress =
							ArrayObject + static_cast<std::uintptr_t>(ValueOffset);
						std::uint64_t RawValue = 0;
						if (!ConnectedProcess->Read(SlotAddress, &RawValue, sizeof(RawValue)))
						{
							continue;
						}
						const std::optional<std::uint32_t> Integer = FixedPointInteger(
							RawValue,
							Settings.Monitor.MaxRefreshValue);
						if (!Integer)
						{
							continue;
						}
						if (FixedPointHits.size() + PuertsArrayHits.size() >=
							Settings.Monitor.MaxDynamicRefreshCandidates)
						{
							FixedPointHitsTruncated = true;
							break;
						}

						PuertsArrayHits.push_back({
							SlotAddress + Layout.ValueHighWordOffset,
							*Integer,
							ArrayObject,
							MapPointer,
							Region.AllocationBase,
							0,
							0,
							0,
							0,
							LengthRaw,
							ValueOffset
						});
					}
				}

				Consumed += Request;
			}
		}

		if (Settings.PuertsArray && !PuertsArrayHits.empty())
		{
			std::unordered_map<std::uintptr_t, std::vector<std::size_t>> HitsByAllocation;
			for (std::size_t Index = 0; Index < PuertsArrayHits.size(); ++Index)
			{
				if (PuertsArrayHits[Index].PuertsAllocationBase != 0)
				{
					HitsByAllocation[
						PuertsArrayHits[Index].PuertsAllocationBase].push_back(Index);
				}
			}

			const PuertsArrayConfig& Layout = *Settings.PuertsArray;
			constexpr std::uint64_t ParentScanLimitBytes = 16ull * 1024ull * 1024ull;
			for (const auto& [AllocationBase, hit_indices] : HitsByAllocation)
			{
				if (StopToken.stop_requested())
				{
					return Instances;
				}

				std::unordered_map<std::uintptr_t, std::vector<std::size_t>> HitTags;
				for (const std::size_t Index : hit_indices)
				{
					HitTags[
						PuertsArrayHits[Index].PuertsArrayObject + 1].push_back(Index);
				}

				bool AllocationTruncated = false;
				std::string AllocationError;
				const std::vector<MemoryRegion> AllocationRegions =
					ConnectedProcess->WritablePrivateAllocationRegions(
						AllocationBase,
						ParentScanLimitBytes,
						AllocationTruncated,
						AllocationError);
				if (!AllocationError.empty())
				{
					if (Error.empty())
					{
						Error = "Puerts parent validation failed: " + AllocationError;
					}
					continue;
				}
				if (AllocationTruncated)
				{
					if (Error.empty())
					{
						Error = "Puerts array allocation exceeded the parent-validation scan limit.";
					}
					continue;
				}

				std::vector<std::size_t> ParentMatches(PuertsArrayHits.size(), 0);
				for (const MemoryRegion& Region : AllocationRegions)
				{
					if (Region.AllocationBase != AllocationBase)
					{
						continue;
					}

					std::size_t Consumed = 0;
					while (Consumed + sizeof(std::uintptr_t) <= Region.Size)
					{
						if (StopToken.stop_requested())
						{
							return Instances;
						}

						const std::size_t Request = std::min(
							Buffer.size(),
							Region.Size - Consumed);
						if (!ConnectedProcess->Read(
								Region.Base + Consumed,
								Buffer.data(),
								Request))
						{
							Consumed += Request;
							continue;
						}

						const std::size_t Usable = Request & ~std::size_t{ 7 };
						for (std::size_t Offset = 0;
							 Offset + sizeof(std::uintptr_t) <= Usable;
							 Offset += sizeof(std::uintptr_t))
						{
							std::uintptr_t TaggedArray = 0;
							std::memcpy(
								&TaggedArray,
								Buffer.data() + Offset,
								sizeof(TaggedArray));
							const auto Found = HitTags.find(TaggedArray);
							if (Found == HitTags.end())
							{
								continue;
							}

							const std::uintptr_t SlotAddress =
								Region.Base + Consumed + Offset;
							if (SlotAddress < Layout.ParentElementsOffset)
							{
								continue;
							}
							const std::uintptr_t ParentObject =
								SlotAddress - static_cast<std::uintptr_t>(
									Layout.ParentElementsOffset);
							std::uintptr_t ParentMapPointer = 0;
							if (!ConnectedProcess->Read(
									ParentObject,
									&ParentMapPointer,
									sizeof(ParentMapPointer)) ||
								!IsV8MapOfType(
									ConnectedProcess,
									ParentMapPointer,
									Layout.MapInstanceTypeOffset,
									Layout.ParentInstanceType))
							{
								continue;
							}
							std::uint64_t ParentPropertiesOrHash = 0;
							if (!ConnectedProcess->Read(
									ParentObject + static_cast<std::uintptr_t>(
										Layout.ParentPropertiesOffset),
									&ParentPropertiesOrHash,
									sizeof(ParentPropertiesOrHash)))
							{
								continue;
							}

							for (const std::size_t Index : Found->second)
							{
								++ParentMatches[Index];
								if (ParentMatches[Index] == 1)
								{
									PuertsArrayHits[Index].PuertsParentObject =
										ParentObject;
									PuertsArrayHits[Index].PuertsParentMapPointer =
										ParentMapPointer;
									PuertsArrayHits[Index].PuertsParentPropertiesOrHash =
										ParentPropertiesOrHash;
								}
							}
						}
						Consumed += Request;
					}
				}

				std::unordered_map<std::uintptr_t, std::vector<std::size_t>>
					HitsByParentTag;
				std::unordered_map<std::uintptr_t, std::uint32_t>
					ParentReferenceCounts;
				for (const std::size_t Index : hit_indices)
				{
					if (ParentMatches[Index] != 1)
					{
						continue;
					}
					const std::uintptr_t ParentTag =
						PuertsArrayHits[Index].PuertsParentObject + 1;
					HitsByParentTag[ParentTag].push_back(Index);
					ParentReferenceCounts.try_emplace(ParentTag, 0);
				}

				for (const MemoryRegion& Region : AllocationRegions)
				{
					if (Region.AllocationBase != AllocationBase)
					{
						continue;
					}

					std::size_t Consumed = 0;
					while (Consumed < Region.Size)
					{
						if (StopToken.stop_requested())
						{
							return Instances;
						}

						const std::size_t Request = std::min(
							Buffer.size(),
							Region.Size - Consumed);
						if (!ConnectedProcess->Read(
								Region.Base + Consumed,
								Buffer.data(),
								Request))
						{
							Consumed += Request;
							continue;
						}

						const std::size_t Usable = Request & ~std::size_t{ 7 };
						for (std::size_t Offset = 0;
							 Offset + sizeof(std::uintptr_t) <= Usable;
							 Offset += sizeof(std::uintptr_t))
						{
							std::uintptr_t TaggedParent = 0;
							std::memcpy(
								&TaggedParent,
								Buffer.data() + Offset,
								sizeof(TaggedParent));
							const auto Found = HitsByParentTag.find(TaggedParent);
							if (Found == HitsByParentTag.end())
							{
								continue;
							}
							std::uint32_t& Count =
								ParentReferenceCounts[TaggedParent];
							if (Count != std::numeric_limits<std::uint32_t>::max())
							{
								++Count;
							}
							for (const std::size_t Index : Found->second)
							{
								PuertsArrayHits[Index]
									.PuertsParentAllocationReferenceCount = Count;
							}
						}
						Consumed += Request;
					}
				}

				for (const std::size_t Index : hit_indices)
				{
					FixedPointValueHit& Hit = PuertsArrayHits[Index];
					if (ParentMatches[Index] != 1)
					{
						Hit.PuertsParentObject = 0;
						Hit.PuertsParentMapPointer = 0;
						continue;
					}
					const std::uintptr_t ParentTag = Hit.PuertsParentObject + 1;
					Hit.PuertsParentAllocationReferenceCount =
						ParentReferenceCounts[ParentTag];

					if (FixedPointHits.size() >=
						Settings.Monitor.MaxDynamicRefreshCandidates)
					{
						FixedPointHitsTruncated = true;
						break;
					}
					FixedPointHits.push_back(Hit);
				}

			}
		}

		return Instances;
	}
}
