#include "AllPotentials.h"
#include "Config.h"
#include "Paths.h"
#include "PotentialProtocol.h"

#include <array>
#include <chrono>
#include <future>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>

namespace
{
    class GameWindowFixture
    {
    public:
        GameWindowFixture()
        {
            auto Ready = Started.get_future();
            Thread = std::jthread([this]
            {
                try
                {
                    WNDCLASSW Class{};
                    Class.lpfnWndProc = DefWindowProcW;
                    Class.hInstance = GetModuleHandleW(nullptr);
                    Class.lpszClassName = L"UnityWndClass";
                    if (!RegisterClassW(&Class))
                        throw std::runtime_error("Cannot register the isolated potential-test window.");
                    Window = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
                        Class.lpszClassName, L"Potential tests", WS_POPUP | WS_VISIBLE,
                        -32000, -32000, 1, 1, nullptr, nullptr, Class.hInstance, nullptr);
                    if (!Window)
                        throw std::runtime_error("Cannot create the isolated potential-test window.");
                    ThreadId = GetCurrentThreadId();
                    Started.set_value();
                    MSG Message{};
                    int Status{};
                    while ((Status = GetMessageW(&Message, nullptr, 0, 0)) > 0)
                    {
                        TranslateMessage(&Message);
                        DispatchMessageW(&Message);
                    }
                    if (Status < 0)
                        ThreadError = "The potential-test message loop failed.";
                    DestroyWindow(Window);
                    UnregisterClassW(Class.lpszClassName, Class.hInstance);
                }
                catch (const std::exception&)
                {
                    Started.set_exception(std::current_exception());
                }
            });
            Ready.get();
        }

        ~GameWindowFixture()
        {
            PostThreadMessageW(ThreadId, WM_QUIT, 0, 0);
            Thread.join();
        }

        HWND Window{};
        DWORD ThreadId{};
        std::string ThreadError;

    private:
        std::promise<void> Started;
        std::jthread Thread;
    };
}

