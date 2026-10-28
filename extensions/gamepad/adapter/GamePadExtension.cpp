// SPDX-License-Identifier: MPL-2.0 OR GPL-3.0-only
#include "GamePadExtension.h"
#include "config/ActiveSettings.h"
#include "input/InputManager.h"
#include "input/api/Controller.h"
#include <rapidjson/document.h>
#include <fstream>
#include <shared_mutex>

namespace RemoteGamePad
{
namespace
{
std::shared_mutex s_mutex;
CgpApi s_api{};
CgpInstance* s_instance{};
HMODULE s_module{};
bool s_initialized{}, s_enabled{}, s_attached{}, s_localAudio{true};
thread_local CgpInput s_vpadInput{};
thread_local bool s_vpadReading{};
uint64 CGP_CALL Clock(void*) { return Now(); }
void CGP_CALL Log(void*, uint32 level, const char* message)
{
    if (level >= 2) cemuLog_log(LogType::Force, "[Remote GamePad] {}", message);
}
void Failure(const char* operation)
{
    cemuLog_log(LogType::Force, "[Remote GamePad] {}: {}", operation, s_instance ? s_api.last_error(s_instance) : "plugin unavailable");
}
}
uint64 Now()
{
    return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}
void Initialize()
{
    std::unique_lock lock(s_mutex);
    if (s_initialized) return;
    s_initialized = true;
    try
    {
        const auto* appData = _wgetenv(L"LOCALAPPDATA");
        if (!appData) return;
        const auto config = fs::path(appData) / "CemuRemoteGamePad/config.json";
        if (!fs::exists(config)) return;
        if (fs::file_size(config) > 65536) throw std::runtime_error("configuration exceeds 64 KiB");
        std::ifstream file(config);
        std::string text((std::istreambuf_iterator<char>(file)), {});
        rapidjson::Document settings;
        settings.Parse(text.c_str());
        if (settings.HasParseError() || !settings.IsObject()) throw std::runtime_error("invalid config.json");
        if (!settings.HasMember("enabled") || !settings["enabled"].IsBool() || !settings["enabled"].GetBool()) return;
        if (settings.HasMember("local_gamepad_audio"))
        {
            if (!settings["local_gamepad_audio"].IsBool()) throw std::runtime_error("local_gamepad_audio must be boolean");
            s_localAudio = settings["local_gamepad_audio"].GetBool();
        }
        auto dll = ActiveSettings::GetExecutablePath().parent_path() / "extensions/gamepad/CemuGamePad.dll";
        s_module = LoadLibraryExW(dll.c_str(), nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
        if (!s_module) throw std::runtime_error(fmt::format("cannot load {} (Windows error {})", _pathToUtf8(dll), GetLastError()));
        auto getApi = reinterpret_cast<CgpGetApi>(GetProcAddress(s_module, "CemuGamePad_GetApi"));
        if (!getApi || getApi(CGP_ABI_MAJOR, sizeof(s_api), &s_api) != CGP_OK || s_api.major != CGP_ABI_MAJOR ||
            s_api.minor < CGP_ABI_MINOR || s_api.size < sizeof(CgpApi) || !s_api.create || !s_api.destroy || !s_api.attach_device || !s_api.detach_device ||
            !s_api.set_game || !s_api.acquire_frame || !s_api.submit_frame || !s_api.cancel_frame || !s_api.submit_audio ||
            !s_api.read_input || !s_api.set_input_owner || !s_api.set_overlay_focus || !s_api.rumble || !s_api.last_error)
            throw std::runtime_error("GamePad extension requires ABI 1.1 motion queue and a complete function table; update CemuGamePad.dll with Cemu.exe");
        CgpHost host{sizeof(CgpHost), CGP_ABI_MAJOR, CGP_ABI_MINOR, nullptr, Clock, Log};
        if (s_api.create(&host, _pathToUtf8(config).c_str(), &s_instance) != CGP_OK || !s_instance)
            throw std::runtime_error("plugin initialization failed; consult the preceding GamePad log");
        s_enabled = true;
    }
    catch (const std::exception& ex)
    {
        cemuLog_log(LogType::Force, "[Remote GamePad] Disabled: {}", ex.what());
        if (s_instance) { s_api.destroy(s_instance); s_instance = nullptr; }
        if (s_module) { FreeLibrary(s_module); s_module = nullptr; }
    }
}
void Shutdown()
{
    std::unique_lock lock(s_mutex);
    s_enabled = s_attached = false;
    if (s_instance) { s_api.destroy(s_instance); s_instance = nullptr; }
    if (s_module) { FreeLibrary(s_module); s_module = nullptr; }
}
bool Enabled() { Initialize(); std::shared_lock lock(s_mutex); return s_enabled; }
bool LocalAudio() { std::shared_lock lock(s_mutex); return !s_attached || s_localAudio; }
bool Attach(const CgpDevice& device)
{
    Initialize();
    std::unique_lock lock(s_mutex);
    if (!s_enabled) return false;
    if (s_api.attach_device(s_instance, &device) != CGP_OK)
    {
        Failure("GPU attachment failed");
        s_api.detach_device(s_instance);
        return false;
    }
    s_attached = true;
    return true;
}
void Detach()
{
    std::unique_lock lock(s_mutex);
    if (!s_attached) return;
    s_attached = false;
    s_api.set_game(s_instance, 0);
    s_api.detach_device(s_instance);
}
void SetGame(bool available)
{
    std::shared_lock lock(s_mutex);
    if (s_attached) s_api.set_game(s_instance, available);
}
void ReportError(const char* reason)
{
    cemuLog_log(LogType::Force, "[Remote GamePad] Session stopped: {}", reason);
    SetGame(false);
}
void UpdateBinding()
{
    size_t owners{};
    bool exclusive = true;
    auto& manager = InputManager::instance();
    for (size_t i = 0; i < InputManager::kMaxVPADControllers; ++i)
    {
        auto vpad = manager.get_vpad_controller(i);
        if (!vpad) continue;
        auto sources = vpad->GetControllersSnapshot();
        for (const auto& source : sources)
            if (source->api() == InputAPI::RemoteGamePad) { ++owners; exclusive &= sources.size() == 1; }
    }
    static bool s_reported{};
    if ((owners > 1 || !exclusive) && !s_reported)
    {
        cemuLog_log(LogType::Force, "[Remote GamePad] Bind the remote source to exactly one Wii U GamePad slot, as that slot's only physical controller");
        s_reported = true;
    }
    if (owners <= 1 && exclusive) s_reported = false;
    std::shared_lock lock(s_mutex);
    if (s_attached) s_api.set_input_owner(s_instance, owners == 1 && exclusive);
}
bool Acquire(CgpFrame& frame)
{
    std::shared_lock lock(s_mutex);
    frame = {};
    frame.size = sizeof(frame);
    return s_attached && s_api.acquire_frame(s_instance, &frame) == CGP_OK;
}
void Submit(const CgpFrame& frame)
{
    std::shared_lock lock(s_mutex);
    if (s_attached && s_api.submit_frame(s_instance, &frame) != CGP_OK) Failure("GPU frame submission failed");
}
void Cancel(const CgpFrame& frame)
{
    std::shared_lock lock(s_mutex);
    if (s_attached) s_api.cancel_frame(s_instance, &frame);
}
void Audio(std::span<const sint16> samples, uint32 channels, uint64 timestamp)
{
    std::shared_lock lock(s_mutex);
    if (!s_attached || !channels) return;
    CgpInput state{};
    state.size = sizeof(state);
    if (s_api.read_input(s_instance, &state, 0) != CGP_OK) return;
    CgpAudio audio{sizeof(CgpAudio), 48000, channels, static_cast<uint32>(samples.size() / channels), state.generation, timestamp, samples.data()};
    if (s_api.submit_audio(s_instance, &audio) < CGP_OK) Failure("DRC PCM submission failed");
}
CgpInput ReadInput(uint32 consumer)
{
    std::shared_lock lock(s_mutex);
    CgpInput state{};
    state.size = sizeof(state);
    if (s_attached && s_api.read_input(s_instance, &state, consumer) != CGP_OK) state = {};
    return state;
}
CgpInput ReadControllerInput() { return s_vpadReading ? s_vpadInput : ReadInput(0); }
InputReadScope::InputReadScope(bool remote) : m_previous(s_vpadInput), m_hadPrevious(s_vpadReading)
{
    if (remote)
    {
        s_vpadInput = ReadInput(1);
        s_vpadReading = true;
    }
}
InputReadScope::~InputReadScope() { s_vpadInput = m_previous; s_vpadReading = m_hadPrevious; }
void SetOverlayFocus(bool focus)
{
    std::shared_lock lock(s_mutex);
    if (s_attached) s_api.set_overlay_focus(s_instance, focus);
}
void Rumble(float strength)
{
    std::shared_lock lock(s_mutex);
    if (s_attached && s_api.rumble(s_instance, strength) != CGP_OK) Failure("Rumble delivery failed");
}
}
