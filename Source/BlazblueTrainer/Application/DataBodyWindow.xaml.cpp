#include "Pch.h"
#include "DataBodyWindow.xaml.h"
#include "Paths.h"
#include "RemoteProcess.h"
#include "UiHelpers.h"
#if __has_include("DataBodyWindow.g.cpp")
#include "DataBodyWindow.g.cpp"
#endif

#include <microsoft.ui.xaml.window.h>
#include <winrt/Microsoft.UI.Windowing.h>
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
        const std::vector<Item>& Items, int Selected)
    {
        auto Values = Items;
        Selector.ItemsSource(winrt::single_threaded_observable_vector<Item>(std::move(Values)));
        Selector.SelectedIndex(Items.empty() ? -1 : std::clamp(Selected, 0, static_cast<int>(Items.size()) - 1));
    }

    std::string PathText(const std::filesystem::path& Path)
    {
        const auto Value = Path.u8string();
        return std::string(Value.begin(), Value.end());
    }
}

namespace winrt::BlazblueTrainer::implementation
{
    DataBodyWindow::DataBodyWindow(TrainerLanguage SelectedLanguage,
        std::shared_ptr<Trainer::GamePresenceMonitor> Monitor)
        : Language(SelectedLanguage), PresenceMonitor(std::move(Monitor))
    {
        InitializeComponent();
        Title(L"Blazblue Entropy Effect Trainer");
        Closed([this](auto const&, auto const&) {Shutdown();});
        try
        {
            Settings = Trainer::LoadConfig(Trainer::DefaultConfigPath());
            if (!PresenceMonitor)
                PresenceMonitor = std::make_shared<Trainer::GamePresenceMonitor>(*Settings);
            const auto NativeWindow = this->try_as<::IWindowNative>();
            if (!NativeWindow) throw hresult_no_interface();
            check_hresult(NativeWindow->get_WindowHandle(&WindowHandle));
            NativeBehavior = std::make_unique<Trainer::WindowBehavior>(
                WindowHandle, Trainer::WindowConfig{780, 680},
                std::array<std::optional<Trainer::HotkeyBinding>, 3>{}, [](TrainerFeature) {},
                [this](const std::string& Error) {Report(Error);});
            InitializeText();
            SaveSlotSelector().Items().Append(box_value(Utf8ToHString(Text("databody_choose_save"))));
            SaveSlotSelector().SelectedIndex(0);
            EditorTabs().SelectedIndex(0);
            const auto Queue = Microsoft::UI::Dispatching::DispatcherQueue::GetForCurrentThread();
            if (!Queue) throw std::runtime_error("Cannot create the offline game-state timer.");
            GameTimer = Queue.CreateTimer();
            GameTimer.Interval(std::chrono::milliseconds(500));
            GameTimer.Tick([this](auto const&, auto const&) {UpdateAvailability();});
            GameTimer.Start();
            Populating = false;
            UpdateAvailability();
            LoadCatalog({});
        }
        catch (const std::exception& Error) {Report(Error.what()); OfflineControls().IsEnabled(false);}
        catch (const hresult_error& Error) {Report(to_string(Error.message())); OfflineControls().IsEnabled(false);}
    }

    DataBodyWindow::~DataBodyWindow() {Shutdown();}

    std::string DataBodyWindow::Text(const char* Key) const
    {
        if (!Settings) throw std::runtime_error("The offline editor configuration is unavailable.");
        return Settings->Text(Key, Language);
    }

    void DataBodyWindow::Report(const std::string& Error)
    {
        LastMessage = Settings ? Settings->Texts.Translate(Error, TrainerLanguageEnglish) : Error;
        SaveOperationText().Text(Utf8ToHString(Settings ?
            Settings->Texts.Translate(LastMessage, Language) : LastMessage));
    }

    void DataBodyWindow::TryAction(const std::function<void()>& Action)
    {
        if (Closing) return;
        try {Action();}
        catch (const std::exception& Error) {Populating = false; Report(Error.what());}
        catch (const hresult_error& Error) {Populating = false; Report(to_string(Error.message()));}
        UpdateAvailability();
    }

