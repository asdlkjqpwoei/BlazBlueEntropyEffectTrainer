#include "DataBodySave.h"
#include "Paths.h"
#include "SaveDocument.h"

#include <Windows.h>
#include <lz4frame.h>

#include <cstring>
#include <algorithm>
#include <fstream>
#include <iostream>
#include <stdexcept>

namespace
{
	using Trainer::Bytes;
	using Trainer::ProtoField;
	using Trainer::ProtoMessage;

	Trainer::DataBodyCatalog Catalog()
	{
		Trainer::DataBodyCatalog Result;
		Result.PageScores = {0, 10000, 30000};
		for (std::uint32_t Id = 1; Id <= 3; ++Id)
		{
			Trainer::CatalogActor Actor;
			Actor.Id = Id;
			Actor.Name = {"测试角色", "Test actor"};
			Actor.Attributes = {{1, 100}, {6, 20}, {18, 50}, {92, 1}};
			Actor.Talents = {{100 + Id, 1, 0, 0}};
			Actor.InitialPotentials = {1000 + Id};
			Actor.InheritanceSkills = {Id * 10 + 1, Id * 10 + 2};
			Result.Actors.emplace(Id, Actor);
		}
		const auto Add = [&](std::uint32_t Id, std::uint32_t Grade, std::uint32_t Slot,
							 std::uint32_t Faction, std::vector<std::uint32_t> Labels,
							 std::vector<std::uint32_t> Preconditions)
		{
			Result.Strategies.emplace(Id, Trainer::CatalogStrategy{Id, {"测试策略", "Test strategy"},
				Grade, Slot, Faction, {1, 2, 3, 4}, Labels, Preconditions, Preconditions});
		};
		Add(101, 1, 1, 1, {1}, {});
		Add(102, 1, 2, 2, {2}, {});
		Add(201, 2, 0, 1, {3}, {1});
		Add(301, 3, 0, 1, {4}, {3});
		return Result;
	}

	Bytes Compress(const ProtoMessage& Root)
	{
		const auto Data = Root.Serialize();
		LZ4F_preferences_t Preferences{};
		Preferences.frameInfo.blockChecksumFlag = LZ4F_blockChecksumEnabled;
		Preferences.frameInfo.contentChecksumFlag = LZ4F_contentChecksumEnabled;
		Preferences.frameInfo.contentSize = Data.size();
		Bytes Result(LZ4F_compressFrameBound(Data.size(), &Preferences));
		const auto Size = LZ4F_compressFrame(Result.data(), Result.size(), Data.data(), Data.size(), &Preferences);
		if (LZ4F_isError(Size)) throw std::runtime_error("Cannot generate the synthetic save.");
		Result.resize(Size);
		return Result;
	}

	ProtoField StrategyField(std::uint32_t Number, std::uint32_t Id, std::uint32_t Quality)
	{
		ProtoMessage Strategy;
		Strategy.Add(ProtoField::Varint(1, Id));
		Strategy.Add(ProtoField::Varint(2, Quality));
		Strategy.Add(ProtoField::Varint(3, 0));
		Strategy.Add(ProtoField::Varint(4, 987654));
		Strategy.Add(ProtoField::Varint(5, 4));
		Strategy.Add(ProtoField::Blob(999, Bytes{1, 2, 3}));
		return ProtoField::Blob(Number, Strategy.Serialize());
	}

