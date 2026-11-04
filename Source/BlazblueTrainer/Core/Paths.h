#pragma once

#include <filesystem>
#include <string_view>

namespace Trainer
{
	inline constexpr wchar_t RuntimeBridgeFileName[] = L"BlazblueRuntimeBridge.dll";

	std::filesystem::path ExecutableDirectory();
	std::filesystem::path DefaultConfigPath();
	std::filesystem::path PathFromUtf8(std::string_view Text);
}
