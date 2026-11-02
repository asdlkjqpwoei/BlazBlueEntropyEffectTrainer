#pragma once

#include "DataBodySave.h"
#include "DataBodyWindow.g.h"
#include "GamePresenceMonitor.h"
#include "WindowBehavior.h"

#include <winrt/Microsoft.UI.Dispatching.h>
#include <winrt/Microsoft.UI.Xaml.Controls.h>
#include <winrt/Windows.Foundation.h>

#include <array>
#include <memory>
#include <optional>

namespace winrt::BlazblueTrainer::implementation
{
    struct DataBodyWindow : DataBodyWindowT<DataBodyWindow>
    {
        explicit DataBodyWindow(TrainerLanguage Language = TrainerLanguageChinese,
            std::shared_ptr<Trainer::GamePresenceMonitor> Monitor = {});
        ~DataBodyWindow();
        void OnBrowseGame(Windows::Foundation::IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
        void OnBrowseSave(Windows::Foundation::IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
        void OnLoadSave(Windows::Foundation::IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
        void OnSaveSlotChanged(Windows::Foundation::IInspectable const&,
            Microsoft::UI::Xaml::Controls::SelectionChangedEventArgs const&);
        void OnBodyChanged(Windows::Foundation::IInspectable const&,
            Microsoft::UI::Xaml::Controls::SelectionChangedEventArgs const&);
        void OnActorChanged(Windows::Foundation::IInspectable const&,
            Microsoft::UI::Xaml::Controls::SelectionChangedEventArgs const&);
        void OnInheritanceChanged(Windows::Foundation::IInspectable const&,
            Microsoft::UI::Xaml::Controls::SelectionChangedEventArgs const&);
        void OnSkillChanged(Windows::Foundation::IInspectable const&,
            Microsoft::UI::Xaml::Controls::SelectionChangedEventArgs const&);
        void OnTabChanged(Windows::Foundation::IInspectable const&,
            Microsoft::UI::Xaml::Controls::SelectionChangedEventArgs const&);
        void OnApplyEdit(Windows::Foundation::IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
        void OnApplyAdd(Windows::Foundation::IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
        void OnEditorLanguageChanged(Windows::Foundation::IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);

    private:
        std::string Text(const char* Key) const;
        void Report(const std::string& Error);
        void TryAction(const std::function<void()>& Action);
        void InitializeText();
        void AssertStopped() const;
        void UpdateAvailability();
        fire_and_forget LoadCatalog(std::filesystem::path Directory);
        void CatalogReady(std::shared_ptr<Trainer::DataBodyCatalog> Result);
        void PopulateCatalogLists();
        fire_and_forget LoadSave();
        void RefreshBodies(std::vector<Trainer::DataBodyInfo> Bodies, std::size_t Capacity,
            std::optional<std::uint32_t> Select = {});
        void UpdateBodyList(std::optional<std::uint32_t> Select = {});
        std::string DescribeBody(const Trainer::DataBodyInfo& Body) const;
        void PopulateBody();
        void PopulateSkillSelector(bool Added, int Selected = -1);
        std::vector<Trainer::StrategyChoice> InheritedChoices(bool Added);
        void RefreshConditions(bool Added);
        Trainer::DataBodyRequest Request(bool Added);
        fire_and_forget ConfirmSave(bool Added);
        fire_and_forget PickPath(bool Folder);
        void Shutdown();

        TrainerLanguage Language{TrainerLanguageChinese};
        std::optional<Trainer::Config> Settings;
        std::shared_ptr<Trainer::DataBodyCatalog> Catalog;
        std::shared_ptr<Trainer::DataBodySave> Save;
        std::shared_ptr<Trainer::GamePresenceMonitor> PresenceMonitor;
        std::optional<Trainer::GamePresence> DisplayedPresence;
        std::stop_source BackgroundStop;
        std::vector<std::filesystem::path> Saves;
        std::vector<Trainer::DataBodyInfo> SavedBodies;
        std::vector<std::uint32_t> ActorIds;
        std::vector<std::uint32_t> StrategyIds;
        std::array<std::vector<Windows::Foundation::IInspectable>, 2> ActorLabels;
        std::array<std::vector<Windows::Foundation::IInspectable>, 2> StrategyLabels;
        std::optional<std::uint32_t> EditingUid;
        HWND WindowHandle{};
        std::unique_ptr<Trainer::WindowBehavior> NativeBehavior;
        Microsoft::UI::Dispatching::DispatcherQueueTimer GameTimer{nullptr};
        Microsoft::UI::Xaml::Controls::ContentDialog Confirmation{nullptr};
        Windows::Foundation::IAsyncInfo ActivePathPicker{nullptr};
        bool Populating{true};
        bool Busy{};
        bool EditValid{};
        bool NewValid{};
        bool Closing{};
        std::string LastMessage;
    };
}

namespace winrt::BlazblueTrainer::factory_implementation
{
    struct DataBodyWindow : DataBodyWindowT<DataBodyWindow, implementation::DataBodyWindow> {};
}