	Bytes SyntheticSave()
	{
		ProtoMessage Body;
		Body.Add(ProtoField::Varint(1, 5));
		Body.Add(ProtoField::Varint(2, 1));
		for (const auto Type : {1, 18, 111})
		{
			ProtoMessage Attribute;
			Attribute.Add(ProtoField::Varint(1, Type));
			Attribute.Add(ProtoField::Double(3, 567.25));
			Attribute.Add(ProtoField::Varint(800, 321));
			Body.Add(ProtoField::Blob(6, Attribute.Serialize()));
		}
		Body.Add(StrategyField(8, 77777, 2));
		Body.Add(StrategyField(30, 101, 2));
		Body.Add(StrategyField(30, 102, 3));
		Body.Add(ProtoField::Blob(15, Bytes{8, 123}));
		Body.Add(ProtoField::Blob(17, Bytes{8, 10, 16, 1, 32, 2}));
		Body.Add(ProtoField::Blob(26, Bytes{8, 21, 16, 3, 24, 2}));
		Body.Add(ProtoField::Double(16, 123.75));
		Body.Add(ProtoField::Varint(36, 0));
		Body.Add(ProtoField::Varint(37, 2));
		Body.Add(ProtoField::Varint(777, 123456));
		ProtoMessage Entry;
		Entry.Add(ProtoField::Varint(1, 5));
		Entry.Add(ProtoField::Blob(2, Body.Serialize()));
		Entry.Add(ProtoField::Varint(999, 987));
		ProtoMessage Pack;
		Pack.Add(ProtoField::Blob(1, Entry.Serialize()));
		Pack.Add(ProtoField::Varint(2, 5));
		Pack.Add(ProtoField::Varint(3, 5));
		Pack.Add(ProtoField::Varint(5, 9000));
		Pack.Add(ProtoField::Varint(222, 789));
		ProtoMessage ModelData;
		ModelData.Add(ProtoField::Blob(1, Pack.Serialize()));
		ModelData.Add(ProtoField::Varint(333, 777));
		ProtoMessage Model;
		const std::string Name = "ModelPlayerNewFesActorPack";
		Model.Add(ProtoField::Blob(1, Bytes(Name.begin(), Name.end())));
		Model.Add(ProtoField::Blob(2, ModelData.Serialize()));
		ProtoMessage Other;
		const std::string OtherName = "UnrelatedFutureModel";
		Other.Add(ProtoField::Blob(1, Bytes(OtherName.begin(), OtherName.end())));
		Other.Add(ProtoField::Blob(2, Bytes{1, 2, 3, 4, 5}));
		ProtoMessage Root;
		const std::string AutoSave = "AutoSave";
		Root.Add(ProtoField::Blob(1, Bytes(AutoSave.begin(), AutoSave.end())));
		Root.Add(ProtoField::Blob(2, Model.Serialize()));
		Root.Add(ProtoField::Blob(2, Other.Serialize()));
		Root.Add(ProtoField::Varint(444, 4321));
		return Compress(Root);
	}

	ProtoMessage PackFrom(const Bytes& File)
	{
		return ProtoMessage::Parse(ProtoMessage::Parse(
			Trainer::SaveDocument::Decode(File).Model("ModelPlayerNewFesActorPack")).Blob(1));
	}

	ProtoMessage BodyFrom(const ProtoMessage& Pack, std::uint32_t Uid)
	{
		for (const auto& Field : Pack.Select(1))
		{
			const auto Entry = ProtoMessage::Parse(Field.Data);
			if (Entry.Integer(1) == Uid) return ProtoMessage::Parse(Entry.Blob(2));
		}
		throw std::runtime_error("The test body was not found.");
	}

	Bytes Except(const ProtoMessage& Message, std::initializer_list<std::uint32_t> Changed)
	{
		ProtoMessage Result;
		for (const auto& Field : Message.Fields())
			if (std::find(Changed.begin(), Changed.end(), Field.Number) == Changed.end()) Result.Add(Field);
		return Result.Serialize();
	}

	void Write(const std::filesystem::path& Path, const Bytes& Data)
	{
		std::ofstream Output(Path, std::ios::binary | std::ios::trunc);
		if (!Output || !Output.write(reinterpret_cast<const char*>(Data.data()), Data.size()))
			throw std::runtime_error("Cannot write the isolated save fixture.");
	}
}

