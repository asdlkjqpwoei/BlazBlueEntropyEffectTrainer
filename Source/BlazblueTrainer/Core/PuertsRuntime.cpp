#include "PuertsRuntime.h"

#include "Operations.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <limits>
#include <unordered_set>
#include <utility>

namespace Trainer
{
    namespace
    {
        constexpr std::uintptr_t MaximumUserAddress = 0x00007fffffffffffull;
        constexpr std::uint16_t MapType = 0xac;
        constexpr std::uint16_t JsObjectType = 0x421;
        constexpr std::uint16_t JsFunctionType = 0x80c;
        constexpr std::uint16_t PropertyArrayType = 0xb0;
        constexpr std::uint16_t SharedFunctionInfoType = 0xb3;
        constexpr std::uint16_t ScriptInstanceType = 0x70;
        constexpr std::uint16_t OddballType = 0x43;
        constexpr std::uint32_t MaximumDescriptors = 1023;

        bool IsTag(std::uint64_t Tag) noexcept
        {
            return Tag >= 0x10001 && Tag <= MaximumUserAddress && (Tag & 7) == 1;
        }
    }

    struct PuertsRuntimeReader::Map
    {
        std::uint16_t InstanceType{};
        std::uint8_t SizeWords{};
        std::uint8_t InObjectStartWords{};
        std::uint32_t OwnDescriptors{};
        std::uintptr_t Prototype{};
        std::uintptr_t Descriptors{};
    };

    struct PuertsRuntimeReader::Property
    {
        std::uintptr_t Address{};
        std::uintptr_t StorageObject{};
        std::uint64_t Value{};
        std::uint8_t Representation{};
        bool Accessor{};
        bool WritableField{};
    };

    PuertsRuntimeReader::PuertsRuntimeReader(const Config& Configuration, PuertsReadMemory Read)
        : Settings(Configuration), ReadMemoryCallback(std::move(Read))
    {
    }

    bool PuertsRuntimeReader::ReadRaw(
        std::uintptr_t Address,
        void* Output,
        std::size_t Size) const
    {
        return Address >= 0x10000 && Size != 0 &&
            Size <= MaximumUserAddress &&
            Address <= MaximumUserAddress - Size &&
            ReadMemoryCallback(Address, Output, Size);
    }

    std::optional<std::uint64_t> PuertsRuntimeReader::ReadGuarded(
        std::uintptr_t Address,
        std::uint8_t Width,
        std::vector<PuertsMemoryGuard>* Guards) const
    {
        if (Width != 1 && Width != 2 && Width != 4 && Width != 8)
        {
            return std::nullopt;
        }
        std::uint64_t Value = 0;
        if (!ReadRaw(Address, &Value, Width))
        {
            return std::nullopt;
        }
        if (Guards)
        {
            Guards->push_back({ Address, Value, Width });
        }
        return Value;
    }

    std::optional<PuertsRuntimeReader::Map> PuertsRuntimeReader::ReadMap(
        std::uintptr_t TaggedMap,
        std::vector<PuertsMemoryGuard>* Guards) const
    {
        if (!IsTag(TaggedMap))
        {
            return std::nullopt;
        }
        const std::uintptr_t Object = TaggedMap - 1;
        const auto Meta = ReadGuarded(Object, 8, Guards);
        if (!Meta || !IsTag(*Meta))
        {
            return std::nullopt;
        }
        const auto Self = ReadGuarded(*Meta - 1, 8, Guards);
        const auto MetaType = ReadGuarded(*Meta - 1 + 0xc, 2, Guards);
        if (!Self || *Self != *Meta || !MetaType || *MetaType != MapType)
        {
            return std::nullopt;
        }
        const auto Layout = ReadGuarded(Object + 8, 8, Guards);
        const auto Bits = ReadGuarded(Object + 0x10, 4, Guards);
        const auto Prototype = ReadGuarded(Object + 0x18, 8, Guards);
        const auto Descriptors = ReadGuarded(Object + 0x28, 8, Guards);
        if (!Layout || !Bits || !Prototype || !Descriptors)
        {
            return std::nullopt;
        }
        Map Result;
        Result.SizeWords = static_cast<std::uint8_t>(*Layout);
        Result.InObjectStartWords = static_cast<std::uint8_t>(*Layout >> 8);
        Result.InstanceType = static_cast<std::uint16_t>(*Layout >> 32);
        Result.OwnDescriptors = static_cast<std::uint32_t>((*Bits >> 10) & 0x3ff);
        Result.Prototype = static_cast<std::uintptr_t>(*Prototype);
        Result.Descriptors = static_cast<std::uintptr_t>(*Descriptors);
        return Result;
    }

