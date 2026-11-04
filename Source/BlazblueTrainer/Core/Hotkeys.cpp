#include "Hotkeys.h"

#include <array>
#include <charconv>
#include <cctype>
#include <stdexcept>
#include <vector>

namespace Trainer
{
	bool HotkeyPress::Poll(bool Down, bool Allowed)
	{
		if (!Down) { Held = false; return false; }
		if (Held) return false;
		Held = true;
		return Allowed;
	}

	bool HotkeyPress::Signal()
	{
		if (Held) return false;
		Held = true;
		return true;
	}

	namespace
	{
		std::string NormalizeToken(std::string_view Text)
		{
			while (!Text.empty() && std::isspace(static_cast<unsigned char>(Text.front())))
				Text.remove_prefix(1);
			while (!Text.empty() && std::isspace(static_cast<unsigned char>(Text.back())))
				Text.remove_suffix(1);
			std::string Result;
			for (const unsigned char Character : Text)
				Result.push_back(static_cast<char>(std::toupper(Character)));
			return Result;
		}
	}

	std::optional<HotkeyBinding> ParseHotkey(std::string_view Text, TrainerFeature Feature)
	{
		if (NormalizeToken(Text).empty())
			return std::nullopt;
		std::vector<std::string> Tokens;
		for (;;)
		{
			const auto Separator = Text.find('+');
			Tokens.push_back(NormalizeToken(Text.substr(0, Separator)));
			if (Tokens.back().empty())
				throw std::runtime_error("A hotkey contains an empty key or modifier.");
			if (Separator == std::string_view::npos)
				break;
			Text.remove_prefix(Separator + 1);
		}
		HotkeyBinding Result;
		Result.Feature = Feature;
		for (std::size_t Index = 0; Index + 1 < Tokens.size(); ++Index)
		{
			const auto& Token = Tokens[Index];
			const std::uint32_t Modifier = Token == "ALT" ? 1 :
				(Token == "CTRL" || Token == "CONTROL") ? 2 :
				Token == "SHIFT" ? 4 : (Token == "WIN" || Token == "WINDOWS") ? 8 : 0;
			if (Modifier == 0 || (Result.Modifiers & Modifier) != 0)
				throw std::runtime_error("Hotkeys support distinct Ctrl, Alt, Shift and Win modifiers.");
			Result.Modifiers |= Modifier;
		}
		const auto& Key = Tokens.back();
		if (Key.size() == 1 &&
			((Key[0] >= '0' && Key[0] <= '9') || (Key[0] >= 'A' && Key[0] <= 'Z')))
		{
			Result.VirtualKey = static_cast<unsigned char>(Key[0]);
		}
		else if (Key.starts_with('F'))
		{
			unsigned Number = 0;
			const auto Parsed = std::from_chars(Key.data() + 1, Key.data() + Key.size(), Number);
			if (Parsed.ec != std::errc{} || Parsed.ptr != Key.data() + Key.size() ||
				Number < 1 || Number > 24)
				throw std::runtime_error("Function hotkeys must be F1 through F24.");
			Result.VirtualKey = 0x70 + Number - 1;
		}
		else
		{
			throw std::runtime_error("A hotkey must end with a main-keyboard digit, letter, or F1-F24.");
		}
		for (const auto& [Mask, Name] : std::array<std::pair<std::uint32_t, const char*>, 4>{
				 {{2, "Ctrl+"}, {1, "Alt+"}, {4, "Shift+"}, {8, "Win+"}}})
		{
			if ((Result.Modifiers & Mask) != 0)
				Result.Display += Name;
		}
		Result.Display += Key;
		return Result;
	}

	bool IsHotkeyContextAllowed(
		std::uint32_t GameProcessId, std::uint32_t ForegroundProcessId, bool FeatureEnabled)
	{
		return FeatureEnabled && GameProcessId != 0 && GameProcessId == ForegroundProcessId;
	}
}
