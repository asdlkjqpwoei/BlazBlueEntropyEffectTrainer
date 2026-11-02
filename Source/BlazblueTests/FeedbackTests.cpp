#include "AudioFeedback.h"
#include "Config.h"
#include "Paths.h"
#include "RemoteProcess.h"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <fstream>
#include <iostream>
#include <thread>

int RunFeedbackTests()
{
    const auto Success = Trainer::HotkeyFeedbackWave(true);
    const auto Failure = Trainer::HotkeyFeedbackWave(false);
    if (Success.size() <= 44 || Failure.size() != Success.size() || std::memcmp(Success.data(), "RIFF", 4) ||
        std::memcmp(Success.data() + 8, "WAVEfmt ", 8) || std::equal(Success.begin(), Success.end(), Failure.begin()))
    {
        std::cerr << "Shortcut success/failure audio must be distinct valid in-memory WAV data.\n";
        return 1;
    }

    const auto Configuration = Trainer::LoadConfig(Trainer::DefaultConfigPath());
    if (!Trainer::RemoteProcess::CanProbeUpdatedRuntime(Configuration, L"BLAZBLUEENTROPYEFFECT.EXE") ||
        Trainer::RemoteProcess::CanProbeUpdatedRuntime(Configuration, L"AnotherUnityGame.exe"))
    {
        std::cerr << "Updated-runtime probing must recognize the configured game without accepting unrelated Unity "
                     "processes.\n";
        return 1;
    }
    auto Legacy = Configuration;
    Legacy.PuertsRuntime.reset();
    if (Trainer::RemoteProcess::CanProbeUpdatedRuntime(Legacy, L"BlazblueEntropyEffect.exe"))
    {
        std::cerr << "Legacy unrooted profiles must not bypass build verification.\n";
        return 1;
    }
    const auto Metadata = std::filesystem::temp_directory_path() /
                          (L"BlazblueIdentity-" + std::to_wstring(GetCurrentProcessId()) + L".dat");
    {
        std::ofstream Output(Metadata, std::ios::binary);
        const unsigned char Header[]{0xaf, 0x1b, 0xb1, 0xfa, 31, 0, 0, 0};
        Output.write(reinterpret_cast<const char *>(Header), sizeof(Header));
        constexpr char Markers[] = "STFesActorPack\0STPlayerNewFesActorPack\0EBreedDungeonResType\0Puerts";
        Output.write(Markers, sizeof(Markers));
        if (!Output)
            throw std::runtime_error("Cannot create the synthetic metadata test.");
    }
    if (!Trainer::RemoteProcess::HasGameMetadataIdentity(Metadata))
    {
        std::filesystem::remove(Metadata);
        std::cerr << "Stable metadata identity must not depend on old TypeInfo RVAs.\n";
        return 1;
    }
    std::filesystem::remove(Metadata);
    return 0;
}

int RunAudioProbe()
{
    std::string Error;
    if (!Trainer::PlayHotkeyFeedback(true, Error))
    {
        std::cerr << Error << '\n';
        return 1;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    if (!Trainer::PlayHotkeyFeedback(false, Error))
    {
        std::cerr << Error << '\n';
        return 1;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    std::cout << "Native asynchronous success/failure audio playback accepted; no game writes.\n";
    return 0;
}
