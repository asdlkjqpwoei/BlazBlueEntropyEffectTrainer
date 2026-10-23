#include "WindowBehavior.h"

#include <algorithm>
#include <stdexcept>
#include <system_error>
#include <unordered_map>
#include <utility>

namespace Trainer
{
    namespace
    {
        constexpr UINT_PTR WindowSubclassId = 0x4b10;
        constexpr int HotkeyIdBase = 0x4b20;
        std::unordered_map<HWINEVENTHOOK, WindowBehavior*> ForegroundListeners;

        std::string WindowError(const std::string& Context)
        {
            return Context + ": " + std::system_category().message(static_cast<int>(GetLastError()));
        }
    }

    ClientSize FitClientSize(
        WindowConfig Preferred, unsigned Dpi, ClientSize WorkArea, ClientSize Frame)
    {
        if (Dpi == 0 || Dpi > 960 || Preferred.Width <= 0 || Preferred.Height <= 0 ||
            WorkArea.Width <= Frame.Width || WorkArea.Height <= Frame.Height)
            throw std::invalid_argument("Invalid DPI or monitor work-area dimensions.");
        const int Margin = MulDiv(12, static_cast<int>(Dpi), 96);
        return {
            std::clamp(MulDiv(Preferred.Width, static_cast<int>(Dpi), 96),
                1, std::max(1, WorkArea.Width - Frame.Width - Margin)),
            std::clamp(MulDiv(Preferred.Height, static_cast<int>(Dpi), 96),
                1, std::max(1, WorkArea.Height - Frame.Height - Margin))
        };
    }

