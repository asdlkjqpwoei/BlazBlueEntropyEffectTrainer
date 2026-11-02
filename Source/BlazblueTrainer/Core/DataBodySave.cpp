#include "DataBodySave.h"

#include <Windows.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <limits>
#include <map>
#include <set>
#include <stdexcept>
#include <system_error>

namespace Trainer
{
    namespace
    {
        constexpr const char *ModelName = "ModelPlayerNewFesActorPack";

        struct BodyPack
        {
            ProtoMessage Model;
            ProtoMessage Pack;
        };

        BodyPack ReadPack(const SaveDocument &Document)
        {
            BodyPack Result;
            Result.Model = ProtoMessage::Parse(Document.Model(ModelName));
            Result.Pack = ProtoMessage::Parse(Result.Model.Blob(1));
            return Result;
        }

        ProtoMessage Nested(const ProtoField &Field)
        {
            if (Field.WireType != 2)
                throw std::runtime_error("Invalid nested data-body save field.");
            return ProtoMessage::Parse(Field.Data);
        }

        std::map<std::uint32_t, ProtoMessage> Entries(const ProtoMessage &Pack)
        {
            std::map<std::uint32_t, ProtoMessage> Result;
            for (const auto &Field : Pack.Select(1))
            {
                const auto Entry = Nested(Field);
                const auto Uid = Entry.Integer(1);
                auto Body = ProtoMessage::Parse(Entry.Blob(2));
                if (!Uid || Uid > UINT32_MAX || Body.Integer(1) != Uid ||
                    !Result.emplace(static_cast<std::uint32_t>(Uid), std::move(Body)).second)
                    throw std::runtime_error("Data-body identifiers are duplicated or inconsistent.");
            }
            return Result;
        }

        std::vector<StrategyChoice> Choices(const ProtoMessage &Body, std::uint32_t FieldNumber)
        {
            std::vector<StrategyChoice> Result;
            for (const auto &Field : Body.Select(FieldNumber))
            {
                const auto Strategy = Nested(Field);
                if (Strategy.Integer(1) > UINT32_MAX || Strategy.Integer(2) > UINT32_MAX)
                    throw std::runtime_error("Invalid saved strategy identifier.");
                Result.push_back(
                    {static_cast<std::uint32_t>(Strategy.Integer(1)), static_cast<std::uint32_t>(Strategy.Integer(2))});
            }
            return Result;
        }

        std::vector<ProtoField> WriteStrategies(std::uint32_t Number, const std::vector<StrategyChoice> &Choices,
                                                const std::set<std::uint32_t> &Active, const ProtoMessage *Existing)
        {
            std::vector<ProtoField> Result;
            for (const auto &Choice : Choices)
            {
                ProtoMessage Strategy;
                if (Existing)
                {
                    for (const auto &Field : Existing->Select(Number))
                    {
                        const auto Old = Nested(Field);
                        if (Old.Integer(1) == Choice.Id)
                            Strategy = Old;
                    }
                }
                Strategy.Replace(1, {ProtoField::Varint(1, Choice.Id)});
                Strategy.Replace(2, {ProtoField::Varint(2, Choice.Quality)});
                Strategy.Replace(3, {ProtoField::Varint(3, Active.contains(Choice.Id) ? 1 : 0)});
                Result.push_back(ProtoField::Blob(Number, Strategy.Serialize()));
            }
            return Result;
        }

        void SetAttribute(ProtoMessage &Body, std::uint32_t Type, double Value)
        {
            auto Attributes = Body.Select(6);
            bool Found = false;
            for (auto &Field : Attributes)
            {
                auto Attribute = Nested(Field);
                if (Attribute.Integer(1) != Type)
                    continue;
                if (Found)
                    throw std::runtime_error("Duplicate data-body base attribute.");
                Found = true;
                Attribute.Double(3);
                Attribute.Replace(3, {ProtoField::Double(3, Value)});
                Field = ProtoField::Blob(6, Attribute.Serialize());
            }
            if (!Found)
            {
                ProtoMessage Attribute;
                Attribute.Add(ProtoField::Varint(1, Type));
                Attribute.Add(ProtoField::Double(3, Value));
                Attributes.push_back(ProtoField::Blob(6, Attribute.Serialize()));
            }
            Body.Replace(6, Attributes);
        }

