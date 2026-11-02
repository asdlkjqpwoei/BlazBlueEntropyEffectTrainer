#pragma once

#include "DataBodyCatalog.h"

#include <functional>
#include <optional>

namespace Trainer
{
    struct DataBodyInfo
    {
        std::uint32_t Uid{};
        std::uint32_t ActorId{};
        double Score{};
        std::vector<StrategyChoice> Inherited;
        std::uint32_t InheritanceSkillIndex{};
        std::uint32_t InheritanceSkillLevel{};
    };

    struct DataBodyRequest
    {
        std::optional<std::uint32_t> ExistingUid;
        std::uint32_t ActorId{};
        double Score{600};
        std::vector<StrategyChoice> Inherited;
        std::optional<std::uint32_t> InheritanceSkillId;
    };

    struct DataBodyPlan
    {
        std::uint32_t Uid{};
        bool Added{};
        Bytes Updated;
        std::vector<StrategyChoice> CompletedOwned;
        std::uint64_t SourceGeneration{};
    };

    class DataBodySave final
    {
      public:
        using StoppedGuard = std::function<void()>;
        DataBodySave(std::filesystem::path Path, StoppedGuard Guard);
        const std::filesystem::path &Path() const
        {
            return FilePath;
        }
        std::vector<DataBodyInfo> Bodies() const;
        std::size_t Capacity(const DataBodyCatalog &Catalog) const;
        DataBodyPlan Prepare(const DataBodyCatalog &Catalog, const DataBodyRequest &Request) const;
        std::filesystem::path Commit(const DataBodyPlan &Plan, bool Confirmed);

      private:
        std::filesystem::path FilePath;
        SaveDocument Document;
        StoppedGuard AssertStopped;
        std::uint64_t Generation{};
    };
} // namespace Trainer
