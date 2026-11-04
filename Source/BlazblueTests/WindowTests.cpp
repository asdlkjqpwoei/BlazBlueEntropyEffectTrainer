#include "WindowBehavior.h"

#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace
{
	struct TestWindow
	{
		explicit TestWindow(const wchar_t* Name) : ClassName(Name)
		{
			WNDCLASSW Descriptor{};
			Descriptor.lpfnWndProc = DefWindowProcW;
			Descriptor.hInstance = GetModuleHandleW(nullptr);
			Descriptor.lpszClassName = Name;
			if (!RegisterClassW(&Descriptor) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS)
				throw std::runtime_error("Cannot register the isolated window-test class.");
			Handle = CreateWindowExW(0, Name, Name, WS_OVERLAPPEDWINDOW,
				0, 0, 100, 100, nullptr, nullptr, Descriptor.hInstance, nullptr);
			if (!Handle)
				throw std::runtime_error("Cannot create the isolated window-test surface.");
		}
		~TestWindow()
		{
			DestroyWindow(Handle);
			UnregisterClassW(ClassName, GetModuleHandleW(nullptr));
		}
		const wchar_t* ClassName{};
		HWND Handle{};
	};
}

int RunWindowTests()
{
	int Failures = 0;
	const auto Require = [&](bool Value, const char* Message)
	{
		if (!Value)
		{
			std::cerr << Message << '\n';
			++Failures;
		}
	};
	const Trainer::WindowConfig Preferred{380, 364};
	Require(Trainer::FitClientSize(Preferred, 96, {1920, 1080}, {16, 40}) ==
		Trainer::ClientSize{380, 364}, "100% DPI dimensions changed.");
	Require(Trainer::FitClientSize(Preferred, 144, {1920, 1080}, {24, 60}) ==
		Trainer::ClientSize{570, 546}, "150% DPI must preserve the logical layout.");
	Require(Trainer::FitClientSize(Preferred, 192, {1920, 1080}, {32, 80}) ==
		Trainer::ClientSize{760, 728}, "200% DPI must scale the entire client area.");
	Require(Trainer::FitClientSize(Preferred, 192, {640, 480}, {32, 80}) ==
		Trainer::ClientSize{584, 376}, "Small work areas must clamp the window rather than put controls off-screen.");
	TestWindow Window(L"BlazblueWindowTests");
	std::vector<std::string> Errors;
	Trainer::WindowBehavior Behavior(Window.Handle, Preferred, {},
		[&](TrainerFeature) { ++Failures; },
		[&](const std::string& Error) { Errors.push_back(Error); });
	const auto Style = GetWindowLongPtrW(Window.Handle, GWL_STYLE);
	Require((Style & WS_MAXIMIZEBOX) == 0 && (Style & WS_THICKFRAME) == 0,
		"The native trainer must not expose maximize or manual resize styles.");
	SendMessageW(Window.Handle, WM_SYSCOMMAND, SC_MAXIMIZE, 0);
	Require(!IsZoomed(Window.Handle), "A maximize command must be rejected.");
	RECT Before{};
	RECT After{};
	GetWindowRect(Window.Handle, &Before);
	SendMessageW(Window.Handle, WM_SYSCOMMAND, SC_SIZE, 0);
	GetWindowRect(Window.Handle, &After);
	Require(EqualRect(&Before, &After), "A manual size command changed the fixed window.");
	Behavior.SetGameState(0, {true, true, true});
	SendMessageW(Window.Handle, WM_HOTKEY, 0x4b20, MAKELPARAM(0, '1'));
	Require(Errors.empty(), "DPI/window setup produced a runtime error.");
	return Failures != 0;
}

