// SPDX-License-Identifier: MPL-2.0 OR GPL-3.0-only
#pragma once
#include "../sdk/gamepad_extension.h"
#include "input/motion/MotionHandler.h"
#include <algorithm>
#include <numbers>

class RemoteMotion
{
public:
    void Process(const CgpInput& sample)
    {
        const uint64_t timestamp = std::max(sample.motion_timestamp_ns, sample.accel_timestamp_ns);
        if (!sample.connected || !timestamp || timestamp <= m_timestamp)
            return;
        // Replayed samples may predate this render frame. Judge each component
        // against its sensor event, not the wall clock at which the queue drains.
        const bool gyroFresh = sample.motion_timestamp_ns && timestamp - sample.motion_timestamp_ns <= 200000000;
        const bool accelFresh = sample.accel_timestamp_ns && timestamp - sample.accel_timestamp_ns <= 200000000;
        if (m_timestamp && timestamp - m_timestamp < 1000000000 && (gyroFresh || accelFresh))
        {
            const float seconds = float(timestamp - m_timestamp) * 1e-9f;
            constexpr float toRadians = std::numbers::pi_v<float> / 180.f;
            // Moonlight uses SDL sensor axes, deg/s and m/s^2. Preserve Cemu's
            // body-axis signs and convert to rad/s and g before integration.
            m_motion.processMotionSample(seconds,
                gyroFresh ? sample.gyro_x * toRadians : 0, gyroFresh ? -sample.gyro_y * toRadians : 0, gyroFresh ? -sample.gyro_z * toRadians : 0,
                accelFresh ? -sample.accel_x / 9.80665f : 0, accelFresh ? sample.accel_y / 9.80665f : 0, accelFresh ? sample.accel_z / 9.80665f : 0);
        }
        m_timestamp = timestamp;
    }
    MotionSample GetSample() { return m_motion.getMotionSample(); }
private:
    // Android/Moonlight reports the calibrated gyro sensor. The legacy bias
    // learner averages ordinary slow rotations as if they were stationary,
    // then subtracts that motion from later samples (confirmed by real replay).
    // Preserve the client's calibrated rates; other Cemu backends keep their
    // existing automatic bias policy through the handler's default constructor.
    // Acceleration feedback shifted the marked shot by about 20 degrees in
    // replay. Gyro-only attitude passed the user's short phone firing retest,
    // so retain it while preserving raw acceleration for the title. Long-term
    // drift and blocking still require runtime verification.
    WiiUMotionHandler m_motion{false, false};
    uint64_t m_timestamp{};
};
