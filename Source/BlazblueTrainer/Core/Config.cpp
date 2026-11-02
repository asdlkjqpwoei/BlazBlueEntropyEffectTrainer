#include "Config.h"

#include <yaml-cpp/yaml.h>

#include <limits>
#include <fstream>
#include <stdexcept>
#include <utility>

namespace Trainer
{
    namespace
    {
        YAML::Node RequiredNode(const YAML::Node& Parent, const char* Key)
        {
            const YAML::Node Value = Parent[Key];
            if (!Value)
            {
                throw std::runtime_error(std::string("Missing required configuration key: ") + Key);
            }
            return Value;
        }

        std::string RequiredString(const YAML::Node& Parent, const char* Key)
        {
            const YAML::Node Value = RequiredNode(Parent, Key);
            if (!Value.IsScalar())
            {
                throw std::runtime_error(std::string("Configuration key must be a scalar: ") + Key);
            }
            return Value.as<std::string>();
        }

        std::uint64_t ParseUnsigned(const YAML::Node& Value, const std::string& Name)
        {
            if (!Value || !Value.IsScalar())
            {
                throw std::runtime_error("Configuration value must be an unsigned integer: " + Name);
            }

            const std::string Text = Value.as<std::string>();
            if (Text.empty() || Text.front() == '-')
            {
                throw std::runtime_error("Invalid unsigned integer in configuration: " + Name);
            }
            std::size_t Parsed = 0;
            const unsigned long long Result = std::stoull(Text, &Parsed, 0);
            if (Parsed != Text.size())
            {
                throw std::runtime_error("Invalid integer in configuration: " + Name);
            }
            return static_cast<std::uint64_t>(Result);
        }

        std::int64_t ParseSigned(const YAML::Node& Value, const std::string& Name)
        {
            if (!Value || !Value.IsScalar())
            {
                throw std::runtime_error("Configuration value must be an integer: " + Name);
            }

            const std::string Text = Value.as<std::string>();
            std::size_t Parsed = 0;
            const long long Result = std::stoll(Text, &Parsed, 0);
            if (Parsed != Text.size())
            {
                throw std::runtime_error("Invalid integer in configuration: " + Name);
            }
            return static_cast<std::int64_t>(Result);
        }

        std::optional<std::uint64_t> OptionalUnsigned(const YAML::Node& Parent, const char* Key)
        {
            const YAML::Node Value = Parent[Key];
            if (!Value || Value.IsNull())
            {
                return std::nullopt;
            }
            return ParseUnsigned(Value, Key);
        }

        RefreshSemantics ParseSemantics(const YAML::Node& Value)
        {
            const std::string Name = Value.as<std::string>();
            if (Name == "remaining")
            {
                return RefreshSemantics::Remaining;
            }
            if (Name == "used")
            {
                return RefreshSemantics::Used;
            }
            if (Name == "config")
            {
                return RefreshSemantics::Config;
            }
            throw std::runtime_error("Refresh field semantics must be remaining, used, or config.");
        }

