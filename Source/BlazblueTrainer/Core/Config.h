#pragma once

#include "Hotkeys.h"
#include "Localization.h"

#include <array>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace Trainer
{
    enum class RefreshSemantics
    {
        Remaining,
        Used,
        Config
    };

    struct TypeInfoConfig
    {
        std::uint64_t Rva{};
        std::string ClassName;
    };

    struct RefreshFieldConfig
    {
        struct PathStep
        {
            std::string Type;
            std::uint64_t Offset{};
            bool Repeated{};
            std::optional<std::uint64_t> UnfinishedFlagOffset;
        };

        std::vector<PathStep> Path;
        std::uint64_t Offset{};
        RefreshSemantics Semantics{};
    };

    struct RefreshRootConfig
    {
        std::string Type;
        std::string ActiveInfoType;
        std::uint64_t ActiveInfoOffset{};
        std::optional<std::uint64_t> FinishedFlagOffset;
        std::vector<RefreshFieldConfig> Fields;
        struct ResourceMap
        {
            std::string Type;
            std::uint64_t InfoOffset{};
            std::uint64_t MapFieldOffset{};
            std::uint64_t DictionaryOffset{};
            std::uint64_t DictionaryEntriesOffset{};
            std::uint64_t DictionaryCountOffset{};
            std::uint64_t ArrayLengthOffset{};
            std::uint64_t ArrayDataOffset{};
            std::uint64_t EntryStride{};
            std::uint64_t EntryHashOffset{};
            std::uint64_t EntryKeyOffset{};
            std::uint64_t EntryNodeOffset{};
            std::uint64_t MapListOffset{};
            std::uint64_t NodeListOffset{};
            std::uint64_t NodeKeyOffset{};
            std::uint64_t NodeValueOffset{};
            std::uint32_t EntryKeyValue{};
        };
        std::optional<ResourceMap> ResourceDictionary;
    };

    struct ResourceTargetConfig
    {
        std::string Type;
        std::optional<std::uint64_t> DiscriminatorOffset;
        std::optional<std::uint32_t> DiscriminatorValue;
        std::uint64_t CountOffset{};
        bool SignedCount{};
        std::optional<std::uint64_t> IdOffset;
        std::optional<std::uint32_t> IdValue;
        std::vector<std::uint64_t> RequiredPointerOffsets;
        std::optional<std::string> RootType;
        bool RootIsSelf{};
        std::optional<std::uint64_t> RootPointerOffset;
        std::optional<std::uint64_t> RootCollectionOffset;
        std::uint64_t RepeatedArrayOffset{};
        std::uint64_t RepeatedCountOffset{};
        std::uint64_t ArrayLengthOffset{};
        std::uint64_t ArrayDataOffset{};
    };

    struct MonitorConfig
    {
        std::uint32_t ProcessCheckMs{};
        std::uint32_t ValueCheckMs{};
        std::uint32_t RescanMs{};
        std::uint32_t StableRescanMs{};
        std::uint32_t ScanChunkBytes{};
        std::uint64_t MaxScanBytes{};
        std::uint32_t MaxObjectsPerType{};
        std::uint32_t MaxDynamicRefreshCandidates{};
        std::uint32_t MaxResourceValue{};
        std::uint32_t MaxRefreshValue{};
        std::uint32_t MaxWriteCandidates{};
    };

    struct PuertsArrayConfig
    {
        std::uint32_t Length{};
        std::uint32_t ResourceIndex{};
        std::uint64_t ObjectLengthOffset{};
        std::uint64_t ElementsOffset{};
        std::uint64_t EntryStride{};
        std::uint64_t ValueHighWordOffset{};
        std::uint64_t MapInstanceTypeOffset{};
        std::uint64_t ParentPropertiesOffset{};
        std::uint64_t ParentElementsOffset{};
        std::uint16_t FixedArrayInstanceType{};
        std::uint16_t ParentInstanceType{};
    };

    struct FeatureDefaults
    {
        std::int64_t Set{};
        std::int64_t Add{};
    };

    struct PuertsRuntimeConfig
    {
        std::string ModelScript;
        std::string ModelMapField;
        std::string SingletonField;
    };

    struct PuertsApConfig
    {
        std::string ModelScript;
        std::string PrototypeMethod;
        std::uint32_t CurrencyId{};
    };

    struct WindowConfig
    {
        int Width{ 380 };
        int Height{ 364 };
    };

    struct Config
    {
        std::string Module;
        std::string UnityVersion;
        std::uint32_t Il2cppVersion{};
        std::uint64_t TypeNameOffset{};
        std::uint32_t IdentityMinTypeMatches{};
        MonitorConfig Monitor;
        WindowConfig Window;
        std::array<std::optional<HotkeyBinding>, 3> Hotkeys;
        std::unordered_map<std::string, TypeInfoConfig> TypeProfile;
        std::vector<RefreshRootConfig> RefreshRoots;
        std::optional<PuertsArrayConfig> PuertsArray;
        std::optional<PuertsRuntimeConfig> PuertsRuntime;
        std::optional<std::uint32_t> ExchangePuertsIndex;
        std::optional<PuertsApConfig> PuertsAp;
        std::optional<std::uint32_t> RefreshScanSeed;
        ResourceTargetConfig Exchange;
        FeatureDefaults RefreshDefaults;
        FeatureDefaults ExchangeDefaults;
        FeatureDefaults ApDefaults;
        Localization Texts;

        const TypeInfoConfig& TypeInfo(const std::string& Name) const;
        const std::string& Text(const std::string& Key) const;
        const std::string& Text(const std::string& Key, TrainerLanguage Language) const;
    };

    Config LoadConfig(const std::filesystem::path& Path);
}