int RunSaveTests()
{
	int Failures = 0;
	const auto Require = [&](bool Value, const char* Message)
	{
		if (!Value) {std::cerr << Message << '\n'; ++Failures;}
	};
	const auto Reject = [&](auto Action, const char* Message)
	{
		bool Rejected = false;
		try {Action();}
		catch (const std::exception&) {Rejected = true;}
		Require(Rejected, Message);
	};
	const auto Original = SyntheticSave();
	const auto Document = Trainer::SaveDocument::Decode(Original);
	Require(Document.Encode() == Original, "An unchanged save must round-trip byte-for-byte.");
	auto Truncated = Original;
	Truncated.pop_back();
	Reject([&] {Trainer::SaveDocument::Decode(Truncated);}, "Truncated LZ4 saves must be rejected.");
	auto Trailing = Original;
	Trailing.push_back(0);
	Reject([&] {Trainer::SaveDocument::Decode(Trailing);}, "Trailing data after a save must be rejected.");
	Reject([&] {ProtoMessage::Parse(Bytes{0});}, "Invalid protobuf field zero must be rejected.");
	Reject([&] {ProtoMessage::Parse(Bytes{10, 10, 1});}, "Truncated protobuf payloads must be rejected.");
	Reject([&] {ProtoMessage::Parse(Bytes(11, 0xff));}, "Overflowing protobuf integers must be rejected.");

	const auto Folder = std::filesystem::temp_directory_path() /
		(L"BlazblueSaveTest-" + std::to_wstring(GetCurrentProcessId()));
	if (std::filesystem::exists(Folder)) throw std::runtime_error("The isolated save-test directory already exists.");
	std::filesystem::create_directories(Folder);
	const auto File = Folder / L"2";
	Write(File, Original);
	bool GameRunning = false;
	unsigned GuardCalls = 0;
	const auto Guard = [&]
	{
		++GuardCalls;
		if (GameRunning) throw std::runtime_error("Game running; offline save edits are blocked.");
	};
	Trainer::DataBodySave Save(File, Guard);
	const auto Rules = Catalog();
	Trainer::DataBodyRequest Request;
	Request.ActorId = 1;
	Request.Inherited = {{301, 4}, {102, 3}};
	Request.InheritanceSkillId = 12;
	const auto Plan = Save.Prepare(Rules, Request);
	Require(Plan.Added && Plan.Uid == 6 && Plan.CompletedOwned.size() == 4,
		"A new body must use a unique UID and safe default strategy prerequisites.");
	Require(Trainer::ReadBinaryFile(File) == Original && !std::filesystem::exists(Folder / L"BlazblueTrainerBackups"),
		"Preparing a change must not write or back up before confirmation.");
	Reject([&] {Save.Commit(Plan, false);}, "Unconfirmed changes must not write a save.");
	GameRunning = true;
	Reject([&] {Save.Commit(Plan, true);}, "Running games must block confirmed save commits.");
	Require(Trainer::ReadBinaryFile(File) == Original, "Blocked save edits changed the original file.");
	GameRunning = false;
	const auto Backup = Save.Commit(Plan, true);
	Reject([&] {Save.Commit(Plan, true);}, "An already-used confirmation must not be reused.");
	Require(Trainer::ReadBinaryFile(Backup) == Original, "The backup must contain the exact original save.");
	const auto Bodies = Save.Bodies();
	Require(Bodies.size() == 2 && Bodies.back().Uid == 6 && Bodies.back().Inherited == Request.Inherited &&
		Bodies.back().InheritanceSkillIndex == 1 && Bodies.back().InheritanceSkillLevel == 0,
		"The new body's inheritance strategy and outgoing skill did not survive decoding.");
	const auto AddedFile = Trainer::ReadBinaryFile(File);
	const auto AddedPack = PackFrom(AddedFile);
	const auto NewBody = BodyFrom(AddedPack, 6);
	Require(NewBody.Select(15).size() == 1 && ProtoMessage::Parse(NewBody.Select(15).front().Data).Integer(1) == 1001 &&
		NewBody.Select(26).empty(), "New bodies must use initial potentials only and no inherited-character mutations.");
	Require(AddedPack.Integer(5) == 9600 && AddedPack.Integer(2) == 6 && AddedPack.Integer(222) == 789,
		"Only addition may update cumulative score and the next UID.");
	Require(BodyFrom(AddedPack, 5).Serialize() == BodyFrom(PackFrom(Original), 5).Serialize(),
		"Adding a body must not change any existing body.");
	Require(Trainer::SaveDocument::Decode(AddedFile).Model("UnrelatedFutureModel") ==
		Document.Model("UnrelatedFutureModel"), "Unrelated save models were modified.");

	Trainer::DataBodyRequest Edit;
	Edit.ExistingUid = 5;
	Edit.ActorId = 1;
	Edit.Score = 700;
	Edit.Inherited = {{301, 2}, {102, 1}};
	Edit.InheritanceSkillId = 12;
	const auto BeforeBody = BodyFrom(AddedPack, 5);
	const auto EditPlan = Save.Prepare(Rules, Edit);
	const auto EditedPack = PackFrom(EditPlan.Updated);
	const auto EditedBody = BodyFrom(EditedPack, 5);
	Require(Except(BeforeBody, {30, 36}) == Except(EditedBody, {30, 36}),
		"Inheritance editing must preserve HP, MP, potentials, owned strategies, score, talents, skills and unknown fields byte-for-byte.");
	Require(Except(AddedPack, {1}) == Except(EditedPack, {1}) && EditedBody.Double(16) == 123.75 &&
		EditedBody.Integer(37) == 2, "Editing must not change archive counters, score, locks or inheritance level.");
	Require(BodyFrom(EditedPack, 6).Serialize() == NewBody.Serialize() &&
		EditedBody.Integer(36) == 1 && EditPlan.CompletedOwned.empty(),
		"Editing must update only the selected body's outgoing inheritance.");
	const auto InheritedMetadata = ProtoMessage::Parse(EditedBody.Select(30).back().Data);
	Require(InheritedMetadata.Integer(4) == 987654 && InheritedMetadata.Blob(999) == Bytes{1, 2, 3},
		"Unknown metadata on retained inheritance strategies must be preserved.");
	Save.Commit(EditPlan, true);
	auto SkillOnly = Edit;
	SkillOnly.InheritanceSkillId = 11;
	const auto SkillPlan = Save.Prepare(Rules, SkillOnly);
	const auto SkillBody = BodyFrom(PackFrom(SkillPlan.Updated), 5);
	Require(Except(EditedBody, {36}) == Except(SkillBody, {36}) && SkillBody.Integer(36) == 0,
		"Changing only the outgoing skill must preserve all other fields, including active acquired skills.");
	Save.Commit(SkillPlan, true);
	auto NoChange = SkillOnly;
	NoChange.InheritanceSkillId.reset();
	const auto NoChangePlan = Save.Prepare(Rules, NoChange);
	Require(NoChangePlan.Updated == Trainer::ReadBinaryFile(File), "An unchanged inheritance draft must not rewrite fields.");
	Reject([&] {Save.Commit(NoChangePlan, true);}, "A no-op save plan must not create another backup.");

	auto Bad = Edit;
	Bad.InheritanceSkillId = 21;
	Reject([&] {Save.Prepare(Rules, Bad);}, "Outgoing skills must belong to the body's own character.");
	Bad = Edit;
	Bad.Inherited = {{301, 4}, {301, 4}};
	Reject([&] {Save.Prepare(Rules, Bad);}, "Duplicate inheritance strategies must be rejected.");
	Bad = Edit;
	Bad.Inherited[0].Quality = 99;
	Reject([&] {Save.Prepare(Rules, Bad);}, "Invalid inheritance quality must be rejected.");
	Bad = Edit;
	Bad.ActorId = 2;
	Reject([&] {Save.Prepare(Rules, Bad);}, "Existing characters must not be changed.");
	auto Full = Rules;
	Full.PageSize = 1;
	Reject([&] {Save.Prepare(Full, Request);}, "Addition must use the already-unlocked warehouse capacity.");

	Require(Rules.InheritanceConditions(101, {{101, 4}}).empty(),
		"Basic inherited strategies must report no activation conditions.");
	Require(Rules.InheritanceConditions(301, {{301, 4}}) ==
		std::vector<std::vector<std::uint32_t>>{{201}},
		"Inherited activation must show in-run strategy providers, not the entire normal acquisition tree.");
	Require(Rules.InheritanceConditions(201, {{101, 4}, {201, 4}}).empty(),
		"A condition already supplied by another active inherited strategy must not require in-run acquisition.");
	auto DifferentConditions = Rules;
	DifferentConditions.Strategies.at(301).InheritanceLabels = {2, 3};
	Require(DifferentConditions.InheritanceConditions(301, {{102, 4}, {301, 4}}) ==
		std::vector<std::vector<std::uint32_t>>{{201}},
		"Only missing inherited prerequisites should appear in the condition prompt.");

	const auto BatchFolder = Folder / L"Batch";
	std::filesystem::create_directory(BatchFolder);
	const auto BatchFile = BatchFolder / L"2";
	Write(BatchFile, AddedFile);
	Trainer::DataBodySave BatchSave(BatchFile, Guard);
	Trainer::DataBodyDrafts Drafts(BatchSave.Bodies());
	const auto Baseline = Drafts.Bodies();
	Drafts.Body(5).Inherited = {Trainer::StrategyChoice{301, 2}, Trainer::StrategyChoice{102, 1}};
	Drafts.Body(5).SkillIndex = 1;
	Drafts.Body(6).SkillIndex = 0;
	Require(Drafts.Body(5).Inherited[0]->Id == 301 && Drafts.Body(6).SkillIndex == 0 && Drafts.ChangeCount() == 2,
		"Edits to multiple bodies must survive switching records and count independently.");
	Drafts.Body(5).Inherited[1] = Drafts.Body(5).Inherited[0];
	Reject([&] {BatchSave.PrepareBatch(Rules, Drafts.Requests(Rules));},
		"Temporarily invalid cached combinations must block final review without discarding the draft.");
	Require(Drafts.Body(5).Inherited[0] == Drafts.Body(5).Inherited[1] && Drafts.ChangeCount() == 2,
		"Invalid intermediate selections must be cached rather than silently reverted.");
	Drafts.Body(5).Inherited[1] = Trainer::StrategyChoice{102, 1};
	auto Third = Request;
	Third.ActorId = 2;
	Third.InheritanceSkillId = 22;
	const auto DraftUid = Drafts.Add(Third, Rules);
	Require(Drafts.Body(DraftUid).Added && Drafts.Body(DraftUid).SkillIndex == 1 && Drafts.ChangeCount() == 3,
		"Pending additions must retain their selected skill and join the same draft session.");
	const auto Pending = Drafts.Requests(Rules);
	const auto BatchPlan = BatchSave.PrepareBatch(Rules, Pending);
	Require(BatchPlan.ChangedUids.size() == 3 && BatchSave.Bodies().size() == 2 &&
		Trainer::ReadBinaryFile(BatchFile) == AddedFile &&
		!std::filesystem::exists(BatchFolder / L"BlazblueTrainerBackups"),
		"Batch preparation must stage every change in memory without updating the loaded save or disk.");
	Reject([&] {BatchSave.Commit(BatchPlan, false);}, "Cancelling batch review must leave all drafts pending.");
	Require(Drafts.ChangeCount() == 3 && Trainer::ReadBinaryFile(BatchFile) == AddedFile,
		"Cancelling final review discarded drafts or modified the save.");
	auto InvalidBatch = Pending;
	InvalidBatch.back().InheritanceSkillId = 11;
	Reject([&] {BatchSave.PrepareBatch(Rules, InvalidBatch);}, "An invalid final item must reject the entire batch.");
	auto FullBatch = Rules;
	FullBatch.PageSize = 2;
	Reject([&] {BatchSave.PrepareBatch(FullBatch, Pending);}, "Batch additions must respect warehouse capacity.");
	Reject([&] {BatchSave.PrepareBatch(Rules, {Pending[0], Pending[0]});}, "Duplicate edits in a batch must be rejected.");
	Drafts.Reset();
	Require(Drafts.ChangeCount() == 0 && Drafts.Bodies() == Baseline && Trainer::ReadBinaryFile(BatchFile) == AddedFile,
		"Reset all must restore exact loaded selections and remove pending additions without a write.");
	const auto BatchBackup = BatchSave.Commit(BatchPlan, true);
	Require(Trainer::ReadBinaryFile(BatchBackup) == AddedFile &&
		std::distance(std::filesystem::directory_iterator(BatchFolder / L"BlazblueTrainerBackups"),
			std::filesystem::directory_iterator{}) == 1,
		"A batch must create one exact pre-edit backup before the single save replacement.");
	const auto BatchBodies = BatchSave.Bodies();
	Require(BatchBodies.size() == 3 && BatchBodies[0].Inherited == Pending[0].Inherited &&
		BatchBodies[0].InheritanceSkillIndex == 1 && BatchBodies[1].InheritanceSkillIndex == 0 &&
		BatchBodies[2].ActorId == 2 && BatchBodies[2].InheritanceSkillIndex == 1,
		"Batch readback did not contain all edited and newly added bodies.");
	Trainer::DataBodyDrafts Rebased(BatchBodies);
	Rebased.Body(5).SkillIndex = 0;
	Rebased.Reset();
	Require(Rebased.ChangeCount() == 0 && Rebased.Body(5).SkillIndex == 1,
		"After a successful save, reset must use the newly saved baseline.");
	Reject([&] {BatchSave.Commit(BatchPlan, true);}, "A batch confirmation must not be reused.");

	auto FilterCatalog = Rules;
	FilterCatalog.Strategies.at(101).Name = {"火焰普攻", "Flame Attack"};
	FilterCatalog.Strategies.at(101).Elements = {1};
	FilterCatalog.Strategies.at(102).Elements = {2};
	FilterCatalog.Strategies.at(201).Elements = {1};
	FilterCatalog.Strategies.at(301).Elements = {1, 2};
	FilterCatalog.Strategies.at(301).Gradation = 4;
	auto SameElementDual = FilterCatalog.Strategies.at(301);
	SameElementDual.Id = 302;
	SameElementDual.Name = {"同系双重", "Flame Dual"};
	SameElementDual.Elements = {1, 1};
	FilterCatalog.Strategies.emplace(302, SameElementDual);
	Trainer::StrategyFilter Filter;
	Filter.Element = 2;
	Filter.Dual = true;
	Filter.Tier = 4;
	Require(FilterCatalog.FilterStrategies(Filter) == std::vector<std::uint32_t>{301},
		"Element, dual and tier filters must combine, including the second element of a dual strategy.");
	Filter.Element = 1;
	Require(FilterCatalog.FilterStrategies(Filter) == std::vector<std::uint32_t>{301, 302},
		"Same-element double strategies must remain classified as dual.");
	Filter = {};
	Filter.Search = "  FLAME  ";
	Require(FilterCatalog.FilterStrategies(Filter) == std::vector<std::uint32_t>{101, 302},
		"Strategy names must support trimmed, case-insensitive search.");
	Filter.Search = "301";
	Require(FilterCatalog.FilterStrategies(Filter) == std::vector<std::uint32_t>{301}, "Strategy IDs must be searchable.");
	Filter = {};
	Filter.Trigger = 0;
	Filter.Tier = 2;
	Require(FilterCatalog.FilterStrategies(Filter) == std::vector<std::uint32_t>{201},
		"A trigger value of zero is a real conditional/enhancement filter, not the all-types sentinel.");
	Filter.Search = "no such strategy";
	Require(FilterCatalog.FilterStrategies(Filter).empty(), "A search with no matches must return an empty result.");

	const auto StalePlan = Save.Prepare(Rules, Request);
	Write(File, Original);
	Reject([&] {Save.Commit(StalePlan, true);}, "An externally replaced save must not be overwritten.");
	Require(Trainer::ReadBinaryFile(File) == Original, "Stale-plan rejection overwrote external changes.");
	GameRunning = true;
	Reject([&] {Trainer::DataBodySave Reload(File, Guard);}, "The game must be stopped before loading for editing.");
	Require(GuardCalls >= 10, "The offline guard was not rechecked across operations.");
	GameRunning = false;
	const auto LateFile = Folder / L"3";
	Write(LateFile, Original);
	unsigned LateCalls = 0;
	bool RejectLate = false;
	Trainer::DataBodySave Late(LateFile, [&]
	{
		++LateCalls;
		if (RejectLate && LateCalls >= 5) throw std::runtime_error("Game started while confirming the save.");
	});
	const auto LatePlan = Late.Prepare(Rules, Request);
	RejectLate = true;
	Reject([&] {Late.Commit(LatePlan, true);}, "Game startup after backup must abort replacement.");
	Require(Trainer::ReadBinaryFile(LateFile) == Original, "A late guard failure changed the save.");
	const auto BadFolder = Folder / L"BackupFailure";
	std::filesystem::create_directory(BadFolder);
	Write(BadFolder / L"2", Original);
	Write(BadFolder / L"BlazblueTrainerBackups", Bytes{1});
	Trainer::DataBodySave BadBackup(BadFolder / L"2", [] {});
	const auto BadPlan = BadBackup.Prepare(Rules, Request);
	Reject([&] {BadBackup.Commit(BadPlan, true);}, "Backup failure must abort save writing.");
	Require(Trainer::ReadBinaryFile(BadFolder / L"2") == Original, "A failed backup changed the original save.");
	std::filesystem::remove_all(Folder);
	return Failures != 0;
}

