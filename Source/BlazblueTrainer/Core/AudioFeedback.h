#pragma once

#include <cstddef>
#include <span>
#include <string>

namespace Trainer
{
    std::span<const std::byte> HotkeyFeedbackWave(bool Succeeded);
    bool PlayHotkeyFeedback(bool Succeeded, std::string &Error);
} // namespace Trainer