    WindowBehavior::WindowBehavior(
        HWND Window, WindowConfig Preferred,
        std::array<std::optional<HotkeyBinding>, 3> Bindings,
        ActionHandler Action, ErrorHandler Error)
        : NativeWindow(Window), PreferredSize(Preferred), Hotkeys(std::move(Bindings)),
          OnAction(std::move(Action)), OnError(std::move(Error))
    {
        if (!IsWindow(Window))
            throw std::runtime_error("The trainer native window is unavailable.");
        SetLastError(0);
        const LONG_PTR Style = GetWindowLongPtrW(Window, GWL_STYLE) & ~(WS_MAXIMIZEBOX | WS_THICKFRAME);
        if (SetWindowLongPtrW(Window, GWL_STYLE, Style) == 0 && GetLastError() != 0)
            throw std::runtime_error(WindowError("Cannot disable window resizing"));
        if (!SetWindowSubclass(Window, WindowMessage, WindowSubclassId, reinterpret_cast<DWORD_PTR>(this)))
            throw std::runtime_error(WindowError("Cannot attach trainer window behavior"));
        ForegroundHook = SetWinEventHook(
            EVENT_SYSTEM_FOREGROUND, EVENT_SYSTEM_FOREGROUND, nullptr, ForegroundChanged,
            0, 0, WINEVENT_OUTOFCONTEXT);
        if (!ForegroundHook)
        {
            RemoveWindowSubclass(Window, WindowMessage, WindowSubclassId);
            throw std::runtime_error(WindowError("Cannot monitor foreground changes"));
        }
        ForegroundListeners.emplace(ForegroundHook, this);
        if (const HMENU Menu = GetSystemMenu(Window, FALSE))
        {
            EnableMenuItem(Menu, SC_MAXIMIZE, MF_BYCOMMAND | MF_GRAYED);
            EnableMenuItem(Menu, SC_SIZE, MF_BYCOMMAND | MF_GRAYED);
        }
        SetWindowPos(Window, nullptr, 0, 0, 0, 0,
            SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
        FitToMonitor(true);
    }

    WindowBehavior::~WindowBehavior()
    {
        ReleaseRegistrations();
        if (ForegroundHook)
        {
            ForegroundListeners.erase(ForegroundHook);
            UnhookWinEvent(ForegroundHook);
        }
        if (IsWindow(NativeWindow))
            RemoveWindowSubclass(NativeWindow, WindowMessage, WindowSubclassId);
    }

    void WindowBehavior::FitToMonitor(bool Center)
    {
        if (IsAdjusting || !IsWindow(NativeWindow) || IsIconic(NativeWindow))
            return;
        MONITORINFO Monitor{ sizeof(MONITORINFO) };
        const UINT Dpi = GetDpiForWindow(NativeWindow);
        RECT Frame{};
        if (!GetMonitorInfoW(MonitorFromWindow(NativeWindow, MONITOR_DEFAULTTONEAREST), &Monitor) ||
            !AdjustWindowRectExForDpi(&Frame,
                static_cast<DWORD>(GetWindowLongPtrW(NativeWindow, GWL_STYLE)), FALSE,
                static_cast<DWORD>(GetWindowLongPtrW(NativeWindow, GWL_EXSTYLE)), Dpi))
        {
            OnError(WindowError("Cannot calculate the DPI-aware window size"));
            return;
        }
        const ClientSize Border{ Frame.right - Frame.left, Frame.bottom - Frame.top };
        const ClientSize WorkArea{
            Monitor.rcWork.right - Monitor.rcWork.left,
            Monitor.rcWork.bottom - Monitor.rcWork.top
        };
        if (Dpi == 0 || Dpi > 960 || WorkArea.Width <= Border.Width || WorkArea.Height <= Border.Height)
        {
            OnError("The current display has no usable client area.");
            return;
        }
        const auto Client = FitClientSize(PreferredSize, Dpi, WorkArea, Border);
        RECT Current{};
        if (!GetWindowRect(NativeWindow, &Current))
        {
            OnError(WindowError("Cannot read the trainer window position"));
            return;
        }
        const int Width = Client.Width + Border.Width;
        const int Height = Client.Height + Border.Height;
        const int Left = Center ? Monitor.rcWork.left + (WorkArea.Width - Width) / 2 :
            std::clamp(static_cast<int>(Current.left), static_cast<int>(Monitor.rcWork.left),
                static_cast<int>(Monitor.rcWork.right) - Width);
        const int Top = Center ? Monitor.rcWork.top + (WorkArea.Height - Height) / 2 :
            std::clamp(static_cast<int>(Current.top), static_cast<int>(Monitor.rcWork.top),
                static_cast<int>(Monitor.rcWork.bottom) - Height);
        IsAdjusting = true;
        const bool Resized = SetWindowPos(
            NativeWindow, nullptr, Left, Top, Width, Height, SWP_NOZORDER | SWP_NOACTIVATE) != FALSE;
        IsAdjusting = false;
        if (!Resized)
            OnError(WindowError("Cannot resize the trainer for the current display"));
    }

    void WindowBehavior::SetGameState(std::uint32_t ProcessId, std::array<bool, 3> Enabled)
    {
        if (GameProcessId == ProcessId && EnabledFeatures == Enabled)
            return;
        GameProcessId = ProcessId;
        EnabledFeatures = Enabled;
        RegistrationFailed.fill(false);
        UpdateRegistrations();
    }

    bool WindowBehavior::CanInvoke(std::size_t Index) const
    {
        DWORD ForegroundProcessId = 0;
        GetWindowThreadProcessId(GetForegroundWindow(), &ForegroundProcessId);
        return Index < EnabledFeatures.size() &&
            IsHotkeyContextAllowed(GameProcessId, ForegroundProcessId, EnabledFeatures[Index]);
    }

    void WindowBehavior::UpdateRegistrations()
    {
        for (std::size_t Index = 0; Index < Hotkeys.size(); ++Index)
        {
            const bool Desired = Hotkeys[Index] && CanInvoke(Index);
            if (!Desired)
            {
                if (Registered[Index])
                    UnregisterHotKey(NativeWindow, HotkeyIdBase + static_cast<int>(Index));
                Registered[Index] = false;
                RegistrationFailed[Index] = false;
            }
            else if (!Registered[Index] && !RegistrationFailed[Index])
            {
                const auto& Binding = *Hotkeys[Index];
                Registered[Index] = RegisterHotKey(NativeWindow,
                    HotkeyIdBase + static_cast<int>(Index),
                    Binding.Modifiers | MOD_NOREPEAT, Binding.VirtualKey) != FALSE;
                RegistrationFailed[Index] = !Registered[Index];
                if (RegistrationFailed[Index])
                    OnError(WindowError("Cannot register hotkey " + Binding.Display));
            }
        }
    }

    void WindowBehavior::ReleaseRegistrations()
    {
        for (std::size_t Index = 0; Index < Registered.size(); ++Index)
        {
            if (Registered[Index])
                UnregisterHotKey(NativeWindow, HotkeyIdBase + static_cast<int>(Index));
            Registered[Index] = false;
        }
    }

    void CALLBACK WindowBehavior::ForegroundChanged(
        HWINEVENTHOOK Hook, DWORD, HWND, LONG, LONG, DWORD, DWORD)
    {
        const auto Found = ForegroundListeners.find(Hook);
        if (Found != ForegroundListeners.end())
            Found->second->UpdateRegistrations();
    }

    LRESULT CALLBACK WindowBehavior::WindowMessage(
        HWND Window, UINT Message, WPARAM Word, LPARAM Long, UINT_PTR, DWORD_PTR Context)
    {
        auto& Behavior = *reinterpret_cast<WindowBehavior*>(Context);
        if (Message == WM_SYSCOMMAND && ((Word & 0xfff0) == SC_MAXIMIZE || (Word & 0xfff0) == SC_SIZE))
            return 0;
        if (Message == WM_HOTKEY && Word >= HotkeyIdBase && Word < HotkeyIdBase + 3)
        {
            const auto Index = static_cast<std::size_t>(Word - HotkeyIdBase);
            if (Behavior.Registered[Index] && Behavior.CanInvoke(Index) && Behavior.Hotkeys[Index] &&
                (static_cast<std::uint32_t>(LOWORD(Long)) & (MOD_ALT | MOD_CONTROL | MOD_SHIFT | MOD_WIN)) ==
                    Behavior.Hotkeys[Index]->Modifiers &&
                HIWORD(Long) == Behavior.Hotkeys[Index]->VirtualKey)
                Behavior.OnAction(Behavior.Hotkeys[Index]->Feature);
            return 0;
        }
        const auto Result = DefSubclassProc(Window, Message, Word, Long);
        if (Message == WM_DPICHANGED || Message == WM_DISPLAYCHANGE ||
            (Message == WM_SETTINGCHANGE && Word == SPI_SETWORKAREA) ||
            (Message == WM_SIZE && Word != SIZE_MINIMIZED))
            Behavior.FitToMonitor();
        return Result;
    }
}