        ResourceTargetConfig ParseResourceTarget(
            const YAML::Node& Node,
            const std::string& Name)
        {
            ResourceTargetConfig Target;
            Target.Type = RequiredString(Node, "type");
            Target.DiscriminatorOffset = OptionalUnsigned(Node, "discriminator_offset");
            if (const auto Discriminator = OptionalUnsigned(Node, "discriminator_value"))
            {
                Target.DiscriminatorValue = static_cast<std::uint32_t>(*Discriminator);
            }
            if (Target.DiscriminatorOffset.has_value() != Target.DiscriminatorValue.has_value())
            {
                throw std::runtime_error(
                    Name + " must provide both discriminator_offset and discriminator_value.");
            }
            Target.CountOffset = ParseUnsigned(
                RequiredNode(Node, "count_offset"),
                Name + ".count_offset");
            if (const YAML::Node KindNode = Node["value_kind"]; KindNode && !KindNode.IsNull())
            {
                const std::string Kind = KindNode.as<std::string>();
                if (Kind == "signed")
                {
                    Target.SignedCount = true;
                }
                else if (Kind != "unsigned")
                {
                    throw std::runtime_error(Name + ".value_kind must be signed or unsigned.");
                }
            }
            Target.IdOffset = OptionalUnsigned(Node, "id_offset");
            if (const auto IdValue = OptionalUnsigned(Node, "id_value"))
            {
                Target.IdValue = static_cast<std::uint32_t>(*IdValue);
            }
            if (const YAML::Node PointerOffsets = Node["required_pointer_offsets"];
                PointerOffsets && !PointerOffsets.IsNull())
            {
                if (!PointerOffsets.IsSequence())
                {
                    throw std::runtime_error(Name + ".required_pointer_offsets must be a sequence.");
                }
                for (const YAML::Node& PointerOffset : PointerOffsets)
                {
                    const std::uint64_t Offset =
                        ParseUnsigned(PointerOffset, Name + ".required_pointer_offsets");
                    if (Offset == 0)
                    {
                        throw std::runtime_error(Name + ".required_pointer_offsets must be non-zero.");
                    }
                    Target.RequiredPointerOffsets.push_back(Offset);
                }
            }

            if (const YAML::Node RootType = Node["root_type"]; RootType && !RootType.IsNull())
            {
                Target.RootType = RootType.as<std::string>();
                if (const YAML::Node RootIsSelf = Node["root_is_self"];
                    RootIsSelf && !RootIsSelf.IsNull())
                {
                    Target.RootIsSelf = RootIsSelf.as<bool>();
                }
                if (Target.RootIsSelf)
                {
                    if (*Target.RootType != Target.Type)
                    {
                        throw std::runtime_error(Name + ".root_is_self requires root_type to equal type.");
                    }
                }
                else if (const auto PointerOffset = OptionalUnsigned(Node, "root_pointer_offset"))
                {
                    Target.RootPointerOffset = PointerOffset;
                    if (*Target.RootPointerOffset == 0)
                    {
                        throw std::runtime_error(Name + ".root_pointer_offset must be non-zero.");
                    }
                }
                else
                {
                    Target.RootCollectionOffset = OptionalUnsigned(Node, "root_collection_offset");
                    Target.RepeatedArrayOffset = ParseUnsigned(
                        RequiredNode(Node, "repeated_array_offset"),
                        Name + ".repeated_array_offset");
                    Target.RepeatedCountOffset = ParseUnsigned(
                        RequiredNode(Node, "repeated_count_offset"),
                        Name + ".repeated_count_offset");
                    Target.ArrayLengthOffset = ParseUnsigned(
                        RequiredNode(Node, "array_length_offset"),
                        Name + ".array_length_offset");
                    Target.ArrayDataOffset = ParseUnsigned(
                        RequiredNode(Node, "array_data_offset"),
                        Name + ".array_data_offset");
                    if (!Target.RootCollectionOffset)
                    {
                        throw std::runtime_error(
                            Name + ".root_collection_offset is required with a collection root.");
                    }
                }
            }

            return Target;
        }

        FeatureDefaults ParseDefaults(const YAML::Node& Node, const char* SetKey, const char* AddKey)
        {
            FeatureDefaults Defaults;
            Defaults.Set = ParseSigned(RequiredNode(Node, SetKey), SetKey);
            Defaults.Add = ParseSigned(RequiredNode(Node, AddKey), AddKey);
            if (Defaults.Set < 0 || Defaults.Add < 0)
            {
                throw std::runtime_error("Feature defaults must not be negative.");
            }
            return Defaults;
        }

    }

    const TypeInfoConfig& Config::TypeInfo(const std::string& Name) const
    {
        const auto Found = TypeProfile.find(Name);
        if (Found == TypeProfile.end())
        {
            throw std::runtime_error("No IL2CPP type information configured for " + Name);
        }
        return Found->second;
    }

    const std::string& Config::Text(const std::string& Key) const
    {
        return Texts.Text(Key, Texts.DefaultLanguage);
    }

    const std::string& Config::Text(const std::string& Key, TrainerLanguage Language) const
    {
        return Texts.Text(Key, Language);
    }

