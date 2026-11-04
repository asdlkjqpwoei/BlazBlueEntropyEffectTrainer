#pragma once

#include "DataBodyCatalog.h"

#include <functional>
#include <array>
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

		bool operator==(const DataBodyInfo&) const = default;
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
		std::vector<std::uint32_t> ChangedUids;
	};

	// UI drafts keep empty slots and temporarily invalid combinations until final review.
	struct DataBodyDraft
	{
		std::uint32_t Uid{};
		std::uint32_t ActorId{};
		double Score{};
		std::array<std::optional<StrategyChoice>, 2> Inherited;
		std::uint32_t SkillIndex{};
		std::uint32_t SkillLevel{};
		bool Added{};
		bool operator==(const DataBodyDraft&) const = default;
		DataBodyRequest Request(const DataBodyCatalog& Catalog) const;
	};

	class DataBodyDrafts final
	{
	public:
		explicit DataBodyDrafts(const std::vector<DataBodyInfo>& Bodies);
		const std::vector<DataBodyDraft>& Bodies() const { return Current; }
		DataBodyDraft& Body(std::uint32_t Uid);
		bool Changed(std::uint32_t Uid) const;
		std::size_t ChangeCount() const;
		void Reset();
		std::uint32_t Add(const DataBodyRequest& Request, const DataBodyCatalog& Catalog);
		std::vector<DataBodyRequest> Requests(const DataBodyCatalog& Catalog) const;
	private:
		std::vector<DataBodyDraft> Original;
		std::vector<DataBodyDraft> Current;
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
		DataBodyPlan PrepareBatch(const DataBodyCatalog& Catalog,
			const std::vector<DataBodyRequest>& Requests) const;
		std::filesystem::path Commit(const DataBodyPlan &Plan, bool Confirmed);

	  private:
		std::filesystem::path FilePath;
		SaveDocument Document;
		StoppedGuard AssertStopped;
		std::uint64_t Generation{};
	};
} // namespace Trainer
