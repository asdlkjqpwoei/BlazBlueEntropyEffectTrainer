#include "Config.h"
#include "Paths.h"
#include "Il2CppScanner.h"
#include "RemoteProcess.h"

#include <array>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <stop_token>
#include <unordered_map>
#include <string_view>
#include <unordered_set>
#include <vector>

namespace
{
    bool ReadPointer(
        const std::shared_ptr<Trainer::RemoteProcess>& Process,
        std::uintptr_t Address,
        std::uintptr_t& Value)
    {
        return Process->Read(Address, &Value, sizeof(Value)) &&
            Value >= 0x10000 &&
            (Value & (alignof(std::uintptr_t) - 1)) == 0;
    }

    bool ReadCount(
        const Trainer::ResourceTargetConfig& Target,
        const std::shared_ptr<Trainer::RemoteProcess>& Process,
        std::uintptr_t Address,
        std::int64_t& Value)
    {
        if (Target.SignedCount)
        {
            std::int32_t Count = 0;
            if (!Process->Read(Address, &Count, sizeof(Count)))
            {
                return false;
            }
            Value = Count;
            return true;
        }

        std::uint32_t Count = 0;
        if (!Process->Read(Address, &Count, sizeof(Count)))
        {
            return false;
        }
        Value = Count;
        return true;
    }

    bool ReadDiscriminator(
        const Trainer::ResourceTargetConfig& Target,
        const std::shared_ptr<Trainer::RemoteProcess>& Process,
        std::uintptr_t Object,
        std::uint32_t& Value)
    {
        return Target.DiscriminatorOffset &&
            Process->Read(
                Object + static_cast<std::uintptr_t>(*Target.DiscriminatorOffset),
                &Value,
                sizeof(Value));
    }

    void DumpObjectFields(
        std::string_view Type,
        const Trainer::Il2CppScanner& Scanner,
        const std::shared_ptr<Trainer::RemoteProcess>& Process,
        std::uintptr_t Object,
        std::uint32_t TargetValue,
        std::size_t& SampleCount)
    {
        std::array<std::byte, 0x80> Fields{};
        if (!Process->Read(Object + 0x8, Fields.data(), Fields.size()))
        {
            return;
        }

        for (std::uintptr_t Offset = 0x8; Offset <= 0x80; Offset += sizeof(std::uint32_t))
        {
            std::uint32_t Value = 0;
            std::memcpy(
                &Value,
                Fields.data() + static_cast<std::ptrdiff_t>(Offset - 0x8),
                sizeof(Value));
            if (Value == TargetValue)
            {
                std::cout << "field-match type=" << Type
                    << " object=0x" << std::hex << Object
                    << " offset=0x" << Offset << std::dec
                    << " value=" << Value << '\n' << std::flush;
            }
        }

        if (SampleCount >= 3)
        {
            return;
        }
        ++SampleCount;

        std::cout << "layout-sample type=" << Type
            << " object=0x" << std::hex << Object;
        for (std::uintptr_t Offset = 0x10; Offset <= 0x50; Offset += sizeof(std::uintptr_t))
        {
            std::uintptr_t Value = 0;
            std::memcpy(
                &Value,
                Fields.data() + static_cast<std::ptrdiff_t>(Offset - 0x8),
                sizeof(Value));
            std::cout << " +" << Offset << "=0x" << Value;
            std::uintptr_t PointedClass = 0;
            std::string PointedType;
            if (Value >= 0x10000 &&
                (Value & (alignof(std::uintptr_t) - 1)) == 0 &&
                Process->Read(Value, &PointedClass, sizeof(PointedClass)) &&
                Scanner.ReadClassName(PointedClass, PointedType))
            {
                std::cout << '<' << PointedType << '>';
            }
        }
        std::cout << std::dec << '\n' << std::flush;
    }

