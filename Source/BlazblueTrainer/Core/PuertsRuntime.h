#pragma once

#include "Config.h"
#include "RemoteProcess.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <stop_token>
#include <string>
#include <vector>

namespace Trainer
{
    using PuertsReadMemory =
        std::function<bool(std::uintptr_t, void*, std::size_t)>;

    struct PuertsMemoryGuard
    {
        std::uintptr_t Address{};
        std::uint64_t Expected{};
        std::uint8_t Width{};

        bool operator==(const PuertsMemoryGuard&) const = default;
    };

    enum class PuertsResourceKind
    {
        Dungeon,
        Ap
    };

    struct PuertsResourceTarget
    {
        PuertsResourceKind Kind{ PuertsResourceKind::Dungeon };
        std::uintptr_t ModelConstructor{};
        std::uintptr_t ModelObject{};
        std::uintptr_t ArrayObject{};
        std::uintptr_t ValueObject{};
        std::uintptr_t ValueAddress{};
        std::uintptr_t SaveDirtyAddress{};
        std::uintptr_t SaveDirtyTrueValue{};
        std::uint32_t ResourceIndex{};
        std::uint32_t IntegerValue{};
        std::vector<PuertsMemoryGuard> Guards;
    };

    class PuertsRuntimeReader final
    {
    public:
        PuertsRuntimeReader(const Config& Configuration, PuertsReadMemory Read);

        bool IsModelClassMap(std::uintptr_t TaggedMap) const;
        bool IsModelConstructor(std::uintptr_t TaggedConstructor) const;
        bool IsApModelConstructor(std::uintptr_t TaggedConstructor) const;
        std::optional<PuertsResourceTarget> ResolveResource(
            std::uintptr_t TaggedConstructor,
            std::uint32_t ResourceIndex,
            std::uint32_t Maximum) const;
        std::optional<PuertsResourceTarget> ResolveAp(
            std::uintptr_t TaggedConstructor,
            std::uint32_t Maximum) const;
        std::optional<std::uint32_t> ReadResource(
            const PuertsResourceTarget& Expected,
            std::uint32_t Maximum) const;

    private:
        struct Map;
        struct Property;
        bool ReadRaw(std::uintptr_t Address, void* Output, std::size_t Size) const;
        std::optional<std::uint64_t> ReadGuarded(
            std::uintptr_t Address,
            std::uint8_t Width,
            std::vector<PuertsMemoryGuard>* Guards) const;
        std::optional<Map> ReadMap(
            std::uintptr_t TaggedMap,
            std::vector<PuertsMemoryGuard>* Guards) const;
        std::optional<std::string> ReadString(
            std::uintptr_t TaggedString,
            unsigned Depth = 0) const;
        std::optional<Property> OwnProperty(
            std::uintptr_t TaggedObject,
            const std::string& Name,
            std::vector<PuertsMemoryGuard>* Guards,
            bool GuardValue = true) const;
        std::optional<std::uint64_t> PropertyValue(
            std::uintptr_t TaggedObject,
            const std::string& Name,
            std::vector<PuertsMemoryGuard>& Guards,
            bool Inherited = false) const;
        bool ModelIdentity(
            std::uintptr_t TaggedConstructor,
            std::vector<PuertsMemoryGuard>* Guards,
            PuertsResourceKind Kind = PuertsResourceKind::Dungeon) const;
        std::optional<std::uintptr_t> ModelInstance(
            std::uintptr_t TaggedConstructor,
            PuertsResourceKind Kind,
            std::vector<PuertsMemoryGuard>& Guards) const;
        std::optional<bool> BooleanValue(std::uintptr_t TaggedBoolean) const;
        std::optional<std::uintptr_t> CanonicalTrue(
            std::uintptr_t TaggedBoolean) const;
        bool ValidateGuards(const std::vector<PuertsMemoryGuard>& Guards) const;

        const Config& Settings;
        PuertsReadMemory ReadMemoryCallback;
    };

    struct PuertsModelScanResult
    {
        std::vector<std::uintptr_t> Constructors;
        std::vector<std::uintptr_t> ApConstructors;
        std::size_t ClassMaps{};
        std::uint64_t BytesScanned{};
        std::size_t ChangedRegions{};
        bool Complete{};
        std::string Error;
    };

    PuertsModelScanResult ScanPuertsModelConstructors(
        const Config& Configuration,
        const std::shared_ptr<RemoteProcess>& Process,
        std::stop_token StopToken);
}
