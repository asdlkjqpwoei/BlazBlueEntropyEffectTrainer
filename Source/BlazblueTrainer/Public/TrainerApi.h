#pragma once

#include <cstddef>
#include <cstdint>

#if defined(TRAINER_SHARED) && defined(TRAINER_CORE_EXPORTS)
#define TRAINER_API __declspec(dllexport)
#elif defined(TRAINER_SHARED)
#define TRAINER_API __declspec(dllimport)
#else
#define TRAINER_API
#endif

struct TrainerEngine;

enum TrainerLanguage : std::uint32_t
{
    TrainerLanguageChinese = 0,
    TrainerLanguageEnglish = 1
};

enum TrainerFeature : std::uint32_t
{
    TrainerFeatureRefresh = 1,
    TrainerFeatureExchange = 2,
    TrainerFeatureAp = 3
};

enum TrainerOperation : std::uint32_t
{
    TrainerOperationSet = 1,
    TrainerOperationAdd = 2
};

enum TrainerState : std::uint32_t
{
    TrainerStateStopped = 0,
    TrainerStateWaiting = 1,
    TrainerStateChecking = 2,
    TrainerStateScanning = 3,
    TrainerStateReady = 4,
    TrainerStateWrongCopy = 5,
    TrainerStateUnsupportedBuild = 6,
    TrainerStateAccessDenied = 7,
    TrainerStateError = 8
};

struct TrainerFeatureSnapshot
{
    std::uint32_t AddressCount;
    std::uint32_t WriteEnabled;
    std::int64_t Minimum;
    std::int64_t Maximum;
};

struct TrainerSnapshot
{
    std::uint32_t State;
    std::uint32_t ProcessDetected;
    std::uint32_t BuildVerified;
    std::uint32_t ScanInProgress;
    std::uint32_t ProcessId;
    TrainerFeatureSnapshot Refresh;
    TrainerFeatureSnapshot Exchange;
    TrainerFeatureSnapshot Ap;
    char StatusMessage[512];
    char DetailMessage[512];
};

struct TrainerPotentialResult
{
    std::uint32_t ActorId;
    std::uint32_t Added;
    std::uint32_t Total;
    std::uint32_t ExecutionThreadId;
};

extern "C"
{
    TRAINER_API TrainerEngine* __cdecl TrainerCreate(
        const wchar_t* ConfigPath,
        char* ErrorBuffer,
        std::size_t ErrorBufferSize);

    TRAINER_API void __cdecl TrainerDestroy(TrainerEngine* Engine);

    TRAINER_API std::int32_t __cdecl TrainerStart(
        TrainerEngine* Engine,
        char* ErrorBuffer,
        std::size_t ErrorBufferSize);

    TRAINER_API void __cdecl TrainerStop(TrainerEngine* Engine);

    TRAINER_API std::int32_t __cdecl TrainerSetLanguage(
        TrainerEngine* Engine, TrainerLanguage Language,
        char* ErrorBuffer, std::size_t ErrorBufferSize);

    TRAINER_API std::int32_t __cdecl TrainerGetSnapshot(
        TrainerEngine* Engine,
        TrainerSnapshot* Snapshot,
        char* ErrorBuffer,
        std::size_t ErrorBufferSize);

    TRAINER_API std::int32_t __cdecl TrainerApply(
        TrainerEngine* Engine,
        TrainerFeature Feature,
        TrainerOperation Operation,
        std::int64_t Value,
        std::int32_t* Writes,
        char* ErrorBuffer,
        std::size_t ErrorBufferSize);

    TRAINER_API std::int32_t __cdecl TrainerGrantAllPotentials(
        TrainerEngine* Engine,
        TrainerPotentialResult* Result,
        char* ErrorBuffer,
        std::size_t ErrorBufferSize);

    TRAINER_API std::int32_t __cdecl TrainerGetText(
        TrainerEngine* Engine,
        const char* Key,
        char* Output,
        std::size_t OutputSize);

    TRAINER_API std::int64_t __cdecl TrainerGetDefault(
        TrainerEngine* Engine,
        TrainerFeature Feature,
        TrainerOperation Operation);
}