    std::optional<std::string> PuertsRuntimeReader::ReadString(
        std::uintptr_t TaggedString,
        unsigned Depth) const
    {
        if (!IsTag(TaggedString) || Depth > 3)
        {
            return std::nullopt;
        }
        const auto MapTag = ReadGuarded(TaggedString - 1, 8, nullptr);
        const auto MapInfo = MapTag ? ReadMap(*MapTag, nullptr) : std::nullopt;
        const auto Length = ReadGuarded(TaggedString - 1 + 0xc, 4, nullptr);
        if (!MapInfo || MapInfo->InstanceType >= 0x40 || !Length || *Length > 256)
        {
            return std::nullopt;
        }
        const auto Representation = MapInfo->InstanceType & 7;
        if (Representation == 5)
        {
            const auto Actual = ReadGuarded(TaggedString - 1 + 0x10, 8, nullptr);
            return Actual ? ReadString(*Actual, Depth + 1) : std::nullopt;
        }
        if (Representation != 0)
        {
            return std::nullopt;
        }
        if (*Length == 0)
        {
            return std::string{};
        }
        std::string Result(static_cast<std::size_t>(*Length), '\0');
        if ((MapInfo->InstanceType & 8) != 0)
        {
            if (!ReadRaw(TaggedString - 1 + 0x10, Result.data(), Result.size()))
            {
                return std::nullopt;
            }
        }
        else
        {
            std::array<std::uint16_t, 256> Data{};
            if (!ReadRaw(TaggedString - 1 + 0x10, Data.data(), Result.size() * 2))
            {
                return std::nullopt;
            }
            for (std::size_t Index = 0; Index < Result.size(); ++Index)
            {
                if (Data[Index] > 0x7f)
                {
                    return std::nullopt;
                }
                Result[Index] = static_cast<char>(Data[Index]);
            }
        }
        return Result;
    }

    std::optional<PuertsRuntimeReader::Property> PuertsRuntimeReader::OwnProperty(
        std::uintptr_t TaggedObject,
        const std::string& Name,
        std::vector<PuertsMemoryGuard>* Guards,
        bool GuardValue) const
    {
        if (!IsTag(TaggedObject))
        {
            return std::nullopt;
        }
        const std::uintptr_t Object = TaggedObject - 1;
        const auto MapTag = ReadGuarded(Object, 8, Guards);
        const auto MapInfo = MapTag ? ReadMap(*MapTag, Guards) : std::nullopt;
        if (!MapInfo ||
            (MapInfo->InstanceType != JsObjectType && MapInfo->InstanceType != JsFunctionType) ||
            !IsTag(MapInfo->Descriptors) || MapInfo->OwnDescriptors > MaximumDescriptors)
        {
            return std::nullopt;
        }
        const auto DescriptorCount = ReadGuarded(MapInfo->Descriptors - 1 + 0xa, 2, Guards);
        if (!DescriptorCount || MapInfo->OwnDescriptors > *DescriptorCount ||
            *DescriptorCount > MaximumDescriptors)
        {
            return std::nullopt;
        }
        for (std::uint32_t Index = 0; Index < MapInfo->OwnDescriptors; ++Index)
        {
            const std::uintptr_t Entry = MapInfo->Descriptors - 1 + 0x18 +
                static_cast<std::uintptr_t>(Index) * 0x18;
            const auto Key = ReadGuarded(Entry, 8, nullptr);
            const auto Text = Key ? ReadString(*Key) : std::nullopt;
            if (!Text || *Text != Name)
            {
                continue;
            }
            const auto DetailsRaw = ReadGuarded(Entry + 8, 8, Guards);
            if (!DetailsRaw || (*DetailsRaw & 0xffffffffull) != 0)
            {
                return std::nullopt;
            }
            if (Guards)
            {
                Guards->push_back({ Entry, *Key, 8 });
            }
            const auto Details = static_cast<std::uint32_t>(*DetailsRaw >> 32);
            Property Result;
            Result.StorageObject = Object;
            Result.Accessor = (Details & 1) != 0;
            Result.Representation = static_cast<std::uint8_t>((Details >> 6) & 7);
            Result.WritableField = !Result.Accessor && (Details & (2 | 8)) == 0;
            if ((Details & 2) != 0)
            {
                Result.Address = Entry + 0x10;
            }
            else
            {
                const std::uint32_t FieldIndex = (Details >> 19) & 0x3ff;
                if (MapInfo->InObjectStartWords > MapInfo->SizeWords)
                {
                    return std::nullopt;
                }
                const std::uint32_t InObjectCount =
                    MapInfo->SizeWords - MapInfo->InObjectStartWords;
                if (FieldIndex < InObjectCount)
                {
                    Result.Address = Object +
                        (static_cast<std::uintptr_t>(MapInfo->InObjectStartWords) + FieldIndex) * 8;
                }
                else
                {
                    const auto Properties = ReadGuarded(Object + 8, 8, Guards);
                    const auto PropertiesMap = Properties && IsTag(*Properties)
                        ? ReadGuarded(*Properties - 1, 8, Guards)
                        : std::nullopt;
                    const auto PropertiesType = PropertiesMap
                        ? ReadMap(*PropertiesMap, Guards)
                        : std::nullopt;
                    const auto Length = Properties && IsTag(*Properties)
                        ? ReadGuarded(*Properties - 1 + 8, 8, Guards)
                        : std::nullopt;
                    const std::uint32_t ArrayIndex = FieldIndex - InObjectCount;
                    if (!PropertiesType || PropertiesType->InstanceType != PropertyArrayType ||
                        !Length || (*Length & 0xffffffffull) != 0 ||
                        ArrayIndex >= ((*Length >> 32) & 0x3ff))
                    {
                        return std::nullopt;
                    }
                    Result.Address = *Properties - 1 + 0x10 +
                        static_cast<std::uintptr_t>(ArrayIndex) * 8;
                    Result.StorageObject = *Properties - 1;
                }
            }
            const auto Value = ReadGuarded(Result.Address, 8, GuardValue ? Guards : nullptr);
            if (!Value)
            {
                return std::nullopt;
            }
            Result.Value = *Value;
            return Result;
        }
        return std::nullopt;
    }

