#include "Config.h"
#include "Paths.h"
#include "Hotkeys.h"

#include <yaml-cpp/yaml.h>
#include <Windows.h>

#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>

int RunHotkeyTests()
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
	const auto MainKey = Trainer::ParseHotkey("1", TrainerFeatureRefresh);
	Require(MainKey && MainKey->VirtualKey == '1' && MainKey->VirtualKey != VK_NUMPAD1 &&
		MainKey->Modifiers == 0, "The default digit must be the main keyboard, not the keypad.");
	const auto Control = Trainer::ParseHotkey(" ctrl + 1 ", TrainerFeatureExchange);
	Require(Control && Control->Modifiers == MOD_CONTROL && Control->Display == "Ctrl+1" &&
		Control->Feature == TrainerFeatureExchange, "Ctrl+1 parsing or feature routing failed.");
	const auto Combined = Trainer::ParseHotkey("Win+Shift+Alt+Ctrl+f24", TrainerFeatureAp);
	Require(Combined && Combined->VirtualKey == VK_F24 && Combined->Modifiers == 15 &&
		Combined->Display == "Ctrl+Alt+Shift+Win+F24", "Combined modifiers must normalize deterministically.");
	Require(!Trainer::ParseHotkey("", TrainerFeatureRefresh), "An empty binding must disable its shortcut.");
	for (const char* Invalid : {
			 "Ctrl", "Ctrl+", "Ctrl++1", "Ctrl+Ctrl+1", "Ctrl+1+2", "Alt+F25", "1+Ctrl", "Num1", "Ctrl+?" })
	{
		bool Rejected = false;
		try { Trainer::ParseHotkey(Invalid, TrainerFeatureRefresh); }
		catch (const std::runtime_error&) { Rejected = true; }
		Require(Rejected, "An invalid hotkey was accepted.");
	}
	Require(Trainer::IsHotkeyContextAllowed(42, 42, true),
		"A ready feature must respond when its game is foreground.");
	Require(!Trainer::IsHotkeyContextAllowed(0, 0, true) &&
		!Trainer::IsHotkeyContextAllowed(42, 43, true) &&
		!Trainer::IsHotkeyContextAllowed(42, 42, false),
		"Shortcuts must not trigger in another app, while typing in the trainer, or without a valid feature.");
	Trainer::HotkeyPress Press;
	Require(Press.Poll(true, true) && !Press.Signal() && !Press.Poll(true, true),
		"Polling and WM_HOTKEY must share one held-key latch.");
	Require(!Press.Poll(false, true) && Press.Signal() && !Press.Poll(true, true),
		"A registered press must not be repeated by physical polling.");
	Press.Poll(false, false);
	Require(!Press.Poll(true, false) && !Press.Poll(true, true),
		"A key held before focus or feature readiness must not fire when the context changes.");
	Press.Poll(false, true);
	Require(Press.Poll(true, true), "Physical polling must fire again after a release.");

	const auto Path = std::filesystem::temp_directory_path() /
		("BlazblueConfigTest-" + std::to_string(GetCurrentProcessId()) + ".yaml");
	const auto Write = [&](const YAML::Node& Node)
	{
		std::ofstream Output(Path);
		if (!Output) throw std::runtime_error("Cannot create a temporary config test.");
		Output << Node;
		if (!Output) throw std::runtime_error("Cannot write the temporary config test.");
	};
	std::ifstream Source(Trainer::DefaultConfigPath(), std::ios::binary);
	if (!Source) throw std::runtime_error("Cannot open the deployed test configuration.");
	auto Node = YAML::Load(Source);
	Node["hotkeys"]["refresh_add"] = "Ctrl+1";
	Write(Node);
	auto Configuration = Trainer::LoadConfig(Path);
	Require(Configuration.Hotkeys[0] && Configuration.Hotkeys[0]->Modifiers == MOD_CONTROL,
		"YAML must control the active key combination.");
	Require(Configuration.Hotkeys[3] && Configuration.Hotkeys[3]->VirtualKey == '4' &&
		Configuration.Hotkeys[3]->Feature == TrainerFeaturePotentials,
		"The potential action must default to the main-keyboard 4 key.");
	Node["hotkeys"].remove("potentials_grant");
	Write(Node);
	Configuration = Trainer::LoadConfig(Path);
	Require(Configuration.Hotkeys[3] && Configuration.Hotkeys[3]->VirtualKey == '4',
		"Existing three-action YAML configs must gain the potential shortcut without failing to load.");
	Node["hotkeys"]["ap_add"] = "control+1";
	Write(Node);
	bool DuplicateRejected = false;
	try { Trainer::LoadConfig(Path); }
	catch (const std::runtime_error&) { DuplicateRejected = true; }
	Require(DuplicateRejected, "Duplicate key combinations must be rejected before registration.");
	Node.remove("hotkeys");
	Write(Node);
	Configuration = Trainer::LoadConfig(Path);
	Require(Configuration.Hotkeys[0]->VirtualKey == '1' &&
		Configuration.Hotkeys[1]->VirtualKey == '2' &&
		Configuration.Hotkeys[2]->VirtualKey == '3' && Configuration.Hotkeys[3]->VirtualKey == '4',
		"Legacy configs must retain safe default bindings.");
	Node["window"]["width"] = 0;
	Write(Node);
	bool SizeRejected = false;
	try { Trainer::LoadConfig(Path); }
	catch (const std::runtime_error&) { SizeRejected = true; }
	Require(SizeRejected, "Invalid configured window dimensions must fail explicitly.");
	std::error_code Error;
	std::filesystem::remove(Path, Error);
	if (Error)
	{
		std::cerr << "Temporary config cleanup failed: " << Error.message() << '\n';
		++Failures;
	}
	return Failures != 0;
}
