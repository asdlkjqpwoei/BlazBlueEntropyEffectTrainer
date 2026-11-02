#pragma once

#include <filesystem>
#include <string_view>

namespace Trainer
{
    std::filesystem::path ExecutableDirectory();
    std::filesystem::path DefaultConfigPath();
    std::filesystem::path PathFromUtf8(std::string_view Text);
}