    std::optional<std::uint64_t> PuertsRuntimeReader::PropertyValue(
        std::uintptr_t TaggedObject,
        const std::string& Name,
        std::vector<PuertsMemoryGuard>& Guards,
        bool Inherited) const
    {
        for (unsigned Depth = 0; Depth < (Inherited ? 6u : 1u); ++Depth)
        {
            const auto Property = OwnProperty(TaggedObject, Name, &Guards);
            if (Property)
            {
                return Property->Accessor ? std::nullopt :
                    std::optional<std::uint64_t>(Property->Value);
            }
            if (!Inherited || !IsTag(TaggedObject))
            {
                return std::nullopt;
            }
            const auto MapTag = ReadGuarded(TaggedObject - 1, 8, &Guards);
            const auto MapInfo = MapTag ? ReadMap(*MapTag, &Guards) : std::nullopt;
            if (!MapInfo || !IsTag(MapInfo->Prototype) || MapInfo->Prototype == TaggedObject)
            {
                return std::nullopt;
            }
            TaggedObject = MapInfo->Prototype;
        }
        return std::nullopt;
    }

    bool PuertsRuntimeReader::IsModelClassMap(std::uintptr_t TaggedMap) const
    {
        if (!Settings.PuertsRuntime)
        {
            return false;
        }
        const auto MapInfo = ReadMap(TaggedMap, nullptr);
        if (!MapInfo || MapInfo->InstanceType != JsFunctionType || !IsTag(MapInfo->Descriptors))
        {
            return false;
        }
        const auto Count = ReadGuarded(MapInfo->Descriptors - 1 + 0xa, 2, nullptr);
        if (!Count || MapInfo->OwnDescriptors > *Count || *Count > MaximumDescriptors)
        {
            return false;
        }
        for (std::uint32_t Index = 0; Index < MapInfo->OwnDescriptors; ++Index)
        {
            const auto Key = ReadGuarded(MapInfo->Descriptors - 1 + 0x18 +
                static_cast<std::uintptr_t>(Index) * 0x18, 8, nullptr);
            const auto Name = Key ? ReadString(*Key) : std::nullopt;
            if (Name && (*Name == Settings.PuertsRuntime->ModelMapField ||
                (Settings.PuertsAp && *Name == Settings.PuertsRuntime->SingletonField)))
            {
                return true;
            }
        }
        return false;
    }

