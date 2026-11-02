#pragma once

#include "MainWindow.g.h"
#include "TrainerApi.h"
#include "WindowBehavior.h"
#include "GamePresenceMonitor.h"

#include <winrt/Microsoft.UI.Dispatching.h>
#include <winrt/Microsoft.UI.Xaml.h>
#include <winrt/Microsoft.UI.Xaml.Controls.h>

#include <memory>
#include <optional>
#include <string>

namespace winrt::BlazblueTrainer::implementation
{
    struct MainWindow : MainWindowT<MainWindow>
    {
        MainWindow();
        ~MainWindow();

        void OnSetRefreshClick(Windows::Foundation::IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
        void OnAddRefreshClick(Windows::Foundation::IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
        void OnSetExchangeClick(Windows::Foundation::IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
        void OnAddExchangeClick(Windows::Foundation::IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
        void OnSetApClick(Windows::Foundation::IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
        void OnAddApClick(Windows::Foundation::IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
        void OnLanguageChanged(Windows::Foundation::IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
        void OnOpenDataBodyEditor(Windows::Foundation::IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);

    private:
        std::string ReadText(const char* Key) const;
        std::optional<std::int64_t> ReadAmount(Microsoft::UI::Xaml::Controls::TextBox const& Input) const;
        bool ApplyFeature(TrainerFeature Feature, TrainerOperation Operation,
            Microsoft::UI::Xaml::Controls::TextBox const& Input);
        bool ApplyAddFeature(TrainerFeature Feature);
        void UpdateSnapshot();
        void SetValueLabels(const TrainerSnapshot& Snapshot);
        void SetDefaultInputs();
        void SetWriteEnabled(const TrainerSnapshot& Snapshot);
        void UpdateLocalizedInterface();
        void ShowOperationMessage(const std::string& Message);
        void Shutdown();

        TrainerEngine* EngineHandle{};
        std::unique_ptr<Trainer::WindowBehavior> NativeBehavior;
        Microsoft::UI::Dispatching::DispatcherQueueTimer SnapshotTimer{ nullptr };
        std::string LastDetail;
        std::string LastOperation;
        std::optional<Trainer::Localization> InterfaceTexts;
        TrainerLanguage InterfaceLanguage{ TrainerLanguageChinese };
        bool IsApplyingLanguage{ true };
        std::optional<Trainer::Config> GameConfiguration;
        std::shared_ptr<Trainer::GamePresenceMonitor> PresenceMonitor;
        std::optional<Trainer::GamePresence> DisplayedPresence;
        Microsoft::UI::Xaml::Window DataBodyEditor{ nullptr };
        bool IsShuttingDown{};
    };
}

namespace winrt::BlazblueTrainer::factory_implementation
{
    struct MainWindow : MainWindowT<MainWindow, implementation::MainWindow> {};
}
