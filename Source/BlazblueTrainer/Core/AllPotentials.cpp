#include "AllPotentials.h"
#include "Paths.h"
#include "PotentialProtocol.h"

#include <bcrypt.h>
#include <yaml-cpp/yaml.h>

#include <array>
#include <cstring>
#include <memory>
#include <mutex>
#include <new>
#include <system_error>

namespace Trainer
{
    namespace
    {
        std::mutex GrantMutex;

        struct HandleCloser
        {
            void operator()(void* Handle) const { CloseHandle(Handle); }
        };
        struct ViewCloser
        {
            void operator()(PotentialCommand* View) const { UnmapViewOfFile(View); }
        };
        using Handle = std::unique_ptr<void, HandleCloser>;
        using CommandView = std::unique_ptr<PotentialCommand, ViewCloser>;

        std::string WindowsError(const char* Operation, DWORD Code)
        {
            return std::string("Potential bridge: ") + Operation +
                " failed with Windows error " + std::to_string(Code) + ".";
        }

        struct GameWindow
        {
            DWORD ProcessId{};
            HWND Window{};
            bool Ambiguous{};
        };

        BOOL CALLBACK FindGameWindow(HWND Window, LPARAM Parameter)
        {
            auto& Target = *reinterpret_cast<GameWindow*>(Parameter);
            DWORD ProcessId{};
            GetWindowThreadProcessId(Window, &ProcessId);
            if (ProcessId != Target.ProcessId || !IsWindowVisible(Window) ||
                GetWindow(Window, GW_OWNER))
                return TRUE;
            wchar_t ClassName[128]{};
            if (!GetClassNameW(Window, ClassName, static_cast<int>(std::size(ClassName))) ||
                std::wcscmp(ClassName, L"UnityWndClass") != 0)
                return TRUE;
            if (Target.Window)
                Target.Ambiguous = true;
            else
                Target.Window = Window;
            return TRUE;
        }
    }

    bool ParsePotentialResult(
        std::string_view Response, TrainerPotentialResult& Result, std::string& Error)
    {
        Result = {};
        Error.clear();
        try
        {
            const auto Node = YAML::Load(std::string(Response));
            if (!Node.IsMap() || !Node["ok"].IsScalar() ||
                (Node["ok"].Scalar() != "true" && Node["ok"].Scalar() != "false"))
                throw std::runtime_error("Invalid potential result.");
            const auto Added = Node["added"].as<std::uint32_t>();
            if (Added > 4096)
                throw std::runtime_error("Invalid potential count.");
            if (!Node["ok"].as<bool>())
            {
                Error = Node["error"].as<std::string>();
                if (Error.empty())
                    throw std::runtime_error("Missing potential error.");
                Result.Added = Added;
                return false;
            }
            const auto ActorId = Node["actorId"].as<std::uint32_t>();
            const auto Total = Node["total"].as<std::uint32_t>();
            if (!ActorId || !Total || Total > 4096 || Added > Total)
                throw std::runtime_error("Invalid potential result bounds.");
            Result.ActorId = ActorId;
            Result.Added = Added;
            Result.Total = Total;
            return true;
        }
        catch (const YAML::Exception&)
        {
            Error = "The game returned an invalid potential result.";
        }
        catch (const std::runtime_error&)
        {
            Error = "The game returned an invalid potential result.";
        }
        Result = {};
        return false;
    }

