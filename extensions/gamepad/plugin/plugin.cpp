// SPDX-License-Identifier: GPL-3.0-only
#include "runtime.h"
#include <cstring>

namespace
{
template<class F> CgpResult Guard(CgpInstance* instance, F&& action) noexcept
{
    if (!instance || !instance->runtime) return CGP_ERROR;
    try { action(*instance->runtime); return CGP_OK; }
    catch (const std::exception& ex) { instance->runtime->Error(ex.what()); return CGP_ERROR; }
    catch (...) { instance->runtime->Error("Unknown exception inside GamePad extension"); return CGP_ERROR; }
}
CgpResult CGP_CALL Create(const CgpHost* host, const char* config, CgpInstance** result)
{
    if (!host || host->size < sizeof(CgpHost) || host->major != CGP_ABI_MAJOR || !host->log || !host->now_ns || !config || !result)
        return CGP_INCOMPATIBLE;
    *result = nullptr;
    try
    {
        auto instance = std::make_unique<CgpInstance>();
        instance->runtime = std::make_unique<cgp::Runtime>(*host, std::filesystem::u8path(config));
        if (instance->runtime->enabled) instance->runtime->StartHost();
        *result = instance.release();
        return CGP_OK;
    }
    catch (const std::exception& ex) { host->log(host->user, error, ex.what()); return CGP_ERROR; }
    catch (...) { host->log(host->user, error, "Could not create GamePad extension"); return CGP_ERROR; }
}
void CGP_CALL Destroy(CgpInstance* instance) { delete instance; }
CgpResult CGP_CALL Attach(CgpInstance* instance, const CgpDevice* device)
{
    if (!device || device->size < sizeof(CgpDevice)) return CGP_INCOMPATIBLE;
    return Guard(instance, [&](auto& runtime) { runtime.Attach(*device); });
}
void CGP_CALL Detach(CgpInstance* instance) { Guard(instance, [](auto& runtime) { runtime.Detach(); }); }
void CGP_CALL Game(CgpInstance* instance, uint32_t available) { Guard(instance, [&](auto& runtime) { runtime.SetGame(available != 0); }); }
CgpResult CGP_CALL Acquire(CgpInstance* instance, CgpFrame* frame)
{
    if (!frame || frame->size < sizeof(CgpFrame)) return CGP_INCOMPATIBLE;
    bool acquired{};
    auto result = Guard(instance, [&](auto& runtime) { acquired = runtime.Acquire(*frame); });
    return result == CGP_OK && !acquired ? CGP_IDLE : result;
}
CgpResult CGP_CALL Submit(CgpInstance* instance, const CgpFrame* frame)
{
    if (!frame || frame->size < sizeof(CgpFrame)) return CGP_INCOMPATIBLE;
    return Guard(instance, [&](auto& runtime) { runtime.Submit(*frame); });
}
void CGP_CALL Cancel(CgpInstance* instance, const CgpFrame* frame)
{
    if (frame && frame->size >= sizeof(CgpFrame)) Guard(instance, [&](auto& runtime) { runtime.Cancel(*frame); });
}
CgpResult CGP_CALL Audio(CgpInstance* instance, const CgpAudio* audio)
{
    if (!audio || audio->size < sizeof(CgpAudio)) return CGP_INCOMPATIBLE;
    bool accepted{};
    auto result = Guard(instance, [&](auto& runtime) { accepted = runtime.Audio(*audio); });
    return result == CGP_OK && !accepted ? CGP_IDLE : result;
}
CgpResult CGP_CALL Input(CgpInstance* instance, CgpInput* state, uint32_t consumer)
{
    if (!state || state->size < sizeof(CgpInput)) return CGP_INCOMPATIBLE;
    const auto result = Guard(instance, [&](auto& runtime) { *state = runtime.ReadInput(consumer); });
    return result == CGP_OK && consumer == CGP_INPUT_MOTION && !state->connected ? CGP_IDLE : result;
}
void CGP_CALL InputOwner(CgpInstance* instance, uint32_t bound)
{
    Guard(instance, [&](auto& runtime)
    {
        runtime.inputOwned = bound != 0;
        if (!bound) { std::lock_guard lock(runtime.inputMutex); runtime.edges.clear(); runtime.motionSamples.Clear(); }
    });
}
void CGP_CALL OverlayFocus(CgpInstance* instance, uint32_t focus)
{
    Guard(instance, [&](auto& runtime) { runtime.overlayFocus = focus != 0; });
}
CgpResult CGP_CALL Rumble(CgpInstance* instance, float strength)
{
    return Guard(instance, [&](auto& runtime) { runtime.Rumble(strength); });
}
const char* CGP_CALL LastError(CgpInstance* instance)
{
    thread_local std::string message;
    if (!instance || !instance->runtime) return "GamePad extension is not loaded";
    std::lock_guard lock(instance->runtime->errorMutex);
    message = instance->runtime->errorMessage;
    return message.c_str();
}
}

extern "C" CGP_EXPORT CgpResult CGP_CALL CemuGamePad_GetApi(uint32_t major, uint32_t size, CgpApi* api)
{
    if (!api || major != CGP_ABI_MAJOR || size < sizeof(CgpApi)) return CGP_INCOMPATIBLE;
    *api = {sizeof(CgpApi), CGP_ABI_MAJOR, CGP_ABI_MINOR, Create, Destroy, Attach, Detach, Game,
            Acquire, Submit, Cancel, Audio, Input, InputOwner, OverlayFocus, Rumble, LastError};
    return CGP_OK;
}
