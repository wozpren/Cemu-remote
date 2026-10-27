// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include <array>
#include <bitset>
#include "platform/common.h"
namespace stream { struct session_t; }
namespace audio
{
enum stream_config_e { STEREO, HIGH_STEREO, SURROUND51, HIGH_SURROUND51, SURROUND71, HIGH_SURROUND71, MAX_STREAM_CONFIG };
struct opus_stream_config_t
{
    int32_t sampleRate;
    int channelCount, streams, coupledStreams;
    const uint8_t* mapping;
    int bitrate;
};
extern const std::array<opus_stream_config_t, MAX_STREAM_CONFIG> stream_configs;
struct stream_params_t { int channelCount, streams, coupledStreams; std::array<uint8_t, 8> mapping; };
struct config_t
{
    enum flags_e { HIGH_QUALITY, HOST_AUDIO, CUSTOM_SURROUND_PARAMS, CONTINUOUS_AUDIO, MAX_FLAGS };
    int packetDuration, channels, mask;
    stream_params_t customStreamParams;
    std::bitset<MAX_FLAGS> flags;
};
using buffer_t = util::buffer_t<uint8_t>;
using packet_t = std::pair<stream::session_t*, buffer_t>;
void capture(safe::mail_t mail, config_t config, stream::session_t* session);
}
