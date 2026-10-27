// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include <array>
#include <chrono>
#include <memory>
#include <optional>
#include <string_view>
#include <vector>
#include "platform/common.h"
namespace stream { struct session_t; }
namespace video
{
struct config_t
{
    int width, height, framerate, framerateX100, bitrate, slicesPerFrame, numRefFrames;
    int encoderCscMode, videoFormat, dynamicRange, chromaSamplingType, enableIntraRefresh;
};
struct packet_raw_t
{
    virtual ~packet_raw_t() = default;
    virtual bool is_idr() = 0;
    virtual int64_t frame_index() = 0;
    virtual uint8_t* data() = 0;
    virtual size_t data_size() = 0;
    struct replace_t { std::string_view old, _new; };
    std::vector<replace_t>* replacements = nullptr;
    stream::session_t* channel_data = nullptr;
    bool after_ref_frame_invalidation = false;
    uint64_t source_sequence{}, source_timestamp_ns{}, source_generation{};
    std::optional<std::chrono::steady_clock::time_point> frame_timestamp;
};
struct packet_raw_generic : packet_raw_t
{
    packet_raw_generic(std::vector<uint8_t>&& bytes, int64_t number, bool key)
        : bytes(std::move(bytes)), number(number), key(key) {}
    bool is_idr() override { return key; }
    int64_t frame_index() override { return number; }
    uint8_t* data() override { return bytes.data(); }
    size_t data_size() override { return bytes.size(); }
    std::vector<uint8_t> bytes;
    int64_t number;
    bool key;
};
using packet_t = std::unique_ptr<packet_raw_t>;
struct hdr_info_raw_t { bool enabled{}; SS_HDR_METADATA metadata{}; };
using hdr_info_t = std::unique_ptr<hdr_info_raw_t>;
extern int active_hevc_mode, active_av1_mode;
extern bool last_encoder_probe_supported_ref_frames_invalidation;
extern std::array<bool, 3> last_encoder_probe_supported_yuv444_for_codec;
extern std::array<bool, 3> last_encoder_probe_supported_dynamic_range_for_codec;
extern std::array<bool, 3> last_encoder_probe_supported_dynamic_range_yuv444_for_codec;
int probe_encoders();
void capture(safe::mail_t mail, config_t config, stream::session_t* session);
}