int RunPotentialTests()
{
    int Failures{};
    const auto Require = [&](bool Condition, const char* Message)
    {
        if (!Condition)
        {
            std::cerr << Message << '\n';
            ++Failures;
        }
    };
    TrainerPotentialResult Result{};
    std::string Error;
    Require(Trainer::ParsePotentialResult(
        R"({"ok":true,"actorId":37,"added":2,"total":8})", Result, Error) &&
        Result.ActorId == 37 && Result.Added == 2 && Result.Total == 8 && Error.empty(),
        "A valid potential result must preserve its exact counts.");
    Require(Trainer::ParsePotentialResult(
        R"({"ok":true,"actorId":37,"added":0,"total":8})", Result, Error) && Result.Added == 0,
        "Already-owned potentials must be represented as a successful no-op.");
    Require(!Trainer::ParsePotentialResult(
        R"({"ok":false,"added":1,"error":"partial grant"})", Result, Error) &&
        Result.Added == 1 && Error == "partial grant",
        "Partial potential failures must remain failures and retain the known changed count.");
    for (const auto Invalid : {
        "", "{}", "[]", "true", R"({"ok":true})",
        R"({"ok":true,"actorId":0,"added":1,"total":8})",
        R"({"ok":true,"actorId":-1,"added":1,"total":8})",
        R"({"ok":true,"actorId":37,"added":9,"total":8})",
        R"({"ok":true,"actorId":37,"added":-1,"total":8})",
        R"({"ok":true,"actorId":37,"added":1,"total":4097})",
        R"({"ok":false,"added":0,"error":""})",
        R"({"ok":false,"error":"missing count"})"})
    {
        Require(!Trainer::ParsePotentialResult(Invalid, Result, Error) &&
            Result.ActorId == 0 && Result.Added == 0 && Result.Total == 0 && !Error.empty(),
            "Malformed potential results must never look successful.");
    }

    Trainer::PotentialCommand Command;
    Command.RequesterProcessId = 12;
    Command.TargetProcessId = 34;
    Command.Nonce = 56;
    Require(Trainer::ValidPotentialCommand(Command, 12, 34, 56),
        "The exact potential request identity must be accepted.");
    Require(!Trainer::ValidPotentialCommand(Command, 13, 34, 56) &&
        !Trainer::ValidPotentialCommand(Command, 12, 35, 56) &&
        !Trainer::ValidPotentialCommand(Command, 12, 34, 57),
        "A request from another process or generation must be rejected.");
    ++Command.Version;
    Require(!Trainer::ValidPotentialCommand(Command, 12, 34, 56),
        "An incompatible potential bridge protocol must be rejected.");
    --Command.Version;
    --Command.Size;
    Require(!Trainer::ValidPotentialCommand(Command, 12, 34, 56),
        "A truncated potential request must be rejected.");
    const auto Name = Trainer::PotentialMappingName(12, 56);
    Require(Name != Trainer::PotentialMappingName(13, 56) &&
        Name != Trainer::PotentialMappingName(12, 57),
        "Potential request mappings must not collide across callers or generations.");
    std::stop_source Cancel;
    Cancel.request_stop();
    Require(!Trainer::RequestAllPotentials(GetCurrentProcessId(), Result, Error, Cancel.get_token()) &&
        Error == "The potential request was cancelled before execution.",
        "Cancellation must be observed before installing a hook or invoking game code.");

    std::array<char, 1024> Message{};
    TrainerEngine* Engine = TrainerCreate(Trainer::DefaultConfigPath().c_str(), Message.data(), Message.size());
    if (!Engine)
        throw std::runtime_error(Message.data());
    TrainerSetLanguage(Engine, TrainerLanguageEnglish, Message.data(), Message.size());
    Require(!TrainerGrantAllPotentials(Engine, &Result, Message.data(), Message.size()) &&
        Result.Added == 0 && std::string(Message.data()) == "A compatible game process is not connected.",
        "The public all-potentials API must block a disconnected engine.");
    Require(!TrainerGrantAllPotentials(Engine, nullptr, Message.data(), Message.size()) &&
        std::string(Message.data()) == "A trainer engine and potential-result destination are required.",
        "Missing potential-result destinations must fail explicitly.");
    TrainerDestroy(Engine);
    Result.Added = 99;
    Require(!TrainerGrantAllPotentials(nullptr, &Result, Message.data(), Message.size()) && Result.Added == 0,
        "A missing engine must not retain an earlier successful potential count.");

    {
        GameWindowFixture Target;
        const auto Started = std::chrono::steady_clock::now();
        Require(!Trainer::RequestAllPotentials(GetCurrentProcessId(), Result, Error) &&
            Error == "The game scripting runtime is not available.",
            "The native hook must report an absent runtime without trying to modify a process.");
        Require(Result.ExecutionThreadId == Target.ThreadId &&
            Result.ExecutionThreadId != GetCurrentThreadId(),
            "The bridge must execute on the target window thread, never the calling worker.");
        Require(std::chrono::steady_clock::now() - Started < std::chrono::seconds(4),
            "A completed native request must return without waiting for the timeout.");
        HWND Second = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
            L"UnityWndClass", L"Ambiguous potential tests", WS_POPUP | WS_VISIBLE,
            -32000, -32000, 1, 1, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
        if (!Second)
            throw std::runtime_error("Cannot create the ambiguous potential-test window.");
        Require(!Trainer::RequestAllPotentials(GetCurrentProcessId(), Result, Error) &&
            Result.ExecutionThreadId == 0 && Error == "A unique active game window could not be verified.",
            "Ambiguous game windows must be blocked before dispatch.");
        DestroyWindow(Second);
    }
    Require(!Trainer::RequestAllPotentials(GetCurrentProcessId(), Result, Error) &&
        Error == "A unique active game window could not be verified.",
        "Closing the target must invalidate subsequent requests.");
    return Failures != 0;
}
