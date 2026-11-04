#include "Pch.h"
#include "DataBodyEditor.xaml.h"
#include "Paths.h"
#include "RemoteProcess.h"
#include "UiHelpers.h"
#if __has_include("DataBodyEditor.g.cpp")
#include "DataBodyEditor.g.cpp"
#endif

#include <winrt/Microsoft.Windows.Storage.Pickers.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <algorithm>
#include <chrono>
#include <set>
#include <utility>

namespace
{
	using Item = winrt::Windows::Foundation::IInspectable;
	void SetItems(const winrt::Microsoft::UI::Xaml::Controls::ComboBox& Selector,
		std::vector<Item> Items, int Selected)
	{
		const auto Count = static_cast<int>(Items.size());
		Selector.ItemsSource(winrt::single_threaded_observable_vector<Item>(std::move(Items)));
		Selector.SelectedIndex(Count ? std::clamp(Selected, 0, Count - 1) : -1);
	}
	std::string PathText(const std::filesystem::path& Path)
	{
		const auto Value = Path.u8string();
		return std::string(Value.begin(), Value.end());
	}
	std::pair<bool, std::size_t> Slot(const Item& Sender)
	{
		const auto Tag = winrt::to_string(winrt::unbox_value<winrt::hstring>(
			Sender.as<winrt::Microsoft::UI::Xaml::Controls::Button>().Tag()));
		return {Tag.starts_with("new:"), Tag.back() == '1' ? 1 : 0};
	}
}

namespace winrt::BlazblueTrainer::implementation
{
	DataBodyEditor::DataBodyEditor() { InitializeComponent(); }
	DataBodyEditor::~DataBodyEditor() { Shutdown(); }

	void DataBodyEditor::Initialize(Microsoft::UI::WindowId Owner, TrainerLanguage SelectedLanguage,
		std::shared_ptr<Trainer::GamePresenceMonitor> Monitor)
	{
		if (Settings || Closing) return;
		OwnerId = Owner;
		Language = SelectedLanguage;
		PresenceMonitor = std::move(Monitor);
		try
		{
			Settings = Trainer::LoadConfig(Trainer::DefaultConfigPath());
			InitializeText();
			SaveSlotSelector().Items().Append(box_value(Utf8ToHString(Text("databody_choose_save"))));
			SaveSlotSelector().SelectedIndex(0);
			EditorTabs().SelectedIndex(0);
			const auto Queue = Microsoft::UI::Dispatching::DispatcherQueue::GetForCurrentThread();
			if (!Queue) throw std::runtime_error("Cannot create the offline game-state timer.");
			GameTimer = Queue.CreateTimer();
			GameTimer.Interval(std::chrono::milliseconds(500));
			GameTimer.Tick([this](auto const&, auto const&) { UpdateAvailability(); });
			GameTimer.Start();
			Populating = false;
			UpdateAvailability();
			LoadCatalog({});
		}
		catch (const std::exception& Error) { Report(Error.what()); OfflineControls().IsEnabled(false); }
		catch (const hresult_error& Error) { Report(to_string(Error.message())); OfflineControls().IsEnabled(false); }
	}

	std::string DataBodyEditor::Text(const char* Key) const
	{
		if (!Settings) throw std::runtime_error("The offline editor configuration is unavailable.");
		return Settings->Text(Key, Language);
	}
	void DataBodyEditor::Report(const std::string& Error)
	{
		LastMessage = Settings ? Settings->Texts.Translate(Error, TrainerLanguageEnglish) : Error;
		SaveOperationText().Text(Utf8ToHString(Settings ? Settings->Texts.Translate(LastMessage, Language) : LastMessage));
	}
	void DataBodyEditor::TryAction(const std::function<void()>& Action)
	{
		if (Closing) return;
		try { Action(); }
		catch (const std::exception& Error) { Populating = false; Report(Error.what()); }
		catch (const hresult_error& Error) { Populating = false; Report(to_string(Error.message())); }
		UpdateAvailability();
	}
	void DataBodyEditor::InitializeText()
	{
		OfflineNotice().Text(Utf8ToHString(Text("databody_offline_hint")));
		BrowseGameButton().Content(box_value(Utf8ToHString(Text("databody_game_directory"))));
		BrowseSaveButton().Content(box_value(Utf8ToHString(Text("databody_browse"))));
		LoadSaveButton().Content(box_value(Utf8ToHString(Text("databody_load"))));
		GameDirectoryInput().PlaceholderText(Utf8ToHString(Text("databody_choose_game")));
		SavePathInput().PlaceholderText(Utf8ToHString(Text("databody_choose_save")));
		EditTab().Header(box_value(Utf8ToHString(Text("databody_tab_edit"))));
		AddTab().Header(box_value(Utf8ToHString(Text("databody_tab_add"))));
		BodyListHeading().Text(Utf8ToHString(Text("databody_list_heading")));
		EditInheritanceHeading().Text(Utf8ToHString(Text("databody_inheritance_heading")));
		NewInheritanceHeading().Text(Utf8ToHString(Text("databody_inheritance_heading")));
		EditSkillHeading().Text(Utf8ToHString(Text("databody_skill")));
		NewSkillHeading().Text(Utf8ToHString(Text("databody_skill")));
		ApplyAddButton().Content(box_value(Utf8ToHString(Text("databody_add_apply"))));
		ResetAllButton().Content(box_value(Utf8ToHString(Text("databody_reset_all"))));
		SaveAllButton().Content(box_value(Utf8ToHString(Text("databody_save_all"))));
		DraftHint().Text(Utf8ToHString(Text("databody_draft_hint")));
		StrategyHeading().Text(Utf8ToHString(Text("strategy_heading")));
		StrategySearch().PlaceholderText(Utf8ToHString(Text("strategy_search")));
		UseFirstButton().Content(box_value(Utf8ToHString(Text("strategy_use_first"))));
		UseSecondButton().Content(box_value(Utf8ToHString(Text("strategy_use_second"))));
		if (!EditingUid) SelectedBodyText().Text(Utf8ToHString(Text("databody_select_body")));
		const auto Color = NewColorSelector().SelectedIndex();
		SetItems(NewColorSelector(), {box_value(Utf8ToHString(Text("databody_purple"))),
			box_value(Utf8ToHString(Text("databody_gold")))}, Color);
		RefreshSlots(false);
		RefreshSlots(true);
		UpdateDraftStatus();
	}
	void DataBodyEditor::AssertStopped() const
	{
		if (!PresenceMonitor) throw std::runtime_error("The offline editor configuration is unavailable.");
		const auto Presence = PresenceMonitor->Snapshot();
		if (Presence.State != Trainer::GameRunState::Stopped)
			throw std::runtime_error(Presence.State == Trainer::GameRunState::Running ?
				"Fully exit the game before editing or writing a save." : Presence.Error.empty() ?
				"Cannot confirm that the game is stopped; save editing remains blocked." : Presence.Error);
	}
	void DataBodyEditor::AssertNoDrafts() const
	{
		if (Drafts && Drafts->ChangeCount()) throw std::runtime_error(Text("databody_pending_reload"));
	}
	void DataBodyEditor::UpdateAvailability()
	{
		if (Closing || !Settings || !PresenceMonitor) return;
		const auto Presence = PresenceMonitor->Snapshot();
		const bool Stopped = Presence.State == Trainer::GameRunState::Stopped;
		const bool Ready = Stopped && !Busy;
		const bool HasDrafts = Drafts && Drafts->ChangeCount() != 0;
		OfflineControls().IsEnabled(Ready && Catalog && Save);
		GameSourceControls().IsEnabled(!Busy && !HasDrafts);
		SaveSourceControls().IsEnabled(Ready && !HasDrafts);
		ApplyAddButton().IsEnabled(Ready && Catalog && Save && Drafts && NewValid &&
			Drafts->Bodies().size() < SavedCapacity);
		ResetAllButton().IsEnabled(!Busy && HasDrafts);
		SaveAllButton().IsEnabled(Ready && HasDrafts);
		const bool CanUse = Ready && Catalog && Save && PreviewId &&
			(EditorTabs().SelectedIndex() == 1 || EditingUid.has_value());
		UseFirstButton().IsEnabled(CanUse);
		UseSecondButton().IsEnabled(CanUse);
		if (Confirmation) Confirmation.IsPrimaryButtonEnabled(Stopped);
		if (!DisplayedPresence || *DisplayedPresence != Presence)
		{
			DisplayedPresence = Presence;
			OfflineNotice().Text(Utf8ToHString(Text(Stopped ? "databody_offline_hint" : "databody_game_running")));
			if (Presence.State == Trainer::GameRunState::Unknown && !Presence.Error.empty()) Report(Presence.Error);
		}
	}

