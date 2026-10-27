// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "platform/common.h"
namespace input
{
struct input_t;
std::shared_ptr<input_t> alloc(safe::mail_t mail, std::string id);
void reset(std::shared_ptr<input_t>& input);
void terminate_gamepads();
void terminate_gamepads(std::string_view identity);
void passthrough(std::shared_ptr<input_t>& input, std::vector<uint8_t>&& bytes);
}
