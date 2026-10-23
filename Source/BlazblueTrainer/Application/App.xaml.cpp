#include "Pch.h"
#include "App.xaml.h"
#include "XamlMetaDataProvider.g.cpp"

#include "MainWindow.xaml.h"

namespace winrt::BlazblueTrainer::implementation
{
    App::App()
    {
        InitializeComponent();
    }

    void App::OnLaunched(Microsoft::UI::Xaml::LaunchActivatedEventArgs const&)
    {
        MainApplicationWindow = winrt::make<MainWindow>();
        MainApplicationWindow.Activate();
    }
}