	fire_and_forget DataBodyEditor::LoadCatalog(std::filesystem::path Directory)
	{
		const auto Lifetime = get_strong();
		const auto Queue = Microsoft::UI::Dispatching::DispatcherQueue::GetForCurrentThread();
		if (Busy || Closing) co_return;
		try { AssertNoDrafts(); } catch (const std::exception& Error) { Report(Error.what()); co_return; }
		Busy = true;
		EditorStatus().Text(Utf8ToHString(Text("databody_catalog_loading")));
		UpdateAvailability();
		const bool Discover = Directory.empty();
		std::shared_ptr<Trainer::DataBodyCatalog> Result;
		std::vector<std::filesystem::path> FoundSaves;
		std::string Error;
		co_await resume_background();
		try
		{
			if (Discover)
			{
				FoundSaves = Trainer::FindGameSaves();
				if (const auto Installed = Trainer::FindInstalledGame()) Directory = *Installed;
			}
			if (!Directory.empty())
				Result = std::make_shared<Trainer::DataBodyCatalog>(Trainer::DataBodyCatalog::Load(Directory));
		}
		catch (const std::exception& Exception) {Error = Exception.what();}
		if (!(co_await ResumeOnUi{Queue}) || Closing) co_return;
		Busy = false;
		TryAction([&]
		{
			if (Discover)
			{
				Saves = std::move(FoundSaves);
				Populating = true;
				for (const auto& Path : Saves) SaveSlotSelector().Items().Append(box_value(hstring(Path.wstring())));
				if (SavePathInput().Text().empty() && !Saves.empty())
					SavePathInput().Text(hstring(Saves.front().wstring()));
				Populating = false;
			}
			if (!Error.empty()) Report(Error);
			else if (!Result) EditorStatus().Text(Utf8ToHString(Text("databody_choose_game")));
			else
			{
				GameDirectoryInput().Text(hstring(Directory.wstring()));
				CatalogReady(std::move(Result));
			}
		});
	}

