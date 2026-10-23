#pragma once

#include "TrainerApi.h"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace Trainer
{
    struct HotkeyBinding
    {
        TrainerFeature Feature{};
        std::uint32_t VirtualKey{};
        std::uint32_t Modifiers{};
        std::string Display;

        bool operator==(const HotkeyBinding&) const = default;
    };

    std::optional<HotkeyBinding> ParseHotkey(std::string_view Text, TrainerFeature Feature);
    bool IsHotkeyContextAllowed(
        std::uint32_t GameProcessId, std::uint32_t ForegroundProcessId, bool FeatureEnabled);
}
