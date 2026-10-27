// SPDX-License-Identifier: MPL-2.0 OR GPL-3.0-only
#include "RemoteController.h"
#include "RemoteControllerProvider.h"
#include <numbers>

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
    if (!m_input.connected) { m_motionTimestamp = 0; return state; }
    if (m_generation != m_input.generation)
    {
        m_generation = m_input.generation;
        m_motionTimestamp = 0;
        m_motion = {};
    }
    const uint32 masks[] = {0x8000, 0x4000, 0x2000, 0x1000, 0x20, 0x10, 8, 4, 0x200, 0x100, 0x800, 0x400, 0x40000, 0x20000, 2};
    for (uint32 i = 0; i < std::size(masks); ++i) state.buttons.SetButtonState(i, (m_input.buttons & masks[i]) != 0);
    state.axis = {m_input.left_x, m_input.left_y};
    state.rotation = {m_input.right_x, m_input.right_y};
    state.trigger = {m_input.left_trigger, m_input.right_trigger};
    const uint64 now = RemoteGamePad::Now();
    const bool gyroFresh = m_input.motion_timestamp_ns && now - m_input.motion_timestamp_ns <= 200000000;
    const bool accelFresh = m_input.accel_timestamp_ns && now - m_input.accel_timestamp_ns <= 200000000;
    const uint64 timestamp = std::max(m_input.motion_timestamp_ns, m_input.accel_timestamp_ns);
    if (timestamp > m_motionTimestamp && (gyroFresh || accelFresh))
    {
        if (m_motionTimestamp && timestamp - m_motionTimestamp < 1000000000)
        {
            float seconds = float(timestamp - m_motionTimestamp) * 1e-9f;
            constexpr float toRadians = std::numbers::pi_v<float> / 180.f;
            // Moonlight follows SDL sensor axes. Match Cemu's SDL conversion,
            // including gyro rad/s, accelerometer g, and its body-axis signs.
            // Unsupported or stale components are neutral, with capability
            // flags still reporting exactly what this client provides.
            m_motion.processMotionSample(seconds,
                gyroFresh ? m_input.gyro_x * toRadians : 0, gyroFresh ? -m_input.gyro_y * toRadians : 0, gyroFresh ? -m_input.gyro_z * toRadians : 0,
                accelFresh ? -m_input.accel_x / 9.80665f : 0, accelFresh ? m_input.accel_y / 9.80665f : 0, accelFresh ? m_input.accel_z / 9.80665f : 0);
        }
        m_motionTimestamp = timestamp;
    }
    return state;
}
bool RemoteGamePadController::has_motion()
{
    auto state = Snapshot();
    const uint64 now = RemoteGamePad::Now();
    return state.connected && ((state.motion_timestamp_ns && now - state.motion_timestamp_ns <= 200000000) ||
                               (state.accel_timestamp_ns && now - state.accel_timestamp_ns <= 200000000));
}
MotionSample RemoteGamePadController::get_motion_sample() { std::lock_guard lock(m_mutex); return m_motion.getMotionSample(); }
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
