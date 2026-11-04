#include "Pch.h"
#include "App.xaml.h"
#include "XamlMetaDataProvider.g.cpp"

#include "MainWindow.xaml.h"

int __stdcall wWinMain(HINSTANCE, HINSTANCE, PWSTR, int)
{
	// Marshal accessibility calls back to the XAML thread instead of letting an
	// MTA UI Automation worker access thread-bound WinUI input objects directly.
	winrt::init_apartment(winrt::apartment_type::single_threaded);
	winrt::Microsoft::UI::Xaml::Application::Start([](auto&&)
	{
		winrt::make<winrt::BlazblueTrainer::implementation::App>();
	});
	return 0;
}

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