        ProtoField TalentField(const BodyTalent &Talent)
        {
            ProtoMessage Value;
            Value.Add(ProtoField::Varint(1, Talent.Id));
            Value.Add(ProtoField::Varint(2, Talent.Quality));
            if (Talent.InheritNumber)
                Value.Add(ProtoField::Varint(3, Talent.InheritNumber));
            if (Talent.FromActor)
                Value.Add(ProtoField::Varint(4, Talent.FromActor));
            return ProtoField::Blob(17, Value.Serialize());
        }

        std::string FileError(const std::string &Context)
        {
            return Context + ": " + std::system_category().message(static_cast<int>(GetLastError()));
        }

        struct FileHandle
        {
            explicit FileHandle(HANDLE Value) : Value(Value)
            {
                if (Value == INVALID_HANDLE_VALUE)
                    throw std::runtime_error(FileError("Cannot access the save file"));
            }
            ~FileHandle()
            {
                CloseHandle(Value);
            }
            HANDLE Value;
        };

        Bytes ReadLocked(HANDLE File)
        {
            LARGE_INTEGER Size{};
            FILE_ATTRIBUTE_TAG_INFO Attributes{};
            if (!GetFileSizeEx(File, &Size) ||
                !GetFileInformationByHandleEx(File, FileAttributeTagInfo, &Attributes, sizeof(Attributes)))
                throw std::runtime_error(FileError("Cannot inspect the save file"));
            if ((Attributes.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) || Size.QuadPart < 0 ||
                Size.QuadPart > 64 * 1024 * 1024)
                throw std::runtime_error("The save path is a link or exceeds the bounded size limit.");
            LARGE_INTEGER Begin{};
            if (!SetFilePointerEx(File, Begin, nullptr, FILE_BEGIN))
                throw std::runtime_error(FileError("Cannot read the save file"));
            Bytes Result(static_cast<std::size_t>(Size.QuadPart));
            std::size_t Position = 0;
            while (Position < Result.size())
            {
                const DWORD Request = static_cast<DWORD>(std::min<std::size_t>(Result.size() - Position, 1024 * 1024));
                DWORD Read = 0;
                if (!ReadFile(File, Result.data() + Position, Request, &Read, nullptr) || Read != Request)
                    throw std::runtime_error(FileError("Cannot read the complete save file"));
                Position += Read;
            }
            return Result;
        }