    bool PuertsRuntimeReader::ModelIdentity(
        std::uintptr_t TaggedConstructor,
        std::vector<PuertsMemoryGuard>* Guards,
        PuertsResourceKind Kind) const
    {
        if (!Settings.PuertsRuntime || !IsTag(TaggedConstructor) ||
            (Kind == PuertsResourceKind::Ap && !Settings.PuertsAp))
        {
            return false;
        }
        const auto MapTag = ReadGuarded(TaggedConstructor - 1, 8, Guards);
        if (!MapTag || !IsModelClassMap(*MapTag) || !ReadMap(*MapTag, Guards))
        {
            return false;
        }
        const auto Shared = ReadGuarded(TaggedConstructor - 1 + 0x18, 8, Guards);
        const auto SharedMap = Shared && IsTag(*Shared)
            ? ReadGuarded(*Shared - 1, 8, Guards) : std::nullopt;
        const auto SharedType = SharedMap ? ReadMap(*SharedMap, Guards) : std::nullopt;
        if (!SharedType || SharedType->InstanceType != SharedFunctionInfoType)
        {
            return false;
        }
        const auto Script = ReadGuarded(*Shared - 1 + 0x20, 8, Guards);
        const auto ScriptMap = Script && IsTag(*Script)
            ? ReadGuarded(*Script - 1, 8, Guards) : std::nullopt;
        const auto ScriptType = ScriptMap ? ReadMap(*ScriptMap, Guards) : std::nullopt;
        const auto ScriptName = Script && IsTag(*Script)
            ? ReadGuarded(*Script - 1 + 0x10, 8, Guards) : std::nullopt;
        const auto Name = ScriptName ? ReadString(*ScriptName) : std::nullopt;
        const auto& ExpectedScript = Kind == PuertsResourceKind::Ap
            ? Settings.PuertsAp->ModelScript
            : Settings.PuertsRuntime->ModelScript;
        if (!ScriptType || ScriptType->InstanceType != ScriptInstanceType ||
            !Name || *Name != ExpectedScript)
        {
            return false;
        }
        if (Kind == PuertsResourceKind::Dungeon)
        {
            return OwnProperty(
                TaggedConstructor, Settings.PuertsRuntime->ModelMapField, Guards).has_value();
        }
        const auto InitialTag = ReadGuarded(TaggedConstructor - 1 + 0x38, 8, Guards);
        const auto Initial = InitialTag ? ReadMap(*InitialTag, Guards) : std::nullopt;
        if (!Initial || Initial->InstanceType != JsObjectType)
        {
            return false;
        }
        const auto Owner = OwnProperty(Initial->Prototype, "constructor", Guards);
        const auto Method = OwnProperty(
            Initial->Prototype, Settings.PuertsAp->PrototypeMethod, Guards);
        const auto MethodMapTag = Method && IsTag(Method->Value)
            ? ReadGuarded(Method->Value - 1, 8, Guards) : std::nullopt;
        const auto MethodMap = MethodMapTag ? ReadMap(*MethodMapTag, Guards) : std::nullopt;
        return Owner && !Owner->Accessor && Owner->Value == TaggedConstructor &&
            Method && !Method->Accessor && MethodMap &&
            MethodMap->InstanceType == JsFunctionType;
    }

    bool PuertsRuntimeReader::IsModelConstructor(
        std::uintptr_t TaggedConstructor) const
    {
        return ModelIdentity(TaggedConstructor, nullptr);
    }

    bool PuertsRuntimeReader::IsApModelConstructor(
        std::uintptr_t TaggedConstructor) const
    {
        return ModelIdentity(TaggedConstructor, nullptr, PuertsResourceKind::Ap);
    }

    std::optional<std::uintptr_t> PuertsRuntimeReader::ModelInstance(
        std::uintptr_t TaggedConstructor,
        PuertsResourceKind Kind,
        std::vector<PuertsMemoryGuard>& Guards) const
    {
        if (!ModelIdentity(TaggedConstructor, &Guards, Kind))
        {
            return std::nullopt;
        }
        const auto Model = PropertyValue(
            TaggedConstructor, Settings.PuertsRuntime->SingletonField, Guards);
        const auto ModelMapTag = Model && IsTag(*Model)
            ? ReadGuarded(*Model - 1, 8, &Guards) : std::nullopt;
        const auto ModelMap = ModelMapTag ? ReadMap(*ModelMapTag, &Guards) : std::nullopt;
        const auto InitialTag = ReadGuarded(TaggedConstructor - 1 + 0x38, 8, &Guards);
        const auto Initial = InitialTag ? ReadMap(*InitialTag, &Guards) : std::nullopt;
        if (!ModelMap || !Initial ||
            ModelMap->InstanceType != JsObjectType ||
            Initial->InstanceType != JsObjectType ||
            ModelMap->Prototype != Initial->Prototype)
        {
            return std::nullopt;
        }
        return *Model;
    }

    std::optional<bool> PuertsRuntimeReader::BooleanValue(
        std::uintptr_t TaggedBoolean) const
    {
        if (!IsTag(TaggedBoolean))
        {
            return std::nullopt;
        }
        const auto MapTag = ReadGuarded(TaggedBoolean - 1, 8, nullptr);
        const auto MapInfo = MapTag ? ReadMap(*MapTag, nullptr) : std::nullopt;
        const auto TextTag = ReadGuarded(TaggedBoolean - 1 + 0x10, 8, nullptr);
        const auto Text = TextTag ? ReadString(*TextTag) : std::nullopt;
        const auto TypeTag = ReadGuarded(TaggedBoolean - 1 + 0x20, 8, nullptr);
        const auto Type = TypeTag ? ReadString(*TypeTag) : std::nullopt;
        if (!MapInfo || MapInfo->InstanceType != OddballType || !Text ||
            (*Text != "true" && *Text != "false") || !Type || *Type != "boolean")
        {
            return std::nullopt;
        }
        const bool Value = *Text == "true";
        const auto Number = ReadGuarded(TaggedBoolean - 1 + 0x18, 8, nullptr);
        const auto RawNumber = ReadGuarded(TaggedBoolean - 1 + 8, 8, nullptr);
        if (Number != std::optional<std::uint64_t>(std::uint64_t{ Value } << 32) ||
            RawNumber != std::optional<std::uint64_t>(Value ? 0x3ff0000000000000ull : 0))
        {
            return std::nullopt;
        }
        return Value;
    }

