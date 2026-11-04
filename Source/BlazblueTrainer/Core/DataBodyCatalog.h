#pragma once

#include "GameData.h"
#include "Localization.h"

#include <cstdint>
#include <map>
#include <set>
#include <vector>

namespace Trainer
{
	struct StrategyChoice
	{
		std::uint32_t Id{};
		std::uint32_t Quality{4};
		bool operator==(const StrategyChoice &) const = default;
	};

	struct BodyAttribute
	{
		std::uint32_t Type{};
		double Value{};
	};

	struct BodyTalent
	{
		std::uint32_t Id{};
		std::uint32_t Quality{1};
		std::uint32_t InheritNumber{};
		std::uint32_t FromActor{};
	};

	struct CatalogActor
	{
		std::uint32_t Id{};
		LocalizedText Name;
		std::vector<BodyAttribute> Attributes;
		std::vector<BodyTalent> Talents;
		std::vector<std::uint32_t> InitialPotentials;
		std::vector<std::uint32_t> InheritanceSkills;
	};

	struct CatalogStrategy
	{
		std::uint32_t Id{};
		LocalizedText Name;
		std::uint32_t Gradation{};
		std::uint32_t TriggerGroup{};
		std::uint32_t Faction{};
		std::set<std::uint32_t> Qualities;
		std::vector<std::uint32_t> Labels;
		std::vector<std::uint32_t> AcquisitionLabels;
		std::vector<std::uint32_t> InheritanceLabels;
		std::vector<std::uint32_t> Elements;

		bool IsDual() const { return Elements.size() > 1; }
	};

	struct StrategyFilter
	{
		std::string Search;
		std::optional<std::uint32_t> Element;
		std::optional<std::uint32_t> Trigger;
		std::optional<bool> Dual;
		std::optional<std::uint32_t> Tier;
	};

	struct CompletedStrategies
	{
		std::vector<StrategyChoice> Choices;
		std::set<std::uint32_t> AutomaticIds;
		std::set<std::uint32_t> ActiveIds;
	};

	class DataBodyCatalog final
	{
	  public:
		static DataBodyCatalog Load(const std::filesystem::path &GameDirectory);
		const CatalogActor &Actor(std::uint32_t Id) const;
		const CatalogStrategy &Strategy(std::uint32_t Id) const;
		CompletedStrategies CompleteStrategies(const std::vector<StrategyChoice> &Targets,
											   const std::vector<StrategyChoice> &Inherited) const;
		void ValidateInheritance(const std::vector<StrategyChoice>& Choices) const;
		std::vector<std::vector<std::uint32_t>> InheritanceConditions(
			std::uint32_t StrategyId, const std::vector<StrategyChoice>& Inherited) const;
		std::string InheritanceSkillName(std::uint32_t Id, TrainerLanguage Language) const;
		std::vector<std::uint32_t> Providers(std::uint32_t Label) const;
		std::vector<std::uint32_t> FilterStrategies(const StrategyFilter& Filter) const;
		std::size_t Capacity(std::uint64_t TotalScore) const;
		static const std::string &Name(const LocalizedText &Text, TrainerLanguage Language);

		std::map<std::uint32_t, CatalogActor> Actors;
		std::map<std::uint32_t, CatalogStrategy> Strategies;
		std::map<std::uint32_t, LocalizedText> SkillNames;
		std::vector<std::uint64_t> PageScores;
		std::size_t PageSize{30};
	};
} // namespace Trainer