    void InspectTypedChildren(
        const Trainer::Il2CppScanner& Scanner,
        const std::shared_ptr<Trainer::RemoteProcess>& Process,
        std::uintptr_t Object,
        std::uint32_t TargetValue,
        std::unordered_set<std::uintptr_t>& InspectedObjects,
        std::unordered_map<std::string, std::size_t>& SampleCounts)
    {
        std::array<std::byte, 0x80> Fields{};
        if (!Process->Read(Object + 0x8, Fields.data(), Fields.size()))
        {
            return;
        }

        for (std::uintptr_t Offset = 0x10; Offset <= 0x80; Offset += sizeof(std::uintptr_t))
        {
            std::uintptr_t Child = 0;
            std::memcpy(
                &Child,
                Fields.data() + static_cast<std::ptrdiff_t>(Offset - 0x8),
                sizeof(Child));
            if (Child < 0x10000 ||
                (Child & (alignof(std::uintptr_t) - 1)) != 0 ||
                !InspectedObjects.insert(Child).second)
            {
                continue;
            }

            std::uintptr_t ChildClass = 0;
            std::string ChildType;
            if (Process->Read(Child, &ChildClass, sizeof(ChildClass)) &&
                Scanner.ReadClassName(ChildClass, ChildType))
            {
                DumpObjectFields(
                    ChildType,
                    Scanner,
                    Process,
                    Child,
                    TargetValue,
                    SampleCounts[ChildType]);
            }
        }
    }

    void ReportMatch(
        std::uintptr_t Object,
        std::uintptr_t Root,
        std::int64_t Value,
        std::uint32_t TargetValue,
        std::unordered_set<std::uintptr_t>& Reported)
    {
        if (Value != static_cast<std::int64_t>(TargetValue) ||
            !Reported.insert(Object).second)
        {
            return;
        }

        std::cout << "match object=0x" << std::hex << Object;
        if (Root != 0)
        {
            std::cout << " root=0x" << Root;
        }
        std::cout << std::dec << " value=" << Value
            << (Root != 0 ? " source=rooted-resource\n" : " source=typed-instance-only\n")
            << std::flush;
    }

    void InspectResource(
        const Trainer::ResourceTargetConfig& Target,
        const std::shared_ptr<Trainer::RemoteProcess>& Process,
        std::uintptr_t Object,
        std::uintptr_t Root,
        std::uint32_t TargetValue,
        std::unordered_set<std::uintptr_t>& Reported,
        std::size_t& EligibleResources)
    {
        std::uint32_t Discriminator = 0;
        std::int64_t Value = 0;
        if (!ReadDiscriminator(Target, Process, Object, Discriminator) ||
            !Target.DiscriminatorValue ||
            Discriminator != *Target.DiscriminatorValue ||
            !ReadCount(
                Target,
                Process,
                Object + static_cast<std::uintptr_t>(Target.CountOffset),
                Value))
        {
            return;
        }

        ++EligibleResources;
        ReportMatch(Object, Root, Value, TargetValue, Reported);
    }

