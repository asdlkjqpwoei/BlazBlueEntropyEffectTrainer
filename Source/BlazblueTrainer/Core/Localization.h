#pragma once

#include "TrainerApi.h"

#include <yaml-cpp/node/node.h>

#include <filesystem>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace Trainer
{
	struct LocalizedText
	{
		std::string Chinese;
		std::string English;
	};

	class Localization
	{
	public:
		static Localization FromYaml(const YAML::Node& Root);
		static Localization Load(const std::filesystem::path& Path);
		const std::string& Text(std::string_view Key, TrainerLanguage Language) const;
		std::string Translate(std::string_view Message, TrainerLanguage Language) const;
		TrainerLanguage DefaultLanguage{ TrainerLanguageChinese };

	private:
		std::unordered_map<std::string, LocalizedText> Messages;
		std::vector<LocalizedText> Diagnostics;
	};
}