    bool RequestAllPotentials(
        DWORD ProcessId, TrainerPotentialResult& Result, std::string& Error, std::stop_token StopToken)
    {
        Result = {};
        Error.clear();
        std::unique_lock Lock(GrantMutex, std::try_to_lock);
        if (!Lock.owns_lock())
        {
            Error = "A potential grant is already in progress.";
            return false;
        }
        if (!ProcessId || StopToken.stop_requested())
        {
            Error = "The potential request was cancelled before execution.";
            return false;
        }
        GameWindow Target{ProcessId};
        if (!EnumWindows(FindGameWindow, reinterpret_cast<LPARAM>(&Target)))
        {
            Error = WindowsError("EnumWindows", GetLastError());
            return false;
        }
        if (!Target.Window || Target.Ambiguous)
        {
            Error = "A unique active game window could not be verified.";
            return false;
        }
        DWORD WindowProcessId{};
        const auto ThreadId = GetWindowThreadProcessId(Target.Window, &WindowProcessId);
        if (!ThreadId || WindowProcessId != ProcessId)
        {
            Error = "A unique active game window could not be verified.";
            return false;
        }
        const auto Message = RegisterWindowMessageW(PotentialMessageName);
        if (!Message)
        {
            Error = WindowsError("RegisterWindowMessage", GetLastError());
            return false;
        }
        std::uint64_t Nonce{};
        if (BCryptGenRandom(nullptr, reinterpret_cast<PUCHAR>(&Nonce), sizeof(Nonce),
            BCRYPT_USE_SYSTEM_PREFERRED_RNG) < 0 || !Nonce)
        {
            Error = "Cannot create a unique potential request.";
            return false;
        }
        const auto Requester = GetCurrentProcessId();
        const auto Name = PotentialMappingName(Requester, Nonce);
        Handle Mapping(CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE,
            0, sizeof(PotentialCommand), Name.c_str()));
        const auto MappingError = GetLastError();
        if (!Mapping || MappingError == ERROR_ALREADY_EXISTS)
        {
            Error = WindowsError("CreateFileMapping", MappingError);
            return false;
        }
        CommandView Command(static_cast<PotentialCommand*>(
            MapViewOfFile(Mapping.get(), FILE_MAP_READ | FILE_MAP_WRITE, 0, 0, sizeof(PotentialCommand))));
        if (!Command)
        {
            Error = WindowsError("MapViewOfFile", GetLastError());
            return false;
        }
        new (Command.get()) PotentialCommand;
        Command->RequesterProcessId = Requester;
        Command->TargetProcessId = ProcessId;
        Command->Nonce = Nonce;
        const auto BridgePath = ExecutableDirectory() / L"BlazbluePotentialBridge.dll";
        HMODULE Module = LoadLibraryExW(BridgePath.c_str(), nullptr,
            LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
        if (!Module)
        {
            Error = WindowsError("LoadLibrary", GetLastError());
            return false;
        }
        const auto Callback = reinterpret_cast<HOOKPROC>(GetProcAddress(Module, "TrainerPotentialHook"));
        if (!Callback)
        {
            const auto Code = GetLastError();
            FreeLibrary(Module);
            Error = WindowsError("GetProcAddress", Code);
            return false;
        }
        const auto Hook = SetWindowsHookExW(WH_CALLWNDPROC, Callback, Module, ThreadId);
        if (!Hook)
        {
            const auto Code = GetLastError();
            FreeLibrary(Module);
            Error = WindowsError("SetWindowsHookEx", Code);
            return false;
        }
        std::stop_callback Cancel(StopToken, [&]
        {
            InterlockedCompareExchange(&Command->State, static_cast<LONG>(PotentialCommandState::Cancelled),
                static_cast<LONG>(PotentialCommandState::Pending));
        });
        DWORD_PTR MessageResult{};
        SetLastError(ERROR_SUCCESS);
        const bool Sent = SendMessageTimeoutW(Target.Window, Message, Requester, static_cast<LPARAM>(Nonce),
            SMTO_ABORTIFHUNG | SMTO_BLOCK | SMTO_ERRORONEXIT, PotentialRequestTimeoutMs, &MessageResult) != 0;
        const auto SendError = GetLastError();
        const auto State = static_cast<PotentialCommandState>(InterlockedCompareExchange(
            &Command->State, static_cast<LONG>(PotentialCommandState::Cancelled),
            static_cast<LONG>(PotentialCommandState::Pending)));
        if (!UnhookWindowsHookEx(Hook))
        {
            // A failed detach must not unload code which Windows may still call.
            Error = WindowsError("UnhookWindowsHookEx", GetLastError());
            return false;
        }
        FreeLibrary(Module);
        Result.ExecutionThreadId = Command->ExecutionThreadId;
        if (State == PotentialCommandState::Completed)
        {
            if (std::memchr(Command->Response, '\0', sizeof(Command->Response)) == nullptr)
            {
                Error = "The game returned an invalid potential result.";
                return false;
            }
            const bool Applied = ParsePotentialResult(Command->Response, Result, Error);
            Result.ExecutionThreadId = Command->ExecutionThreadId;
            return Applied;
        }
        if (State == PotentialCommandState::Failed)
        {
            Error = std::memchr(Command->Response, '\0', sizeof(Command->Response))
                ? Command->Response : "The game returned an invalid potential result.";
            if (Error.empty())
                Error = "The game returned an invalid potential result.";
        }
        else if (State == PotentialCommandState::Executing)
            Error = "The potential request timed out during execution; changes may already have applied. Do not retry immediately.";
        else if (State == PotentialCommandState::Cancelled)
            Error = "The potential request was cancelled before execution.";
        else if (!Sent)
            Error = WindowsError("SendMessageTimeout", SendError);
        else
            Error = "The game did not execute the potential request.";
        return false;
    }
}
