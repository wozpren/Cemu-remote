// SPDX-License-Identifier: MPL-2.0 OR GPL-3.0-only
#pragma once
#include "input/api/Controller.h"
#include "input/api/ControllerProvider.h"
#include "RemoteMotion.h"
#include "GamePadExtension.h"

class RemoteGamePadController final : public ControllerBase
{
public:
    RemoteGamePadController();
    InputAPI::Type api() const override { return InputAPI::RemoteGamePad; }
    std::string_view api_name() const override { return "Remote GamePad"; }
    bool is_connected() override;
    ControllerState raw_state() override;
    void update() override;
    bool has_motion() override;
    MotionSample get_motion_sample() override;
    bool has_position() override;
    glm::vec2 get_position() override;
    bool has_rumble() override;
    void start_rumble() override;
    void stop_rumble() override;
    std::string get_button_name(uint64 button) const override;
    CgpInput Snapshot() const;
private:
    mutable std::mutex m_mutex;
    CgpInput m_input{};
    RemoteMotion m_motion;
    uint64 m_generation{}, m_motionSamples{}, m_motionBatches{};
};