    void DataBodyWindow::InitializeText()
    {
        EditorTitle().Text(Utf8ToHString(Text("databody_title")));
        EditorLanguageButton().Content(box_value(Language == TrainerLanguageEnglish ? L"中文" : L"English"));
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
        ApplyEditButton().Content(box_value(Utf8ToHString(Text("databody_preview_apply"))));
        ApplyAddButton().Content(box_value(Utf8ToHString(Text("databody_add_apply"))));
        if (!EditingUid) SelectedBodyText().Text(Utf8ToHString(Text("databody_select_body")));
        for (const auto& Selector : {EditQualityFirst(), EditQualitySecond(), NewQualityFirst(), NewQualitySecond()})
        {
            if (Selector.Items().Size()) continue;
            for (unsigned Quality = 1; Quality <= 4; ++Quality)
                Selector.Items().Append(box_value(to_hstring(Quality)));
            Selector.SelectedIndex(3);
        }
        const auto Color = NewColorSelector().SelectedIndex();
        SetItems(NewColorSelector(), {box_value(Utf8ToHString(Text("databody_purple"))),
            box_value(Utf8ToHString(Text("databody_gold")))}, Color);
    }

    void DataBodyWindow::AssertStopped() const
    {
        if (!PresenceMonitor) throw std::runtime_error("The offline editor configuration is unavailable.");
        const auto Presence = PresenceMonitor->Snapshot();
        if (Presence.State != Trainer::GameRunState::Stopped)
            throw std::runtime_error(Presence.State == Trainer::GameRunState::Running ?
                "Fully exit the game before editing or writing a save." : Presence.Error.empty() ?
                "Cannot confirm that the game is stopped; save editing remains blocked." : Presence.Error);
    }

    void DataBodyWindow::UpdateAvailability()
    {
        if (Closing || !Settings || !PresenceMonitor) return;
        const auto Presence = PresenceMonitor->Snapshot();
        const bool Stopped = Presence.State == Trainer::GameRunState::Stopped;
        const bool Ready = Stopped && !Busy;
        if (OfflineControls().IsEnabled() != Ready) OfflineControls().IsEnabled(Ready);
        BrowseGameButton().IsEnabled(!Busy);
        GameDirectoryInput().IsEnabled(!Busy);
        EditorLanguageButton().IsEnabled(!Busy);
        ApplyEditButton().IsEnabled(Ready && Catalog && Save && EditingUid && EditValid);
        ApplyAddButton().IsEnabled(Ready && Catalog && Save && NewValid);
        if (Confirmation) Confirmation.IsPrimaryButtonEnabled(Stopped);
        if (!DisplayedPresence || *DisplayedPresence != Presence)
        {
            DisplayedPresence = Presence;
            OfflineNotice().Text(Utf8ToHString(Text(Stopped ? "databody_offline_hint" : "databody_game_running")));
            if (Presence.State == Trainer::GameRunState::Unknown && !Presence.Error.empty()) Report(Presence.Error);
        }
    }

