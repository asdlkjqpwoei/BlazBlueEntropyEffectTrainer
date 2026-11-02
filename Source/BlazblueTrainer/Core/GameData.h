#pragma once

#include "SaveDocument.h"

#include <filesystem>
#include <optional>
#include <string>
#include <unordered_map>

namespace Trainer
{
    Bytes ReadBinaryFile(const std::filesystem::path &Path, std::size_t Maximum = 64 * 1024 * 1024);
    std::optional<std::filesystem::path> FindInstalledGame();
    std::vector<std::filesystem::path> FindGameSaves();

    class GameAssets final
    {
      public:
        explicit GameAssets(const std::filesystem::path &GameDirectory);
        Bytes TextAsset(const std::string &Resource) const;
        const std::filesystem::path &Directory() const
        {
            return Root;
        }

      private:
        struct Location
        {
            std::filesystem::path File;
            std::uint64_t Offset{};
        };
        std::filesystem::path Root;
        std::unordered_map<std::string, Location> Locations;
    };
} // namespace Trainer
