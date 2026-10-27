// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include <bitset>
#include <chrono>
#include <cstdint>
#include <string>

namespace config
{
inline constexpr int ENCRYPTION_MODE_NEVER = 0;
inline constexpr int ENCRYPTION_MODE_OPPORTUNISTIC = 1;
inline constexpr int ENCRYPTION_MODE_MANDATORY = 2;
inline constexpr int PACKETSIZE_MIN = 200, PACKETSIZE_MAX = 65535;
inline constexpr int PACKETSIZE_SMALL = 500, PACKETSIZE_LARGE = 1456;
namespace flag { enum flag_e { PIN_STDIN, FRESH_STATE, FORCE_VIDEO_HEADER_REPLACE, UPNP, CONST_PIN, FLAG_SIZE }; }
struct stream_t
{
    std::chrono::milliseconds ping_timeout{10000};
    int fec_percentage{20};
    int lan_encryption_mode{ENCRYPTION_MODE_OPPORTUNISTIC};
    int wan_encryption_mode{ENCRYPTION_MODE_MANDATORY};
    int packetsize{0};
};
struct sunshine_t
{
    std::bitset<flag::FLAG_SIZE> flags;
    uint16_t port{47989};
    std::string address_family{"ipv4"};
    std::string bind_address{"0.0.0.0"};
};
struct nvhttp_t
{
    std::string cert, pkey, file_state;
    std::string sunshine_name{"Cemu GamePad"};
    std::string external_ip;
};
extern stream_t stream;
extern sunshine_t sunshine;
extern nvhttp_t nvhttp;
}
