#include "Config.h"
#include "Paths.h"
#include "PuertsRuntime.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace
{
    class Heap
    {
    public:
        using Fields = std::vector<std::pair<std::string, std::uint64_t>>;

        Heap() : MemoryBytes(1024 * 1024)
        {
            MetaMapTag = Allocate(0x48) + 1;
            Store(MetaMapTag - 1, MetaMapTag);
            Store(MetaMapTag - 1 + 8, std::uint64_t{ 0xac } << 32);
            EmptyDescriptors = Allocate(0x18) + 1;
            StringMap = MakeMap(8, 0, 0, 0, {});
            PropertiesMap = MakeMap(0xb0, 0, 0, 0, {});
            ArrayMap = MakeMap(0x77, 0, 0, 0, {});
        }

        std::uintptr_t Allocate(std::size_t Size)
        {
            const auto Result = 0x100000 + NextOffset;
            NextOffset += (Size + 7) & ~std::size_t{ 7 };
            if (NextOffset > MemoryBytes.size())
            {
                throw std::runtime_error("Synthetic heap exhausted.");
            }
            return Result;
        }

        template<typename T>
        void Store(std::uintptr_t Address, T Value)
        {
            std::memcpy(MemoryBytes.data() + Address - 0x100000, &Value, sizeof(Value));
        }

        bool Read(std::uintptr_t Address, void* Output, std::size_t Size) const
        {
            if (Address < 0x100000 || Address - 0x100000 > MemoryBytes.size() ||
                Size > MemoryBytes.size() - (Address - 0x100000))
            {
                return false;
            }
            std::memcpy(Output, MemoryBytes.data() + Address - 0x100000, Size);
            return true;
        }

        std::uintptr_t String(const std::string& Value)
        {
            const auto Object = Allocate(0x10 + Value.size());
            Store(Object, StringMap);
            Store(Object + 0xc, static_cast<std::uint32_t>(Value.size()));
            std::memcpy(MemoryBytes.data() + Object - 0x100000 + 0x10, Value.data(), Value.size());
            return Object + 1;
        }

        std::uintptr_t MakeMap(
            std::uint16_t Type,
            std::uint8_t Words,
            std::uint8_t Start,
            std::uintptr_t Prototype,
            const Fields& Fields)
        {
            const auto Object = Allocate(0x48);
            Store(Object, MetaMapTag);
            Store(Object + 8, static_cast<std::uint64_t>(Words) |
                (static_cast<std::uint64_t>(Start) << 8) |
                (static_cast<std::uint64_t>(Type) << 32));
            Store(Object + 0x10, static_cast<std::uint32_t>(Fields.size()) << 10);
            Store(Object + 0x18, Prototype);
            std::uintptr_t Descriptors = EmptyDescriptors;
            if (!Fields.empty())
            {
                Descriptors = Allocate(0x18 + Fields.size() * 0x18) + 1;
                Store(Descriptors - 1 + 8, static_cast<std::uint16_t>(Fields.size()));
                Store(Descriptors - 1 + 0xa, static_cast<std::uint16_t>(Fields.size()));
                for (std::size_t Index = 0; Index < Fields.size(); ++Index)
                {
                    const auto Entry = Descriptors - 1 + 0x18 + Index * 0x18;
                    Store(Entry, String(Fields[Index].first));
                    const auto Details = (static_cast<std::uint64_t>(Index) << 19) | 0x104;
                    Store(Entry + 8, Details << 32);
                    Store(Entry + 0x10, std::uint64_t{ 1 } << 32);
                }
            }
            Store(Object + 0x28, Descriptors);
            return Object + 1;
        }

        std::uintptr_t Object(
            const Fields& Fields,
            std::uint8_t InObjectCount = 0,
            std::uintptr_t Prototype = 0,
            std::uint16_t Type = 0x421,
            std::uint8_t HeaderWords = 3)
        {
            const auto Words = static_cast<std::uint8_t>(HeaderWords + InObjectCount);
            const auto MapInfo = MakeMap(Type, Words, HeaderWords, Prototype, Fields);
            const auto Object = Allocate(static_cast<std::size_t>(Words) * 8);
            Store(Object, MapInfo);
            if (Fields.size() > InObjectCount)
            {
                const auto Count = Fields.size() - InObjectCount;
                const auto Properties = Allocate(0x10 + Count * 8);
                Store(Properties, PropertiesMap);
                Store(Properties + 8, (std::uint64_t{ 0x50000 } | Count) << 32);
                Store(Object + 8, Properties + 1);
            }
            for (std::size_t Index = 0; Index < Fields.size(); ++Index)
            {
                std::uintptr_t Address = 0;
                if (Index < InObjectCount)
                {
                    Address = Object + (HeaderWords + Index) * 8;
                }
                else
                {
                    std::uintptr_t Properties = 0;
                    Read(Object + 8, &Properties, 8);
                    Address = Properties - 1 + 0x10 + (Index - InObjectCount) * 8;
                }
                Store(Address, Fields[Index].second);
                ObjectFields[Object + 1].emplace(Fields[Index].first, Address);
            }
            return Object + 1;
        }

        std::uintptr_t Field(std::uintptr_t Object, const std::string& Name) const
        {
            return ObjectFields.at(Object).at(Name);
        }

        std::uintptr_t MapOf(std::uintptr_t Object) const
        {
            std::uintptr_t Result = 0;
            Read(Object - 1, &Result, 8);
            return Result;
        }

        std::uintptr_t RawObject(std::uint16_t Type, std::size_t Size)
        {
            const auto MapInfo = MakeMap(Type, static_cast<std::uint8_t>(Size / 8), 0, 0, {});
            const auto Object = Allocate(Size);
            Store(Object, MapInfo);
            return Object + 1;
        }

        std::uintptr_t Boolean(bool Value)
        {
            const auto Object = RawObject(0x43, 0x30);
            Store(Object - 1 + 8, Value ? std::uint64_t{ 0x3ff0000000000000 } : 0);
            Store(Object - 1 + 0x10, String(Value ? "true" : "false"));
            Store(Object - 1 + 0x18, std::uint64_t{ Value } << 32);
            Store(Object - 1 + 0x20, String("boolean"));
            Store(Object - 1 + 0x28, std::uint64_t{ Value } << 32);
            return Object;
        }

        std::uintptr_t Elements(const std::vector<std::uint64_t>& Values)
        {
            const auto Object = Allocate(0x10 + Values.size() * 8);
            Store(Object, ArrayMap);
            Store(Object + 8, static_cast<std::uint64_t>(Values.size()) << 32);
            for (std::size_t Index = 0; Index < Values.size(); ++Index)
            {
                Store(Object + 0x10 + Index * 8, Values[Index]);
            }
            return Object + 1;
        }

        std::uintptr_t Array(std::uint32_t Refresh, std::uint32_t Exchange)
        {
            const auto Object = Allocate(0x10 + 19 * 8);
            Store(Object, ArrayMap);
            Store(Object + 8, std::uint64_t{ 19 } << 32);
            Store(Object + 0x10 + 5 * 8, static_cast<std::uint64_t>(Exchange) << 32);
            Store(Object + 0x10 + 14 * 8, static_cast<std::uint64_t>(Refresh) << 32);
            return Object + 1;
        }

    private:
        std::vector<std::byte> MemoryBytes;
        std::size_t NextOffset{};
        std::uintptr_t MetaMapTag{};
        std::uintptr_t EmptyDescriptors{};
        std::uintptr_t StringMap{};
        std::uintptr_t PropertiesMap{};
        std::uintptr_t ArrayMap{};
        std::unordered_map<std::uintptr_t,
            std::unordered_map<std::string, std::uintptr_t>> ObjectFields;
    };

    struct Fixture
    {
        Heap Memory;
        std::uintptr_t FalseValue{};
        std::uintptr_t FinishedPrototype{};
        std::uintptr_t Base{};
        std::uintptr_t Array{};
        std::uintptr_t Counts{};
        std::uintptr_t Resources{};
        std::uintptr_t Data{};
        std::uintptr_t Model{};
        std::uintptr_t Constructor{};
        std::uintptr_t Script{};

        Fixture()
        {
            FalseValue = Memory.Boolean(false);
            FinishedPrototype = Memory.Object({ { "finished", FalseValue } }, 1);
            Base = Memory.Object({ { "actorId", std::uint64_t{ 37 } << 32 } }, 1);
            Array = Memory.Array(57, 3132);
            Counts = Memory.Object({});
            Memory.Store(Counts - 1 + 0x10, Array);
            Resources = Memory.Object({ { "count", Counts } }, 1);
            Data = Memory.Object(
                { { "baseInfo", Base }, { "resourceInfo", Resources } }, 0, FinishedPrototype);
            const auto ModelPrototype = Memory.Object({});
            Model = Memory.Object({ { "originData", Data } }, 1, ModelPrototype);
            Constructor = Memory.Object(
                { { "ForceMapName", Memory.String("") }, { "_instance", Model } },
                0, 0, 0x80c, 8);
            const auto Shared = Memory.RawObject(0xb3, 0x40);
            Script = Memory.RawObject(0x70, 0x80);
            Memory.Store(Script - 1 + 0x10, Memory.String("Data/Js/Server/Model/BreedDungeon.js"));
            Memory.Store(Shared - 1 + 0x20, Script);
            Memory.Store(Constructor - 1 + 0x18, Shared);
            Memory.Store(Constructor - 1 + 0x38, Memory.MapOf(Model));
        }

        Trainer::PuertsRuntimeReader Reader(const Trainer::Config& Configuration)
        {
            return Trainer::PuertsRuntimeReader(Configuration, [this](auto Address, auto Output, auto Size)
            {
                return Memory.Read(Address, Output, Size);
            });
        }
    };

    struct ApFixture
    {
        Heap Memory;
        std::uintptr_t TrueValue{};
        std::uintptr_t FalseValue{};
        std::uintptr_t Prototype{};
        std::uintptr_t Checker{};
        std::uintptr_t Currency{};
        std::uintptr_t Elements{};
        std::uintptr_t Dict{};
        std::uintptr_t Pack{};
        std::uintptr_t Data{};
        std::uintptr_t Model{};
        std::uintptr_t Constructor{};
        std::uintptr_t Script{};

        ApFixture()
        {
            TrueValue = Memory.Boolean(true);
            FalseValue = Memory.Boolean(false);
            Checker = Memory.Object({ { "dirty", FalseValue } }, 1);
            Currency = Memory.Object(
                { { "id", std::uint64_t{ 1 } << 32 }, { "count", std::uint64_t{ 8123 } << 32 } }, 2);
            Elements = Memory.Elements({ 0, Currency, 0, 0, 0 });
            Dict = Memory.Object({});
            Memory.Store(Dict - 1 + 0x10, Elements);
            Pack = Memory.Object({ { "dict", Dict } }, 1);
            Data = Memory.Object({ { "currencyPack", Pack } }, 1);
            Prototype = Memory.Object({
                { "constructor", 0 },
                { "BuildCurrencyPack", Memory.RawObject(0x80c, 0x40) }
            });
            Model = Memory.Object(
                { { "originData", Data }, { "dirtyChecker", Checker } }, 2, Prototype);
            Constructor = Memory.Object({ { "_instance", Model } }, 0, 0, 0x80c, 8);
            Memory.Store(Memory.Field(Prototype, "constructor"), Constructor);
            const auto Shared = Memory.RawObject(0xb3, 0x40);
            Script = Memory.RawObject(0x70, 0x80);
            Memory.Store(Script - 1 + 0x10, Memory.String("Data/Js/Server/Model/PlayerSplitStruct.js"));
            Memory.Store(Shared - 1 + 0x20, Script);
            Memory.Store(Constructor - 1 + 0x18, Shared);
            Memory.Store(Constructor - 1 + 0x38, Memory.MapOf(Model));
        }

        Trainer::PuertsRuntimeReader Reader(const Trainer::Config& Configuration)
        {
            return Trainer::PuertsRuntimeReader(Configuration, [this](auto Address, auto Output, auto Size)
            {
                return Memory.Read(Address, Output, Size);
            });
        }
    };
}

