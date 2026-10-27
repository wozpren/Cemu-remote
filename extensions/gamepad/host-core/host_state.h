// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include <array>
#include <cstdint>
#include <string>
namespace cemu_host
{
struct app_t { std::string name; std::string id; };
const std::array<app_t, 1>& apps();
int app_running();
int launch(int id);
void cancel();
void release_failed_launch();
void report_negotiation_error(const std::string& message);
void start_media();
void stop_media();
void input_heartbeat();
void begin_send(uint64_t generation, uint64_t sequence, uint64_t timestamp);
void packet_submitted(bool success);
void end_send();
}
