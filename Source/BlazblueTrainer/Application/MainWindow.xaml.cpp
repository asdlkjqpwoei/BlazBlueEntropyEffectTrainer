#include "Pch.h"
#include "MainWindow.xaml.h"
#include "Paths.h"
#if __has_include("MainWindow.g.cpp")
#include "MainWindow.g.cpp"
#endif

#include <microsoft.ui.xaml.window.h>
#include <winrt/Microsoft.UI.Xaml.Automation.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <cwchar>
#include <cwctype>
#include <filesystem>
#include <stdexcept>
#include <string_view>

namespace
{
    winrt::hstring Utf8ToHString(std::string_view Text)
    {
        if (Text.empty())
            return {};
        const int Required = MultiByteToWideChar(
            CP_UTF8, MB_ERR_INVALID_CHARS, Text.data(), static_cast<int>(Text.size()), nullptr, 0);
        if (Required <= 0)
            throw std::runtime_error("Invalid UTF-8 interface text.");
        std::wstring Wide(static_cast<std::size_t>(Required), L'\0');
        if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, Text.data(),
                static_cast<int>(Text.size()), Wide.data(), Required) != Required)
            throw std::runtime_error("Could not convert the interface text.");
        return winrt::hstring(Wide);
    }

    void SetTooltip(
        winrt::Microsoft::UI::Xaml::DependencyObject const& Control, const winrt::hstring& Text)
    {
        winrt::Microsoft::UI::Xaml::Controls::TextBlock Content;
        Content.Text(Text);
        Content.MaxWidth(360);
        Content.TextWrapping(winrt::Microsoft::UI::Xaml::TextWrapping::Wrap);
        winrt::Microsoft::UI::Xaml::Controls::ToolTipService::SetToolTip(Control, Content);
    }
}

namespace winrt::BlazblueTrainer::implementation
{
    MainWindow::MainWindow()
    {
        InitializeComponent();
        Title(L"Blazblue Entropy Effect Trainer");
        Trainer::WindowConfig Preferred;
        std::array<std::optional<Trainer::HotkeyBinding>, 3> Bindings{};
        std::string StartupError;
        try
        {
            const auto ConfigPath = Trainer::DefaultConfigPath();
            InterfaceTexts = Trainer::Localization::Load(ConfigPath);
            InterfaceLanguage = InterfaceTexts->DefaultLanguage;
            const auto Configuration = Trainer::LoadConfig(ConfigPath);
            Preferred = Configuration.Window;
            Bindings = Configuration.Hotkeys;
            std::array<char, 1024> Error{};
            EngineHandle = TrainerCreate(ConfigPath.c_str(), Error.data(), Error.size());
            if (!EngineHandle)
                throw std::runtime_error(Error.data());
            if (!TrainerSetLanguage(EngineHandle, InterfaceLanguage, Error.data(), Error.size()))
                throw std::runtime_error(Error.data());
            UpdateLocalizedInterface();
            const std::array Labels{RefreshShortcutText(), ExchangeShortcutText(), ApShortcutText()};
            for (std::size_t Index = 0; Index < Bindings.size(); ++Index)
            {
                Labels[Index].Text(Bindings[Index] ? Utf8ToHString(Bindings[Index]->Display) : L"");
                if (Bindings[Index])
                    SetTooltip(Labels[Index], Utf8ToHString(Bindings[Index]->Display));
            }
            SetDefaultInputs();
        }
        catch (const std::exception& Error)
        {
            StartupError = Error.what();
            TrainerDestroy(EngineHandle);
            EngineHandle = nullptr;
        }
        try
        {
            HWND Handle{};
            const auto WindowNative = this->try_as<::IWindowNative>();
            if (!WindowNative)
                throw hresult_no_interface();
            check_hresult(WindowNative->get_WindowHandle(&Handle));
            NativeBehavior = std::make_unique<Trainer::WindowBehavior>(
                Handle, Preferred, std::move(Bindings),
                [this](TrainerFeature Feature) { ApplyAddFeature(Feature); },
                [this](const std::string& Error) { ShowOperationMessage(Error); });
        }
        catch (const std::exception& Error)
        {
            StartupError = Error.what();
        }
        catch (const hresult_error& Error)
        {
            StartupError = to_string(Error.message());
        }
        Closed([this](auto const&, auto const&) { Shutdown(); });
        if (!StartupError.empty())
        {
            StatusText().Text(InterfaceTexts ? Utf8ToHString(ReadText("initialization_failed")) :
                L"初始化失败 / Initialization failed");
            ShowOperationMessage(StartupError);
            LanguageSelector().IsEnabled(false);
            Shutdown();
            return;
        }
        std::array<char, 1024> Error{};
        if (!TrainerStart(EngineHandle, Error.data(), Error.size()))
        {
            StatusText().Text(Utf8ToHString(ReadText("monitor_start_failed")));
            ShowOperationMessage(Error.data());
            Shutdown();
            return;
        }
        const auto Dispatcher = Microsoft::UI::Dispatching::DispatcherQueue::GetForCurrentThread();
        if (!Dispatcher)
        {
            StatusText().Text(Utf8ToHString(ReadText("timer_failed")));
            Shutdown();
            return;
        }
        SnapshotTimer = Dispatcher.CreateTimer();
        SnapshotTimer.Interval(std::chrono::milliseconds(500));
        SnapshotTimer.Tick([this](auto const&, auto const&) { UpdateSnapshot(); });
        SnapshotTimer.Start();
        IsApplyingLanguage = false;
        UpdateSnapshot();
    }

