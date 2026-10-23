#pragma once

#include <filesystem>

namespace Trainer
{
    std::filesystem::path ExecutableDirectory();
    std::filesystem::path DefaultConfigPath();
}