	void DataBodyEditor::CatalogReady(std::shared_ptr<Trainer::DataBodyCatalog> Result)
	{
		Catalog = std::move(Result);
		Populating = true;
		ActorIds.clear();
		for (const auto& [Id, Actor] : Catalog->Actors) ActorIds.push_back(Id);
		PopulateCatalogLists();
		PopulateSkillSelector(true);
		PopulateFilters();
		Populating = false;
		FilterStrategies();
		RefreshConditions(true);
		EditorStatus().Text(Utf8ToHString(Text("databody_catalog_ready") + " " +
			std::to_string(ActorIds.size()) + " / " + std::to_string(Catalog->Strategies.size())));
		if (!SavePathInput().Text().empty() && PresenceMonitor &&
			PresenceMonitor->Snapshot().State == Trainer::GameRunState::Stopped) LoadSave();
	}
	void DataBodyEditor::PopulateCatalogLists()
	{
		std::vector<Item> Labels;
		for (const auto Id : ActorIds)
			Labels.push_back(box_value(Utf8ToHString(Trainer::DataBodyCatalog::Name(Catalog->Actor(Id).Name, Language))));
		SetItems(NewActorSelector(), std::move(Labels), NewActorSelector().SelectedIndex());
	}
	void DataBodyEditor::PopulateFilters()
	{
		const std::array Previous{ElementFilter().SelectedIndex(), TriggerFilter().SelectedIndex(),
			DualFilter().SelectedIndex(), TierFilter().SelectedIndex()};
		std::set<std::uint32_t> Elements, Triggers, Tiers;
		for (const auto& [Id, Strategy] : Catalog->Strategies)
		{
			if (Strategy.Qualities.empty()) continue;
			Elements.insert(Strategy.Elements.begin(), Strategy.Elements.end());
			Triggers.insert(Strategy.TriggerGroup);
			Tiers.insert(Strategy.Gradation);
		}
		ElementIds.assign(Elements.begin(), Elements.end());
		TriggerIds.assign(Triggers.begin(), Triggers.end());
		TierIds.assign(Tiers.begin(), Tiers.end());
		const auto Category = [this](const char* Prefix, std::uint32_t Id, std::uint32_t Max)
		{
			return Id <= Max ? Text((std::string(Prefix) + std::to_string(Id)).c_str()) : "#" + std::to_string(Id);
		};
		std::vector<Item> ElementLabels{box_value(Utf8ToHString(Text("strategy_all_elements")))};
		std::vector<Item> TriggerLabels{box_value(Utf8ToHString(Text("strategy_all_triggers")))};
		std::vector<Item> TierLabels{box_value(Utf8ToHString(Text("strategy_all_tiers")))};
		for (const auto Id : ElementIds) ElementLabels.push_back(box_value(Utf8ToHString(Category("strategy_element_", Id, 7))));
		for (const auto Id : TriggerIds) TriggerLabels.push_back(box_value(Utf8ToHString(Category("strategy_trigger_", Id, 5))));
		for (const auto Id : TierIds) TierLabels.push_back(box_value(Utf8ToHString(Text("strategy_tier") + std::to_string(Id))));
		SetItems(ElementFilter(), std::move(ElementLabels), Previous[0]);
		SetItems(TriggerFilter(), std::move(TriggerLabels), Previous[1]);
		SetItems(DualFilter(), {box_value(Utf8ToHString(Text("strategy_all_dual"))),
			box_value(Utf8ToHString(Text("strategy_single"))), box_value(Utf8ToHString(Text("strategy_dual")))}, Previous[2]);
		SetItems(TierFilter(), std::move(TierLabels), Previous[3]);
	}
	std::string DataBodyEditor::StrategyMetadata(std::uint32_t Id) const
	{
		const auto& Strategy = Catalog->Strategy(Id);
		std::string Result;
		for (const auto Element : Strategy.Elements)
		{
			if (!Result.empty()) Result += " / ";
			Result += Element <= 7 ? Text(("strategy_element_" + std::to_string(Element)).c_str()) : "#" + std::to_string(Element);
		}
		if (!Result.empty()) Result += " · ";
		Result += Text("strategy_tier") + std::to_string(Strategy.Gradation) + " · " +
			Text(Strategy.IsDual() ? "strategy_dual" : "strategy_single") + " · ";
		Result += Strategy.TriggerGroup <= 5 ? Text(("strategy_trigger_" + std::to_string(Strategy.TriggerGroup)).c_str()) :
			"#" + std::to_string(Strategy.TriggerGroup);
		return Result;
	}
	void DataBodyEditor::FilterStrategies()
	{
		if (!Catalog || Populating) return;
		Trainer::StrategyFilter Filter;
		Filter.Search = to_string(StrategySearch().Text());
		const auto Get = [](const auto& Selector, const auto& Ids) -> std::optional<std::uint32_t>
		{
			const auto Index = Selector.SelectedIndex();
			if (Index <= 0 || static_cast<std::size_t>(Index) > Ids.size()) return {};
			return Ids[Index - 1];
		};
		Filter.Element = Get(ElementFilter(), ElementIds);
		Filter.Trigger = Get(TriggerFilter(), TriggerIds);
		Filter.Tier = Get(TierFilter(), TierIds);
		if (DualFilter().SelectedIndex() > 0) Filter.Dual = DualFilter().SelectedIndex() == 2;
		FilteredStrategyIds = Catalog->FilterStrategies(Filter);
		std::vector<Item> Labels;
		int Selected = -1;
		for (std::size_t Index = 0; Index < FilteredStrategyIds.size(); ++Index)
		{
			const auto Id = FilteredStrategyIds[Index];
			Labels.push_back(box_value(Utf8ToHString(Trainer::DataBodyCatalog::Name(Catalog->Strategy(Id).Name, Language) +
				" · " + std::to_string(Id) + "\n" + StrategyMetadata(Id))));
			if (PreviewId == Id) Selected = static_cast<int>(Index);
		}
		Populating = true;
		StrategyList().ItemsSource(single_threaded_observable_vector<Item>(std::move(Labels)));
		StrategyList().SelectedIndex(Selected);
		Populating = false;
		StrategyCount().Text(Utf8ToHString(Text(FilteredStrategyIds.empty() ? "strategy_no_results" : "strategy_results") +
			(FilteredStrategyIds.empty() ? "" : std::to_string(FilteredStrategyIds.size()))));
		PreviewStrategy();
		UpdateAvailability();
	}
	std::string DataBodyEditor::Conditions(std::uint32_t Id, const std::vector<Trainer::StrategyChoice>& Choices) const
	{
		const auto Required = Catalog->InheritanceConditions(Id, Choices);
		std::string Description;
		for (const auto& Alternatives : Required)
		{
			if (!Description.empty()) Description += " + ";
			std::set<std::string> Seen;
			std::string Group;
			for (const auto Provider : Alternatives)
			{
				const auto Name = Trainer::DataBodyCatalog::Name(Catalog->Strategy(Provider).Name, Language);
				if (!Seen.insert(Name).second) continue;
				if (!Group.empty()) Group += " / ";
				Group += Name;
			}
			Description += Seen.size() > 1 ? "[" + Group + "]" : Group;
		}
		return Description.empty() ? Text("databody_no_requirement") : Text("databody_condition_prefix") + Description;
	}
	void DataBodyEditor::PreviewStrategy()
	{
		if (!Catalog || !PreviewId)
		{
			StrategyPreview().Text(Utf8ToHString(Text("strategy_select")));
			return;
		}
		std::string Description = Trainer::DataBodyCatalog::Name(Catalog->Strategy(*PreviewId).Name, Language) +
			" · " + std::to_string(*PreviewId) + "\n" + StrategyMetadata(*PreviewId);
		try { Description += "\n" + Conditions(*PreviewId, InheritedChoices(EditorTabs().SelectedIndex() == 1)); }
		catch (const std::exception& Error) { Description += "\n" + Settings->Texts.Translate(Error.what(), Language); }
		StrategyPreview().Text(Utf8ToHString(Description));
	}