    MainWindow::~MainWindow() { Shutdown(); }

    std::string MainWindow::ReadText(const char* Key) const
    {
        if (!InterfaceTexts)
            throw std::runtime_error(std::string("Missing interface text: ") + Key);
        return InterfaceTexts->Text(Key, InterfaceLanguage);
    }

    void MainWindow::UpdateLocalizedInterface()
    {
        IsApplyingLanguage = true;
        TitleText().Text(Utf8ToHString(ReadText("title")));
        SetTooltip(TitleText(), Utf8ToHString(ReadText("subtitle")));
        SetTooltip(LanguageSelector(), Utf8ToHString(ReadText("language")));
        Microsoft::UI::Xaml::Automation::AutomationProperties::SetName(
            LanguageSelector(), Utf8ToHString(ReadText("language")));
        LanguageSelector().Content(box_value(InterfaceLanguage == TrainerLanguageEnglish ? L"中文" : L"English"));
        RefreshTitleText().Text(Utf8ToHString(ReadText("title_refresh")));
        ExchangeTitleText().Text(Utf8ToHString(ReadText("title_exchange")));
        ApTitleText().Text(Utf8ToHString(ReadText("title_ap")));
        SetTooltip(RefreshTitleText(), Utf8ToHString(ReadText("refresh_hint")));
        for (const auto& Button : {RefreshSetButton(), ExchangeSetButton(), ApSetButton()})
            Button.Content(box_value(Utf8ToHString(ReadText("set"))));
        for (const auto& Button : {RefreshAddButton(), ExchangeAddButton(), ApAddButton()})
            Button.Content(box_value(Utf8ToHString(ReadText("add"))));
        for (const auto& Input : {RefreshSetInput(), ExchangeSetInput(), ApSetInput()})
            Input.PlaceholderText(Utf8ToHString(ReadText("set_placeholder")));
        for (const auto& Input : {RefreshAddInput(), ExchangeAddInput(), ApAddInput()})
            Input.PlaceholderText(Utf8ToHString(ReadText("add_placeholder")));
        if (!LastOperation.empty())
            OperationText().Text(Utf8ToHString(InterfaceTexts->Translate(LastOperation, InterfaceLanguage)));
        LastDetail.clear();
        IsApplyingLanguage = false;
    }