    Config LoadConfig(const std::filesystem::path& Path)
    {
        std::ifstream Input(Path, std::ios::binary);
        if (!Input)
            throw std::runtime_error("Cannot open the trainer configuration.");
        const YAML::Node Root = YAML::Load(Input);
        Config Configuration;
        Configuration.Texts = Localization::FromYaml(Root);

        const YAML::Node Game = RequiredNode(Root, "game");
        Configuration.Module = RequiredString(Game, "module");
        if (const auto Executable = Game["executable"]; Executable && !Executable.IsNull())
            Configuration.ExecutableName = Executable.as<std::string>();
        if (Configuration.ExecutableName.empty() ||
            Configuration.ExecutableName.find_first_of("\\/:") != std::string::npos)
            throw std::runtime_error("The game executable must be a file name, not an installation path.");
        Configuration.UnityVersion = RequiredString(Game, "unity_version");
        Configuration.Il2cppVersion = static_cast<std::uint32_t>(
            ParseUnsigned(RequiredNode(Game, "il2cpp_version"), "game.il2cpp_version"));
        Configuration.TypeNameOffset = ParseUnsigned(
            RequiredNode(Game, "type_name_offset"),
            "game.type_name_offset");
        Configuration.IdentityMinTypeMatches = static_cast<std::uint32_t>(
            ParseUnsigned(
                RequiredNode(Game, "identity_min_type_matches"),
                "game.identity_min_type_matches"));
        if (Configuration.Module.empty() ||
            Configuration.UnityVersion.empty() ||
            Configuration.Il2cppVersion == 0 ||
            Configuration.TypeNameOffset == 0 ||
            Configuration.IdentityMinTypeMatches == 0)
        {
            throw std::runtime_error("The game identity profile must contain valid IL2CPP module settings.");
        }

        const YAML::Node Monitor = RequiredNode(Root, "monitor");
        Configuration.Monitor.ProcessCheckMs = static_cast<std::uint32_t>(
            ParseUnsigned(RequiredNode(Monitor, "process_check_ms"), "monitor.process_check_ms"));
        Configuration.Monitor.ValueCheckMs = static_cast<std::uint32_t>(
            ParseUnsigned(RequiredNode(Monitor, "value_check_ms"), "monitor.value_check_ms"));
        Configuration.Monitor.RescanMs = static_cast<std::uint32_t>(
            ParseUnsigned(RequiredNode(Monitor, "rescan_ms"), "monitor.rescan_ms"));
        Configuration.Monitor.StableRescanMs = static_cast<std::uint32_t>(
            ParseUnsigned(RequiredNode(Monitor, "stable_rescan_ms"), "monitor.stable_rescan_ms"));
        Configuration.Monitor.ScanChunkBytes = static_cast<std::uint32_t>(
            ParseUnsigned(RequiredNode(Monitor, "scan_chunk_bytes"), "monitor.scan_chunk_bytes"));
        Configuration.Monitor.MaxScanBytes = ParseUnsigned(
            RequiredNode(Monitor, "max_scan_mib"),
            "monitor.max_scan_mib") * 1024ull * 1024ull;
        Configuration.Monitor.MaxObjectsPerType = static_cast<std::uint32_t>(
            ParseUnsigned(RequiredNode(Monitor, "max_objects_per_type"), "monitor.max_objects_per_type"));
        Configuration.Monitor.MaxDynamicRefreshCandidates = static_cast<std::uint32_t>(
            ParseUnsigned(
                RequiredNode(Monitor, "max_dynamic_refresh_candidates"),
                "monitor.max_dynamic_refresh_candidates"));
        Configuration.Monitor.MaxResourceValue = static_cast<std::uint32_t>(
            ParseUnsigned(RequiredNode(Monitor, "max_resource_value"), "monitor.max_resource_value"));
        Configuration.Monitor.MaxRefreshValue = static_cast<std::uint32_t>(
            ParseUnsigned(RequiredNode(Monitor, "max_refresh_value"), "monitor.max_refresh_value"));
        Configuration.Monitor.MaxWriteCandidates = static_cast<std::uint32_t>(
            ParseUnsigned(
                RequiredNode(Monitor, "max_write_candidates"),
                "monitor.max_write_candidates"));
        if (Configuration.Monitor.ScanChunkBytes < sizeof(std::uint64_t) ||
            Configuration.Monitor.ProcessCheckMs == 0 ||
            Configuration.Monitor.ValueCheckMs == 0 ||
            Configuration.Monitor.RescanMs == 0 ||
            Configuration.Monitor.StableRescanMs < Configuration.Monitor.RescanMs ||
            Configuration.Monitor.MaxScanBytes == 0 ||
            Configuration.Monitor.MaxDynamicRefreshCandidates == 0 ||
            Configuration.Monitor.MaxWriteCandidates == 0)
        {
            throw std::runtime_error("Monitor intervals and scan limits must be positive.");
        }

        const YAML::Node Defaults = RequiredNode(Root, "defaults");
        Configuration.RefreshDefaults = ParseDefaults(Defaults, "refresh_set", "refresh_add");
        Configuration.ExchangeDefaults = ParseDefaults(Defaults, "exchange_set", "exchange_add");
        Configuration.ApDefaults = ParseDefaults(Defaults, "ap_set", "ap_add");

        if (const auto Window = Root["window"]; Window && !Window.IsNull())
        {
            const auto Width = ParseUnsigned(RequiredNode(Window, "width"), "window.width");
            const auto Height = ParseUnsigned(RequiredNode(Window, "height"), "window.height");
            if (Width < 250 || Width > 2000 || Height < 200 || Height > 2000)
                throw std::runtime_error("Window dimensions must be bounded logical-pixel sizes.");
            Configuration.Window.Width = static_cast<int>(Width);
            Configuration.Window.Height = static_cast<int>(Height);
        }
        const auto Hotkeys = Root["hotkeys"];
        static constexpr std::array<const char*, 3> HotkeyKeys{
            "refresh_add", "exchange_add", "ap_add"
        };
        static constexpr std::array<const char*, 3> DefaultKeys{ "1", "2", "3" };
        for (std::size_t Index = 0; Index < HotkeyKeys.size(); ++Index)
        {
            const std::string Text = Hotkeys && !Hotkeys.IsNull()
                ? RequiredString(Hotkeys, HotkeyKeys[Index]) : DefaultKeys[Index];
            Configuration.Hotkeys[Index] = ParseHotkey(
                Text, static_cast<TrainerFeature>(Index + 1));
            for (std::size_t Previous = 0; Previous < Index; ++Previous)
            {
                if (Configuration.Hotkeys[Index] && Configuration.Hotkeys[Previous] &&
                    Configuration.Hotkeys[Index]->VirtualKey == Configuration.Hotkeys[Previous]->VirtualKey &&
                    Configuration.Hotkeys[Index]->Modifiers == Configuration.Hotkeys[Previous]->Modifiers)
                    throw std::runtime_error("Feature hotkeys must not overlap.");
            }
        }

        const YAML::Node TypeInfo = RequiredNode(Root, "type_info");
        for (const auto& Entry : TypeInfo)
        {
            const std::string Name = Entry.first.as<std::string>();
            TypeInfoConfig Descriptor;
            Descriptor.Rva = ParseUnsigned(
                RequiredNode(Entry.second, "rva"),
                "type_info." + Name + ".rva");
            Descriptor.ClassName = RequiredString(Entry.second, "class_name");
            if (Descriptor.Rva == 0 || Descriptor.ClassName.empty())
            {
                throw std::runtime_error("IL2CPP type information must include a non-zero RVA and class name.");
            }
            Configuration.TypeProfile.emplace(Name, std::move(Descriptor));
        }
        if (Configuration.IdentityMinTypeMatches > Configuration.TypeProfile.size())
        {
            throw std::runtime_error(
                "game.identity_min_type_matches cannot exceed the configured IL2CPP type profile size.");
        }

        const YAML::Node Features = RequiredNode(Root, "features");
        if (const YAML::Node Runtime = Root["puerts_runtime"];
            Runtime && !Runtime.IsNull())
        {
            PuertsRuntimeConfig Profile;
            Profile.ModelScript = RequiredString(Runtime, "model_script");
            Profile.ModelMapField = RequiredString(Runtime, "model_map_field");
            Profile.SingletonField = RequiredString(Runtime, "singleton_field");
            if (Profile.ModelScript.empty() || Profile.ModelScript.size() > 256 ||
                Profile.ModelMapField.empty() || Profile.ModelMapField.size() > 256 ||
                Profile.SingletonField.empty() || Profile.SingletonField.size() > 256)
            {
                throw std::runtime_error("The Puerts model identity must contain bounded, non-empty names.");
            }
            Configuration.PuertsRuntime = std::move(Profile);
        }
        const YAML::Node Refresh = RequiredNode(Features, "refresh");
        if (const YAML::Node PuertsArray = Refresh["puerts_array"];
            PuertsArray && !PuertsArray.IsNull())
        {
            PuertsArrayConfig Array;
            const std::uint64_t Length = ParseUnsigned(
                RequiredNode(PuertsArray, "length"),
                "features.refresh.puerts_array.length");
            const std::uint64_t ResourceIndex = ParseUnsigned(
                RequiredNode(PuertsArray, "resource_index"),
                "features.refresh.puerts_array.resource_index");
            Array.ObjectLengthOffset = ParseUnsigned(
                RequiredNode(PuertsArray, "object_length_offset"),
                "features.refresh.puerts_array.object_length_offset");
            Array.ElementsOffset = ParseUnsigned(
                RequiredNode(PuertsArray, "elements_offset"),
                "features.refresh.puerts_array.elements_offset");
            Array.EntryStride = ParseUnsigned(
                RequiredNode(PuertsArray, "entry_stride"),
                "features.refresh.puerts_array.entry_stride");
            Array.ValueHighWordOffset = ParseUnsigned(
                RequiredNode(PuertsArray, "value_high_word_offset"),
                "features.refresh.puerts_array.value_high_word_offset");
            Array.MapInstanceTypeOffset = ParseUnsigned(
                RequiredNode(PuertsArray, "map_instance_type_offset"),
                "features.refresh.puerts_array.map_instance_type_offset");
            Array.ParentPropertiesOffset = ParseUnsigned(
                RequiredNode(PuertsArray, "parent_properties_offset"),
                "features.refresh.puerts_array.parent_properties_offset");
            Array.ParentElementsOffset = ParseUnsigned(
                RequiredNode(PuertsArray, "parent_elements_offset"),
                "features.refresh.puerts_array.parent_elements_offset");
            const std::uint64_t InstanceType = ParseUnsigned(
                RequiredNode(PuertsArray, "fixed_array_instance_type"),
                "features.refresh.puerts_array.fixed_array_instance_type");
            const std::uint64_t ParentInstanceType = ParseUnsigned(
                RequiredNode(PuertsArray, "parent_instance_type"),
                "features.refresh.puerts_array.parent_instance_type");
            const std::uint64_t MaximumAddress =
                std::numeric_limits<std::uintptr_t>::max();
            const bool SlotOffsetOverflow =
                Array.EntryStride == 0 ||
                ResourceIndex >
                    (std::numeric_limits<std::uint64_t>::max() - Array.ElementsOffset) /
                        Array.EntryStride;
            const std::uint64_t ValueOffset = SlotOffsetOverflow
                ? 0
                : Array.ElementsOffset + ResourceIndex * Array.EntryStride;
            if (Length == 0 ||
                Length > std::numeric_limits<std::uint32_t>::max() ||
                ResourceIndex >= Length ||
                ResourceIndex > std::numeric_limits<std::uint32_t>::max() ||
                Array.ObjectLengthOffset == 0 ||
                Array.ElementsOffset == 0 ||
                Array.EntryStride < sizeof(std::uint64_t) ||
                Array.ValueHighWordOffset > Array.EntryStride - sizeof(std::uint32_t) ||
                Array.MapInstanceTypeOffset == 0 ||
                Array.MapInstanceTypeOffset >
                    MaximumAddress - sizeof(std::uint16_t) ||
                Array.ParentElementsOffset == 0 ||
                Array.ParentPropertiesOffset == 0 ||
                Array.ParentElementsOffset >
                    MaximumAddress - sizeof(std::uintptr_t) ||
                Array.ParentPropertiesOffset >
                    MaximumAddress - sizeof(std::uint64_t) ||
                Array.ObjectLengthOffset >
                    MaximumAddress - sizeof(std::uint64_t) ||
                SlotOffsetOverflow ||
                ValueOffset > MaximumAddress - sizeof(std::uint64_t) ||
                Array.ValueHighWordOffset > MaximumAddress - ValueOffset ||
                InstanceType == 0 ||
                InstanceType > std::numeric_limits<std::uint16_t>::max() ||
                ParentInstanceType == 0 ||
                ParentInstanceType > std::numeric_limits<std::uint16_t>::max())
            {
                throw std::runtime_error("features.refresh.puerts_array layout is invalid.");
            }
            Array.Length = static_cast<std::uint32_t>(Length);
            Array.ResourceIndex = static_cast<std::uint32_t>(ResourceIndex);
            Array.FixedArrayInstanceType = static_cast<std::uint16_t>(InstanceType);
            Array.ParentInstanceType = static_cast<std::uint16_t>(ParentInstanceType);
            Configuration.PuertsArray = Array;
        }
        if (const auto ScanSeed = OptionalUnsigned(Refresh, "scan_seed"))
        {
            if (*ScanSeed == 0 || *ScanSeed > Configuration.Monitor.MaxRefreshValue)
            {
                throw std::runtime_error(
                    "features.refresh.scan_seed must be positive and within monitor.max_refresh_value.");
            }
            Configuration.RefreshScanSeed = static_cast<std::uint32_t>(*ScanSeed);
        }
        for (const auto& RootNode : RequiredNode(Refresh, "roots"))
        {
            RefreshRootConfig RefreshRoot;
            RefreshRoot.Type = RequiredString(RootNode, "type");
            RefreshRoot.ActiveInfoType = RequiredString(RootNode, "active_info_type");
            RefreshRoot.ActiveInfoOffset = ParseUnsigned(
                RequiredNode(RootNode, "active_info_offset"),
                "features.refresh.roots.active_info_offset");
            RefreshRoot.FinishedFlagOffset = OptionalUnsigned(RootNode, "finished_flag_offset");
            if (RefreshRoot.ActiveInfoOffset == 0 ||
                Configuration.TypeProfile.find(RefreshRoot.Type) == Configuration.TypeProfile.end() ||
                Configuration.TypeProfile.find(RefreshRoot.ActiveInfoType) == Configuration.TypeProfile.end())
            {
                throw std::runtime_error(
                    "A refresh root references an unknown root or active-info IL2CPP type.");
            }
            if (const YAML::Node Fields = RootNode["fields"]; Fields && !Fields.IsNull())
            {
                if (!Fields.IsSequence())
                {
                    throw std::runtime_error("features.refresh.roots.fields must be a sequence.");
                }
                for (const auto& FieldNode : Fields)
                {
                    RefreshFieldConfig Field;
                    Field.Offset = ParseUnsigned(
                        RequiredNode(FieldNode, "offset"),
                        "features.refresh.roots.fields.offset");
                    Field.Semantics = ParseSemantics(RequiredNode(FieldNode, "semantics"));
                    for (const auto& PathNode : RequiredNode(FieldNode, "path"))
                    {
                        RefreshFieldConfig::PathStep Step;
                        Step.Type = RequiredString(PathNode, "type");
                        Step.Offset = ParseUnsigned(
                            RequiredNode(PathNode, "offset"),
                            "features.refresh.roots.fields.path.offset");
                        const std::string Kind = RequiredString(PathNode, "kind");
                        if (Kind == "pointer")
                        {
                            Step.Repeated = false;
                        }
                        else if (Kind == "repeated")
                        {
                            Step.Repeated = true;
                        }
                        else
                        {
                            throw std::runtime_error(
                                "Refresh path kind must be pointer or repeated.");
                        }
                        Step.UnfinishedFlagOffset =
                            OptionalUnsigned(PathNode, "unfinished_flag_offset");
                        if (Step.Offset == 0 ||
                            Configuration.TypeProfile.find(Step.Type) == Configuration.TypeProfile.end())
                        {
                            throw std::runtime_error(
                                "Refresh path references an unknown IL2CPP type: " + Step.Type);
                        }
                        Field.Path.push_back(std::move(Step));
                    }
                    if (Field.Path.empty())
                    {
                        throw std::runtime_error("Every refresh field must have a rooted object path.");
                    }
                    RefreshRoot.Fields.push_back(std::move(Field));
                }
            }

            if (const YAML::Node MapNode = RootNode["resource_map"];
                MapNode && !MapNode.IsNull())
            {
                RefreshRootConfig::ResourceMap MapInfo;
                MapInfo.Type = RequiredString(MapNode, "type");
                MapInfo.InfoOffset = ParseUnsigned(
                    RequiredNode(MapNode, "info_offset"),
                    "features.refresh.roots.resource_map.info_offset");
                MapInfo.MapFieldOffset = ParseUnsigned(
                    RequiredNode(MapNode, "map_field_offset"),
                    "features.refresh.roots.resource_map.map_field_offset");
                MapInfo.DictionaryOffset = ParseUnsigned(
                    RequiredNode(MapNode, "dictionary_offset"),
                    "features.refresh.roots.resource_map.dictionary_offset");
                MapInfo.DictionaryEntriesOffset = ParseUnsigned(
                    RequiredNode(MapNode, "dictionary_entries_offset"),
                    "features.refresh.roots.resource_map.dictionary_entries_offset");
                MapInfo.DictionaryCountOffset = ParseUnsigned(
                    RequiredNode(MapNode, "dictionary_count_offset"),
                    "features.refresh.roots.resource_map.dictionary_count_offset");
                MapInfo.ArrayLengthOffset = ParseUnsigned(
                    RequiredNode(MapNode, "array_length_offset"),
                    "features.refresh.roots.resource_map.array_length_offset");
                MapInfo.ArrayDataOffset = ParseUnsigned(
                    RequiredNode(MapNode, "array_data_offset"),
                    "features.refresh.roots.resource_map.array_data_offset");
                MapInfo.EntryStride = ParseUnsigned(
                    RequiredNode(MapNode, "entry_stride"),
                    "features.refresh.roots.resource_map.entry_stride");
                MapInfo.EntryHashOffset = ParseUnsigned(
                    RequiredNode(MapNode, "entry_hash_offset"),
                    "features.refresh.roots.resource_map.entry_hash_offset");
                MapInfo.EntryKeyOffset = ParseUnsigned(
                    RequiredNode(MapNode, "entry_key_offset"),
                    "features.refresh.roots.resource_map.entry_key_offset");
                MapInfo.EntryNodeOffset = ParseUnsigned(
                    RequiredNode(MapNode, "entry_node_offset"),
                    "features.refresh.roots.resource_map.entry_node_offset");
                MapInfo.MapListOffset = ParseUnsigned(
                    RequiredNode(MapNode, "map_list_offset"),
                    "features.refresh.roots.resource_map.map_list_offset");
                MapInfo.NodeListOffset = ParseUnsigned(
                    RequiredNode(MapNode, "node_list_offset"),
                    "features.refresh.roots.resource_map.node_list_offset");
                MapInfo.NodeKeyOffset = ParseUnsigned(
                    RequiredNode(MapNode, "node_key_offset"),
                    "features.refresh.roots.resource_map.node_key_offset");
                MapInfo.NodeValueOffset = ParseUnsigned(
                    RequiredNode(MapNode, "node_value_offset"),
                    "features.refresh.roots.resource_map.node_value_offset");
                const std::uint64_t KeyValue = ParseUnsigned(
                    RequiredNode(MapNode, "entry_key_value"),
                    "features.refresh.roots.resource_map.entry_key_value");
                if (MapInfo.Type.empty() ||
                    Configuration.TypeProfile.find(MapInfo.Type) == Configuration.TypeProfile.end() ||
                    MapInfo.InfoOffset == 0 ||
                    MapInfo.MapFieldOffset == 0 ||
                    MapInfo.DictionaryOffset == 0 ||
                    MapInfo.DictionaryEntriesOffset == 0 ||
                    MapInfo.DictionaryCountOffset == 0 ||
                    MapInfo.ArrayLengthOffset == 0 ||
                    MapInfo.ArrayDataOffset == 0 ||
                    MapInfo.EntryStride < sizeof(std::uint32_t) ||
                    MapInfo.EntryKeyOffset > MapInfo.EntryStride - sizeof(std::uint32_t) ||
                    MapInfo.EntryNodeOffset > MapInfo.EntryStride - sizeof(std::uintptr_t) ||
                    MapInfo.EntryHashOffset > MapInfo.EntryStride - sizeof(std::int32_t) ||
                    MapInfo.MapListOffset == 0 ||
                    MapInfo.NodeListOffset == 0 ||
                    MapInfo.NodeKeyOffset == 0 ||
                    MapInfo.NodeValueOffset <= MapInfo.NodeKeyOffset ||
                    MapInfo.NodeValueOffset - MapInfo.NodeKeyOffset < sizeof(std::uint32_t) ||
                    KeyValue > std::numeric_limits<std::uint32_t>::max())
                {
                    throw std::runtime_error(
                        "The refresh resource-map layout is invalid or references an unknown IL2CPP type.");
                }
                MapInfo.EntryKeyValue = static_cast<std::uint32_t>(KeyValue);
                RefreshRoot.ResourceDictionary = std::move(MapInfo);
            }

            if (RefreshRoot.Fields.empty() && !RefreshRoot.ResourceDictionary)
            {
                throw std::runtime_error(
                    "Every refresh root must define a rooted field or resource map.");
            }
            Configuration.RefreshRoots.push_back(std::move(RefreshRoot));
        }
        if (Configuration.RefreshRoots.empty() && !Configuration.PuertsArray)
        {
            throw std::runtime_error(
                "A single-player refresh root or validated Puerts array layout must be configured.");
        }

        const YAML::Node Exchange = RequiredNode(Features, "exchange");
        if (const auto Index = OptionalUnsigned(Exchange, "puerts_resource_index"))
        {
            if (!Configuration.PuertsRuntime || !Configuration.PuertsArray ||
                *Index >= Configuration.PuertsArray->Length ||
                *Index == Configuration.PuertsArray->ResourceIndex ||
                !Configuration.RefreshRoots.empty() || Configuration.RefreshScanSeed)
            {
                throw std::runtime_error(
                    "The Puerts exchange slot requires a shared runtime model, a distinct array index, and no legacy refresh roots or value seed.");
            }
            Configuration.ExchangePuertsIndex = static_cast<std::uint32_t>(*Index);
            const PuertsArrayConfig& Layout = *Configuration.PuertsArray;
            if (Layout.ObjectLengthOffset != 8 || Layout.ElementsOffset != 0x10 ||
                Layout.EntryStride != 8 || Layout.ValueHighWordOffset != 4 ||
                Layout.MapInstanceTypeOffset != 0xc ||
                Layout.ParentPropertiesOffset != 8 || Layout.ParentElementsOffset != 0x10 ||
                Layout.FixedArrayInstanceType != 0x77 || Layout.ParentInstanceType != 0x421)
            {
                throw std::runtime_error(
                    "The named Puerts runtime profile requires the verified 64-bit V8 heap layout.");
            }
        }
        else
        {
            if (Configuration.PuertsRuntime)
            {
                throw std::runtime_error("A Puerts runtime profile requires an exchange resource index.");
            }
            Configuration.Exchange = ParseResourceTarget(Exchange, "features.exchange");
            if (!Configuration.Exchange.RootType)
            {
                throw std::runtime_error("The exchange-point target must be rooted in a validated owner object.");
            }
            if (Configuration.TypeProfile.find(Configuration.Exchange.Type) == Configuration.TypeProfile.end())
            {
                throw std::runtime_error("The exchange-point feature references an unknown IL2CPP type.");
            }
            if (Configuration.TypeProfile.find(*Configuration.Exchange.RootType) == Configuration.TypeProfile.end())
            {
                throw std::runtime_error("The exchange-point root type is not configured.");
            }
        }

        if (const YAML::Node Ap = Features["ap"]; Ap && !Ap.IsNull())
        {
            PuertsApConfig Profile;
            Profile.ModelScript = RequiredString(Ap, "model_script");
            Profile.PrototypeMethod = RequiredString(Ap, "prototype_method");
            const auto Id = ParseUnsigned(
                RequiredNode(Ap, "currency_id"), "features.ap.currency_id");
            if (!Configuration.PuertsRuntime || !Configuration.PuertsArray ||
                Profile.ModelScript.empty() || Profile.ModelScript.size() > 256 ||
                Profile.ModelScript == Configuration.PuertsRuntime->ModelScript ||
                Profile.PrototypeMethod.empty() || Profile.PrototypeMethod.size() > 256 ||
                Id == 0 || Id >= Configuration.Monitor.MaxObjectsPerType ||
                Configuration.Monitor.MaxResourceValue > INT32_MAX)
            {
                throw std::runtime_error(
                    "The AP currency profile requires a distinct named model, a bounded prototype method and currency key, and signed Smi counts.");
            }
            Profile.CurrencyId = static_cast<std::uint32_t>(Id);
            Configuration.PuertsAp = std::move(Profile);
        }

        return Configuration;
    }
}
