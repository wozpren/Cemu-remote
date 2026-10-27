// SPDX-License-Identifier: MPL-2.0 OR GPL-3.0-only
#pragma once
#include "../sdk/gamepad_extension.h"
#include <span>
class LatteTextureView;

namespace RemoteGamePad
{
void Initialize();
void Shutdown();
bool Enabled();
bool LocalAudio();
bool Attach(const CgpDevice& device);
void Detach();
void SetGame(bool available);
void UpdateBinding();
void Capture(LatteTextureView* source);
void ReportError(const char* reason);
bool Acquire(CgpFrame& frame);
void Submit(const CgpFrame& frame);
void Cancel(const CgpFrame& frame);
void Audio(std::span<const sint16> samples, uint32 channels, uint64 timestamp);
CgpInput ReadInput(uint32 consumer = 0);
CgpInput ReadControllerInput();
class InputReadScope
{
public:
    explicit InputReadScope(bool remote);
    ~InputReadScope();
private:
    CgpInput m_previous{};
    bool m_hadPrevious{};
};
void SetOverlayFocus(bool focus);
void Rumble(float strength);
uint64 Now();
}