    std::optional<std::uintptr_t> PuertsRuntimeReader::CanonicalTrue(
        std::uintptr_t TaggedBoolean) const
    {
        const auto Current = BooleanValue(TaggedBoolean);
        if (!Current)
        {
            return std::nullopt;
        }
        if (*Current)
        {
            return TaggedBoolean;
        }
        // The verified V8 read-only boolean roots share one page, not game-dependent addresses.
        const auto Page = (TaggedBoolean - 1) & ~std::uintptr_t{ 0xfff };
        std::array<std::byte, 0x1000> Data{};
        if (!ReadRaw(Page, Data.data(), Data.size()))
        {
            return std::nullopt;
        }
        for (std::size_t Offset = 0; Offset + 0x30 <= Data.size(); Offset += 8)
        {
            std::uint64_t RawNumber = 0;
            std::uint64_t Number = 0;
            std::memcpy(&RawNumber, Data.data() + Offset + 8, 8);
            std::memcpy(&Number, Data.data() + Offset + 0x18, 8);
            if (RawNumber == 0x3ff0000000000000ull && Number == (std::uint64_t{ 1 } << 32))
            {
                const auto Candidate = Page + Offset + 1;
                const auto Value = BooleanValue(Candidate);
                if (Value && *Value)
                {
                    return Candidate;
                }
            }
        }
        return std::nullopt;
    }

    bool PuertsRuntimeReader::ValidateGuards(
        const std::vector<PuertsMemoryGuard>& Guards) const
    {
        for (const PuertsMemoryGuard& Guard : Guards)
        {
            const auto Actual = ReadGuarded(Guard.Address, Guard.Width, nullptr);
            if (!Actual || *Actual != Guard.Expected)
            {
                return false;
            }
        }
        return true;
    }

    std::optional<PuertsResourceTarget> PuertsRuntimeReader::ResolveResource(
        std::uintptr_t TaggedConstructor,
        std::uint32_t ResourceIndex,
        std::uint32_t Maximum) const
    {
        if (!Settings.PuertsRuntime || !Settings.PuertsArray ||
            ResourceIndex >= Settings.PuertsArray->Length)
        {
            return std::nullopt;
        }
        PuertsResourceTarget Result;
        Result.ModelConstructor = TaggedConstructor;
        Result.ResourceIndex = ResourceIndex;
        auto& Guards = Result.Guards;
        const auto Model = ModelInstance(
            TaggedConstructor, PuertsResourceKind::Dungeon, Guards);
        if (!Model)
        {
            return std::nullopt;
        }
        Result.ModelObject = *Model - 1;

        const auto Data = PropertyValue(*Model, "originData", Guards);
        const auto Base = Data ? PropertyValue(*Data, "baseInfo", Guards) : std::nullopt;
        const auto Actor = Base ? PropertyValue(*Base, "actorId", Guards) : std::nullopt;
        const auto ActorId = Actor ? FixedPointInteger(*Actor, INT32_MAX) : std::nullopt;
        const auto Finished = Data ? PropertyValue(*Data, "finished", Guards, true) : std::nullopt;
        const auto FinishedMapTag = Finished && IsTag(*Finished)
            ? ReadGuarded(*Finished - 1, 8, &Guards) : std::nullopt;
        const auto FinishedMap = FinishedMapTag ? ReadMap(*FinishedMapTag, &Guards) : std::nullopt;
        const auto FinishedTextTag = Finished && IsTag(*Finished)
            ? ReadGuarded(*Finished - 1 + 0x10, 8, &Guards) : std::nullopt;
        const auto FinishedText = FinishedTextTag ? ReadString(*FinishedTextTag) : std::nullopt;
        if (!ActorId || *ActorId == 0 || !FinishedMap ||
            FinishedMap->InstanceType != OddballType || !FinishedText || *FinishedText != "false")
        {
            return std::nullopt;
        }

        const auto Resources = PropertyValue(*Data, "resourceInfo", Guards);
        const auto Counts = Resources ? PropertyValue(*Resources, "count", Guards) : std::nullopt;
        if (!Counts || !IsTag(*Counts))
        {
            return std::nullopt;
        }
        const PuertsArrayConfig& Layout = *Settings.PuertsArray;
        const auto CountsMapTag = ReadGuarded(*Counts - 1, 8, &Guards);
        const auto CountsMap = CountsMapTag ? ReadMap(*CountsMapTag, &Guards) : std::nullopt;
        const auto Elements = ReadGuarded(*Counts - 1 +
            static_cast<std::uintptr_t>(Layout.ParentElementsOffset), 8, &Guards);
        if (!CountsMap || CountsMap->InstanceType != Layout.ParentInstanceType ||
            !Elements || !IsTag(*Elements))
        {
            return std::nullopt;
        }
        Result.ArrayObject = *Elements - 1;
        const auto ArrayMapTag = ReadGuarded(Result.ArrayObject, 8, &Guards);
        const auto ArrayMap = ArrayMapTag ? ReadMap(*ArrayMapTag, &Guards) : std::nullopt;
        const auto Length = ReadGuarded(Result.ArrayObject +
            static_cast<std::uintptr_t>(Layout.ObjectLengthOffset), 8, &Guards);
        const auto ValueOffset = Layout.ElementsOffset +
            static_cast<std::uint64_t>(ResourceIndex) * Layout.EntryStride;
        if (!ArrayMap || ArrayMap->InstanceType != Layout.FixedArrayInstanceType ||
            !Length || *Length != (static_cast<std::uint64_t>(Layout.Length) << 32) ||
            ValueOffset > MaximumUserAddress - Result.ArrayObject)
        {
            return std::nullopt;
        }
        Result.ValueAddress = Result.ArrayObject + static_cast<std::uintptr_t>(ValueOffset);
        Result.ValueObject = Result.ArrayObject;
        const auto Raw = ReadGuarded(Result.ValueAddress, 8, nullptr);
        const auto Integer = Raw ? FixedPointInteger(*Raw, Maximum) : std::nullopt;
        if (!Integer || !ValidateGuards(Guards))
        {
            return std::nullopt;
        }
        Result.IntegerValue = *Integer;
        return Result;
    }

