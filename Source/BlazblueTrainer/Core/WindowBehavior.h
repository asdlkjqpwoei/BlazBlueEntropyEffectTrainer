#pragma once

#include "Config.h"

#include <Windows.h>
#include <commctrl.h>

#include <array>
#include <functional>
#include <string>

namespace Trainer
{
    struct ClientSize
    {
        int Width{};
        int Height{};
        bool operator==(const ClientSize&) const = default;
    };

    ClientSize FitClientSize(
        WindowConfig Preferred, unsigned Dpi, ClientSize WorkArea, ClientSize Frame);

    class WindowBehavior final
    {
    public:
        using ActionHandler = std::function<void(TrainerFeature)>;
        using ErrorHandler = std::function<void(const std::string&)>;

        WindowBehavior(
            HWND Window, WindowConfig Preferred,
            std::array<std::optional<HotkeyBinding>, 3> Bindings,
            ActionHandler OnAction, ErrorHandler OnError);
        WindowBehavior(const WindowBehavior&) = delete;
        WindowBehavior& operator=(const WindowBehavior&) = delete;
        ~WindowBehavior();

        void SetGameState(std::uint32_t ProcessId, std::array<bool, 3> Enabled);
        void FitToMonitor(bool Center = false);

    private:
        static LRESULT CALLBACK WindowMessage(
            HWND Window, UINT Message, WPARAM Word, LPARAM Long, UINT_PTR Id, DWORD_PTR Context);
        static void CALLBACK ForegroundChanged(
            HWINEVENTHOOK Hook, DWORD Event, HWND Window, LONG Object, LONG Child, DWORD Thread, DWORD Time);
        void UpdateRegistrations();
        void ReleaseRegistrations();
        bool CanInvoke(std::size_t Index) const;

        HWND NativeWindow{};
        HWINEVENTHOOK ForegroundHook{};
        WindowConfig PreferredSize;
        std::array<std::optional<HotkeyBinding>, 3> Hotkeys;
        std::array<bool, 3> Registered{};
        std::array<bool, 3> RegistrationFailed{};
        std::array<bool, 3> EnabledFeatures{};
        std::uint32_t GameProcessId{};
        bool IsAdjusting{};
        ActionHandler OnAction;
        ErrorHandler OnError;
    };
}
