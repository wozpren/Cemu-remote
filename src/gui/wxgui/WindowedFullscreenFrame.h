// SPDX-License-Identifier: MPL-2.0 OR GPL-3.0-only
#pragma once
#include <wx/frame.h>
#include <memory>

// Windows fullscreen keeps an ordinary top-level window and only changes its
// decorations and monitor bounds. Other platforms retain wxWidgets behavior.
class WindowedFullscreenFrame : public wxFrame
{
public:
    WindowedFullscreenFrame(wxWindow* parent, wxWindowID id, const wxString& title,
                            const wxPoint& position = wxDefaultPosition, const wxSize& size = wxDefaultSize,
                            long style = wxDEFAULT_FRAME_STYLE);
    ~WindowedFullscreenFrame() override;
    bool ShowFullScreen(bool show, long style = wxFULLSCREEN_ALL) override;
    bool IsFullScreen() const override;
private:
    struct WindowedState;
    std::unique_ptr<WindowedState> m_windowedState;
};
