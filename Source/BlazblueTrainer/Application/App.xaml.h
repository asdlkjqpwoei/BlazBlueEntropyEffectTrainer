#pragma once

#include "App.xaml.g.h"

namespace winrt::BlazblueTrainer::implementation
{
	struct App : AppT<App>
	{
		App();
		void OnLaunched(Microsoft::UI::Xaml::LaunchActivatedEventArgs const& Args);

	private:
		Microsoft::UI::Xaml::Window MainApplicationWindow{ nullptr };
	};
}
