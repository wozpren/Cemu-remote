// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "thread_pool.h"
extern thread_pool_util::ThreadPool task_pool;
namespace mail
{
extern safe::mail_t man;
inline constexpr std::string_view shutdown = "shutdown", broadcast_shutdown = "broadcast_shutdown";
inline constexpr std::string_view video_packets = "video_packets", audio_packets = "audio_packets";
inline constexpr std::string_view idr = "idr", invalidate_ref_frames = "invalidate_ref_frames";
inline constexpr std::string_view gamepad_feedback = "gamepad_feedback", hdr = "hdr";
}