    fire_and_forget DataBodyWindow::LoadCatalog(std::filesystem::path Directory)
    {
        const auto Lifetime = get_strong();
        const auto Queue = Microsoft::UI::Dispatching::DispatcherQueue::GetForCurrentThread();
        if (Busy || Closing) co_return;
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

    void DataBodyWindow::CatalogReady(std::shared_ptr<Trainer::DataBodyCatalog> Result)
    {
        Catalog = std::move(Result);
        Populating = true;
        ActorIds.clear();
        StrategyIds.clear();
        for (auto& Labels : ActorLabels) Labels.clear();
        for (auto& Labels : StrategyLabels) Labels.clear();
        for (const auto& [Id, Actor] : Catalog->Actors)
        {
            ActorIds.push_back(Id);
            for (unsigned Index = 0; Index < 2; ++Index)
                ActorLabels[Index].push_back(box_value(Utf8ToHString(
                    Trainer::DataBodyCatalog::Name(Actor.Name, static_cast<TrainerLanguage>(Index)))));
        }
        for (const auto& [Id, Strategy] : Catalog->Strategies)
        {
            if (Strategy.Qualities.empty()) continue;
            StrategyIds.push_back(Id);
            for (unsigned Index = 0; Index < 2; ++Index)
                StrategyLabels[Index].push_back(box_value(Utf8ToHString(
                    Trainer::DataBodyCatalog::Name(Strategy.Name, static_cast<TrainerLanguage>(Index)) +
                    " · " + std::to_string(Id))));
        }
        PopulateCatalogLists();
        PopulateSkillSelector(true);
        Populating = false;
        RefreshConditions(true);
        EditorStatus().Text(Utf8ToHString(Text("databody_catalog_ready") + " " +
            std::to_string(ActorIds.size()) + " / " + std::to_string(StrategyIds.size())));
        if (!SavePathInput().Text().empty()) LoadSave();
    }

    void DataBodyWindow::PopulateCatalogLists()
    {
        const auto Index = static_cast<std::size_t>(Language);
        SetItems(NewActorSelector(), ActorLabels[Index], NewActorSelector().SelectedIndex());
        std::vector<Item> Strategies{box_value(Utf8ToHString(Text("databody_none")))};
        Strategies.insert(Strategies.end(), StrategyLabels[Index].begin(), StrategyLabels[Index].end());
        for (const auto& Selector : {EditInheritanceFirst(), EditInheritanceSecond(),
                NewInheritanceFirst(), NewInheritanceSecond()})
            SetItems(Selector, Strategies, Selector.SelectedIndex());
    }

    fire_and_forget DataBodyWindow::PickPath(bool Folder)
    {
        const auto Lifetime = get_strong();
        const auto Queue = Microsoft::UI::Dispatching::DispatcherQueue::GetForCurrentThread();
        if (Busy || Closing) co_return;
        const auto PreviousStatus = EditorStatus().Text();
        std::optional<std::filesystem::path> Selected;
        std::string Error;
        try
        {
            if (!Folder) AssertStopped();
            Busy = true;
            EditorStatus().Text(Utf8ToHString(Text("databody_picker_opening")));
            UpdateAvailability();
            const auto Owner = AppWindow().Id();
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

    fire_and_forget DataBodyWindow::LoadSave()
    {
        const auto Lifetime = get_strong();
        const auto Queue = Microsoft::UI::Dispatching::DispatcherQueue::GetForCurrentThread();
        if (Busy || Closing) co_return;
        std::filesystem::path Path;
        try
        {
            AssertStopped();
            Save.reset();
            SavedBodies.clear();
            EditingUid.reset();
            EditValid = false;
            BodyList().ItemsSource(nullptr);
            SelectedBodyText().Text(Utf8ToHString(Text("databody_select_body")));
            WarehouseInfo().Text(L"");
            if (!Catalog)
            {
                if (GameDirectoryInput().Text().empty())
                    throw std::runtime_error("Select the current game's installation directory first.");
                LoadCatalog(std::filesystem::path(std::wstring(GameDirectoryInput().Text())));
                co_return;
            }
            Path = std::filesystem::path(std::wstring(SavePathInput().Text()));
        }
        catch (const std::exception& Error) {Report(Error.what()); UpdateAvailability(); co_return;}
        catch (const hresult_error& Error) {Report(to_string(Error.message())); UpdateAvailability(); co_return;}
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
        catch (const std::exception& Exception) {Error = Exception.what();}
        if (!(co_await ResumeOnUi{Queue}) || Closing) co_return;
        Busy = false;
        TryAction([&]
        {
            if (!Error.empty()) Report(Error);
            else
            {
                Save = std::move(Loaded);
                RefreshBodies(std::move(Bodies), Capacity);
                Report(Text("databody_loaded"));
            }
            EditorStatus().Text(Utf8ToHString(Text("databody_catalog_ready") + " " +
                std::to_string(ActorIds.size()) + " / " + std::to_string(StrategyIds.size())));
        });
    }

    void DataBodyWindow::RefreshBodies(std::vector<Trainer::DataBodyInfo> Bodies, std::size_t Capacity,
        std::optional<std::uint32_t> Select)
    {
        SavedBodies = std::move(Bodies);
        UpdateBodyList(Select);
        WarehouseInfo().Text(Utf8ToHString(Text("databody_capacity") + " " +
            std::to_string(SavedBodies.size()) + " / " + std::to_string(Capacity)));
        PopulateBody();
    }

    std::string DataBodyWindow::DescribeBody(const Trainer::DataBodyInfo& Body) const
    {
        const auto Found = Catalog->Actors.find(Body.ActorId);
        std::string Result = std::to_string(Body.Uid) + " · " + (Found == Catalog->Actors.end() ?
            std::to_string(Body.ActorId) : Trainer::DataBodyCatalog::Name(Found->second.Name, Language));
        for (std::size_t Index = 0; Index < Body.Inherited.size(); ++Index)
        {
            const auto Strategy = Catalog->Strategies.find(Body.Inherited[Index].Id);
            Result += Index ? " / " : "\n";
            Result += Strategy == Catalog->Strategies.end() ? std::to_string(Body.Inherited[Index].Id) :
                Trainer::DataBodyCatalog::Name(Strategy->second.Name, Language);
        }
        if (Found != Catalog->Actors.end() && Body.InheritanceSkillIndex < Found->second.InheritanceSkills.size())
            Result += "\n" + Catalog->InheritanceSkillName(
                Found->second.InheritanceSkills[Body.InheritanceSkillIndex], Language);
        return Result;
    }

    void DataBodyWindow::UpdateBodyList(std::optional<std::uint32_t> Select)
    {
        const bool PreviousPopulating = Populating;
        Populating = true;
        std::vector<Item> Labels;
        int Selected = SavedBodies.empty() ? -1 : 0;
        const auto Requested = Select ? Select : EditingUid;
        for (std::size_t Index = 0; Index < SavedBodies.size(); ++Index)
        {
            Labels.push_back(box_value(Utf8ToHString(DescribeBody(SavedBodies[Index]))));
            if (Requested && SavedBodies[Index].Uid == *Requested) Selected = static_cast<int>(Index);
        }
        BodyList().ItemsSource(single_threaded_observable_vector<Item>(std::move(Labels)));
        BodyList().SelectedIndex(Selected);
        Populating = PreviousPopulating;
    }

    void DataBodyWindow::PopulateBody()
    {
        EditValid = false;
        EditingUid.reset();
        const auto Selected = BodyList().SelectedIndex();
        if (!Catalog || Selected < 0 || static_cast<std::size_t>(Selected) >= SavedBodies.size())
        {
            SelectedBodyText().Text(Utf8ToHString(Text("databody_select_body")));
            UpdateAvailability();
            return;
        }
        const auto& Body = SavedBodies[Selected];
        Catalog->Actor(Body.ActorId);
        EditingUid = Body.Uid;
        const auto Description = DescribeBody(Body);
        SelectedBodyText().Text(Utf8ToHString(Description.substr(0, Description.find('\n'))));
        Populating = true;
        const std::array Selectors{EditInheritanceFirst(), EditInheritanceSecond()};
        const std::array Qualities{EditQualityFirst(), EditQualitySecond()};
        for (std::size_t Index = 0; Index < 2; ++Index)
        {
            Selectors[Index].SelectedIndex(0);
            Qualities[Index].SelectedIndex(3);
            if (Index >= Body.Inherited.size()) continue;
            const auto Found = std::find(StrategyIds.begin(), StrategyIds.end(), Body.Inherited[Index].Id);
            if (Found == StrategyIds.end())
                throw std::runtime_error("A saved inheritance strategy is absent from the current catalog; the record was not changed.");
            Selectors[Index].SelectedIndex(static_cast<int>(Found - StrategyIds.begin() + 1));
            Qualities[Index].SelectedIndex(static_cast<int>(Body.Inherited[Index].Quality) - 1);
        }
        PopulateSkillSelector(false);
        EditSkillHeading().Text(Utf8ToHString(Text("databody_skill") + " · Lv." +
            std::to_string(Body.InheritanceSkillLevel + 1)));
        Populating = false;
        RefreshConditions(false);
    }

    void DataBodyWindow::PopulateSkillSelector(bool Added, int Selected)
    {
        const auto Selector = Added ? NewSkillSelector() : EditSkillSelector();
        std::vector<Item> Labels;
        if (!Added) Labels.push_back(box_value(Utf8ToHString(Text("databody_keep_skill"))));
        std::uint32_t ActorId = 0;
        if (Added)
        {
            const auto Index = NewActorSelector().SelectedIndex();
            if (Index >= 0 && static_cast<std::size_t>(Index) < ActorIds.size()) ActorId = ActorIds[Index];
        }
        else if (EditingUid)
        {
            const auto Index = BodyList().SelectedIndex();
            ActorId = SavedBodies.at(Index).ActorId;
            if (Selected < 0)
            {
                const auto& Actor = Catalog->Actor(ActorId);
                const auto SkillIndex = SavedBodies[Index].InheritanceSkillIndex;
                Selected = SkillIndex < Actor.InheritanceSkills.size() ? static_cast<int>(SkillIndex + 1) : 0;
            }
        }
        if (ActorId)
        {
            for (const auto Id : Catalog->Actor(ActorId).InheritanceSkills)
                Labels.push_back(box_value(Utf8ToHString(Catalog->InheritanceSkillName(Id, Language))));
        }
        SetItems(Selector, Labels, Selected);
    }

    std::vector<Trainer::StrategyChoice> DataBodyWindow::InheritedChoices(bool Added)
    {
        std::vector<Trainer::StrategyChoice> Result;
        const std::array Selectors{Added ? NewInheritanceFirst() : EditInheritanceFirst(),
            Added ? NewInheritanceSecond() : EditInheritanceSecond()};
        const std::array Qualities{Added ? NewQualityFirst() : EditQualityFirst(),
            Added ? NewQualitySecond() : EditQualitySecond()};
        for (std::size_t Index = 0; Index < 2; ++Index)
        {
            const auto Selected = Selectors[Index].SelectedIndex();
            if (Selected <= 0) continue;
            if (static_cast<std::size_t>(Selected) > StrategyIds.size() || Qualities[Index].SelectedIndex() < 0)
                throw std::runtime_error("Select a valid inheritance strategy and quality.");
            Result.push_back({StrategyIds[Selected - 1],
                static_cast<std::uint32_t>(Qualities[Index].SelectedIndex() + 1)});
        }
        return Result;
    }

    void DataBodyWindow::RefreshConditions(bool Added)
    {
        if (!Catalog || Populating) return;
        auto& Valid = Added ? NewValid : EditValid;
        Valid = false;
        const std::array Selectors{Added ? NewInheritanceFirst() : EditInheritanceFirst(),
            Added ? NewInheritanceSecond() : EditInheritanceSecond()};
        const std::array Conditions{Added ? NewConditionFirst() : EditConditionFirst(),
            Added ? NewConditionSecond() : EditConditionSecond()};
        try
        {
            const auto Choices = InheritedChoices(Added);
            Catalog->ValidateInheritance(Choices);
            for (std::size_t Index = 0; Index < 2; ++Index)
            {
                const auto Selected = Selectors[Index].SelectedIndex();
                if (Selected <= 0) {Conditions[Index].Text(L""); continue;}
                const auto Required = Catalog->InheritanceConditions(StrategyIds[Selected - 1], Choices);
                std::string Description;
                for (const auto& Alternatives : Required)
                {
                    if (!Description.empty()) Description += " + ";
                    std::set<std::string> Seen;
                    std::string Group;
                    for (const auto Id : Alternatives)
                    {
                        const auto Name = Trainer::DataBodyCatalog::Name(Catalog->Strategy(Id).Name, Language);
                        if (!Seen.insert(Name).second) continue;
                        if (!Group.empty()) Group += " / ";
                        Group += Name;
                    }
                    Description += Seen.size() > 1 ? "[" + Group + "]" : Group;
                }
                Conditions[Index].Text(Utf8ToHString(Description.empty() ? Text("databody_no_requirement") :
                    Text("databody_condition_prefix") + Description));
            }
            Valid = Added ? Choices.size() == 2 : EditingUid.has_value();
        }
        catch (const std::exception& Error)
        {
            for (const auto& Condition : Conditions) Condition.Text(L"");
            Report(Error.what());
        }
        UpdateAvailability();
    }

    Trainer::DataBodyRequest DataBodyWindow::Request(bool Added)
    {
        Trainer::DataBodyRequest Result;
        Result.Inherited = InheritedChoices(Added);
        if (Added)
        {
            const auto Actor = NewActorSelector().SelectedIndex();
            if (Actor < 0 || static_cast<std::size_t>(Actor) >= ActorIds.size())
                throw std::runtime_error("Select a character from the current game catalog.");
            Result.ActorId = ActorIds[Actor];
            const auto Color = NewColorSelector().SelectedIndex();
            if (Color != 0 && Color != 1) throw std::runtime_error("Select a valid data-body quality.");
            Result.Score = Color == 1 ? 700 : 600;
        }
        else
        {
            if (!EditingUid) throw std::runtime_error("Select a saved data body before editing inheritance.");
            Result.ExistingUid = EditingUid;
            Result.ActorId = SavedBodies.at(BodyList().SelectedIndex()).ActorId;
        }
        const auto Skill = (Added ? NewSkillSelector() : EditSkillSelector()).SelectedIndex();
        if (Skill >= (Added ? 0 : 1))
            Result.InheritanceSkillId = Catalog->Actor(Result.ActorId).InheritanceSkills.at(Skill - (Added ? 0 : 1));
        return Result;
    }

    fire_and_forget DataBodyWindow::ConfirmSave(bool Added)
    {
        const auto Lifetime = get_strong();
        const auto Queue = Microsoft::UI::Dispatching::DispatcherQueue::GetForCurrentThread();
        if (Busy || !Save || !Catalog || Closing) co_return;
        Trainer::DataBodyRequest Selected;
        try {AssertStopped(); Selected = Request(Added);}
        catch (const std::exception& Error) {Report(Error.what()); co_return;}
        const auto CurrentSave = Save;
        const auto CurrentCatalog = Catalog;
        Busy = true;
        EditorStatus().Text(Utf8ToHString(Text("databody_save_preparing")));
        UpdateAvailability();
        Trainer::DataBodyPlan Plan;
        std::string Error;
        co_await resume_background();
        try {Plan = CurrentSave->Prepare(*CurrentCatalog, Selected);}
        catch (const std::exception& Exception) {Error = Exception.what();}
        if (!(co_await ResumeOnUi{Queue}) || Closing) co_return;
        try
        {
            if (!Error.empty()) throw std::runtime_error(Error);
            AssertStopped();
            Confirmation = Microsoft::UI::Xaml::Controls::ContentDialog();
            Confirmation.XamlRoot(EditorRoot().XamlRoot());
            Confirmation.Title(box_value(Utf8ToHString(Text("databody_confirm_title"))));
            std::string Details = Text("databody_confirm_notice") + "\n\n" + PathText(CurrentSave->Path()) +
                "\n" + (Added ? Text("databody_new") : Text("databody_edit")) + " · UID " +
                std::to_string(Plan.Uid) + "\n" +
                Trainer::DataBodyCatalog::Name(CurrentCatalog->Actor(Selected.ActorId).Name, Language);
            if (Added) Details += " · " + std::to_string(static_cast<int>(Selected.Score));
            for (const auto& Choice : Selected.Inherited)
                Details += "\n" + Trainer::DataBodyCatalog::Name(CurrentCatalog->Strategy(Choice.Id).Name, Language) +
                    " · Q" + std::to_string(Choice.Quality);
            Details += "\n" + Text("databody_skill") + ": " + (Selected.InheritanceSkillId ?
                CurrentCatalog->InheritanceSkillName(*Selected.InheritanceSkillId, Language) : Text("databody_keep_skill"));
            Microsoft::UI::Xaml::Controls::TextBlock Summary;
            Summary.TextWrapping(Microsoft::UI::Xaml::TextWrapping::Wrap);
            Summary.Text(Utf8ToHString(Details));
            Microsoft::UI::Xaml::Controls::ScrollViewer Review;
            Review.MaxHeight(320);
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
                catch (const std::exception& Exception) {Error = Exception.what();}
                if (!(co_await ResumeOnUi{Queue}) || Closing) co_return;
                if (!Error.empty()) throw std::runtime_error(Error);
                RefreshBodies(std::move(Bodies), Capacity, Plan.Uid);
                Report(Text("databody_saved") + "\n" + PathText(Backup));
            }
        }
        catch (const std::exception& Exception) {if (!Closing) Report(Exception.what());}
        catch (const hresult_error& Exception) {if (!Closing) Report(to_string(Exception.message()));}
        Confirmation = nullptr;
        Busy = false;
        EditorStatus().Text(Utf8ToHString(Text("databody_catalog_ready") + " " +
            std::to_string(ActorIds.size()) + " / " + std::to_string(StrategyIds.size())));
        UpdateAvailability();
    }

    void DataBodyWindow::Shutdown()
    {
        if (Closing) return;
        Closing = true;
        BackgroundStop.request_stop();
        if (const auto Picker = std::exchange(ActivePathPicker, nullptr))
        {
            try {Picker.Cancel();}
            catch (const hresult_error& Error) {OutputDebugStringW(Error.message().c_str());}
        }
        if (Confirmation) Confirmation.Hide();
        NativeBehavior.reset();
        if (GameTimer) {GameTimer.Stop(); GameTimer = nullptr;}
        PresenceMonitor.reset();
    }

    void DataBodyWindow::OnBrowseGame(Windows::Foundation::IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&)
    {PickPath(true);}
    void DataBodyWindow::OnBrowseSave(Windows::Foundation::IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&)
    {PickPath(false);}
    void DataBodyWindow::OnLoadSave(Windows::Foundation::IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&)
    {LoadSave();}
    void DataBodyWindow::OnSaveSlotChanged(Windows::Foundation::IInspectable const&, Microsoft::UI::Xaml::Controls::SelectionChangedEventArgs const&)
    {if (!Populating) TryAction([this] {const auto Index = SaveSlotSelector().SelectedIndex(); if (Index > 0 && static_cast<std::size_t>(Index) <= Saves.size()) {SavePathInput().Text(hstring(Saves[Index - 1].wstring())); LoadSave();}});}
    void DataBodyWindow::OnBodyChanged(Windows::Foundation::IInspectable const&, Microsoft::UI::Xaml::Controls::SelectionChangedEventArgs const&)
    {if (!Populating) TryAction([this] {PopulateBody();});}
    void DataBodyWindow::OnActorChanged(Windows::Foundation::IInspectable const&, Microsoft::UI::Xaml::Controls::SelectionChangedEventArgs const&)
    {if (!Populating && Catalog) TryAction([this] {PopulateSkillSelector(true); RefreshConditions(true);});}
    void DataBodyWindow::OnInheritanceChanged(Windows::Foundation::IInspectable const& Sender, Microsoft::UI::Xaml::Controls::SelectionChangedEventArgs const&)
    {
        if (!Populating)
            TryAction([this, Sender] {RefreshConditions(Sender == NewInheritanceFirst() || Sender == NewInheritanceSecond() ||
                Sender == NewQualityFirst() || Sender == NewQualitySecond());});
    }
    void DataBodyWindow::OnSkillChanged(Windows::Foundation::IInspectable const&, Microsoft::UI::Xaml::Controls::SelectionChangedEventArgs const&)
    {if (!Populating) UpdateAvailability();}
    void DataBodyWindow::OnTabChanged(Windows::Foundation::IInspectable const&, Microsoft::UI::Xaml::Controls::SelectionChangedEventArgs const&)
    {if (!Populating) UpdateAvailability();}
    void DataBodyWindow::OnApplyEdit(Windows::Foundation::IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&)
    {ConfirmSave(false);}
    void DataBodyWindow::OnApplyAdd(Windows::Foundation::IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&)
    {ConfirmSave(true);}
    void DataBodyWindow::OnEditorLanguageChanged(Windows::Foundation::IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&)
    {
        if (Busy) return;
        TryAction([this]
        {
            const auto EditSkill = EditSkillSelector().SelectedIndex();
            const auto NewSkill = NewSkillSelector().SelectedIndex();
            Language = Language == TrainerLanguageEnglish ? TrainerLanguageChinese : TrainerLanguageEnglish;
            Populating = true;
            InitializeText();
            DisplayedPresence.reset();
            if (Catalog)
            {
                PopulateCatalogLists();
                UpdateBodyList();
                PopulateSkillSelector(false, EditSkill);
                PopulateSkillSelector(true, NewSkill);
                if (EditingUid)
                {
                    const auto& Body = SavedBodies.at(BodyList().SelectedIndex());
                    const auto Description = DescribeBody(Body);
                    SelectedBodyText().Text(Utf8ToHString(Description.substr(0, Description.find('\n'))));
                    EditSkillHeading().Text(Utf8ToHString(Text("databody_skill") + " · Lv." +
                        std::to_string(Body.InheritanceSkillLevel + 1)));
                }
            }
            Populating = false;
            if (Catalog) {RefreshConditions(false); RefreshConditions(true);}
            if (!LastMessage.empty())
                SaveOperationText().Text(Utf8ToHString(Settings->Texts.Translate(LastMessage, Language)));
        });
    }
}
