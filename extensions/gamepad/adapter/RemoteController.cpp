// SPDX-License-Identifier: MPL-2.0 OR GPL-3.0-only
#include "RemoteController.h"
#include "RemoteControllerProvider.h"

RemoteGamePadController::RemoteGamePadController() : ControllerBase("remote:0", "Moonlight GamePad")
{
    m_settings.motion = true;
    // Network axes are already normalized. Treating the first received held
    // button as a calibration baseline would permanently mask that button.
    m_is_calibrated = true;
    m_settings.axis.deadzone = m_settings.rotation.deadzone = 0.1f;
}
bool RemoteGamePadController::is_connected() { return RemoteGamePad::ReadInput().connected != 0; }
CgpInput RemoteGamePadController::Snapshot() const { std::lock_guard lock(m_mutex); return m_input; }
ControllerState RemoteGamePadController::raw_state()
{
    std::lock_guard lock(m_mutex);
    m_input = RemoteGamePad::ReadControllerInput();
    ControllerState state{};
    if (!m_input.connected) return state;
    const uint32 masks[] = {0x8000, 0x4000, 0x2000, 0x1000, 0x20, 0x10, 8, 4, 0x200, 0x100, 0x800, 0x400, 0x40000, 0x20000, 2};
    for (uint32 i = 0; i < std::size(masks); ++i) state.buttons.SetButtonState(i, (m_input.buttons & masks[i]) != 0);
    state.axis = {m_input.left_x, m_input.left_y};
    state.rotation = {m_input.right_x, m_input.right_y};
    state.trigger = {m_input.left_trigger, m_input.right_trigger};
    return state;
}
// The input worker runs independently of guest VPADRead and game pauses.
// Keep integrating while paused so sensor history cannot build up behind a menu.
void RemoteGamePadController::update()
{
    std::lock_guard lock(m_mutex);
    const auto input = RemoteGamePad::ReadInput();
    if (!input.connected) { m_motion = {}; return; }
    if (m_generation != input.generation)
    {
        m_generation = input.generation;
        m_motion = {};
        m_motionSamples = m_motionBatches = 0;
        cemuLog_log(LogType::InputAPI, "[Remote motion] Replaying every received sensor sample, generation={}", m_generation);
    }
    uint32 consumed = 0;
    for (; consumed < CGP_MAX_MOTION_SAMPLES; ++consumed)
    {
        const auto sample = RemoteGamePad::ReadInput(CGP_INPUT_MOTION);
        if (!sample.connected)
            break;
        if (sample.generation == m_generation)
        {
            m_motion.Process(sample);
            if (cemuLog_isLoggingEnabled(LogType::InputAPI))
            {
                // Retain original packets beside the fusion output. The VPAD
                // trace alone cannot distinguish sensor bias from corrections.
                auto motion = m_motion.GetSample();
                std::array<float, 3> debiased{};
                std::array<float, 9> direction{};
                motion.getGyrometer(debiased.data());
                motion.getVPADAttitudeMatrix(direction.data());
                cemuLog_log(LogType::InputAPI, "[Remote sensor] generation={} t={} gt={} at={} gyro_deg=[{},{},{}] accel_ms2=[{},{},{}] debiased_rad=[{}] dir=[{}]",
                    sample.generation, std::max(sample.motion_timestamp_ns, sample.accel_timestamp_ns),
                    sample.motion_timestamp_ns, sample.accel_timestamp_ns,
                    sample.gyro_x, sample.gyro_y, sample.gyro_z, sample.accel_x, sample.accel_y, sample.accel_z,
                    fmt::join(debiased, ","), fmt::join(direction, ","));
            }
        }
    }
    m_motionSamples += consumed;
    if (consumed && ++m_motionBatches % 120 == 0)
        cemuLog_log(LogType::InputAPI, "[Remote motion] samples={} last_batch={} generation={}", m_motionSamples, consumed, m_generation);
}
bool RemoteGamePadController::has_motion()
{
    auto state = Snapshot();
    const uint64 now = RemoteGamePad::Now();
    return state.connected && ((state.motion_timestamp_ns && now - state.motion_timestamp_ns <= 200000000) ||
                               (state.accel_timestamp_ns && now - state.accel_timestamp_ns <= 200000000));
}
MotionSample RemoteGamePadController::get_motion_sample() { std::lock_guard lock(m_mutex); return m_motion.GetSample(); }
bool RemoteGamePadController::has_position() { auto state = Snapshot(); return state.connected && state.touching; }
glm::vec2 RemoteGamePadController::get_position() { auto state = Snapshot(); return {state.touch_x, state.touch_y}; }
bool RemoteGamePadController::has_rumble() { auto state = Snapshot(); return state.connected && (state.capabilities & CGP_CAP_RUMBLE); }
void RemoteGamePadController::start_rumble() { RemoteGamePad::Rumble(get_settings().rumble); }
void RemoteGamePadController::stop_rumble() { RemoteGamePad::Rumble(0); }
std::string RemoteGamePadController::get_button_name(uint64 button) const
{
    static const char* names[] = {"A", "B", "X", "Y", "L", "R", "+", "-", "Up", "Down", "Left", "Right", "Left stick", "Right stick", "Home"};
    return button < std::size(names) ? names[button] : ControllerBase::get_button_name(button);
}
std::vector<std::shared_ptr<ControllerBase>> RemoteGamePadProvider::get_controllers()
{
    // It remains selectable before a client connects; enumeration does not load
    // the streaming DLL or replace an existing controller profile.
    return {std::make_shared<RemoteGamePadController>()};
}