        Bytes ReadSave(const std::filesystem::path &Path)
        {
            FileHandle File(CreateFileW(Path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_DELETE, nullptr,
                                        OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
            return ReadLocked(File.Value);
        }

        void WriteNew(const std::filesystem::path &Path, const Bytes &Data)
        {
            FileHandle File(
                CreateFileW(Path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr));
            std::size_t Position = 0;
            while (Position < Data.size())
            {
                const DWORD Request = static_cast<DWORD>(std::min<std::size_t>(Data.size() - Position, 1024 * 1024));
                DWORD Written = 0;
                if (!WriteFile(File.Value, Data.data() + Position, Request, &Written, nullptr) || Written != Request)
                    throw std::runtime_error(FileError("Cannot write the backup or temporary save"));
                Position += Written;
            }
            if (!FlushFileBuffers(File.Value))
                throw std::runtime_error(FileError("Cannot flush the backup or temporary save"));
        }

        std::wstring UniqueSuffix()
        {
            FILETIME Time{};
            GetSystemTimePreciseAsFileTime(&Time);
            static std::atomic<std::uint32_t> Sequence{};
            const auto Stamp = (static_cast<std::uint64_t>(Time.dwHighDateTime) << 32) | Time.dwLowDateTime;
            return std::to_wstring(Stamp) + L"-" + std::to_wstring(GetCurrentProcessId()) + L"-" +
                   std::to_wstring(Sequence.fetch_add(1));
        }
    } // namespace

    DataBodySave::DataBodySave(std::filesystem::path Path, StoppedGuard Guard)
        : FilePath(std::filesystem::absolute(Path)), AssertStopped(std::move(Guard))
    {
        if (!AssertStopped)
            throw std::invalid_argument("An offline game-state guard is required for save editing.");
        AssertStopped();
        static std::atomic<std::uint64_t> NextGeneration{1};
        Generation = NextGeneration.fetch_add(1) << 32;
        const auto Bytes = ReadSave(FilePath);
        Document = SaveDocument::Decode(Bytes);
        Entries(ReadPack(Document).Pack);
        if (ReadSave(FilePath) != Bytes)
            throw std::runtime_error("The save changed while it was being loaded.");
        AssertStopped();
    }

    std::vector<DataBodyInfo> DataBodySave::Bodies() const
    {
        std::vector<DataBodyInfo> Result;
        for (const auto &[Uid, Body] : Entries(ReadPack(Document).Pack))
        {
            if (Body.Integer(2) > UINT32_MAX)
                throw std::runtime_error("Invalid saved actor identifier.");
            DataBodyInfo Entry{Uid,
                               static_cast<std::uint32_t>(Body.Integer(2)),
                               Body.Double(16),
                               Choices(Body, 30),
                               0, 0};
            const auto SkillIndex = Body.Integer(36);
            const auto SkillLevel = Body.Integer(37);
            if (SkillIndex > UINT32_MAX || SkillLevel >= UINT32_MAX)
                throw std::runtime_error("Invalid saved inheritance skill selection.");
            Entry.InheritanceSkillIndex = static_cast<std::uint32_t>(SkillIndex);
            Entry.InheritanceSkillLevel = static_cast<std::uint32_t>(SkillLevel);
            Result.push_back(std::move(Entry));
        }
        return Result;
    }

    std::size_t DataBodySave::Capacity(const DataBodyCatalog &Catalog) const
    {
        return Catalog.Capacity(ReadPack(Document).Pack.Integer(5));
    }

    DataBodyPlan DataBodySave::Prepare(const DataBodyCatalog &Catalog, const DataBodyRequest &Request) const
    {
        AssertStopped();
        if (!Request.ExistingUid && (Request.Inherited.size() != 2 || (Request.Score != 600 && Request.Score != 700)))
            throw std::runtime_error("A new data body requires purple/gold quality and two inheritance strategies.");
        const auto &Actor = Catalog.Actor(Request.ActorId);
        auto Updated = Document;
        auto Pack = ReadPack(Updated);
        const auto Saved = Entries(Pack.Pack);
        DataBodyPlan Plan;
        Plan.SourceGeneration = Generation;
        Plan.Added = !Request.ExistingUid;
        ProtoMessage Body;
        if (Request.ExistingUid)
        {
            const auto Found = Saved.find(*Request.ExistingUid);
            if (Found == Saved.end() || Found->second.Integer(2) != Request.ActorId)
                throw std::runtime_error("An existing data body's character cannot be replaced.");
            Plan.Uid = Found->first;
            Body = Found->second;
        }
        else
        {
            if (Saved.size() >= Catalog.Capacity(Pack.Pack.Integer(5)))
                throw std::runtime_error("The currently unlocked data-body warehouse is full.");
            const auto Last = std::max<std::uint64_t>(Pack.Pack.Integer(2), Saved.empty() ? 0 : Saved.rbegin()->first);
            if (Last >= UINT32_MAX)
                throw std::runtime_error("The data-body identifier space is exhausted.");
            Plan.Uid = static_cast<std::uint32_t>(Last + 1);
            Body.Add(ProtoField::Varint(1, Plan.Uid));
            Body.Add(ProtoField::Varint(2, Actor.Id));
            for (const auto &Attribute : Actor.Attributes)
                SetAttribute(Body, Attribute.Type, Attribute.Value);
            for (const auto &Talent : Actor.Talents)
                Body.Add(TalentField(Talent));
            Body.Add(ProtoField::Varint(35, 1));
            Pack.Pack.Replace(2, {ProtoField::Varint(2, Plan.Uid)});
            const auto Total = Pack.Pack.Integer(5);
            const auto AddedScore = static_cast<std::uint64_t>(Request.Score);
            if (Total > UINT64_MAX - AddedScore)
                throw std::runtime_error("Cumulative data-body score overflow.");
            Pack.Pack.Replace(5, {ProtoField::Varint(5, Total + AddedScore)});
        }
        CompletedStrategies Completed;
        if (Plan.Added)
        {
            Completed = Catalog.CompleteStrategies({}, Request.Inherited);
            Body.Replace(16, {ProtoField::Double(16, Request.Score)});
            Body.Replace(8, WriteStrategies(8, Completed.Choices, Completed.ActiveIds, nullptr));
            std::vector<ProtoField> Potentials;
            for (const auto Id : Actor.InitialPotentials)
            {
                ProtoMessage Potential;
                Potential.Add(ProtoField::Varint(1, Id));
                Potentials.push_back(ProtoField::Blob(15, Potential.Serialize()));
            }
            Body.Replace(15, Potentials);
        }
        if (Plan.Added || Choices(Body, 30) != Request.Inherited)
        {
            Catalog.ValidateInheritance(Request.Inherited);
            std::set<std::uint32_t> InheritedIds;
            for (const auto &Choice : Request.Inherited)
                InheritedIds.insert(Choice.Id);
            const auto PreviousBody = Body;
            Body.Replace(30, WriteStrategies(30, Request.Inherited, InheritedIds, &PreviousBody));
        }
        if (Request.InheritanceSkillId)
        {
            const auto Found = std::find(Actor.InheritanceSkills.begin(), Actor.InheritanceSkills.end(),
                *Request.InheritanceSkillId);
            if (Found == Actor.InheritanceSkills.end())
                throw std::runtime_error("Select an inheritance skill belonging to this data body's character.");
            const auto Index = static_cast<std::uint32_t>(Found - Actor.InheritanceSkills.begin());
            if (Body.Integer(36) != Index)
                Body.Replace(36, {ProtoField::Varint(36, Index)});
        }
        auto BodyFields = Pack.Pack.Select(1);
        if (Plan.Added)
        {
            ProtoMessage Entry;
            Entry.Add(ProtoField::Varint(1, Plan.Uid));
            Entry.Add(ProtoField::Blob(2, Body.Serialize()));
            BodyFields.push_back(ProtoField::Blob(1, Entry.Serialize()));
        }
        else
        {
            for (auto &Field : BodyFields)
            {
                auto Entry = Nested(Field);
                if (Entry.Integer(1) == Plan.Uid)
                {
                    Entry.Replace(2, {ProtoField::Blob(2, Body.Serialize())});
                    Field = ProtoField::Blob(1, Entry.Serialize());
                }
            }
        }
        Pack.Pack.Replace(1, BodyFields);
        if (Plan.Added)
        {
            auto Scores = Pack.Pack.Select(4);
            bool ScoreFound = false;
            for (auto &Field : Scores)
            {
                auto Entry = Nested(Field);
                if (Entry.Integer(1) != Request.ActorId)
                    continue;
                if (ScoreFound)
                    throw std::runtime_error("Duplicate character maximum-score record.");
                ScoreFound = true;
                if (Entry.Double(2) < Request.Score)
                {
                    Entry.Replace(2, {ProtoField::Double(2, Request.Score)});
                    Field = ProtoField::Blob(4, Entry.Serialize());
                }
            }
            if (!ScoreFound)
            {
                ProtoMessage Entry;
                Entry.Add(ProtoField::Varint(1, Request.ActorId));
                Entry.Add(ProtoField::Double(2, Request.Score));
                Scores.push_back(ProtoField::Blob(4, Entry.Serialize()));
            }
            Pack.Pack.Replace(4, Scores);
        }
        Pack.Model.Replace(1, {ProtoField::Blob(1, Pack.Pack.Serialize())});
        Updated.ReplaceModel(ModelName, Pack.Model.Serialize());
        Plan.Updated = Updated.Encode();
        const auto Readback = Entries(ReadPack(SaveDocument::Decode(Plan.Updated)).Pack);
        if (Readback.at(Plan.Uid).Serialize() != Body.Serialize())
            throw std::runtime_error("Data-body serialization readback failed.");
        Plan.CompletedOwned = Completed.Choices;
        return Plan;
    }

    std::filesystem::path DataBodySave::Commit(const DataBodyPlan &Plan, bool Confirmed)
    {
        if (!Confirmed)
            throw std::runtime_error("Save modification was not confirmed.");
        if (Plan.SourceGeneration != Generation)
            throw std::runtime_error("The save plan is stale; review the current save again.");
        AssertStopped();
        FileHandle Source(CreateFileW(FilePath.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_DELETE, nullptr,
                                      OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
        if (ReadLocked(Source.Value) != Document.Original())
            throw std::runtime_error("The save changed externally; reload it before modifying.");
        const auto Validated = SaveDocument::Decode(Plan.Updated);
        if (!Entries(ReadPack(Validated).Pack).contains(Plan.Uid))
            throw std::runtime_error("The prepared data-body record is missing.");
        if (Plan.Updated == Document.Original())
            throw std::runtime_error("There are no save changes to apply.");
        const auto BackupDirectory = FilePath.parent_path() / L"BlazblueTrainerBackups";
        std::filesystem::create_directories(BackupDirectory);
        const auto BackupAttributes = GetFileAttributesW(BackupDirectory.c_str());
        if (BackupAttributes == INVALID_FILE_ATTRIBUTES || (BackupAttributes & FILE_ATTRIBUTE_REPARSE_POINT))
            throw std::runtime_error("The save-backup directory must be a normal local directory, not a link.");
        const auto Suffix = UniqueSuffix();
        const auto Backup = BackupDirectory / (FilePath.filename().wstring() + L"-" + Suffix + L".bak");
        WriteNew(Backup, Document.Original());
        if (ReadBinaryFile(Backup) != Document.Original())
            throw std::runtime_error("Backup verification failed; the original save was not modified.");
        const auto Temporary = FilePath.parent_path() / (L".blazblue-" + Suffix + L".tmp");
        struct Cleanup
        {
            std::filesystem::path Path;
            bool Active{true};
            ~Cleanup()
            {
                if (!Active)
                    return;
                std::error_code Error;
                std::filesystem::remove(Path, Error);
                if (Error)
                    OutputDebugStringW(L"BlazblueTrainer temporary-save cleanup failed.\n");
            }
        } CleanupFile{Temporary};
        WriteNew(Temporary, Plan.Updated);
        if (ReadBinaryFile(Temporary) != Plan.Updated)
            throw std::runtime_error("Temporary-save verification failed; the original save was not modified.");
        AssertStopped();
        if (ReadSave(FilePath) != Document.Original())
            throw std::runtime_error("The save changed externally; reload it before modifying.");
        if (!ReplaceFileW(FilePath.c_str(), Temporary.c_str(), nullptr, 0, nullptr, nullptr))
            throw std::runtime_error(FileError("Cannot atomically replace the save; its backup has been kept"));
        CleanupFile.Active = false;
        if (ReadSave(FilePath) != Plan.Updated)
            throw std::runtime_error("Save readback failed; restore the preserved backup before continuing.");
        Document = Validated;
        ++Generation;
        return Backup;
    }
} // namespace Trainer