	fire_and_forget DataBodyEditor::PickPath(bool Folder)
	{
		const auto Lifetime = get_strong();
		const auto Queue = Microsoft::UI::Dispatching::DispatcherQueue::GetForCurrentThread();
		if (Busy || Closing) co_return;
		const auto PreviousStatus = EditorStatus().Text();
		std::optional<std::filesystem::path> Selected;
		std::string Error;
		try
		{
			AssertNoDrafts();
			if (!Folder) AssertStopped();
			Busy = true;
			EditorStatus().Text(Utf8ToHString(Text("databody_picker_opening")));
			UpdateAvailability();
			const auto Owner = OwnerId;
			if (Folder)
			{
				Microsoft::Windows::Storage::Pickers::FolderPicker Picker(Owner);
				Picker.CommitButtonText(Utf8ToHString(Text("databody_choose_game")));
				const auto Operation = Picker.PickSingleFolderAsync();
				ActivePathPicker = Operation.as<Windows::Foundation::IAsyncInfo>();
				if (const auto Result = co_await Operation)
					Selected = std::filesystem::path(std::wstring(Result.Path()));
			}
			else
			{
				Microsoft::Windows::Storage::Pickers::FileOpenPicker Picker(Owner);
				Picker.CommitButtonText(Utf8ToHString(Text("databody_load")));
				const auto Operation = Picker.PickSingleFileAsync();
				ActivePathPicker = Operation.as<Windows::Foundation::IAsyncInfo>();
				if (const auto Result = co_await Operation)
					Selected = std::filesystem::path(std::wstring(Result.Path()));
			}
		}
		catch (const hresult_canceled&)
		{
			Selected.reset();
		}
		catch (const hresult_error& Exception) {Error = to_string(Exception.message());}
		catch (const std::exception& Exception) {Error = Exception.what();}
		if (!(co_await ResumeOnUi{Queue}) || Closing) co_return;
		ActivePathPicker = nullptr;
		Busy = false;
		EditorStatus().Text(PreviousStatus);
		TryAction([&]
		{
			if (!Error.empty()) Report(Error);
			else if (Selected)
			{
				if (Folder)
				{
					GameDirectoryInput().Text(hstring(Selected->wstring()));
					LoadCatalog(*Selected);
				}
				else
				{
					AssertStopped();
					SavePathInput().Text(hstring(Selected->wstring()));
					LoadSave();
				}
			}
		});
	}