    std::optional<PuertsResourceTarget> PuertsRuntimeReader::ResolveAp(
        std::uintptr_t TaggedConstructor,
        std::uint32_t Maximum) const
    {
        if (!Settings.PuertsAp)
        {
            return std::nullopt;
        }
        PuertsResourceTarget Result;
        Result.Kind = PuertsResourceKind::Ap;
        Result.ModelConstructor = TaggedConstructor;
        Result.ResourceIndex = Settings.PuertsAp->CurrencyId;
        auto& Guards = Result.Guards;
        const auto Model = ModelInstance(TaggedConstructor, Result.Kind, Guards);
        if (!Model)
        {
            return std::nullopt;
        }
        Result.ModelObject = *Model - 1;
        const auto Data = PropertyValue(*Model, "originData", Guards);
        const auto Pack = Data ? PropertyValue(*Data, "currencyPack", Guards) : std::nullopt;
        const auto Dict = Pack ? PropertyValue(*Pack, "dict", Guards) : std::nullopt;
        const auto DictMapTag = Dict && IsTag(*Dict)
            ? ReadGuarded(*Dict - 1, 8, &Guards) : std::nullopt;
        const auto DictMap = DictMapTag ? ReadMap(*DictMapTag, &Guards) : std::nullopt;
        const auto Elements = Dict && IsTag(*Dict)
            ? ReadGuarded(*Dict - 1 + 0x10, 8, &Guards) : std::nullopt;
        const auto ElementsMapTag = Elements && IsTag(*Elements)
            ? ReadGuarded(*Elements - 1, 8, &Guards) : std::nullopt;
        const auto ElementsMap = ElementsMapTag ? ReadMap(*ElementsMapTag, &Guards) : std::nullopt;
        const auto LengthRaw = Elements && IsTag(*Elements)
            ? ReadGuarded(*Elements - 1 + 8, 8, &Guards) : std::nullopt;
        const auto Length = LengthRaw
            ? FixedPointInteger(*LengthRaw, Settings.Monitor.MaxObjectsPerType) : std::nullopt;
        if (!DictMap || DictMap->InstanceType != JsObjectType ||
            !ElementsMap || ElementsMap->InstanceType != 0x77 ||
            !Length || Result.ResourceIndex >= *Length)
        {
            return std::nullopt;
        }
        Result.ArrayObject = *Elements - 1;
        const auto Currency = ReadGuarded(Result.ArrayObject + 0x10 +
            static_cast<std::uintptr_t>(Result.ResourceIndex) * 8, 8, &Guards);
        const auto IdRaw = Currency ? PropertyValue(*Currency, "id", Guards) : std::nullopt;
        const auto Id = IdRaw ? FixedPointInteger(*IdRaw, INT32_MAX) : std::nullopt;
        const auto Count = Currency ? OwnProperty(*Currency, "count", &Guards, false) : std::nullopt;
        const auto Integer = Count ? FixedPointInteger(Count->Value, Maximum) : std::nullopt;
        const auto Checker = PropertyValue(*Model, "dirtyChecker", Guards);
        const auto Dirty = Checker ? OwnProperty(*Checker, "dirty", &Guards, false) : std::nullopt;
        const auto TrueValue = Dirty ? CanonicalTrue(Dirty->Value) : std::nullopt;
        if (!Currency || !IsTag(*Currency) || !Id || *Id != Result.ResourceIndex ||
            !Count || !Count->WritableField || !Integer ||
            (Count->Representation != 1 && Count->Representation != 4) ||
            !Dirty || !Dirty->WritableField ||
            (Dirty->Representation != 3 && Dirty->Representation != 4) ||
            !TrueValue || !ValidateGuards(Guards))
        {
            return std::nullopt;
        }
        Result.ValueObject = Count->StorageObject;
        Result.ValueAddress = Count->Address;
        Result.IntegerValue = *Integer;
        Result.SaveDirtyAddress = Dirty->Address;
        Result.SaveDirtyTrueValue = *TrueValue;
        return Result;
    }