int RunPuertsRuntimeTests()
{
    const auto Configuration = Trainer::LoadConfig(Trainer::DefaultConfigPath());
    int Failures = 0;
    const auto Require = [&](bool Condition, const char* Message)
    {
        if (!Condition)
        {
            std::cerr << Message << '\n';
            ++Failures;
        }
    };
    {
        Fixture F;
        auto Reader = F.Reader(Configuration);
        const auto Refresh = Reader.ResolveResource(F.Constructor, 14, 10000);
        const auto Exchange = Reader.ResolveResource(F.Constructor, 5, 1000000);
        Require(Reader.IsModelClassMap(F.Memory.MapOf(F.Constructor)) &&
            Reader.IsModelConstructor(F.Constructor), "Named model class identity failed.");
        Require(Refresh && Refresh->IntegerValue == 57 &&
            Refresh->ValueAddress == F.Array - 1 + 0x80,
            "Refresh must resolve without an observed value change or fixed startup count.");
        Require(Exchange && Exchange->IntegerValue == 3132 &&
            Exchange->ValueAddress == F.Array - 1 + 0x38,
            "Coin must resolve slot 5 through out-of-object named properties.");
        if (Exchange)
        {
            F.Memory.Store(Exchange->ValueAddress, std::uint64_t{ 3133 } << 32);
            Require(Reader.ReadResource(*Exchange, 1000000) == 3133,
                "Normal counter changes must not invalidate an unchanged owner chain.");
            F.Memory.Store(Exchange->ValueAddress, std::uint64_t{ 0 });
            const auto Zero = Reader.ResolveResource(F.Constructor, 5, 1000000);
            Require(Zero && Zero->IntegerValue == 0,
                "Zero Coin must remain discoverable.");
        }
    }
    {
        Fixture F;
        auto Reader = F.Reader(Configuration);
        const auto Old = Reader.ResolveResource(F.Constructor, 5, 1000000);
        const auto Replacement = F.Memory.Array(15, 3132);
        F.Memory.Store(F.Counts - 1 + 0x10, Replacement);
        const auto Current = Reader.ResolveResource(F.Constructor, 5, 1000000);
        Require(Old && !Reader.ReadResource(*Old, 1000000),
            "A detached but still readable old array must not remain writable.");
        Require(Current && Current->ValueAddress == Replacement - 1 + 0x38,
            "Replacing the live array must reacquire its new address without a value transition.");
    }
    {
        Fixture F;
        auto Reader = F.Reader(Configuration);
        const auto Old = Reader.ResolveResource(F.Constructor, 5, 1000000);
        const auto ReplacementData = F.Memory.Object(
            { { "baseInfo", F.Base }, { "resourceInfo", F.Resources } }, 0, F.FinishedPrototype);
        F.Memory.Store(F.Memory.Field(F.Model, "originData"), ReplacementData);
        Require(Old && !Reader.ReadResource(*Old, 1000000),
            "A changed dungeon root must invalidate old candidates even when the value address matches.");
        Require(Reader.ResolveResource(F.Constructor, 5, 1000000).has_value(),
            "A new valid dungeon root must be reacquired immediately.");
    }
    {
        Fixture F;
        auto Reader = F.Reader(Configuration);
        const auto Old = Reader.ResolveResource(F.Constructor, 5, 1000000);
        F.Memory.Store(F.Memory.Field(F.Base, "actorId"), std::uint64_t{ 0 });
        Require(Old && !Reader.ReadResource(*Old, 1000000) &&
            !Reader.ResolveResource(F.Constructor, 5, 1000000),
            "Returning to an uninitialized world state must disable stale resource targets.");
        F.Memory.Store(F.Memory.Field(F.Base, "actorId"), std::uint64_t{ 71 } << 32);
        Require(Reader.ResolveResource(F.Constructor, 5, 1000000).has_value(),
            "Starting a new initialized run must not require consuming a resource.");
    }
    {
        Fixture F;
        auto Reader = F.Reader(Configuration);
        const auto Old = Reader.ResolveResource(F.Constructor, 5, 1000000);
        F.Memory.Store(F.Memory.Field(F.FinishedPrototype, "finished"), F.Memory.Boolean(true));
        Require(Old && !Reader.ReadResource(*Old, 1000000) &&
            !Reader.ResolveResource(F.Constructor, 5, 1000000),
            "Finished dungeon state must not be writable.");
    }
    {
        Fixture F;
        auto Reader = F.Reader(Configuration);
        const auto Old = Reader.ResolveResource(F.Constructor, 5, 1000000);
        const auto Other = F.Memory.Object({ { "originData", F.Data } }, 1);
        F.Memory.Store(F.Memory.Field(F.Constructor, "_instance"), Other);
        Require(Old && !Reader.ReadResource(*Old, 1000000) &&
            !Reader.ResolveResource(F.Constructor, 5, 1000000),
            "A singleton pointing at a different model prototype must be rejected.");
    }
    {
        Fixture F;
        auto Reader = F.Reader(Configuration);
        F.Memory.Store(F.Script - 1 + 0x10, F.Memory.String("Data/Js/Server/Model/SecondBreedDungeon.js"));
        Require(!Reader.IsModelConstructor(F.Constructor) &&
            !Reader.ResolveResource(F.Constructor, 5, 1000000),
            "A similarly shaped secondary or cached model must not impersonate the primary class.");
    }
    {
        Fixture F;
        auto Reader = F.Reader(Configuration);
        const auto Address = F.Array - 1 + 0x38;
        F.Memory.Store(Address, (std::uint64_t{ 3132 } << 32) | 1);
        Require(!Reader.ResolveResource(F.Constructor, 5, 1000000),
            "A fractional or non-Smi resource slot must be rejected.");
        F.Memory.Store(Address, std::uint64_t{ 1000001 } << 32);
        Require(!Reader.ResolveResource(F.Constructor, 5, 1000000),
            "Out-of-range resources must be rejected.");
        F.Memory.Store(Address, std::uint64_t{ 3132 } << 32);
        F.Memory.Store(F.Array - 1 + 8, std::uint64_t{ 18 } << 32);
        Require(!Reader.ResolveResource(F.Constructor, 5, 1000000),
            "A reused array with a different length must be rejected.");
    }
    {
        Fixture F;
        auto Reader = F.Reader(Configuration);
        const auto Target = Reader.ResolveResource(F.Constructor, 5, 1000000);
        F.Memory.Store(F.Array - 1, F.Memory.MapOf(F.Model));
        Require(Target && !Reader.ReadResource(*Target, 1000000),
            "A readable address reused as another object type must be rejected.");
    }
    {
        Fixture F;
        const auto OtherData = F.Memory.Object(
            { { "baseInfo", F.Base }, { "resourceInfo", F.Resources } }, 0, F.FinishedPrototype);
        bool Changed = false;
        Trainer::PuertsRuntimeReader Reader(Configuration, [&](auto Address, auto Output, auto Size)
        {
            const bool Result = F.Memory.Read(Address, Output, Size);
            if (!Changed && Address == F.Array - 1 + 0x38)
            {
                Changed = true;
                F.Memory.Store(F.Memory.Field(F.Model, "originData"), OtherData);
            }
            return Result;
        });
        Require(!Reader.ResolveResource(F.Constructor, 5, 1000000),
            "An owner-chain change during collection must fail its final guard pass.");
    }
    {
        Fixture F;
        auto Reader = F.Reader(Configuration);
        const auto UiCache = F.Memory.Object({ { "LastShowNum", std::uint64_t{ 3132 } << 32 } }, 1);
        Require(!Reader.IsModelConstructor(UiCache) &&
            !Reader.ResolveResource(UiCache, 5, 1000000),
            "A display cache with the same number must never be treated as a resource owner.");
        Require(!Reader.ResolveResource(F.Constructor, 19, 1000000),
            "An out-of-bounds resource index must be rejected.");
    }
    {
        ApFixture F;
        auto Reader = F.Reader(Configuration);
        const auto Ap = Reader.ResolveAp(F.Constructor, 1000000);
        Require(Reader.IsModelClassMap(F.Memory.MapOf(F.Constructor)) &&
            Reader.IsApModelConstructor(F.Constructor) && !Reader.IsModelConstructor(F.Constructor),
            "The AP class must be recognized without ForceMapName and without impersonating the dungeon.");
        Require(Ap && Ap->IntegerValue == 8123 &&
            Ap->ValueAddress == F.Memory.Field(F.Currency, "count") &&
            Ap->SaveDirtyAddress == F.Memory.Field(F.Checker, "dirty") &&
            Ap->SaveDirtyTrueValue == F.TrueValue,
            "AP must resolve its currency record and canonical save-dirty boolean without a value transition.");
        if (Ap)
        {
            F.Memory.Store(Ap->ValueAddress, std::uint64_t{ 8124 } << 32);
            Require(Reader.ReadResource(*Ap, 1000000) == 8124,
                "A normal AP change must not invalidate its owner chain.");
            F.Memory.Store(F.Memory.Field(F.Checker, "dirty"), F.TrueValue);
            Require(Reader.ReadResource(*Ap, 1000000) == 8124,
                "Normal dirty-flag changes must not invalidate AP ownership.");
            F.Memory.Store(Ap->ValueAddress, std::uint64_t{ 0 });
            Require(Reader.ReadResource(*Ap, 1000000) == 0,
                "Zero AP must remain discoverable and writable.");
        }
    }
    {
        ApFixture F;
        auto Reader = F.Reader(Configuration);
        const auto Old = Reader.ResolveAp(F.Constructor, 1000000);
        const auto Replacement = F.Memory.Object(
            { { "id", std::uint64_t{ 1 } << 32 }, { "count", std::uint64_t{ 8123 } << 32 } });
        F.Memory.Store(F.Elements - 1 + 0x18, Replacement);
        const auto Current = Reader.ResolveAp(F.Constructor, 1000000);
        Require(Old && !Reader.ReadResource(*Old, 1000000) &&
            Current && Current->ValueAddress == F.Memory.Field(Replacement, "count") &&
            Current->ValueObject < Current->ValueAddress,
            "Replacing an AP record at the same value must invalidate the old target and support out-of-object counts.");
    }
    {
        ApFixture F;
        auto Reader = F.Reader(Configuration);
        const auto Old = Reader.ResolveAp(F.Constructor, 1000000);
        std::vector<std::uint64_t> Entries(43);
        Entries[1] = F.Currency;
        const auto Replacement = F.Memory.Elements(Entries);
        F.Memory.Store(F.Dict - 1 + 0x10, Replacement);
        const auto Current = Reader.ResolveAp(F.Constructor, 1000000);
        Require(Old && !Reader.ReadResource(*Old, 1000000) &&
            Current && Current->ArrayObject == Replacement - 1,
            "AP dictionary capacity is not fixed at 19 and relocation must not require an AP change.");
    }
    {
        ApFixture F;
        auto Reader = F.Reader(Configuration);
        const auto Old = Reader.ResolveAp(F.Constructor, 1000000);
        const auto Replacement = F.Memory.Object({ { "currencyPack", F.Pack } }, 1);
        F.Memory.Store(F.Memory.Field(F.Model, "originData"), Replacement);
        Require(Old && !Reader.ReadResource(*Old, 1000000) &&
            Reader.ResolveAp(F.Constructor, 1000000).has_value(),
            "Replacing AP originData must invalidate old ownership even when the currency address is unchanged.");
    }
    {
        ApFixture F;
        auto Reader = F.Reader(Configuration);
        const auto Old = Reader.ResolveAp(F.Constructor, 1000000);
        const auto Replacement = F.Memory.Object(
            { { "originData", F.Data }, { "dirtyChecker", F.Checker } }, 2, F.Prototype);
        F.Memory.Store(F.Memory.Field(F.Constructor, "_instance"), Replacement);
        Require(Old && !Reader.ReadResource(*Old, 1000000) &&
            Reader.ResolveAp(F.Constructor, 1000000).has_value(),
            "A new valid AP singleton must be reacquired instead of retaining the old instance.");
        F.Memory.Store(F.Memory.Field(F.Constructor, "_instance"), std::uint64_t{ 0 });
        Require(!Reader.ResolveAp(F.Constructor, 1000000),
            "A missing AP singleton must disable writes.");
    }
    {
        ApFixture F;
        auto Reader = F.Reader(Configuration);
        F.Memory.Store(F.Memory.Field(F.Currency, "id"), std::uint64_t{ 2 } << 32);
        Require(!Reader.ResolveAp(F.Constructor, 1000000),
            "The AP dictionary key and currency record ID must agree.");
        F.Memory.Store(F.Memory.Field(F.Currency, "id"), std::uint64_t{ 1 } << 32);
        F.Memory.Store(F.Elements - 1 + 8, std::uint64_t{ 1 } << 32);
        Require(!Reader.ResolveAp(F.Constructor, 1000000),
            "A currency slot beyond the elements length must not be read.");
    }
    {
        ApFixture F;
        auto Reader = F.Reader(Configuration);
        const auto Address = F.Memory.Field(F.Currency, "count");
        for (const auto Invalid : {
                 (std::uint64_t{ 8123 } << 32) | 1,
                 std::uint64_t{ 1000001 } << 32,
                 std::uint64_t{ 0xffffffff } << 32 })
        {
            F.Memory.Store(Address, Invalid);
            Require(!Reader.ResolveAp(F.Constructor, 1000000),
                "Fractional, negative, or out-of-range AP must fail closed.");
        }
    }
    for (const std::uint64_t Flag : { 1ull, 2ull, 8ull })
    {
        ApFixture F;
        auto Reader = F.Reader(Configuration);
        std::uintptr_t Descriptors = 0;
        F.Memory.Read(F.Memory.MapOf(F.Currency) - 1 + 0x28, &Descriptors, 8);
        const auto DetailsAddress = Descriptors - 1 + 0x18 + 0x18 + 8;
        std::uint64_t Details = 0;
        F.Memory.Read(DetailsAddress, &Details, 8);
        F.Memory.Store(DetailsAddress, Details | (Flag << 32));
        Require(!Reader.ResolveAp(F.Constructor, 1000000),
            "An accessor, descriptor constant, or read-only AP property must never be a write target.");
    }
    {
        ApFixture F;
        auto Reader = F.Reader(Configuration);
        std::uintptr_t Descriptors = 0;
        F.Memory.Read(F.Memory.MapOf(F.Currency) - 1 + 0x28, &Descriptors, 8);
        const auto DetailsAddress = Descriptors - 1 + 0x18 + 0x18 + 8;
        std::uint64_t Details = 0;
        F.Memory.Read(DetailsAddress, &Details, 8);
        Details = (Details & ~(std::uint64_t{ 7 } << 38)) | (std::uint64_t{ 2 } << 38);
        F.Memory.Store(DetailsAddress, Details);
        Require(!Reader.ResolveAp(F.Constructor, 1000000),
            "An unboxed double field must not be overwritten with a tagged Smi.");
    }
    {
        ApFixture F;
        auto Reader = F.Reader(Configuration);
        F.Memory.Store(F.Memory.Field(F.Prototype, "BuildCurrencyPack"), F.FalseValue);
        Require(!Reader.IsApModelConstructor(F.Constructor) &&
            !Reader.ResolveAp(F.Constructor, 1000000),
            "Other classes in PlayerSplitStruct.js must not impersonate the AP model.");
    }
    {
        ApFixture F;
        auto Reader = F.Reader(Configuration);
        F.Memory.Store(F.Script - 1 + 0x10, F.Memory.String("Data/Js/Widget/Currency.js"));
        Require(!Reader.IsApModelConstructor(F.Constructor),
            "An AP display model with the same property names must be rejected.");
    }
    {
        ApFixture F;
        auto Reader = F.Reader(Configuration);
        F.Memory.Store(F.Memory.Field(F.Checker, "dirty"), F.Memory.String("false"));
        Require(!Reader.ResolveAp(F.Constructor, 1000000),
            "A save-dirty field that is not a real V8 boolean must be rejected.");
    }
    {
        ApFixture F;
        bool Changed = false;
        const auto Replacement = F.Memory.Object({ { "currencyPack", F.Pack } }, 1);
        Trainer::PuertsRuntimeReader Reader(Configuration, [&](auto Address, auto Output, auto Size)
        {
            const bool Result = F.Memory.Read(Address, Output, Size);
            if (!Changed && Address == F.Memory.Field(F.Currency, "count"))
            {
                Changed = true;
                F.Memory.Store(F.Memory.Field(F.Model, "originData"), Replacement);
            }
            return Result;
        });
        Require(!Reader.ResolveAp(F.Constructor, 1000000),
            "An AP owner change during collection must fail the final guard pass.");
    }
    {
        ApFixture F;
        auto WithoutAp = Configuration;
        WithoutAp.PuertsAp.reset();
        auto Reader = F.Reader(WithoutAp);
        Require(!Reader.IsApModelConstructor(F.Constructor) &&
            !Reader.ResolveAp(F.Constructor, 1000000),
            "A configuration without an AP profile must not enable AP.");
    }
    return Failures == 0 ? 0 : 1;
}