int RunWindowIntegration()
{
	TestWindow Window(L"BlazblueShortcutValidation");
	std::vector<TrainerFeature> Actions;
	std::vector<std::string> Errors;
	const std::array Bindings{
		Trainer::ParseHotkey("Ctrl+Alt+F22", TrainerFeatureRefresh),
		Trainer::ParseHotkey("Ctrl+Alt+F23", TrainerFeatureExchange),
		Trainer::ParseHotkey("Ctrl+Alt+F24", TrainerFeatureAp),
		Trainer::ParseHotkey("Ctrl+Alt+F21", TrainerFeaturePotentials)
	};
	Trainer::WindowBehavior Behavior(Window.Handle, {}, Bindings,
		[&](TrainerFeature Feature) { Actions.push_back(Feature); },
		[&](const std::string& Error) { Errors.push_back(Error); });
	DWORD ForegroundProcessId{};
	GetWindowThreadProcessId(GetForegroundWindow(), &ForegroundProcessId);
	if (ForegroundProcessId == 0)
	{
		std::cerr << "The desktop has no foreground context for shortcut integration.\n";
		return 3;
	}
	// Use the current foreground as a synthetic target; callbacks never touch a game engine.
	Behavior.SetGameState(ForegroundProcessId, {true, true, true, true});
	for (std::size_t Index = 0; Index < Bindings.size(); ++Index)
	{
		SendMessageW(Window.Handle, WM_HOTKEY, 0x4b20 + Index,
			MAKELPARAM(Bindings[Index]->Modifiers, Bindings[Index]->VirtualKey));
	}
	if (Actions != std::vector<TrainerFeature>{
			TrainerFeatureRefresh, TrainerFeatureExchange, TrainerFeatureAp, TrainerFeaturePotentials})
		Errors.push_back("A configured shortcut routed to the wrong increase action.");
	{
		TestWindow FallbackWindow(L"BlazblueShortcutFallbackValidation");
		std::array<bool, 256> PhysicalKeys{};
		std::vector<TrainerFeature> FallbackActions;
		Trainer::WindowBehavior Fallback(FallbackWindow.Handle, {}, Bindings,
			[&](TrainerFeature Feature) { FallbackActions.push_back(Feature); },
			[&](const std::string& Error) { Errors.push_back(Error); },
			[&](int Key) { return PhysicalKeys.at(static_cast<std::size_t>(Key)); });
		// The first window already owns these registrations. The second must
		// still observe physical presses through its native WM_TIMER path.
		Fallback.SetGameState(ForegroundProcessId, {true, true, true, true});
		const auto Poll = [&] { SendMessageW(FallbackWindow.Handle, WM_TIMER, 0x4b30, 0); };
		PhysicalKeys[VK_CONTROL] = PhysicalKeys[VK_MENU] = true;
		for (std::size_t Index = 0; Index < Bindings.size(); ++Index)
		{
			const auto Key = Bindings[Index]->VirtualKey;
			PhysicalKeys[Key] = true;
			Poll();
			Poll();
			PhysicalKeys[Key] = false;
			Poll();
		}
		if (FallbackActions != std::vector<TrainerFeature>{
			TrainerFeatureRefresh, TrainerFeatureExchange, TrainerFeatureAp, TrainerFeaturePotentials})
			Errors.push_back("Registration conflicts blocked physical polling or a held key repeated.");
		Fallback.SetGameState(ForegroundProcessId, {});
		PhysicalKeys[VK_F22] = true;
		Poll();
		Fallback.SetGameState(ForegroundProcessId, {true, true, true, true});
		Poll();
		if (FallbackActions.size() != 4)
			Errors.push_back("A held key fired when feature readiness changed.");
		PhysicalKeys[VK_F22] = false;
		Poll();
		PhysicalKeys[VK_SHIFT] = PhysicalKeys[VK_F22] = true;
		Poll();
		if (FallbackActions.size() != 4)
			Errors.push_back("An extra modifier incorrectly matched a polling shortcut.");
		PhysicalKeys[VK_F22] = false;
		Poll();
		PhysicalKeys[VK_SHIFT] = false;
		PhysicalKeys[VK_F22] = true;
		Poll();
		if (FallbackActions.size() != 5 || FallbackActions.back() != TrainerFeatureRefresh)
			Errors.push_back("A fresh physical press failed after readiness recovered.");
		Fallback.SetGameState(0, {});
		PhysicalKeys[VK_F22] = false;
		Poll();
		PhysicalKeys[VK_F22] = true;
		Poll();
		if (FallbackActions.size() != 5)
			Errors.push_back("An inactive game context still accepted a physical shortcut.");
	}
	Behavior.SetGameState(0, {false, false, false});
	SendMessageW(Window.Handle, WM_HOTKEY, 0x4b20, MAKELPARAM(MOD_CONTROL | MOD_ALT, VK_F22));
	if (Actions.size() != 4)
		Errors.push_back("An inactive shortcut still invoked an action.");
	if (!Errors.empty())
	{
		for (const auto& Error : Errors)
			std::cerr << Error << '\n';
		return 1;
	}
	std::cout << "All four native and physical-polling routes, registration conflicts, held keys, modifiers, readiness transitions and inactive-context blocking passed; no game writes.\n";
	return 0;
}
