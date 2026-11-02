#include "AudioFeedback.h"

#include <Windows.h>
#include <mmsystem.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <numbers>
#include <system_error>
#include <vector>

namespace Trainer
{
    namespace
    {
        std::vector<std::byte> MakeWave(bool Succeeded)
        {
            constexpr std::uint32_t Rate = 22050;
            constexpr std::uint32_t Samples = Rate / 4;
            std::vector<std::byte> Bytes(44 + Samples * 2);
            const auto Store = [&](std::size_t Offset, auto Value) {
                std::memcpy(Bytes.data() + Offset, &Value, sizeof(Value));
            };
            std::memcpy(Bytes.data(), "RIFF", 4);
            Store(4, static_cast<std::uint32_t>(Bytes.size() - 8));
            std::memcpy(Bytes.data() + 8, "WAVEfmt ", 8);
            Store(16, std::uint32_t{16});
            Store(20, std::uint16_t{1});
            Store(22, std::uint16_t{1});
            Store(24, Rate);
            Store(28, Rate * 2);
            Store(32, std::uint16_t{2});
            Store(34, std::uint16_t{16});
            std::memcpy(Bytes.data() + 36, "data", 4);
            Store(40, Samples * 2);
            for (std::uint32_t Index = 0; Index < Samples; ++Index)
            {
                const double Progress = static_cast<double>(Index) / Samples;
                const double Frequency = Succeeded ? (Progress < 0.5 ? 880.0 : 1174.66) : 220.0;
                const double Envelope = std::min({1.0, Progress * 30.0, (1.0 - Progress) * 20.0});
                const auto Sample = static_cast<std::int16_t>(
                    5000.0 * Envelope * std::sin(2.0 * std::numbers::pi * Frequency * Index / Rate));
                Store(44 + Index * 2, Sample);
            }
            return Bytes;
        }
    } // namespace

    std::span<const std::byte> HotkeyFeedbackWave(bool Succeeded)
    {
        static const auto Success = MakeWave(true);
        static const auto Failure = MakeWave(false);
        return Succeeded ? std::span<const std::byte>(Success) : std::span<const std::byte>(Failure);
    }

    bool PlayHotkeyFeedback(bool Succeeded, std::string &Error)
    {
        const auto Wave = HotkeyFeedbackWave(Succeeded);
        if (!PlaySoundW(reinterpret_cast<LPCWSTR>(Wave.data()), nullptr, SND_MEMORY | SND_ASYNC | SND_NODEFAULT))
        {
            Error =
                "Cannot play shortcut feedback: " + std::system_category().message(static_cast<int>(GetLastError()));
            return false;
        }
        Error.clear();
        return true;
    }
} // namespace Trainer
