#pragma once

#include "DataBodySave.h"
#include "DataBodyEditor.g.h"
#include "GamePresenceMonitor.h"

#include <winrt/Microsoft.UI.Dispatching.h>
#include <winrt/Microsoft.UI.Xaml.Controls.h>
#include <winrt/Windows.Foundation.h>

#include <array>
#include <memory>
#include <optional>

namespace winrt::BlazblueTrainer::implementation
{
	struct DataBodyEditor : DataBodyEditorT<DataBodyEditor>
	{
		DataBodyEditor();
		~DataBodyEditor();
		void Initialize(Microsoft::UI::WindowId Owner, TrainerLanguage SelectedLanguage,
			std::shared_ptr<Trainer::GamePresenceMonitor> Monitor);
		void SetLanguage(TrainerLanguage SelectedLanguage);
		void Shutdown();
		void OnBrowseGame(Windows::Foundation::IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
		void OnBrowseSave(Windows::Foundation::IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
		void OnLoadSave(Windows::Foundation::IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
		void OnSaveSlotChanged(Windows::Foundation::IInspectable const&, Microsoft::UI::Xaml::Controls::SelectionChangedEventArgs const&);
		void OnBodyChanged(Windows::Foundation::IInspectable const&, Microsoft::UI::Xaml::Controls::SelectionChangedEventArgs const&);
		void OnActorChanged(Windows::Foundation::IInspectable const&, Microsoft::UI::Xaml::Controls::SelectionChangedEventArgs const&);
		void OnQualityChanged(Windows::Foundation::IInspectable const&, Microsoft::UI::Xaml::Controls::SelectionChangedEventArgs const&);
		void OnSkillChanged(Windows::Foundation::IInspectable const&, Microsoft::UI::Xaml::Controls::SelectionChangedEventArgs const&);
		void OnTabChanged(Windows::Foundation::IInspectable const&, Microsoft::UI::Xaml::Controls::SelectionChangedEventArgs const&);
		void OnApplyAdd(Windows::Foundation::IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
		void OnResetAll(Windows::Foundation::IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
		void OnSaveAll(Windows::Foundation::IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
		void OnInspectSlot(Windows::Foundation::IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
		void OnClearSlot(Windows::Foundation::IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
		void OnUseStrategy(Windows::Foundation::IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
		void OnStrategySearchChanged(Windows::Foundation::IInspectable const&, Microsoft::UI::Xaml::Controls::TextChangedEventArgs const&);
		void OnStrategyFilterChanged(Windows::Foundation::IInspectable const&, Microsoft::UI::Xaml::Controls::SelectionChangedEventArgs const&);
		void OnStrategySelected(Windows::Foundation::IInspectable const&, Microsoft::UI::Xaml::Controls::SelectionChangedEventArgs const&);

	private:
		std::string Text(const char* Key) const;
		void Report(const std::string& Error);
		void TryAction(const std::function<void()>& Action);
		void InitializeText();
		void AssertStopped() const;
		void AssertNoDrafts() const;
		void UpdateAvailability();
		fire_and_forget LoadCatalog(std::filesystem::path Directory);
		void CatalogReady(std::shared_ptr<Trainer::DataBodyCatalog> Result);
		void PopulateCatalogLists();
		fire_and_forget LoadSave();
		void RefreshBodies(std::vector<Trainer::DataBodyInfo> Bodies, std::size_t Capacity,
			std::optional<std::uint32_t> Select = {});
		void UpdateBodyList(std::optional<std::uint32_t> Select = {});
		std::string DescribeBody(const Trainer::DataBodyDraft& Body) const;
		void PopulateBody();
		void PopulateSkillSelector(bool Added, int Selected = -1);
		std::vector<Trainer::StrategyChoice> InheritedChoices(bool Added) const;
		void RefreshSlots(bool Added);
		void RefreshConditions(bool Added);
		void CacheCurrentBody();
		void UpdateDraftStatus();
		Trainer::DataBodyRequest Request(bool Added);
		fire_and_forget ConfirmSave();
		fire_and_forget PickPath(bool Folder);
		std::string StrategyMetadata(std::uint32_t Id) const;
		std::string Conditions(std::uint32_t Id, const std::vector<Trainer::StrategyChoice>& Choices) const;
		void PopulateFilters();
		void FilterStrategies();
		void PreviewStrategy();

		TrainerLanguage Language{TrainerLanguageChinese};
		Microsoft::UI::WindowId OwnerId{};
		std::optional<Trainer::Config> Settings;
		std::shared_ptr<Trainer::DataBodyCatalog> Catalog;
		std::shared_ptr<Trainer::DataBodySave> Save;
		std::unique_ptr<Trainer::DataBodyDrafts> Drafts;
		std::shared_ptr<Trainer::GamePresenceMonitor> PresenceMonitor;
		std::optional<Trainer::GamePresence> DisplayedPresence;
		std::stop_source BackgroundStop;
		std::vector<std::filesystem::path> Saves;
		std::vector<Trainer::DataBodyInfo> SavedBodies;
		std::size_t SavedCapacity{};
		std::vector<std::uint32_t> ActorIds;
		std::vector<std::uint32_t> ElementIds;
		std::vector<std::uint32_t> TriggerIds;
		std::vector<std::uint32_t> TierIds;
		std::vector<std::uint32_t> FilteredStrategyIds;
		std::array<std::array<std::optional<Trainer::StrategyChoice>, 2>, 2> Selections{};
		std::optional<std::uint32_t> EditingUid;
		std::optional<std::uint32_t> PreviewId;
		Microsoft::UI::Dispatching::DispatcherQueueTimer GameTimer{nullptr};
		Microsoft::UI::Xaml::Controls::ContentDialog Confirmation{nullptr};
		Windows::Foundation::IAsyncInfo ActivePathPicker{nullptr};
		bool Populating{true};
		bool Busy{};
		bool NewValid{};
		bool Closing{};
		std::string LastMessage;
	};
}

namespace winrt::BlazblueTrainer::factory_implementation
{
	struct DataBodyEditor : DataBodyEditorT<DataBodyEditor, implementation::DataBodyEditor> {};
}
