#include "DataBodyCatalog.h"

#include <algorithm>
#include <functional>
#include <stdexcept>
#include <unordered_map>

namespace Trainer
{
    namespace
    {
        std::uint32_t U32(std::span<const std::uint8_t> Data, std::size_t Offset)
        {
            if (Offset > Data.size() || Data.size() - Offset < 4)
                throw std::runtime_error("Truncated catalog table.");
            return static_cast<std::uint32_t>(Data[Offset]) | (static_cast<std::uint32_t>(Data[Offset + 1]) << 8) |
                   (static_cast<std::uint32_t>(Data[Offset + 2]) << 16) |
                   (static_cast<std::uint32_t>(Data[Offset + 3]) << 24);
        }

        std::vector<ProtoMessage> Table(const GameAssets &Assets, const std::string &Name)
        {
            const auto Data = Assets.TextAsset("data/xlsx/" + Name + ".ab");
            std::vector<ProtoMessage> Result;
            std::size_t Position = 0;
            while (Position < Data.size())
            {
                const auto Size = U32(Data, Position);
                Position += 4;
                if (Size > Data.size() - Position)
                    throw std::runtime_error("Truncated catalog table row.");
                Result.push_back(ProtoMessage::Parse(std::span<const std::uint8_t>(Data).subspan(Position, Size)));
                Position += Size;
            }
            return Result;
        }

        std::unordered_map<std::uint32_t, std::string> ReadLocale(const GameAssets &Assets, const char *Language)
        {
            const auto Data = Assets.TextAsset(std::string("data/localization/localization_") + Language + ".ab");
            std::unordered_map<std::uint32_t, std::string> Result;
            std::size_t Position = 0;
            while (Position < Data.size())
            {
                const auto Key = U32(Data, Position);
                const auto Size = U32(Data, Position + 4);
                Position += 8;
                if (Size > Data.size() - Position)
                    throw std::runtime_error("Truncated game localization table.");
                Result.emplace(Key, std::string(Data.begin() + Position, Data.begin() + Position + Size));
                Position += Size;
            }
            return Result;
        }

        std::uint32_t LocaleHash(const std::string &Key)
        {
            std::uint32_t Result = 0x1234567;
            for (const unsigned char Character : Key)
                Result = (Result ^ Character) * 0x89abcdefu;
            return Result * 0x89abcdefu;
        }

        bool Contains(const std::vector<std::uint32_t> &Values, std::uint32_t Value)
        {
            return std::find(Values.begin(), Values.end(), Value) != Values.end();
        }

        std::uint32_t Integer32(const ProtoMessage &Row, std::uint32_t Field)
        {
            const auto Value = Row.Integer(Field);
            if (Value > UINT32_MAX)
                throw std::runtime_error("Catalog identifier exceeds uint32.");
            return static_cast<std::uint32_t>(Value);
        }
    } // namespace