int RunDataBodyProbe(int ArgumentCount, char** Arguments)
{
	if (ArgumentCount != 3 && !(ArgumentCount == 5 && std::strcmp(Arguments[3], "--prepare-output") == 0))
	{
		std::cerr << "Usage: BlazblueTests --databody-probe <game-directory> <save-copy> [--prepare-output <new-file>]\n";
		return 1;
	}
	const auto Catalog = Trainer::DataBodyCatalog::Load(Trainer::PathFromUtf8(Arguments[1]));
	Trainer::DataBodySave Save(Trainer::PathFromUtf8(Arguments[2]), [] {});
	const auto Bodies = Save.Bodies();
	std::cout << "read_only=true actors=" << Catalog.Actors.size() << " strategies=" << Catalog.Strategies.size()
		<< " bodies=" << Bodies.size() << " capacity=" << Save.Capacity(Catalog) << '\n';
	for (const auto& Body : Bodies)
		std::cout << "uid=" << Body.Uid << " actor=" << Body.ActorId << " inherited=" << Body.Inherited.size()
			<< " inheritance_skill_index=" << Body.InheritanceSkillIndex << '\n';
	std::size_t Completed = 0;
	for (const auto& [Id, Row] : Catalog.Strategies)
	{
		if (Row.Qualities.empty()) continue;
		Catalog.CompleteStrategies({{Id, *Row.Qualities.rbegin()}}, {});
		++Completed;
	}
	std::cout << "independent_normal_strategy_completions=" << Completed << '\n';
	if (Bodies.empty()) return 0;
	Trainer::DataBodyRequest Request;
	Request.ActorId = Bodies.front().ActorId;
	Request.Inherited = Bodies.front().Inherited;
	const auto& Actor = Catalog.Actor(Request.ActorId);
	if (!Actor.InheritanceSkills.empty()) Request.InheritanceSkillId = Actor.InheritanceSkills.front();
	const auto Plan = Save.Prepare(Catalog, Request);
	if (ArgumentCount == 5)
	{
		const auto Destination = Trainer::PathFromUtf8(Arguments[4]);
		if (std::filesystem::exists(Destination)) throw std::runtime_error("The validation output must not already exist.");
		Write(Destination, Plan.Updated);
		Write(std::filesystem::path(Destination.wstring() + L".model.bytes"),
			Trainer::SaveDocument::Decode(Plan.Updated).Model("ModelPlayerNewFesActorPack"));
	}
	std::cout << "prepared_single_body_uid=" << Plan.Uid << " confirmed_file_overwrite=false\n";
	return 0;
}
