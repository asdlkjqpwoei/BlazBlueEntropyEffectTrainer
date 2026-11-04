#include "Config.h"
#include "Localization.h"
#include "Paths.h"
#include "Text.h"
#include "TrainerEngine.h"

#include <yaml-cpp/yaml.h>

#include <array>
#include <fstream>
#include <iostream>
#include <stdexcept>

int RunLocalizationTests()
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
	const auto ConfigPath = Trainer::DefaultConfigPath();
	const auto Texts = Trainer::Localization::Load(ConfigPath);
	Require(Texts.DefaultLanguage == TrainerLanguageChinese, "The shipped UI must default to Chinese.");
	Require(Texts.Text("add", TrainerLanguageEnglish) == "Add" &&
		Texts.Text("add", TrainerLanguageChinese) != "Add", "Both interface languages must be present.");
	Require(Texts.Text("databody_picker_opening", TrainerLanguageEnglish) !=
		Texts.Text("databody_picker_opening", TrainerLanguageChinese),
		"The async file picker status must support both interface languages.");
	Require(Texts.Text("potentials_grant", TrainerLanguageEnglish) !=
		Texts.Text("potentials_grant", TrainerLanguageChinese) &&
		Texts.Text("potentials_busy", TrainerLanguageEnglish) !=
		Texts.Text("potentials_busy", TrainerLanguageChinese),
		"The one-click potential action and its busy state must support both interface languages.");
	const std::string PotentialCounts = "Potentials added: 5; total for this character: 12";
	Require(Texts.Text("perfect_guard", TrainerLanguageEnglish) != Texts.Text("perfect_guard", TrainerLanguageChinese) &&
		Texts.Translate("The current game does not expose a compatible perfect-guard API.", TrainerLanguageChinese) !=
			"The current game does not expose a compatible perfect-guard API.",
		"The perfect switch and compatibility failures must support both interface languages.");
	const auto ChineseCounts = Texts.Translate(PotentialCounts, TrainerLanguageChinese);
	Require(ChineseCounts != PotentialCounts &&
		Texts.Translate(ChineseCounts, TrainerLanguageEnglish) == PotentialCounts,
		"Dynamic potential counts must switch languages without losing their values.");
	const auto ChineseError = Texts.Translate("A compatible game process is not connected.", TrainerLanguageChinese);
	Require(ChineseError != "A compatible game process is not connected." &&
		Texts.Translate(ChineseError, TrainerLanguageEnglish) ==
			"A compatible game process is not connected.", "Action diagnostics must switch in both directions.");
	std::array<char, 5> Small{};
	const auto& ChineseTitle = Texts.Text("title", TrainerLanguageChinese);
	Trainer::CopyUtf8(Small.data(), Small.size(), ChineseTitle);
	Require(std::string(Small.data()) == ChineseTitle.substr(0, 3),
		"A truncated translated message must not split a UTF-8 character.");

	std::array<char, 512> Error{};
	TrainerEngine* Handle = TrainerCreate(ConfigPath.c_str(), Error.data(), Error.size());
	if (!Handle) throw std::runtime_error(Error.data());
	std::array<char, 128> Label{};
	Require(TrainerSetLanguage(Handle, TrainerLanguageEnglish, Error.data(), Error.size()) != 0 &&
		TrainerGetText(Handle, "set", Label.data(), Label.size()) != 0 &&
		std::string(Label.data()) == "Set", "The public API must switch labels immediately.");
	TrainerSnapshot Snapshot{};
	TrainerGetSnapshot(Handle, &Snapshot, Error.data(), Error.size());
	Require(std::string(Snapshot.StatusMessage) == Texts.Text("waiting", TrainerLanguageEnglish) &&
		Snapshot.State == TrainerStateStopped, "Switching language must preserve engine state.");
	Require(TrainerSetLanguage(Handle, static_cast<TrainerLanguage>(99), Error.data(), Error.size()) == 0,
		"Invalid languages must fail explicitly.");
	Require(TrainerSetLanguage(Handle, TrainerLanguageChinese, Error.data(), Error.size()) != 0,
		"Switching back to Chinese failed.");
	std::int32_t Writes = -1;
	Require(TrainerApply(Handle, TrainerFeatureAp, TrainerOperationAdd, 1,
		&Writes, Error.data(), Error.size()) == 0 && Writes == 0 &&
		std::string(Error.data()) == ChineseError, "Localized errors must not allow disconnected writes.");
	TrainerDestroy(Handle);

	std::ifstream Input(ConfigPath, std::ios::binary);
	auto Node = YAML::Load(Input);
	Node["texts"]["add"].remove("en-US");
	bool MissingRejected = false;
	try { Trainer::Localization::FromYaml(Node); }
	catch (const std::runtime_error&) { MissingRejected = true; }
	Require(MissingRejected, "Missing translations must not silently fall back.");
	Input.clear();
	Input.seekg(0);
	Node = YAML::Load(Input);
	Node["texts"].remove("databody_picker_opening");
	MissingRejected = false;
	try {Trainer::Localization::FromYaml(Node);}
	catch (const std::runtime_error&) {MissingRejected = true;}
	Require(MissingRejected, "The async file picker status must not be silently omitted.");
	Input.clear();
	Input.seekg(0);
	Node = YAML::Load(Input);
	Node["texts"].remove("potentials_grant");
	MissingRejected = false;
	try {Trainer::Localization::FromYaml(Node);}
	catch (const std::runtime_error&) {MissingRejected = true;}
	Require(MissingRejected, "The one-click potential action must not silently omit its translations.");
	Input.clear();
	Input.seekg(0);
	Node = YAML::Load(Input);
	Node["language"] = "en-US";
	Require(Trainer::Localization::FromYaml(Node).DefaultLanguage == TrainerLanguageEnglish,
		"The configured default language must be honored.");

	auto Configuration = Trainer::LoadConfig(ConfigPath);
	Configuration.Module = "BlazblueLanguageTestNoGame.dll";
	Trainer::Engine Engine(std::move(Configuration));
	std::string StartError;
	if (!Engine.Start(StartError)) throw std::runtime_error(StartError);
	for (int Index = 0; Index < 20; ++Index)
	{
		const auto Language = Index % 2 ? TrainerLanguageEnglish : TrainerLanguageChinese;
		Engine.SetLanguage(Language);
		Require(Engine.Text("add") == Texts.Text("add", Language),
			"Concurrent monitoring must not retain the previous language.");
	}
	Engine.Stop();
	return Failures != 0;
}