    DataBodyCatalog DataBodyCatalog::Load(const std::filesystem::path &GameDirectory)
    {
        const GameAssets Assets(GameDirectory);
        const auto Chinese = ReadLocale(Assets, "chs");
        const auto English = ReadLocale(Assets, "enu");
        DataBodyCatalog Result;
        const auto AttributeRows = Table(Assets, "baseactorattrconf");
        const auto TalentRows = Table(Assets, "baseactortalentqualityconf");
        for (const auto &Row : Table(Assets, "baseactorconf"))
        {
            CatalogActor Actor;
            Actor.Id = Integer32(Row, 1);
            Actor.Name.Chinese = Row.String(99);
            if (!Actor.Id || Actor.Name.Chinese.empty())
                continue;
            Actor.Name.English = "Character " + std::to_string(Actor.Id);
            for (const auto &[Key, Text] : Chinese)
            {
                if (Text == Actor.Name.Chinese)
                {
                    const auto Found = English.find(Key);
                    if (Found != English.end() && !Found->second.empty() && Found->second.size() < 100)
                    {
                        Actor.Name.English = Found->second;
                        break;
                    }
                }
            }
            for (const auto &AttributeRow : AttributeRows)
            {
                if (AttributeRow.Integer(1) != Actor.Id)
                    continue;
                for (const auto &Field : AttributeRow.Select(2))
                {
                    const auto Attribute = ProtoMessage::Parse(Field.Data);
                    Actor.Attributes.push_back({Integer32(Attribute, 1), Attribute.Double(3)});
                }
            }
            if (Actor.Attributes.empty())
                continue;
            for (const auto &Field : Row.Select(17))
            {
                const auto Talent = ProtoMessage::Parse(Field.Data);
                BodyTalent Entry{Integer32(Talent, 1), Integer32(Talent, 2), 0, 0};
                for (const auto &Quality : TalentRows)
                    if (Quality.Integer(1) == Entry.Id && Quality.Integer(2) == Entry.Quality)
                        Entry.InheritNumber = Integer32(Quality, 8);
                Actor.Talents.push_back(Entry);
            }
            Actor.InheritanceSkills = Row.PackedIntegers(27);
            for (const auto Id : Actor.InheritanceSkills)
            {
                LocalizedText Name{"传承技 " + std::to_string(Id), "Inheritance skill " + std::to_string(Id)};
                const auto Key = LocaleHash("InheritSkill_" + std::to_string(Id));
                if (const auto Found = Chinese.find(Key); Found != Chinese.end())
                    Name.Chinese = Found->second;
                if (const auto Found = English.find(Key); Found != English.end())
                    Name.English = Found->second;
                Result.SkillNames.emplace(Id, std::move(Name));
            }
            Result.Actors.emplace(Actor.Id, std::move(Actor));
        }
        for (const auto &Row : Table(Assets, "baseactorpotentialconf"))
        {
            const auto Actor = Result.Actors.find(Integer32(Row, 17));
            if (Actor == Result.Actors.end() || Row.Integer(21))
                continue;
            const auto Id = Integer32(Row, 1);
            if (Id && Row.Integer(23) && !Contains(Actor->second.InitialPotentials, Id))
                Actor->second.InitialPotentials.push_back(Id);
        }
        for (const auto &Row : Table(Assets, "blessconf"))
        {
            CatalogStrategy Strategy;
            Strategy.Id = Integer32(Row, 1);
            Strategy.TriggerGroup = Integer32(Row, 2);
            Strategy.Gradation = Integer32(Row, 4);
            Strategy.Faction = Integer32(Row, 13);
            Strategy.Labels = Row.PackedIntegers(5);
            Strategy.AcquisitionLabels = Row.PackedIntegers(6);
            Strategy.InheritanceLabels = Row.PackedIntegers(16);
            if (!Strategy.Id || Row.Integer(15) || Row.Integer(8) || Strategy.Gradation == 0 || Strategy.Gradation > 4)
                continue;
            Strategy.Name = {"策略 " + std::to_string(Strategy.Id), "Strategy " + std::to_string(Strategy.Id)};
            Result.Strategies.emplace(Strategy.Id, std::move(Strategy));
        }
        for (const auto &Row : Table(Assets, "blessqualityconf"))
        {
            const auto Found = Result.Strategies.find(Integer32(Row, 1));
            const auto Quality = Integer32(Row, 2);
            if (Found == Result.Strategies.end() || Quality < 1 || Quality > 4)
                continue;
            Found->second.Qualities.insert(Quality);
            const auto Key = LocaleHash("TriggerName_" + std::to_string(Row.Integer(4)));
            if (const auto Name = Chinese.find(Key); Name != Chinese.end())
                Found->second.Name.Chinese = Name->second;
            if (const auto Name = English.find(Key); Name != English.end())
                Found->second.Name.English = Name->second;
        }
        for (const auto &Row : Table(Assets, "fesactorpageunlockconf"))
        {
            if (Row.Integer(1) != 0 && Row.Integer(1) != 3)
                throw std::runtime_error("Unsupported current-game data-body warehouse unlock rule.");
            const auto Score = Row.Integer(1) == 0 ? 0 : Row.Integer(2);
            const auto Pages = Row.Integer(5);
            if (Pages > 100 || Result.PageScores.size() + Pages > 100)
                throw std::runtime_error("Invalid data-body warehouse page count.");
            for (std::uint64_t Index = 0; Index < Pages; ++Index)
                Result.PageScores.push_back(Score);
        }
        if (Result.Actors.empty() || Result.Strategies.empty() || Result.PageScores.empty())
            throw std::runtime_error("The current game does not provide a complete data-body catalog.");
        return Result;
    }