    void InspectRoot(
        const Trainer::ResourceTargetConfig& Target,
        const Trainer::Il2CppScanner& Scanner,
        const std::shared_ptr<Trainer::RemoteProcess>& Process,
        std::uintptr_t Root,
        std::uintptr_t ResourceClass,
        std::uint32_t TargetValue,
        std::unordered_set<std::uintptr_t>& Reported,
        std::size_t& EligibleResources,
        std::size_t& RootCollectionPointers,
        std::size_t& ValidRootArrays,
        std::size_t& RootResourceElements,
        bool DumpLayout)
    {
        if (!Target.RootCollectionOffset || !Target.DiscriminatorValue)
        {
            return;
        }

        std::uintptr_t RepeatedField = 0;
        std::uintptr_t Array = 0;
        std::uint32_t Count = 0;
        std::uint64_t Length = 0;
        if (!ReadPointer(
                Process,
                Root + static_cast<std::uintptr_t>(*Target.RootCollectionOffset),
                RepeatedField))
        {
            return;
        }
        ++RootCollectionPointers;
        const bool ArrayPointerValid = ReadPointer(
            Process,
            RepeatedField + static_cast<std::uintptr_t>(Target.RepeatedArrayOffset),
            Array);
        const bool CountRead = Process->Read(
            RepeatedField + static_cast<std::uintptr_t>(Target.RepeatedCountOffset),
            &Count,
            sizeof(Count));
        const bool LengthRead = ArrayPointerValid &&
            Process->Read(
                Array + static_cast<std::uintptr_t>(Target.ArrayLengthOffset),
                &Length,
                sizeof(Length));
        if (DumpLayout)
        {
            std::uintptr_t CollectionClass = 0;
            std::uintptr_t ArrayClass = 0;
            std::string CollectionType;
            std::string ArrayType;
            const bool CollectionTypeRead =
                Process->Read(
                    RepeatedField,
                    &CollectionClass,
                    sizeof(CollectionClass)) &&
                Scanner.ReadClassName(CollectionClass, CollectionType);
            const bool ArrayTypeRead = ArrayPointerValid &&
                Process->Read(Array, &ArrayClass, sizeof(ArrayClass)) &&
                Scanner.ReadClassName(ArrayClass, ArrayType);
            std::cout << "root-layout root=0x" << std::hex << Root
                << " collection=0x" << RepeatedField
                << " array=0x" << Array
                << std::dec
                << " collection_type="
                << (CollectionTypeRead ? CollectionType : "unresolved")
                << " array_type="
                << (ArrayTypeRead ? ArrayType : "unresolved")
                << " count=" << Count
                << " length=" << Length
                << " array_pointer_valid=" << ArrayPointerValid
                << " count_read=" << CountRead
                << " length_read=" << LengthRead << '\n' << std::flush;
        }
        if (!ArrayPointerValid ||
            !CountRead ||
            !LengthRead ||
            Count == 0 ||
            Count > 2048 ||
            Length < Count ||
            Length > 4096)
        {
            return;
        }
        ++ValidRootArrays;

        for (std::uint32_t Index = 0; Index < Count; ++Index)
        {
            const std::uintptr_t ElementAddress = Array +
                static_cast<std::uintptr_t>(Target.ArrayDataOffset) +
                static_cast<std::uintptr_t>(Index) * sizeof(std::uintptr_t);
            std::uintptr_t Object = 0;
            std::uintptr_t ActualClass = 0;
            if (!ReadPointer(Process, ElementAddress, Object) ||
                !Process->Read(Object, &ActualClass, sizeof(ActualClass)) ||
                ActualClass != ResourceClass)
            {
                continue;
            }

            ++RootResourceElements;
            InspectResource(
                Target,
                Process,
                Object,
                Root,
                TargetValue,
                Reported,
                EligibleResources);
        }
    }
}

