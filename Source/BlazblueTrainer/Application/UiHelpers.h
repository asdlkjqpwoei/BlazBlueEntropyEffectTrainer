#pragma once

#include <Windows.h>
#include <winrt/base.h>
#include <winrt/Microsoft.UI.Dispatching.h>

#include <coroutine>
#include <string_view>

// WinUI may use an MTA; returning to that apartment does not restore the XAML UI thread.
struct ResumeOnUi
{
    winrt::Microsoft::UI::Dispatching::DispatcherQueue Queue;
    bool Cancelled{};

    bool await_ready() const
    {
        return Queue.HasThreadAccess();
    }
    bool await_suspend(std::coroutine_handle<> Continuation)
    {
        const auto Dispatcher = Queue;
        if (!Dispatcher.TryEnqueue([Continuation] { Continuation.resume(); }))
        {
            Cancelled = true;
            return false;
        }
        return true;
    }
    bool await_resume() const
    {
        return !Cancelled;
    }
};

inline winrt::hstring Utf8ToHString(std::string_view Text)
{
    if (Text.empty())
        return {};
    const int Required =
        MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, Text.data(), static_cast<int>(Text.size()), nullptr, 0);
    if (Required <= 0)
        throw std::runtime_error("Invalid UTF-8 interface text.");
    std::wstring Wide(static_cast<std::size_t>(Required), L'\0');
    if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, Text.data(), static_cast<int>(Text.size()), Wide.data(),
                            Required) != Required)
        throw std::runtime_error("Could not convert the interface text.");
    return winrt::hstring(Wide);
}