    std::optional<std::uint32_t> PuertsRuntimeReader::ReadResource(
        const PuertsResourceTarget& Expected,
        std::uint32_t Maximum) const
    {
        const auto Actual = Expected.Kind == PuertsResourceKind::Ap
            ? ResolveAp(Expected.ModelConstructor, Maximum)
            : ResolveResource(Expected.ModelConstructor, Expected.ResourceIndex, Maximum);
        if (!Actual || Actual->Kind != Expected.Kind ||
            Actual->ResourceIndex != Expected.ResourceIndex ||
            Actual->ModelObject != Expected.ModelObject ||
            Actual->ArrayObject != Expected.ArrayObject ||
            Actual->ValueObject != Expected.ValueObject ||
            Actual->ValueAddress != Expected.ValueAddress ||
            Actual->SaveDirtyAddress != Expected.SaveDirtyAddress ||
            Actual->SaveDirtyTrueValue != Expected.SaveDirtyTrueValue ||
            Actual->Guards != Expected.Guards)
        {
            return std::nullopt;
        }
        return Actual->IntegerValue;
    }

    PuertsModelScanResult ScanPuertsModelConstructors(
        const Config& Configuration,
        const std::shared_ptr<RemoteProcess>& Process,
        std::stop_token StopToken)
    {
        PuertsModelScanResult Result;
        if (!Process || !Configuration.PuertsRuntime || !Configuration.PuertsArray)
        {
            Result.Error = "A compatible process and Puerts runtime profile are required.";
            return Result;
        }
        PuertsRuntimeReader Reader(Configuration, [Process](auto Address, auto Output, auto Size)
        {
            return Process->Read(Address, Output, Size);
        });
        std::unordered_set<std::uintptr_t> SeenMaps;
        std::unordered_set<std::uintptr_t> ClassMaps;
        std::unordered_set<std::uintptr_t> Constructors;
        std::unordered_set<std::uintptr_t> ApConstructors;
        constexpr std::uint64_t MaximumSweepBytes = 32ull * 1024 * 1024 * 1024;
        const std::size_t ChunkSize = Configuration.Monitor.ScanChunkBytes;
        std::vector<std::byte> Buffer(ChunkSize + 0x30);
        for (unsigned Phase = 0; Phase < 2; ++Phase)
        {
            const auto Inspect = [&](std::uintptr_t Base, std::size_t Bytes,
                                     std::size_t Usable)
            {
                for (std::size_t Offset = 0; Offset + 8 <= Usable; Offset += 8)
                {
                    std::uintptr_t Tag = 0;
                    std::memcpy(&Tag, Buffer.data() + Offset, sizeof(Tag));
                    const std::uintptr_t Object = Base + Offset;
                    if (Phase == 0)
                    {
                        if (!IsTag(Tag))
                        {
                            continue;
                        }
                        std::uint16_t Type = 0;
                        if (Offset + 0xe <= Bytes)
                        {
                            std::memcpy(&Type, Buffer.data() + Offset + 0xc, sizeof(Type));
                        }
                        else if (!Process->Read(Object + 0xc, &Type, sizeof(Type)))
                        {
                            continue;
                        }
                        if (Type != JsFunctionType ||
                            !SeenMaps.insert(Object + 1).second)
                        {
                            continue;
                        }
                        if (Reader.IsModelClassMap(Object + 1))
                        {
                            ClassMaps.insert(Object + 1);
                            if (ClassMaps.size() > Configuration.Monitor.MaxObjectsPerType)
                            {
                                Result.Error = "Too many Puerts model class maps were found; writes remain disabled.";
                                return false;
                            }
                        }
                    }
                    else if (ClassMaps.contains(Tag))
                    {
                        if (Reader.IsModelConstructor(Object + 1))
                        {
                            Constructors.insert(Object + 1);
                        }
                        if (Reader.IsApModelConstructor(Object + 1))
                        {
                            ApConstructors.insert(Object + 1);
                        }
                        if (Constructors.size() > Configuration.Monitor.MaxWriteCandidates ||
                            ApConstructors.size() > Configuration.Monitor.MaxWriteCandidates)
                        {
                            Result.Error = "Too many Puerts model constructors were found; writes remain disabled.";
                            return false;
                        }
                    }
                }
                return true;
            };
            std::uintptr_t Cursor = 0;
            std::uint64_t SweepBytes = 0;
            do
            {
                bool Truncated = false;
                const auto Regions = Process->WritablePrivateRegions(
                    Configuration.Monitor.MaxScanBytes, Cursor, Truncated, Result.Error);
                if (!Result.Error.empty())
                {
                    return Result;
                }
                for (const MemoryRegion& Region : Regions)
                {
                    for (std::size_t Consumed = 0; Consumed < Region.Size; Consumed += ChunkSize)
                    {
                        if (StopToken.stop_requested())
                        {
                            Result.Error = "The Puerts model scan was cancelled.";
                            return Result;
                        }
                        const std::size_t Bytes = std::min(Buffer.size(), Region.Size - Consumed);
                        const std::size_t ConsumedBytes = std::min(ChunkSize, Bytes);
                        SweepBytes += ConsumedBytes;
                        Result.BytesScanned += ConsumedBytes;
                        if (SweepBytes > MaximumSweepBytes)
                        {
                            Result.Error = "The Puerts model scan exceeded its bounded sweep limit.";
                            return Result;
                        }
                        const std::uintptr_t Base = Region.Base + Consumed;
                        if (Process->Read(Base, Buffer.data(), Bytes))
                        {
                            if (!Inspect(Base, Bytes, ConsumedBytes))
                            {
                                return Result;
                            }
                            continue;
                        }
                        ++Result.ChangedRegions;
                        std::size_t PageOffset = 0;
                        while (PageOffset < ConsumedBytes)
                        {
                            if (StopToken.stop_requested())
                            {
                                Result.Error = "The Puerts model scan was cancelled.";
                                return Result;
                            }
                            const std::uintptr_t Page = Base + PageOffset;
                            const std::size_t Step = std::min(
                                std::size_t{ 0x1000 } - (Page & 0xfff),
                                ConsumedBytes - PageOffset);
                            const auto Current = Process->WritablePrivateRegionAt(Page, Result.Error);
                            if (!Result.Error.empty())
                            {
                                return Result;
                            }
                            if (Current)
                            {
                                const std::size_t Request = std::min(
                                    Step + 0x30,
                                    Current->Size - (Page - Current->Base));
                                std::size_t Available = Request;
                                if (!Process->Read(Page, Buffer.data(), Available))
                                {
                                    Available = std::min(Step, Request);
                                    if (!Process->Read(Page, Buffer.data(), Available))
                                    {
                                        const auto Rechecked = Process->WritablePrivateRegionAt(
                                            Page, Result.Error);
                                        if (!Result.Error.empty())
                                        {
                                            return Result;
                                        }
                                        if (Rechecked)
                                        {
                                            Result.Error = "A committed writable private page remained unreadable; the Puerts scan is incomplete.";
                                            return Result;
                                        }
                                        Available = 0;
                                    }
                                }
                                if (Available != 0 &&
                                    !Inspect(Page, Available, std::min(Step, Available)))
                                {
                                    return Result;
                                }
                            }
                            PageOffset += Step;
                        }
                    }
                }
                if (Truncated && Cursor == 0)
                {
                    Result.Error = "The Puerts model scan did not advance to its next memory range.";
                    return Result;
                }
            } while (Cursor != 0);
            if (Phase == 0 && ClassMaps.empty())
            {
                break;
            }
        }
        Result.ClassMaps = ClassMaps.size();
        Result.Constructors.assign(Constructors.begin(), Constructors.end());
        std::sort(Result.Constructors.begin(), Result.Constructors.end());
        Result.ApConstructors.assign(ApConstructors.begin(), ApConstructors.end());
        std::sort(Result.ApConstructors.begin(), Result.ApConstructors.end());
        Result.Complete = true;
        return Result;
    }
}