    void MainWindow::ShowOperationMessage(const std::string& Message)
    {
        LastOperation = InterfaceTexts ? InterfaceTexts->Translate(Message, TrainerLanguageEnglish) : Message;
        OperationText().Text(Utf8ToHString(InterfaceTexts ?
            InterfaceTexts->Translate(LastOperation, InterfaceLanguage) : LastOperation));
    }

    void MainWindow::OnLanguageChanged(
        Windows::Foundation::IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&)
    {
        if (IsApplyingLanguage || IsShuttingDown || !EngineHandle || !InterfaceTexts)
            return;
        const auto Language = InterfaceLanguage == TrainerLanguageEnglish
            ? TrainerLanguageChinese : TrainerLanguageEnglish;
        std::array<char, 512> Error{};
        if (!TrainerSetLanguage(EngineHandle, Language, Error.data(), Error.size()))
        {
            ShowOperationMessage(Error.data());
            return;
        }
        InterfaceLanguage = Language;
        UpdateLocalizedInterface();
        UpdateSnapshot();
    }

    std::optional<std::int64_t> MainWindow::ReadAmount(
        Microsoft::UI::Xaml::Controls::TextBox const& Input) const
    {
        std::wstring Text(Input.Text());
        const auto IsSpace = [](wchar_t Character) { return std::iswspace(Character) != 0; };
        const auto First = std::find_if_not(Text.begin(), Text.end(), IsSpace);
        const auto Last = std::find_if_not(Text.rbegin(), Text.rend(), IsSpace).base();
        if (First >= Last)
            return std::nullopt;
        Text = std::wstring(First, Last);
        errno = 0;
        wchar_t* End{};
        const auto Value = std::wcstoll(Text.c_str(), &End, 10);
        if (errno == ERANGE || End == Text.c_str() || !End || *End != L'\0' || Value < 0)
            return std::nullopt;
        return Value;
    }

    void MainWindow::ApplyFeature(TrainerFeature Feature, TrainerOperation Operation,
        Microsoft::UI::Xaml::Controls::TextBox const& Input)
    {
        if (!EngineHandle || IsShuttingDown)
            return;
        const auto Amount = ReadAmount(Input);
        if (!Amount)
        {
            ShowOperationMessage(ReadText("input_error"));
            return;
        }
        std::array<char, 1024> Error{};
        std::int32_t Writes{};
        TrainerApply(EngineHandle, Feature, Operation, *Amount, &Writes, Error.data(), Error.size());
        ShowOperationMessage(Error.data());
        UpdateSnapshot();
    }

    void MainWindow::ApplyAddFeature(TrainerFeature Feature)
    {
        switch (Feature)
        {
        case TrainerFeatureRefresh: ApplyFeature(Feature, TrainerOperationAdd, RefreshAddInput()); break;
        case TrainerFeatureExchange: ApplyFeature(Feature, TrainerOperationAdd, ExchangeAddInput()); break;
        case TrainerFeatureAp: ApplyFeature(Feature, TrainerOperationAdd, ApAddInput()); break;
        }
    }

    void MainWindow::SetDefaultInputs()
    {
        RefreshSetInput().Text(to_hstring(TrainerGetDefault(EngineHandle, TrainerFeatureRefresh, TrainerOperationSet)));
        RefreshAddInput().Text(to_hstring(TrainerGetDefault(EngineHandle, TrainerFeatureRefresh, TrainerOperationAdd)));
        ExchangeSetInput().Text(to_hstring(TrainerGetDefault(EngineHandle, TrainerFeatureExchange, TrainerOperationSet)));
        ExchangeAddInput().Text(to_hstring(TrainerGetDefault(EngineHandle, TrainerFeatureExchange, TrainerOperationAdd)));
        ApSetInput().Text(to_hstring(TrainerGetDefault(EngineHandle, TrainerFeatureAp, TrainerOperationSet)));
        ApAddInput().Text(to_hstring(TrainerGetDefault(EngineHandle, TrainerFeatureAp, TrainerOperationAdd)));
    }