int RunExchangeProbe(int ArgumentCount, char** Arguments)
{
    if (ArgumentCount != 3 || std::string_view(Arguments[1]) != "--target-value")
    {
        std::cerr << "Usage: exchange_value_probe --target-value <current-count>\n";
        return 1;
    }

    std::uint32_t TargetValue = 0;
    const std::string_view TargetArgument(Arguments[2]);
    const auto [end, parse_error] = std::from_chars(
        TargetArgument.data(),
        TargetArgument.data() + TargetArgument.size(),
        TargetValue);
    if (parse_error != std::errc{} ||
        end != TargetArgument.data() + TargetArgument.size())
    {
        std::cerr << "The target count must be an unsigned 32-bit integer.\n";
        return 1;
    }

    try
    {
        const Trainer::Config Configuration = Trainer::LoadConfig(
            Trainer::DefaultConfigPath());
        if (Configuration.PuertsRuntime)
        {
            std::cerr << "This diagnostic requires a legacy IL2CPP exchange profile. Use puerts_runtime_probe for the configured live-model profile.\n";
            return 2;
        }
        const Trainer::AttachResult Attach =
            Trainer::RemoteProcess::FindCompatibleGameProcess(Configuration);
        if (Attach.Status != Trainer::AttachStatus::Attached || !Attach.Process)
        {
            std::cerr << "Could not attach to a compatible game process: "
                << Attach.Message << '\n';
            return 2;
        }

        Trainer::Il2CppScanner Scanner(Configuration, Attach.Process);
        std::vector<std::string> UnresolvedTypes;
        const Trainer::ClassPointerMap Classes = Scanner.ResolveClasses(UnresolvedTypes);
        const auto ResourceClass = Classes.find(Configuration.Exchange.Type);
        const auto RootClass = Configuration.Exchange.RootType
            ? Classes.find(*Configuration.Exchange.RootType)
            : Classes.end();
        if (ResourceClass == Classes.end() ||
            !Configuration.Exchange.RootType ||
            RootClass == Classes.end())
        {
            std::cerr << "Could not resolve the configured exchange resource/root types.\n";
            return 2;
        }

        std::cout << "Read-only exchange scan: pid=" << Attach.Process->ProcessId()
            << " target=" << TargetValue
            << " resource_type=" << Configuration.Exchange.Type
            << " root_type=" << *Configuration.Exchange.RootType << '\n';

        std::uintptr_t MemoryCursor = 0;
        Trainer::ClassPointerHitMap ClassPointerHits;
        std::vector<Trainer::FixedPointValueHit> FixedPointHits;
        std::unordered_set<std::uintptr_t> Reported;
        std::unordered_set<std::uintptr_t> InspectedRoots;
        std::size_t EligibleResources = 0;
        std::size_t RootCollectionPointers = 0;
        std::size_t ValidRootArrays = 0;
        std::size_t RootResourceElements = 0;
        std::size_t ResourceLayoutSamples = 0;
        std::size_t RootLayoutSamples = 0;
        std::unordered_set<std::uintptr_t> InspectedNestedObjects;
        std::unordered_map<std::string, std::size_t> NestedLayoutSamples;
        std::size_t Pass = 0;

        do
        {
            bool FixedPointHitsTruncated = false;
            bool Truncated = false;
            std::string ScanError;
            Trainer::InstanceMap Instances = Scanner.ScanInstances(
                Classes,
                std::stop_token{},
                MemoryCursor,
                ClassPointerHits,
                FixedPointHits,
                FixedPointHitsTruncated,
                Truncated,
                ScanError);
            if (!ScanError.empty())
            {
                std::cerr << "Memory scan failed: " << ScanError << '\n';
                return 2;
            }

            ++Pass;
            const auto Resources = Instances.find(Configuration.Exchange.Type);
            if (Resources != Instances.end())
            {
                for (const std::uintptr_t Object : Resources->second)
                {
                    DumpObjectFields(
                        Configuration.Exchange.Type,
                        Scanner,
                        Attach.Process,
                        Object,
                        TargetValue,
                        ResourceLayoutSamples);
                    InspectTypedChildren(
                        Scanner,
                        Attach.Process,
                        Object,
                        TargetValue,
                        InspectedNestedObjects,
                        NestedLayoutSamples);
                    InspectResource(
                        Configuration.Exchange,
                        Attach.Process,
                        Object,
                        0,
                        TargetValue,
                        Reported,
                        EligibleResources);
                }
            }

            const auto Roots = Instances.find(*Configuration.Exchange.RootType);
            if (Roots != Instances.end())
            {
                for (const std::uintptr_t Root : Roots->second)
                {
                    const bool DumpRootLayout = RootLayoutSamples < 3;
                    DumpObjectFields(
                        *Configuration.Exchange.RootType,
                        Scanner,
                        Attach.Process,
                        Root,
                        TargetValue,
                        RootLayoutSamples);
                    if (!InspectedRoots.insert(Root).second ||
                        !Scanner.IsObjectOfClass(Root, RootClass->second))
                    {
                        continue;
                    }
                    InspectRoot(
                        Configuration.Exchange,
                        Scanner,
                        Attach.Process,
                        Root,
                        ResourceClass->second,
                        TargetValue,
                        Reported,
                        EligibleResources,
                        RootCollectionPointers,
                        ValidRootArrays,
                        RootResourceElements,
                        DumpRootLayout);
                }
            }

            std::cout << "pass=" << Pass
                << " cursor=0x" << std::hex << MemoryCursor << std::dec
                << " resource_instances="
                << (Resources == Instances.end() ? 0 : Resources->second.size())
                << " roots="
                << (Roots == Instances.end() ? 0 : Roots->second.size())
                << " scan_truncated=" << Truncated
                << " target_matches=" << Reported.size() << '\n' << std::flush;
        } while (MemoryCursor != 0);

        std::cout << "scan_complete=1 eligible_exchange_resources="
            << EligibleResources
            << " root_collection_pointers=" << RootCollectionPointers
            << " valid_root_arrays=" << ValidRootArrays
            << " root_resource_elements=" << RootResourceElements
            << " target_matches=" << Reported.size() << '\n';
        return Reported.empty() ? 3 : 0;
    }
    catch (const std::exception& Exception)
    {
        std::cerr << "Exchange value probe failed: " << Exception.what() << '\n';
        return 2;
    }
}
