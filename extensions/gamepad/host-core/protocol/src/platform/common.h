// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include <boost/asio.hpp>
#include <array>
#include <cstdint>
#include <memory>
#include <string_view>
#include "src/thread_safe.h"
#include "src/logging.h"
extern "C" {
#include <moonlight-common-c/src/Limelight.h>
}
namespace platf
{
class deinit_t { public: virtual ~deinit_t() = default; };
enum class thread_priority_e { low, normal, high, critical };
void set_thread_name(std::string_view name);
void adjust_thread_priority(thread_priority_e priority);
std::string get_mac_address(const std::string_view& address);
std::string from_sockaddr(const sockaddr* address);
std::pair<uint16_t, std::string> from_sockaddr_ex(const sockaddr* address);
void streaming_will_start();
void streaming_will_stop();
uint32_t get_capabilities();
class high_precision_timer
{
public:
    virtual ~high_precision_timer() = default;
    virtual void sleep_for(const std::chrono::nanoseconds& duration) = 0;
    virtual explicit operator bool() = 0;
};
std::unique_ptr<high_precision_timer> create_high_precision_timer();
}
#include "wire_types.inc"
