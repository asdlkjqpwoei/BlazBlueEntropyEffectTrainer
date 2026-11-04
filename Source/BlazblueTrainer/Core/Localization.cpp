#include "Localization.h"

#include <yaml-cpp/yaml.h>

#include <algorithm>
#include <fstream>
#include <stdexcept>

namespace Trainer
{
	namespace
	{
		LocalizedText ParseText(const YAML::Node& Node)
		{
			if (!Node.IsMap() || !Node["zh-CN"].IsScalar() || !Node["en-US"].IsScalar())
				throw std::runtime_error("Every text must provide zh-CN and en-US translations.");
			LocalizedText Result{Node["zh-CN"].as<std::string>(), Node["en-US"].as<std::string>()};
			if (Result.Chinese.empty() || Result.English.empty())
				throw std::runtime_error("Translations must not be empty.");
			return Result;
		}

		const std::string& Select(const LocalizedText& Text, TrainerLanguage Language)
		{
			if (Language == TrainerLanguageEnglish)
				return Text.English;
			if (Language == TrainerLanguageChinese)
				return Text.Chinese;
			throw std::invalid_argument("Unsupported interface language.");
		}
	}

	Localization Localization::Load(const std::filesystem::path& Path)
	{
		std::ifstream Input(Path, std::ios::binary);
		if (!Input)
			throw std::runtime_error("Cannot open the localization configuration.");
		return FromYaml(YAML::Load(Input));
	}

	Localization Localization::FromYaml(const YAML::Node& Root)
	{
		Localization Result;
		if (const auto Language = Root["language"]; Language && !Language.IsNull())
		{
			const auto Name = Language.as<std::string>();
			if (Name != "zh-CN" && Name != "en-US")
				throw std::runtime_error("The default language must be zh-CN or en-US.");
			Result.DefaultLanguage = Name == "en-US" ? TrainerLanguageEnglish : TrainerLanguageChinese;
		}
		const auto Texts = Root["texts"];
		if (!Texts.IsMap())
			throw std::runtime_error("The bilingual text catalog is missing.");
		for (const auto& Entry : Texts)
			Result.Messages.emplace(Entry.first.as<std::string>(), ParseText(Entry.second));
		static constexpr const char* RequiredKeys[]{
			"title", "subtitle", "waiting", "checking", "scanning", "ready", "no_targets",
			"refresh_calibrating", "unsupported_build", "access_denied", "process_error",
			"title_refresh", "title_exchange", "title_ap", "ap_unavailable", "set", "add",
			"potentials_grant", "potentials_hint", "potentials_busy", "potentials_already_owned",
			"perfect_guard", "perfect_on", "perfect_off", "perfect_hint", "perfect_busy", "perfect_enabled", "perfect_disabled",
			"refresh_hint", "process_connected", "process_waiting", "value_detected",
			"value_blocked", "value_not_detected", "value_safety_blocked", "input_error",
			"operation_succeeded", "operation_no_address", "operation_safety_error",
			"language", "set_placeholder", "add_placeholder", "initialization_failed",
			"monitor_start_failed", "timer_failed", "snapshot_failed",
			"databody_open", "databody_title", "databody_offline_hint", "databody_game_running",
			"databody_confirm_title", "databody_confirm_notice", "databody_backup_apply", "databody_saved",
			"databody_picker_opening", "databody_save_loading", "databody_save_preparing", "databody_save_writing",
			"databody_tab_edit", "databody_tab_add", "databody_list_heading", "databody_select_body",
			"databody_condition_prefix", "databody_no_requirement", "databody_add_apply",
			"tab_runtime", "databody_reset_all", "databody_save_all", "databody_draft_hint",
			"databody_pending", "databody_draft_only", "databody_no_changes", "databody_pending_reload",
			"databody_staged", "databody_reset_done", "databody_before", "databody_after",
			"strategy_heading", "strategy_search", "strategy_all_elements", "strategy_all_triggers",
			"strategy_all_dual", "strategy_all_tiers", "strategy_single", "strategy_dual", "strategy_tier",
			"strategy_results", "strategy_no_results", "strategy_select", "strategy_empty_slot",
			"strategy_use_first", "strategy_use_second",
			"strategy_element_0", "strategy_element_1", "strategy_element_2", "strategy_element_3",
			"strategy_element_4", "strategy_element_5", "strategy_element_6", "strategy_element_7",
			"strategy_trigger_0", "strategy_trigger_1", "strategy_trigger_2", "strategy_trigger_3",
			"strategy_trigger_4", "strategy_trigger_5"
		};
		for (const auto Key : RequiredKeys)
		{
			if (!Result.Messages.contains(Key))
				throw std::runtime_error(std::string("Missing bilingual UI text: ") + Key);
		}
		if (const auto DiagnosticNode = Root["diagnostics"]; DiagnosticNode && !DiagnosticNode.IsNull())
		{
			if (!DiagnosticNode.IsMap())
				throw std::runtime_error("Diagnostic translations must be a map.");
			for (const auto& Entry : DiagnosticNode)
				Result.Diagnostics.push_back(ParseText(Entry.second));
			std::sort(Result.Diagnostics.begin(), Result.Diagnostics.end(),
				[](const auto& Left, const auto& Right) { return Left.English.size() > Right.English.size(); });
		}
		return Result;
	}

	const std::string& Localization::Text(std::string_view Key, TrainerLanguage Language) const
	{
		const auto Found = Messages.find(std::string(Key));
		if (Found == Messages.end())
			throw std::runtime_error(std::string("Missing interface text: ") + std::string(Key));
		return Select(Found->second, Language);
	}

	std::string Localization::Translate(std::string_view Message, TrainerLanguage Language) const
	{
		for (const auto& [Key, Text] : Messages)
		{
			if (Message == Text.Chinese || Message == Text.English)
				return Select(Text, Language);
		}
		std::string Result(Message);
		for (const auto& Text : Diagnostics)
		{
			const auto& Source = Language == TrainerLanguageEnglish ? Text.Chinese : Text.English;
			const auto& Replacement = Select(Text, Language);
			std::size_t Position = 0;
			while ((Position = Result.find(Source, Position)) != std::string::npos)
			{
				Result.replace(Position, Source.size(), Replacement);
				Position += Replacement.size();
			}
		}
		return Result;
	}
}