    void MainWindow::SetValueLabels(const TrainerSnapshot& Snapshot)
    {
        const auto Label = [this](const TrainerFeatureSnapshot& Feature)
        {
            if (Feature.AddressCount == 0)
                return Utf8ToHString(ReadText("value_not_detected"));
            if (!Feature.WriteEnabled)
                return Utf8ToHString(ReadText("value_safety_blocked"));
            if (Feature.Minimum == Feature.Maximum)
                return to_hstring(Feature.Minimum);
            return to_hstring(Feature.Minimum) + L"–" + to_hstring(Feature.Maximum);
        };
        RefreshValueText().Text(Label(Snapshot.Refresh));
        ExchangeValueText().Text(Label(Snapshot.Exchange));
        ApValueText().Text(Label(Snapshot.Ap));
    }

    void MainWindow::SetWriteEnabled(const TrainerSnapshot& Snapshot)
    {
        const bool Verified = Snapshot.BuildVerified != 0;
        const bool Refresh = Verified && Snapshot.Refresh.WriteEnabled != 0;
        const bool Exchange = Verified && Snapshot.Exchange.WriteEnabled != 0;
        const bool Ap = Verified && Snapshot.Ap.WriteEnabled != 0;
        RefreshSetButton().IsEnabled(Refresh);
        RefreshAddButton().IsEnabled(Refresh);
        ExchangeSetButton().IsEnabled(Exchange);
        ExchangeAddButton().IsEnabled(Exchange);
        ApSetButton().IsEnabled(Ap);
        ApAddButton().IsEnabled(Ap);
        if (NativeBehavior)
            NativeBehavior->SetGameState(Snapshot.ProcessId, {Refresh, Exchange, Ap});
    }

    void MainWindow::UpdateSnapshot()
    {
        if (!EngineHandle || IsShuttingDown)
            return;
        TrainerSnapshot Snapshot{};
        std::array<char, 512> Error{};
        if (!TrainerGetSnapshot(EngineHandle, &Snapshot, Error.data(), Error.size()))
        {
            SetWriteEnabled({});
            StatusText().Text(Utf8ToHString(ReadText("snapshot_failed")));
            ShowOperationMessage(Error.data());
            return;
        }
        StatusText().Text(Utf8ToHString(InterfaceTexts->Translate(Snapshot.StatusMessage, InterfaceLanguage)));
        if (LastDetail != Snapshot.DetailMessage)
        {
            LastDetail = Snapshot.DetailMessage;
            SetTooltip(StatusText(), Utf8ToHString(InterfaceTexts->Translate(LastDetail, InterfaceLanguage)));
        }
        SetValueLabels(Snapshot);
        SetWriteEnabled(Snapshot);
    }

    void MainWindow::Shutdown()
    {
        if (IsShuttingDown)
            return;
        IsShuttingDown = true;
        NativeBehavior.reset();
        if (SnapshotTimer)
        {
            SnapshotTimer.Stop();
            SnapshotTimer = nullptr;
        }
        TrainerStop(EngineHandle);
        TrainerDestroy(EngineHandle);
        EngineHandle = nullptr;
    }

    void MainWindow::OnSetRefreshClick(Windows::Foundation::IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&)
    { ApplyFeature(TrainerFeatureRefresh, TrainerOperationSet, RefreshSetInput()); }
    void MainWindow::OnAddRefreshClick(Windows::Foundation::IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&)
    { ApplyAddFeature(TrainerFeatureRefresh); }
    void MainWindow::OnSetExchangeClick(Windows::Foundation::IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&)
    { ApplyFeature(TrainerFeatureExchange, TrainerOperationSet, ExchangeSetInput()); }
    void MainWindow::OnAddExchangeClick(Windows::Foundation::IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&)
    { ApplyAddFeature(TrainerFeatureExchange); }
    void MainWindow::OnSetApClick(Windows::Foundation::IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&)
    { ApplyFeature(TrainerFeatureAp, TrainerOperationSet, ApSetInput()); }
    void MainWindow::OnAddApClick(Windows::Foundation::IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&)
    { ApplyAddFeature(TrainerFeatureAp); }
}