    const CatalogActor &DataBodyCatalog::Actor(std::uint32_t Id) const
    {
        const auto Found = Actors.find(Id);
        if (Found == Actors.end())
            throw std::runtime_error("Select a character from the current game catalog.");
        return Found->second;
    }

    const CatalogStrategy &DataBodyCatalog::Strategy(std::uint32_t Id) const
    {
        const auto Found = Strategies.find(Id);
        if (Found == Strategies.end() || Found->second.Qualities.empty())
            throw std::runtime_error("Select a strategy from the current game catalog.");
        return Found->second;
    }

    const std::string &DataBodyCatalog::Name(const LocalizedText &Text, TrainerLanguage Language)
    {
        return Language == TrainerLanguageEnglish ? Text.English : Text.Chinese;
    }

    std::vector<std::uint32_t> DataBodyCatalog::Providers(std::uint32_t Label) const
    {
        std::vector<std::uint32_t> Result;
        for (const auto &[Id, Row] : Strategies)
            if (!Row.Qualities.empty() && Contains(Row.Labels, Label))
                Result.push_back(Id);
        return Result;
    }

    CompletedStrategies DataBodyCatalog::CompleteStrategies(const std::vector<StrategyChoice> &Targets,
                                                            const std::vector<StrategyChoice> &Inherited) const
    {
        if (Inherited.size() > 2 || (Inherited.size() == 2 && Inherited[0].Id == Inherited[1].Id))
            throw std::runtime_error("Select two different inheritance strategies.");
        if (Targets.size() > 150)
            throw std::runtime_error("Too many owned strategies.");
        std::map<std::uint32_t, StrategyChoice> Selected;
        const auto Validate = [&](const StrategyChoice &Choice) {
            if (!Strategy(Choice.Id).Qualities.contains(Choice.Quality))
                throw std::runtime_error("The selected strategy quality is not valid in the current game.");
        };
        for (const auto &Choice : Targets)
        {
            Validate(Choice);
            if (!Selected.emplace(Choice.Id, Choice).second)
                throw std::runtime_error("Owned strategies must not be duplicated.");
        }
        for (const auto &Choice : Inherited)
        {
            Validate(Choice);
            const auto [Found, Inserted] = Selected.emplace(Choice.Id, Choice);
            if (!Inserted && Found->second.Quality != Choice.Quality)
                throw std::runtime_error("An inheritance strategy must match its owned quality.");
        }
        const auto HasConflict = [&](std::uint32_t Candidate) {
            const auto &Row = Strategy(Candidate);
            if (!Row.TriggerGroup || Row.Gradation != 1)
                return false;
            for (const auto &[Id, Choice] : Selected)
            {
                const auto &Existing = Strategy(Id);
                if (Id != Candidate && Existing.Gradation == 1 && Existing.TriggerGroup &&
                    (Existing.TriggerGroup == Row.TriggerGroup || Existing.Faction == Row.Faction))
                    return true;
            }
            return false;
        };
        for (const auto &[Id, Choice] : Selected)
            if (HasConflict(Id))
                throw std::runtime_error("The selected base strategies conflict in their action slot or faction.");
        CompletedStrategies Result;
        const auto Explicit = Selected;
        std::size_t Searches = 0;
        bool LimitReached = false;
        std::function<bool()> Complete = [&] {
            if (++Searches > 10000 || Selected.size() > 150)
            {
                LimitReached = true;
                return false;
            }
            std::vector<std::uint32_t> Ordered;
            for (const auto &[Id, Choice] : Selected)
                Ordered.push_back(Id);
            std::stable_sort(Ordered.begin(), Ordered.end(), [&](auto Left, auto Right) {
                return Strategy(Left).Gradation < Strategy(Right).Gradation;
            });
            std::set<std::uint32_t> Labels;
            for (const auto Id : Ordered)
            {
                const auto &Row = Strategy(Id);
                const auto Missing = std::find_if(Row.AcquisitionLabels.begin(), Row.AcquisitionLabels.end(),
                                                  [&](auto Label) { return !Labels.contains(Label); });
                if (Missing == Row.AcquisitionLabels.end())
                {
                    Labels.insert(Row.Labels.begin(), Row.Labels.end());
                    continue;
                }
                for (const auto Candidate : Providers(*Missing))
                {
                    const auto &Provider = Strategy(Candidate);
                    if (Selected.contains(Candidate) || Provider.Gradation >= Row.Gradation || HasConflict(Candidate))
                        continue;
                    Selected.emplace(Candidate, StrategyChoice{Candidate, *Provider.Qualities.rbegin()});
                    if (Complete())
                        return true;
                    Selected.erase(Candidate);
                    if (LimitReached)
                        return false;
                }
                return false;
            }
            return true;
        };
        if (!Complete())
            throw std::runtime_error(
                LimitReached
                    ? "Strategy completion exceeded its bounded search; reduce or adjust the requested targets."
                    : "The selected strategies cannot satisfy their normal acquisition prerequisites.");
        for (const auto &[Id, Choice] : Selected)
            if (!Explicit.contains(Id))
                Result.AutomaticIds.insert(Id);
        for (const auto &[Id, Choice] : Selected)
            Result.Choices.push_back(Choice);
        std::stable_sort(Result.Choices.begin(), Result.Choices.end(), [&](const auto &Left, const auto &Right) {
            return Strategy(Left.Id).Gradation < Strategy(Right.Id).Gradation;
        });
        std::set<std::uint32_t> Labels;
        for (const auto &Choice : Result.Choices)
        {
            const auto &Row = Strategy(Choice.Id);
            if (!std::all_of(Row.AcquisitionLabels.begin(), Row.AcquisitionLabels.end(),
                             [&](auto Label) { return Labels.contains(Label); }))
                throw std::runtime_error("A completed owned strategy still lacks its activation prerequisites.");
            Labels.insert(Row.Labels.begin(), Row.Labels.end());
            Result.ActiveIds.insert(Choice.Id);
        }
        return Result;
    }