	fire_and_forget DataBodyEditor::LoadSave()
	{
		const auto Lifetime = get_strong();
		const auto Queue = Microsoft::UI::Dispatching::DispatcherQueue::GetForCurrentThread();
		if (Busy || Closing) co_return;
		std::filesystem::path Path;
		try
		{
			AssertNoDrafts();
			AssertStopped();
			if (!Catalog)
			{
				if (GameDirectoryInput().Text().empty())
					throw std::runtime_error("Select the current game's installation directory first.");
				LoadCatalog(std::filesystem::path(std::wstring(GameDirectoryInput().Text())));
				co_return;
			}
			Path = std::filesystem::path(std::wstring(SavePathInput().Text()));
			if (Path.empty()) throw std::runtime_error(Text("databody_choose_save"));
		}
		catch (const std::exception& Error) { Report(Error.what()); UpdateAvailability(); co_return; }
		catch (const hresult_error& Error) { Report(to_string(Error.message())); UpdateAvailability(); co_return; }
		Busy = true;
		EditorStatus().Text(Utf8ToHString(Text("databody_save_loading")));
		UpdateAvailability();
		const auto Configuration = *Settings;
		const auto CurrentCatalog = Catalog;
		const auto Stop = BackgroundStop.get_token();
		std::shared_ptr<Trainer::DataBodySave> Loaded;
		std::vector<Trainer::DataBodyInfo> Bodies;
		std::size_t Capacity = 0;
		std::string Error;
		co_await resume_background();
		try
		{
			Loaded = std::make_shared<Trainer::DataBodySave>(Path, [Configuration, Stop]
			{
				if (Stop.stop_requested()) throw std::runtime_error("The offline operation was cancelled.");
				const auto Presence = Trainer::RemoteProcess::ObserveGamePresence(Configuration);
				if (Presence.State != Trainer::GameRunState::Stopped)
					throw std::runtime_error(Presence.State == Trainer::GameRunState::Running ?
						"Fully exit the game before editing or writing a save." : Presence.Error);
			});
			Bodies = Loaded->Bodies();
			Capacity = Loaded->Capacity(*CurrentCatalog);
		}
		catch (const std::exception& Exception) { Error = Exception.what(); }
		if (!(co_await ResumeOnUi{Queue}) || Closing) co_return;
		Busy = false;
		TryAction([&]
		{
			if (!Error.empty()) Report(Error);
			else
			{
				AssertStopped();
				Save = std::move(Loaded);
				RefreshBodies(std::move(Bodies), Capacity);
				Report(Text("databody_loaded"));
			}
			EditorStatus().Text(Utf8ToHString(Text("databody_catalog_ready") + " " +
				std::to_string(ActorIds.size()) + " / " + std::to_string(CurrentCatalog->Strategies.size())));
		});
	}
	void DataBodyEditor::RefreshBodies(std::vector<Trainer::DataBodyInfo> Bodies, std::size_t Capacity,
		std::optional<std::uint32_t> Select)
	{
		auto LoadedDrafts = std::make_unique<Trainer::DataBodyDrafts>(Bodies);
		SavedBodies = std::move(Bodies);
		SavedCapacity = Capacity;
		Drafts = std::move(LoadedDrafts);
		UpdateBodyList(Select);
		PopulateBody();
		UpdateDraftStatus();
	}
	std::string DataBodyEditor::DescribeBody(const Trainer::DataBodyDraft& Body) const
	{
		const auto Actor = Catalog->Actors.find(Body.ActorId);
		std::string Result = (Drafts->Changed(Body.Uid) ? "● " : "") +
			(Body.Added ? Text("databody_new") : std::to_string(Body.Uid)) + " · " +
			(Actor == Catalog->Actors.end() ? std::to_string(Body.ActorId) : Trainer::DataBodyCatalog::Name(Actor->second.Name, Language));
		for (std::size_t Index = 0; Index < 2; ++Index)
		{
			if (!Body.Inherited[Index]) continue;
			const auto Strategy = Catalog->Strategies.find(Body.Inherited[Index]->Id);
			Result += "\n" + (Strategy == Catalog->Strategies.end() ? std::to_string(Body.Inherited[Index]->Id) :
				Trainer::DataBodyCatalog::Name(Strategy->second.Name, Language)) + " · Q" +
				std::to_string(Body.Inherited[Index]->Quality);
		}
		if (Actor != Catalog->Actors.end() && Body.SkillIndex < Actor->second.InheritanceSkills.size())
			Result += "\n" + Catalog->InheritanceSkillName(Actor->second.InheritanceSkills[Body.SkillIndex], Language);
		return Result;
	}
	void DataBodyEditor::UpdateBodyList(std::optional<std::uint32_t> Select)
	{
		const bool WasPopulating = Populating;
		Populating = true;
		std::vector<Item> Labels;
		int Selected = -1;
		const auto Requested = Select ? Select : EditingUid;
		if (Drafts)
		{
			for (const auto& Body : Drafts->Bodies())
			{
				if (Requested == Body.Uid) Selected = static_cast<int>(Labels.size());
				Labels.push_back(box_value(Utf8ToHString(DescribeBody(Body))));
			}
		}
		if (Selected < 0 && !Labels.empty()) Selected = 0;
		BodyList().ItemsSource(single_threaded_observable_vector<Item>(std::move(Labels)));
		BodyList().SelectedIndex(Selected);
		Populating = WasPopulating;
	}
	void DataBodyEditor::PopulateBody()
	{
		EditingUid.reset();
		Selections[0] = {};
		const auto Selected = BodyList().SelectedIndex();
		if (!Catalog || !Drafts || Selected < 0 || static_cast<std::size_t>(Selected) >= Drafts->Bodies().size())
		{
			SelectedBodyText().Text(Utf8ToHString(Text("databody_select_body")));
			RefreshSlots(false);
			UpdateAvailability();
			return;
		}
		const auto& Body = Drafts->Bodies()[Selected];
		EditingUid = Body.Uid;
		Selections[0] = Body.Inherited;
		const auto Description = DescribeBody(Body);
		SelectedBodyText().Text(Utf8ToHString(Description.substr(0, Description.find('\n'))));
		Populating = true;
		RefreshSlots(false);
		PopulateSkillSelector(false);
		EditSkillHeading().Text(Utf8ToHString(Text("databody_skill") + " · Lv." + std::to_string(Body.SkillLevel + 1)));
		Populating = false;
		RefreshConditions(false);
		UpdateAvailability();
	}
	void DataBodyEditor::PopulateSkillSelector(bool Added, int Selected)
	{
		const auto Selector = Added ? NewSkillSelector() : EditSkillSelector();
		std::vector<Item> Labels;
		std::uint32_t ActorId = 0;
		if (Added)
		{
			const auto Index = NewActorSelector().SelectedIndex();
			if (Index >= 0 && static_cast<std::size_t>(Index) < ActorIds.size()) ActorId = ActorIds[Index];
			if (Selected < 0) Selected = 0;
		}
		else if (EditingUid && Drafts)
		{
			const auto& Draft = Drafts->Body(*EditingUid);
			ActorId = Draft.ActorId;
			if (Selected < 0) Selected = static_cast<int>(Draft.SkillIndex);
		}
		if (ActorId && Catalog->Actors.contains(ActorId))
			for (const auto Id : Catalog->Actor(ActorId).InheritanceSkills)
				Labels.push_back(box_value(Utf8ToHString(Catalog->InheritanceSkillName(Id, Language))));
		const bool KnownSelection = Selected >= 0 && static_cast<std::size_t>(Selected) < Labels.size();
		SetItems(Selector, std::move(Labels), Selected);
		if (!KnownSelection) Selector.SelectedIndex(-1);
	}
	std::vector<Trainer::StrategyChoice> DataBodyEditor::InheritedChoices(bool Added) const
	{
		std::vector<Trainer::StrategyChoice> Result;
		for (const auto& Choice : Selections[Added ? 1 : 0]) if (Choice) Result.push_back(*Choice);
		return Result;
	}
	void DataBodyEditor::RefreshSlots(bool Added)
	{
		const bool WasPopulating = Populating;
		Populating = true;
		const std::array Labels{Added ? NewFirstLabel() : EditFirstLabel(), Added ? NewSecondLabel() : EditSecondLabel()};
		const std::array Qualities{Added ? NewQualityFirst() : EditQualityFirst(), Added ? NewQualitySecond() : EditQualitySecond()};
		for (std::size_t Index = 0; Index < 2; ++Index)
		{
			const auto& Choice = Selections[Added ? 1 : 0][Index];
			Labels[Index].Text(Utf8ToHString(Choice && Catalog && Catalog->Strategies.contains(Choice->Id) ?
				Trainer::DataBodyCatalog::Name(Catalog->Strategy(Choice->Id).Name, Language) : Choice ?
				std::to_string(Choice->Id) : Text("strategy_empty_slot") + std::to_string(Index + 1)));
			std::vector<Item> Values;
			int Selected = -1;
			if (Choice && Catalog && Catalog->Strategies.contains(Choice->Id))
			{
				for (const auto Quality : Catalog->Strategy(Choice->Id).Qualities)
				{
					if (Quality == Choice->Quality) Selected = static_cast<int>(Values.size());
					Values.push_back(box_value(to_hstring(Quality)));
				}
			}
			SetItems(Qualities[Index], std::move(Values), Selected);
			if (Selected < 0) Qualities[Index].SelectedIndex(-1);
			Qualities[Index].IsEnabled(Choice.has_value());
		}
		Populating = WasPopulating;
	}
	void DataBodyEditor::RefreshConditions(bool Added)
	{
		if (!Catalog || Populating) return;
		const std::array Prompts{Added ? NewConditionFirst() : EditConditionFirst(), Added ? NewConditionSecond() : EditConditionSecond()};
		if (Added) NewValid = false;
		try
		{
			const auto Choices = InheritedChoices(Added);
			Catalog->ValidateInheritance(Choices);
			for (std::size_t Index = 0; Index < 2; ++Index)
			{
				const auto& Choice = Selections[Added ? 1 : 0][Index];
				Prompts[Index].Text(Choice ? Utf8ToHString(Conditions(Choice->Id, Choices)) : L"");
			}
			if (Added) NewValid = Choices.size() == 2 && NewSkillSelector().SelectedIndex() >= 0;
		}
		catch (const std::exception& Error)
		{
			Prompts[0].Text(Utf8ToHString(Settings->Texts.Translate(Error.what(), Language)));
			Prompts[1].Text(L"");
		}
		PreviewStrategy();
		UpdateAvailability();
	}
	void DataBodyEditor::CacheCurrentBody()
	{
		if (Populating || !Drafts || !EditingUid) return;
		auto& Draft = Drafts->Body(*EditingUid);
		Draft.Inherited = Selections[0];
		const auto Skill = EditSkillSelector().SelectedIndex();
		Draft.SkillIndex = Skill < 0 ? UINT32_MAX : static_cast<std::uint32_t>(Skill);
		const auto Description = DescribeBody(Draft);
		SelectedBodyText().Text(Utf8ToHString(Description.substr(0, Description.find('\n'))));
		const bool WasPopulating = Populating;
		Populating = true;
		const auto Index = BodyList().SelectedIndex();
		if (Index >= 0)
		{
			BodyList().ItemsSource().as<Windows::Foundation::Collections::IVector<Item>>().SetAt(
				static_cast<std::uint32_t>(Index), box_value(Utf8ToHString(Description)));
			BodyList().SelectedIndex(Index);
		}
		Populating = WasPopulating;
		UpdateDraftStatus();
	}
	void DataBodyEditor::UpdateDraftStatus()
	{
		if (!Settings) return;
		const auto Count = Drafts ? Drafts->ChangeCount() : 0;
		PendingText().Text(Utf8ToHString(Count ? Text("databody_pending") + std::to_string(Count) +
			" · " + Text("databody_draft_only") : Text("databody_no_changes")));
		WarehouseInfo().Text(Save && Drafts ? Utf8ToHString(Text("databody_capacity") + " " +
			std::to_string(Drafts->Bodies().size()) + " / " + std::to_string(SavedCapacity)) : L"");
	}
	Trainer::DataBodyRequest DataBodyEditor::Request(bool Added)
	{
		if (!Added)
		{
			if (!Drafts || !EditingUid) throw std::runtime_error(Text("databody_select_body"));
			return Drafts->Body(*EditingUid).Request(*Catalog);
		}
		Trainer::DataBodyRequest Result;
		Result.Inherited = InheritedChoices(true);
		const auto Actor = NewActorSelector().SelectedIndex();
		if (Actor < 0 || static_cast<std::size_t>(Actor) >= ActorIds.size())
			throw std::runtime_error("Select a character from the current game catalog.");
		Result.ActorId = ActorIds[Actor];
		const auto Color = NewColorSelector().SelectedIndex();
		if (Color != 0 && Color != 1) throw std::runtime_error("Select a valid data-body quality.");
		Result.Score = Color == 1 ? 700 : 600;
		const auto Skill = NewSkillSelector().SelectedIndex();
		const auto& Skills = Catalog->Actor(Result.ActorId).InheritanceSkills;
		if (Skill < 0 || static_cast<std::size_t>(Skill) >= Skills.size())
			throw std::runtime_error("Select an inheritance skill belonging to this data body's character.");
		Result.InheritanceSkillId = Skills[Skill];
		return Result;
	}
	fire_and_forget DataBodyEditor::ConfirmSave()
	{
		const auto Lifetime = get_strong();
		const auto Queue = Microsoft::UI::Dispatching::DispatcherQueue::GetForCurrentThread();
		if (Busy || !Save || !Catalog || !Drafts || Closing) co_return;
		std::vector<Trainer::DataBodyRequest> Requests;
		try { AssertStopped(); CacheCurrentBody(); Requests = Drafts->Requests(*Catalog); }
		catch (const std::exception& Error) { Report(Error.what()); co_return; }
		const auto CurrentSave = Save;
		const auto CurrentCatalog = Catalog;
		Busy = true;
		EditorStatus().Text(Utf8ToHString(Text("databody_save_preparing")));
		UpdateAvailability();
		Trainer::DataBodyPlan Plan;
		std::string Error;
		co_await resume_background();
		try { Plan = CurrentSave->PrepareBatch(*CurrentCatalog, Requests); }
		catch (const std::exception& Exception) { Error = Exception.what(); }
		if (!(co_await ResumeOnUi{Queue}) || Closing) co_return;
		try
		{
			if (!Error.empty()) throw std::runtime_error(Error);
			AssertStopped();
			Confirmation = Microsoft::UI::Xaml::Controls::ContentDialog();
			Confirmation.XamlRoot(EditorRoot().XamlRoot());
			Confirmation.Title(box_value(Utf8ToHString(Text("databody_confirm_title"))));
			std::string Details = Text("databody_confirm_notice") + "\n\n" + PathText(CurrentSave->Path());
			const auto DescribeChoices = [&](const std::vector<Trainer::StrategyChoice>& Choices)
			{
				std::string Result;
				for (const auto& Choice : Choices)
				{
					const auto Found = CurrentCatalog->Strategies.find(Choice.Id);
					if (!Result.empty()) Result += " / ";
					Result += (Found == CurrentCatalog->Strategies.end() ? std::to_string(Choice.Id) :
						Trainer::DataBodyCatalog::Name(Found->second.Name, Language)) + " · Q" + std::to_string(Choice.Quality);
				}
				return Result.empty() ? Text("databody_none") : Result;
			};
			for (std::size_t Index = 0; Index < Requests.size(); ++Index)
			{
				const auto& Request = Requests[Index];
				const auto& Actor = CurrentCatalog->Actor(Request.ActorId);
				Details += "\n\n" + (Request.ExistingUid ? Text("databody_edit") : Text("databody_new")) +
					" · UID " + std::to_string(Plan.ChangedUids.at(Index)) + " · " +
					Trainer::DataBodyCatalog::Name(Actor.Name, Language);
				if (Request.ExistingUid)
				{
					const auto Before = std::find_if(SavedBodies.begin(), SavedBodies.end(),
						[&](const auto& Body) { return Body.Uid == *Request.ExistingUid; });
					if (Before != SavedBodies.end())
					{
						Details += "\n" + Text("databody_before") + DescribeChoices(Before->Inherited);
						if (Before->InheritanceSkillIndex < Actor.InheritanceSkills.size())
							Details += "\n" + Text("databody_skill") + ": " +
								CurrentCatalog->InheritanceSkillName(Actor.InheritanceSkills[Before->InheritanceSkillIndex], Language);
					}
				}
				else Details += " · " + std::to_string(static_cast<int>(Request.Score));
				Details += "\n" + Text("databody_after") + DescribeChoices(Request.Inherited) + "\n" +
					Text("databody_skill") + ": " + CurrentCatalog->InheritanceSkillName(*Request.InheritanceSkillId, Language);
			}
			Microsoft::UI::Xaml::Controls::TextBlock Summary;
			Summary.TextWrapping(Microsoft::UI::Xaml::TextWrapping::Wrap);
			Summary.Text(Utf8ToHString(Details));
			Microsoft::UI::Xaml::Controls::ScrollViewer Review;
			Review.MaxHeight(400);
			Review.Content(Summary);
			Confirmation.Content(Review);
			Confirmation.PrimaryButtonText(Utf8ToHString(Text("databody_backup_apply")));
			Confirmation.CloseButtonText(Utf8ToHString(Text("databody_cancel")));
			Confirmation.DefaultButton(Microsoft::UI::Xaml::Controls::ContentDialogButton::Close);
			UpdateAvailability();
			const auto Response = co_await Confirmation.ShowAsync();
			Confirmation = nullptr;
			if (Response == Microsoft::UI::Xaml::Controls::ContentDialogResult::Primary && !Closing)
			{
				EditorStatus().Text(Utf8ToHString(Text("databody_save_writing")));
				std::filesystem::path Backup;
				std::vector<Trainer::DataBodyInfo> Bodies;
				std::size_t Capacity = 0;
				Error.clear();
				co_await resume_background();
				try
				{
					Backup = CurrentSave->Commit(Plan, true);
					Bodies = CurrentSave->Bodies();
					Capacity = CurrentSave->Capacity(*CurrentCatalog);
				}
				catch (const std::exception& Exception) { Error = Exception.what(); }
				if (!(co_await ResumeOnUi{Queue}) || Closing) co_return;
				if (!Error.empty()) throw std::runtime_error(Error);
				RefreshBodies(std::move(Bodies), Capacity, Plan.Uid);
				Report(Text("databody_saved") + "\n" + PathText(Backup));
			}
		}
		catch (const std::exception& Exception) { if (!Closing) Report(Exception.what()); }
		catch (const hresult_error& Exception) { if (!Closing) Report(to_string(Exception.message())); }
		Confirmation = nullptr;
		Busy = false;
		EditorStatus().Text(Utf8ToHString(Text("databody_catalog_ready") + " " +
			std::to_string(ActorIds.size()) + " / " + std::to_string(CurrentCatalog->Strategies.size())));
		UpdateAvailability();
	}
	void DataBodyEditor::Shutdown()
	{
		if (Closing) return;
		Closing = true;
		BackgroundStop.request_stop();
		if (const auto Picker = std::exchange(ActivePathPicker, nullptr))
		{
			try { Picker.Cancel(); }
			catch (const hresult_error& Error) { OutputDebugStringW(Error.message().c_str()); }
		}
		if (Confirmation) Confirmation.Hide();
		if (GameTimer) { GameTimer.Stop(); GameTimer = nullptr; }
		PresenceMonitor.reset();
	}
	void DataBodyEditor::OnBrowseGame(Item const&, Microsoft::UI::Xaml::RoutedEventArgs const&) { PickPath(true); }
	void DataBodyEditor::OnBrowseSave(Item const&, Microsoft::UI::Xaml::RoutedEventArgs const&) { PickPath(false); }
	void DataBodyEditor::OnLoadSave(Item const&, Microsoft::UI::Xaml::RoutedEventArgs const&) { LoadSave(); }
	void DataBodyEditor::OnSaveSlotChanged(Item const&, Microsoft::UI::Xaml::Controls::SelectionChangedEventArgs const&)
	{
		if (!Populating) TryAction([this]
		{
			AssertNoDrafts();
			const auto Index = SaveSlotSelector().SelectedIndex();
			if (Index > 0 && static_cast<std::size_t>(Index) <= Saves.size())
			{ SavePathInput().Text(hstring(Saves[Index - 1].wstring())); LoadSave(); }
		});
	}
	void DataBodyEditor::OnBodyChanged(Item const&, Microsoft::UI::Xaml::Controls::SelectionChangedEventArgs const&)
	{ if (!Populating) TryAction([this] { PopulateBody(); }); }
	void DataBodyEditor::OnActorChanged(Item const&, Microsoft::UI::Xaml::Controls::SelectionChangedEventArgs const&)
	{
		if (!Populating && Catalog) TryAction([this]
		{
			Populating = true;
			PopulateSkillSelector(true);
			Populating = false;
			RefreshConditions(true);
		});
	}
	void DataBodyEditor::OnQualityChanged(Item const& Sender, Microsoft::UI::Xaml::Controls::SelectionChangedEventArgs const&)
	{
		if (!Populating) TryAction([this, Sender]
		{
			const bool Added = Sender == NewQualityFirst() || Sender == NewQualitySecond();
			const std::size_t Index = Sender == NewQualitySecond() || Sender == EditQualitySecond() ? 1 : 0;
			auto& Choice = Selections[Added ? 1 : 0][Index];
			const auto Selector = Sender.as<Microsoft::UI::Xaml::Controls::ComboBox>();
			if (Choice && Selector.SelectedIndex() >= 0)
				Choice->Quality = static_cast<std::uint32_t>(std::stoul(to_string(unbox_value<hstring>(Selector.SelectedItem()))));
			if (!Added) CacheCurrentBody();
			RefreshConditions(Added);
		});
	}
	void DataBodyEditor::OnSkillChanged(Item const& Sender, Microsoft::UI::Xaml::Controls::SelectionChangedEventArgs const&)
	{
		if (!Populating) TryAction([this, Sender]
		{
			if (Sender == EditSkillSelector()) CacheCurrentBody();
			else RefreshConditions(true);
		});
	}
	void DataBodyEditor::OnTabChanged(Item const&, Microsoft::UI::Xaml::Controls::SelectionChangedEventArgs const&)
	{ if (!Populating && Settings) TryAction([this] { PreviewStrategy(); }); }
	void DataBodyEditor::OnInspectSlot(Item const& Sender, Microsoft::UI::Xaml::RoutedEventArgs const&)
	{
		TryAction([this, Sender]
		{
			const auto [Added, Index] = Slot(Sender);
			const auto& Choice = Selections[Added ? 1 : 0][Index];
			if (Choice) { PreviewId = Choice->Id; PreviewStrategy(); }
		});
	}
	void DataBodyEditor::OnClearSlot(Item const& Sender, Microsoft::UI::Xaml::RoutedEventArgs const&)
	{
		TryAction([this, Sender]
		{
			AssertStopped();
			const auto [Added, Index] = Slot(Sender);
			if (!Added && !EditingUid) return;
			Selections[Added ? 1 : 0][Index].reset();
			RefreshSlots(Added);
			if (!Added) CacheCurrentBody();
			RefreshConditions(Added);
		});
	}
	void DataBodyEditor::OnUseStrategy(Item const& Sender, Microsoft::UI::Xaml::RoutedEventArgs const&)
	{
		TryAction([this, Sender]
		{
			AssertStopped();
			if (!PreviewId || !Catalog) return;
			const bool Added = EditorTabs().SelectedIndex() == 1;
			if (!Added && !EditingUid) return;
			const auto& Strategy = Catalog->Strategy(*PreviewId);
			const auto Index = unbox_value<hstring>(Sender.as<Microsoft::UI::Xaml::Controls::Button>().Tag()) == L"1" ? 1 : 0;
			auto& Choice = Selections[Added ? 1 : 0][Index];
			const auto Quality = Choice && Strategy.Qualities.contains(Choice->Quality) ? Choice->Quality : *Strategy.Qualities.rbegin();
			Choice = Trainer::StrategyChoice{*PreviewId, Quality};
			RefreshSlots(Added);
			if (!Added) CacheCurrentBody();
			RefreshConditions(Added);
		});
	}
	void DataBodyEditor::OnStrategySearchChanged(Item const&, Microsoft::UI::Xaml::Controls::TextChangedEventArgs const&)
	{ if (!Populating) TryAction([this] { FilterStrategies(); }); }
	void DataBodyEditor::OnStrategyFilterChanged(Item const&, Microsoft::UI::Xaml::Controls::SelectionChangedEventArgs const&)
	{ if (!Populating) TryAction([this] { FilterStrategies(); }); }
	void DataBodyEditor::OnStrategySelected(Item const&, Microsoft::UI::Xaml::Controls::SelectionChangedEventArgs const&)
	{
		if (!Populating) TryAction([this]
		{
			const auto Index = StrategyList().SelectedIndex();
			PreviewId = Index < 0 || static_cast<std::size_t>(Index) >= FilteredStrategyIds.size() ?
				std::optional<std::uint32_t>{} : FilteredStrategyIds[Index];
			PreviewStrategy();
		});
	}
	void DataBodyEditor::OnApplyAdd(Item const&, Microsoft::UI::Xaml::RoutedEventArgs const&)
	{
		TryAction([this]
		{
			AssertStopped();
			if (!Drafts || !Catalog || Busy) return;
			if (Drafts->Bodies().size() >= SavedCapacity)
				throw std::runtime_error("The currently unlocked data-body warehouse is full.");
			const auto Uid = Drafts->Add(Request(true), *Catalog);
			UpdateBodyList(Uid);
			PopulateBody();
			EditorTabs().SelectedIndex(0);
			UpdateDraftStatus();
			Report(Text("databody_staged"));
		});
	}
	void DataBodyEditor::OnResetAll(Item const&, Microsoft::UI::Xaml::RoutedEventArgs const&)
	{
		if (!Busy) TryAction([this]
		{
			if (!Drafts) return;
			Drafts->Reset();
			Selections[1] = {};
			RefreshSlots(true);
			Populating = true;
			PopulateSkillSelector(true, 0);
			NewColorSelector().SelectedIndex(0);
			Populating = false;
			UpdateBodyList();
			PopulateBody();
			RefreshConditions(true);
			UpdateDraftStatus();
			Report(Text("databody_reset_done"));
		});
	}
	void DataBodyEditor::OnSaveAll(Item const&, Microsoft::UI::Xaml::RoutedEventArgs const&) { ConfirmSave(); }
	void DataBodyEditor::SetLanguage(TrainerLanguage SelectedLanguage)
	{
		Language = SelectedLanguage;
		if (!Settings || Closing) return;
		TryAction([this]
		{
			const auto EditSkill = EditSkillSelector().SelectedIndex();
			const auto NewSkill = NewSkillSelector().SelectedIndex();
			Populating = true;
			InitializeText();
			DisplayedPresence.reset();
			if (Catalog)
			{
				PopulateCatalogLists();
				UpdateBodyList();
				PopulateFilters();
				PopulateSkillSelector(false, EditSkill);
				PopulateSkillSelector(true, NewSkill);
			}
			Populating = false;
			if (Catalog)
			{
				FilterStrategies();
				if (EditingUid && Drafts)
				{
					const auto& Body = Drafts->Body(*EditingUid);
					const auto Description = DescribeBody(Body);
					SelectedBodyText().Text(Utf8ToHString(Description.substr(0, Description.find('\n'))));
					EditSkillHeading().Text(Utf8ToHString(Text("databody_skill") + " · Lv." + std::to_string(Body.SkillLevel + 1)));
				}
				RefreshConditions(false);
				RefreshConditions(true);
			}
			if (!LastMessage.empty()) SaveOperationText().Text(Utf8ToHString(Settings->Texts.Translate(LastMessage, Language)));
		});
	}
}
