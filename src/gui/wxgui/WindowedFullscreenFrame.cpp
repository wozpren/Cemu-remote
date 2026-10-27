// SPDX-License-Identifier: MPL-2.0 OR GPL-3.0-only
#include "WindowedFullscreenFrame.h"

struct WindowedFullscreenFrame::WindowedState
{
#ifdef __WXMSW__
    WINDOWPLACEMENT placement{sizeof(WINDOWPLACEMENT)};
    LONG_PTR nativeStyle{}, nativeExtendedStyle{};
    long wxStyle{};
#endif
};

WindowedFullscreenFrame::WindowedFullscreenFrame(wxWindow* parent, wxWindowID id, const wxString& title,
                                               const wxPoint& position, const wxSize& size, long style)
    : wxFrame(parent, id, title, position, size, style)
{
}
WindowedFullscreenFrame::~WindowedFullscreenFrame() = default;

bool WindowedFullscreenFrame::IsFullScreen() const
{
#ifdef __WXMSW__
    return m_windowedState != nullptr;
#else
    return wxFrame::IsFullScreen();
#endif
}

bool WindowedFullscreenFrame::ShowFullScreen(bool show, long style)
{
#ifdef __WXMSW__
    if (show == IsFullScreen()) return true;
    const auto window = static_cast<HWND>(GetHandle());
    auto setStyle = [window](int index, LONG_PTR value)
    {
        SetLastError(ERROR_SUCCESS);
        return SetWindowLongPtrW(window, index, value) != 0 || GetLastError() == ERROR_SUCCESS;
    };
    auto restore = [&]
    {
        const auto& saved = *m_windowedState;
        SetWindowStyleFlag(saved.wxStyle);
        const bool restoredStyle = setStyle(GWL_STYLE, saved.nativeStyle);
        const bool restoredExtendedStyle = setStyle(GWL_EXSTYLE, saved.nativeExtendedStyle);
        // WINDOWPLACEMENT preserves the normal restore rectangle even when the
        // user entered fullscreen from a maximized window, including other monitors.
        const bool restoredPlacement = SetWindowPlacement(window, &saved.placement) != FALSE;
        const bool refreshed = SetWindowPos(window, nullptr, 0, 0, 0, 0,
            SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOOWNERZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED) != FALSE;
        return restoredStyle && restoredExtendedStyle && restoredPlacement && refreshed;
    };
    if (show)
    {
        auto saved = std::make_unique<WindowedState>();
        MONITORINFO monitor{sizeof(MONITORINFO)};
        if (!GetWindowPlacement(window, &saved->placement) ||
            !GetMonitorInfoW(MonitorFromWindow(window, MONITOR_DEFAULTTONEAREST), &monitor))
        {
            cemuLog_log(LogType::Force, "Cannot enter fullscreen window: Windows error {}", GetLastError());
            return false;
        }
        saved->wxStyle = GetWindowStyleFlag();
        saved->nativeStyle = GetWindowLongPtrW(window, GWL_STYLE);
        saved->nativeExtendedStyle = GetWindowLongPtrW(window, GWL_EXSTYLE);
        // Size/move events during this transition must not overwrite the normal
        // window placement used by the main window and the GamePad window.
        m_windowedState = std::move(saved);
        if (IsMaximized() || IsIconized()) ShowWindow(window, SW_RESTORE);
        constexpr long decorations = wxCAPTION | wxRESIZE_BORDER | wxSYSTEM_MENU |
                                     wxMINIMIZE_BOX | wxMAXIMIZE_BOX | wxCLOSE_BOX | wxBORDER_MASK;
        SetWindowStyleFlag((m_windowedState->wxStyle & ~decorations) | wxBORDER_NONE);
        constexpr LONG_PTR nativeDecorations = WS_POPUP | WS_CAPTION | WS_THICKFRAME |
            WS_SYSMENU | WS_MINIMIZEBOX | WS_MAXIMIZEBOX | WS_MINIMIZE | WS_MAXIMIZE;
        constexpr LONG_PTR extendedBorders = WS_EX_DLGMODALFRAME | WS_EX_WINDOWEDGE | WS_EX_CLIENTEDGE | WS_EX_STATICEDGE;
        const bool styled = setStyle(GWL_STYLE, GetWindowLongPtrW(window, GWL_STYLE) & ~nativeDecorations);
        const bool extendedStyled = setStyle(GWL_EXSTYLE, GetWindowLongPtrW(window, GWL_EXSTYLE) & ~extendedBorders);
        const auto& bounds = monitor.rcMonitor;
        // Preserve Z order: fullscreen must not make the window permanently
        // topmost, and this path never changes the display mode or refresh rate.
        const bool positioned = styled && extendedStyled && SetWindowPos(window, nullptr,
            bounds.left, bounds.top, bounds.right - bounds.left, bounds.bottom - bounds.top,
            SWP_NOZORDER | SWP_NOOWNERZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED) != FALSE;
        if (!positioned)
        {
            const auto error = GetLastError();
            if (restore()) m_windowedState.reset();
            cemuLog_log(LogType::Force, "Cannot enter fullscreen window: Windows error {}", error);
            return false;
        }
        if (!IsShown()) Show();
    }
    else
    {
        if (!restore())
        {
            cemuLog_log(LogType::Force, "Cannot restore window placement: Windows error {}", GetLastError());
            return false;
        }
        m_windowedState.reset();
    }
    Layout();
    SendSizeEvent();
    return true;
#else
    return wxFrame::ShowFullScreen(show, style);
#endif
}