    void DataBodyCatalog::ValidateInheritance(const std::vector<StrategyChoice>& Choices) const
    {
        if (Choices.size() > 2)
            throw std::runtime_error("Select at most two inheritance strategies.");
        std::set<std::uint32_t> Seen;
        for (const auto& Choice : Choices)
        {
            if (!Seen.insert(Choice.Id).second)
                throw std::runtime_error("Select two different inheritance strategies.");
            if (!Strategy(Choice.Id).Qualities.contains(Choice.Quality))
                throw std::runtime_error("The selected strategy quality is not valid in the current game.");
        }
    }

    std::vector<std::vector<std::uint32_t>> DataBodyCatalog::InheritanceConditions(
        std::uint32_t StrategyId, const std::vector<StrategyChoice>& Inherited) const
    {
        ValidateInheritance(Inherited);
        std::set<std::uint32_t> Labels;
        auto Ordered = Inherited;
        std::stable_sort(Ordered.begin(), Ordered.end(), [&](const auto& Left, const auto& Right)
        {
            return Strategy(Left.Id).Gradation < Strategy(Right.Id).Gradation;
        });
        for (const auto& Choice : Ordered)
        {
            const auto& Row = Strategy(Choice.Id);
            if (std::all_of(Row.InheritanceLabels.begin(), Row.InheritanceLabels.end(),
                    [&](auto Label) {return Labels.contains(Label);}))
                Labels.insert(Row.Labels.begin(), Row.Labels.end());
        }
        const auto& Row = Strategy(StrategyId);
        std::vector<std::vector<std::uint32_t>> Result;
        for (const auto Label : Row.InheritanceLabels)
        {
            if (Labels.contains(Label))
                continue;
            auto Choices = Providers(Label);
            Choices.erase(std::remove_if(Choices.begin(), Choices.end(), [&](auto Id)
            {
                return Strategy(Id).Gradation >= Row.Gradation;
            }), Choices.end());
            if (Choices.empty())
                throw std::runtime_error("An inheritance activation condition has no known in-run strategy.");
            Result.push_back(std::move(Choices));
        }
        return Result;
    }

    std::string DataBodyCatalog::InheritanceSkillName(std::uint32_t Id, TrainerLanguage Language) const
    {
        const auto Found = SkillNames.find(Id);
        return Found == SkillNames.end() ? std::to_string(Id) : Name(Found->second, Language);
    }

    std::size_t DataBodyCatalog::Capacity(std::uint64_t TotalScore) const
    {
        return static_cast<std::size_t>(std::count_if(PageScores.begin(), PageScores.end(),
                                                      [&](auto Score) { return TotalScore >= Score; })) *
               PageSize;
    }
} // namespace Trainer
