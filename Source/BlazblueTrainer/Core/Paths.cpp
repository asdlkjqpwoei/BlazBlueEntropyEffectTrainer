#include "Paths.h"

#include <Windows.h>

#include <stdexcept>
#include <string>

namespace Trainer
{
	std::filesystem::path ExecutableDirectory()
	{
		std::wstring Buffer(32768, L'\0');
		const DWORD Length = GetModuleFileNameW(nullptr, Buffer.data(), static_cast<DWORD>(Buffer.size()));
		if (Length == 0 || Length >= Buffer.size())
			throw std::runtime_error("Could not determine the trainer executable path.");
		Buffer.resize(Length);
		return std::filesystem::path(Buffer).parent_path();
	}

	std::filesystem::path DefaultConfigPath()
	{
		return ExecutableDirectory() / L"Config" / L"Defaults.yaml";
	}

	std::filesystem::path PathFromUtf8(std::string_view Text)
	{
		std::u8string Value;
		Value.reserve(Text.size());
		for (const auto Character : Text)
			Value.push_back(static_cast<char8_t>(Character));
		return std::filesystem::path(Value);
	}
}
