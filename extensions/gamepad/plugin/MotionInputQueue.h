// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "../sdk/gamepad_extension.h"
#include <deque>

namespace cgp
{
// Sensor packets must survive until the emulator integrates them. A latest-only
// snapshot loses angular travel between reads, especially during rapid reversals.
class MotionInputQueue
{
public:
    bool Push(const CgpInput& previous, const CgpInput& state)
    {
        if ((!state.motion_timestamp_ns && !state.accel_timestamp_ns) ||
            (previous.motion_timestamp_ns == state.motion_timestamp_ns &&
             previous.accel_timestamp_ns == state.accel_timestamp_ns))
            return true;
        if (m_samples.size() >= CGP_MAX_MOTION_SAMPLES)
            return false;
        m_samples.push_back(state);
        return true;
    }
    CgpInput Pop(uint64_t generation)
    {
        if (m_samples.empty())
        {
            CgpInput neutral{};
            neutral.size = sizeof(neutral);
            neutral.generation = generation;
            return neutral;
        }
        auto state = m_samples.front();
        m_samples.pop_front();
        return state;
    }
    void Clear() { m_samples.clear(); }
private:
    std::deque<CgpInput> m_samples;
};
}
