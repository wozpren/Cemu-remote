// SPDX-License-Identifier: GPL-3.0-only
#include "runtime.h"
#include "input.h"
#include "globals.h"
#include "logging.h"
#include <moonlight-common-c/src/Input.h>
#include <algorithm>
#include <bit>
#include <cmath>
#include <cstring>
#include <set>

namespace input
{
struct input_t
{
    CgpInput state{};
    uint32_t pointerId{};
    bool hasPointer{}, absolutePointer{};
    float pointerX{}, pointerY{};
    std::set<uint32_t> rejected;
};

template<class Packet> Packet Read(const std::vector<uint8_t>& bytes)
{
    cgp::Require(bytes.size() == sizeof(Packet), "Invalid Moonlight input packet length");
    Packet packet;
    std::memcpy(&packet, bytes.data(), sizeof(packet));
    return packet;
}
float NetFloat(const netfloat& bytes)
{
    uint32_t bits;
    std::memcpy(&bits, bytes, sizeof(bits));
    float value = std::bit_cast<float>(util::endian::little(bits));
    cgp::Require(std::isfinite(value), "Non-finite value in Moonlight motion/touch input");
    return value;
}
float Stick(int16_t value) { return value < 0 ? value / 32768.f : value / 32767.f; }

void Touch(input_t& context, uint8_t type, uint32_t pointer, float x, float y)
{
    if (type == LI_TOUCH_EVENT_CANCEL_ALL ||
        ((type == LI_TOUCH_EVENT_UP || type == LI_TOUCH_EVENT_CANCEL) && context.hasPointer && context.pointerId == pointer))
    {
        context.hasPointer = false;
        context.state.touching = context.state.pointer_down = 0;
        return;
    }
    if (type != LI_TOUCH_EVENT_DOWN && type != LI_TOUCH_EVENT_MOVE) return;
    if (type == LI_TOUCH_EVENT_DOWN && !context.hasPointer) { context.hasPointer = true; context.pointerId = pointer; }
    if (!context.hasPointer || context.pointerId != pointer) return;
    auto& runtime = *cgp::active;
    CgpFrame viewport;
    { std::lock_guard lock(runtime.mediaMutex); viewport = runtime.viewport; }
    context.state.pointer_down = 1;
    context.state.pointer_x = std::clamp(x, 0.f, 1.f);
    context.state.pointer_y = std::clamp(y, 0.f, 1.f);
    context.state.capabilities |= CGP_CAP_TOUCH;
    if (!viewport.width || viewport.content_width <= 0 || viewport.content_height <= 0)
    {
        context.state.touching = 0;
        return;
    }
    auto localX = (x * viewport.width - viewport.content_x) / viewport.content_width;
    auto localY = (y * viewport.height - viewport.content_y) / viewport.content_height;
    context.state.touching = localX >= 0 && localX <= 1 && localY >= 0 && localY <= 1;
    context.state.touch_x = std::clamp(localX, 0.f, 1.f);
    context.state.touch_y = std::clamp(localY, 0.f, 1.f);
}

std::shared_ptr<input_t> alloc(safe::mail_t sessionMail, std::string identity)
{
    auto context = std::make_shared<input_t>();
    auto& runtime = *cgp::active;
    context->state.size = sizeof(CgpInput);
    context->state.generation = runtime.generation;
    {
        std::lock_guard lock(runtime.inputMutex);
        runtime.clientIdentity = std::move(identity);
        runtime.feedback = sessionMail->queue<platf::gamepad_feedback_msg_t>(mail::gamepad_feedback);
    }
    return context;
}
void reset(std::shared_ptr<input_t>& context)
{
    if (cgp::active) cgp::active->ResetInput();
    context.reset();
}
void terminate_gamepads() { if (cgp::active) cgp::active->ResetInput(); }
void terminate_gamepads(std::string_view identity)
{
    if (!cgp::active) return;
    bool matches{};
    { std::lock_guard lock(cgp::active->inputMutex); matches = cgp::active->clientIdentity == identity; }
    if (matches) cgp::active->ResetInput();
}

void passthrough(std::shared_ptr<input_t>& context, std::vector<uint8_t>&& bytes)
{
    if (!context || !cgp::active || cgp::active->mediaStopped || context->state.generation != cgp::active->generation) return;
    auto& runtime = *cgp::active;
    try
    {
        cgp::Require(bytes.size() >= sizeof(NV_INPUT_HEADER) && bytes.size() <= 256, "Invalid Moonlight input packet");
        NV_INPUT_HEADER header;
        std::memcpy(&header, bytes.data(), sizeof(header));
        cgp::Require(util::endian::big(header.size) == bytes.size() - sizeof(header.size), "Moonlight input size field is inconsistent");
        const auto magic = util::endian::little(header.magic);
        const auto before = context->state;
        switch (magic)
        {
        case MULTI_CONTROLLER_MAGIC_GEN5:
        {
            const auto packet = Read<NV_MULTI_CONTROLLER_PACKET>(bytes);
            auto number = util::endian::little(packet.controllerNumber);
            if (number != 0) return; // One explicitly bound GamePad is supported.
            if (!(util::endian::little(packet.activeGamepadMask) & 1))
            {
                context->state.buttons = 0;
                context->state.touching = context->state.pointer_down = 0;
                context->state.left_x = context->state.left_y = context->state.right_x = context->state.right_y = 0;
                context->state.left_trigger = context->state.right_trigger = 0;
                context->state.motion_timestamp_ns = 0;
                context->state.accel_timestamp_ns = 0;
                context->state.capabilities &= CGP_CAP_TOUCH;
                context->hasPointer = false;
                break;
            }
            uint32_t flags = uint16_t(util::endian::little(packet.buttonFlags));
            uint32_t buttons{};
            const std::pair<uint32_t, uint32_t> mapping[] = {
                {platf::A, 0x8000}, {platf::B, 0x4000}, {platf::X, 0x2000}, {platf::Y, 0x1000},
                {platf::LEFT_BUTTON, 0x20}, {platf::RIGHT_BUTTON, 0x10}, {platf::START, 8}, {platf::BACK, 4},
                {platf::HOME, 2}, {platf::DPAD_UP, 0x200}, {platf::DPAD_DOWN, 0x100},
                {platf::DPAD_LEFT, 0x800}, {platf::DPAD_RIGHT, 0x400}, {platf::LEFT_STICK, 0x40000}, {platf::RIGHT_STICK, 0x20000}
            };
            for (auto [source, target] : mapping) if (flags & source) buttons |= target;
            context->state.left_trigger = packet.leftTrigger / 255.f;
            context->state.right_trigger = packet.rightTrigger / 255.f;
            if (packet.leftTrigger >= 128) buttons |= 0x80;
            if (packet.rightTrigger >= 128) buttons |= 0x40;
            context->state.buttons = buttons;
            context->state.left_x = Stick(util::endian::little(packet.leftStickX));
            context->state.left_y = Stick(util::endian::little(packet.leftStickY));
            context->state.right_x = Stick(util::endian::little(packet.rightStickX));
            context->state.right_y = Stick(util::endian::little(packet.rightStickY));
            break;
        }
        case SS_CONTROLLER_ARRIVAL_MAGIC:
        {
            auto packet = Read<SS_CONTROLLER_ARRIVAL_PACKET>(bytes);
            if (packet.controllerNumber != 0) return;
            auto capabilities = util::endian::little(packet.capabilities);
            if (capabilities & LI_CCAP_RUMBLE) context->state.capabilities |= CGP_CAP_RUMBLE;
            if (capabilities & LI_CCAP_GYRO) context->state.capabilities |= CGP_CAP_GYRO;
            if (capabilities & LI_CCAP_ACCEL) context->state.capabilities |= CGP_CAP_ACCEL;
            if (capabilities & LI_CCAP_TOUCHPAD) context->state.capabilities |= CGP_CAP_TOUCH;
            std::lock_guard lock(runtime.inputMutex);
            runtime.controllerId = packet.controllerNumber;
            if (runtime.feedback)
            {
                if (capabilities & LI_CCAP_GYRO) runtime.feedback->raise(platf::gamepad_feedback_msg_t::make_motion_event_state(packet.controllerNumber, LI_MOTION_TYPE_GYRO, 100));
                if (capabilities & LI_CCAP_ACCEL) runtime.feedback->raise(platf::gamepad_feedback_msg_t::make_motion_event_state(packet.controllerNumber, LI_MOTION_TYPE_ACCEL, 100));
            }
            runtime.Log(info, "Moonlight controller capabilities: touch=" + std::to_string(bool(capabilities & LI_CCAP_TOUCHPAD)) +
                " gyro=" + std::to_string(bool(capabilities & LI_CCAP_GYRO)) + " accelerometer=" + std::to_string(bool(capabilities & LI_CCAP_ACCEL)) +
                " rumble=" + std::to_string(bool(capabilities & LI_CCAP_RUMBLE)));
            break;
        }
        case ENABLE_HAPTICS_MAGIC:
            if (util::endian::little(Read<NV_HAPTICS_PACKET>(bytes).enable)) context->state.capabilities |= CGP_CAP_RUMBLE;
            break;
        case SS_CONTROLLER_MOTION_MAGIC:
        {
            const auto packet = Read<SS_CONTROLLER_MOTION_PACKET>(bytes);
            if (packet.controllerNumber != 0) return;
            float x = NetFloat(packet.x), y = NetFloat(packet.y), z = NetFloat(packet.z);
            if (packet.motionType == LI_MOTION_TYPE_GYRO)
            {
                context->state.gyro_x = x; context->state.gyro_y = y; context->state.gyro_z = z;
                context->state.capabilities |= CGP_CAP_GYRO;
                context->state.motion_timestamp_ns = runtime.Now();
            }
            else if (packet.motionType == LI_MOTION_TYPE_ACCEL)
            {
                context->state.accel_x = x; context->state.accel_y = y; context->state.accel_z = z;
                context->state.capabilities |= CGP_CAP_ACCEL;
                context->state.accel_timestamp_ns = runtime.Now();
            }
            break;
        }
        case SS_TOUCH_MAGIC:
        {
            const auto packet = Read<SS_TOUCH_PACKET>(bytes);
            const bool terminal = packet.eventType == LI_TOUCH_EVENT_UP || packet.eventType == LI_TOUCH_EVENT_CANCEL || packet.eventType == LI_TOUCH_EVENT_CANCEL_ALL;
            Touch(*context, packet.eventType, util::endian::little(packet.pointerId), terminal ? 0.f : NetFloat(packet.x), terminal ? 0.f : NetFloat(packet.y));
            break;
        }
        case SS_CONTROLLER_TOUCH_MAGIC:
        {
            const auto packet = Read<SS_CONTROLLER_TOUCH_PACKET>(bytes);
            if (packet.controllerNumber != 0 || packet.touchpadIndex != 0) return;
            const bool terminal = packet.eventType == LI_TOUCH_EVENT_UP || packet.eventType == LI_TOUCH_EVENT_CANCEL || packet.eventType == LI_TOUCH_EVENT_CANCEL_ALL;
            Touch(*context, packet.eventType, util::endian::little(packet.pointerId), terminal ? 0.f : NetFloat(packet.x), terminal ? 0.f : NetFloat(packet.y));
            break;
        }
        case SS_PEN_MAGIC:
        {
            const auto packet = Read<SS_PEN_PACKET>(bytes);
            const bool terminal = packet.eventType == LI_TOUCH_EVENT_UP || packet.eventType == LI_TOUCH_EVENT_CANCEL || packet.eventType == LI_TOUCH_EVENT_CANCEL_ALL;
            Touch(*context, packet.eventType, 0xfffffffe, terminal ? 0.f : NetFloat(packet.x), terminal ? 0.f : NetFloat(packet.y));
            break;
        }
        case MOUSE_MOVE_ABS_MAGIC:
        {
            const auto packet = Read<NV_ABS_MOUSE_MOVE_PACKET>(bytes);
            if (!runtime.pointerTouch) return;
            const auto width = util::endian::big(packet.width), height = util::endian::big(packet.height);
            cgp::Require(width > 0 && height > 0, "Invalid absolute pointer dimensions");
            context->absolutePointer = true;
            context->pointerX = float(util::endian::big(packet.x)) / width;
            context->pointerY = float(util::endian::big(packet.y)) / height;
            if (context->hasPointer) Touch(*context, LI_TOUCH_EVENT_MOVE, 0xffffffff, context->pointerX, context->pointerY);
            break;
        }
        case MOUSE_BUTTON_DOWN_EVENT_MAGIC_GEN5:
        case MOUSE_BUTTON_UP_EVENT_MAGIC_GEN5:
        {
            const auto packet = Read<NV_MOUSE_BUTTON_PACKET>(bytes);
            if (!runtime.pointerTouch || !context->absolutePointer || packet.button != 1) return;
            Touch(*context, magic == MOUSE_BUTTON_DOWN_EVENT_MAGIC_GEN5 ? LI_TOUCH_EVENT_DOWN : LI_TOUCH_EVENT_UP,
                  0xffffffff, context->pointerX, context->pointerY);
            break;
        }
        default:
            if (context->rejected.insert(magic).second)
                runtime.Log(info, "Ignoring input type " + std::to_string(magic) + ": only GamePad, absolute touch/pointer, and controller motion are accepted");
            return;
        }
        const auto& after = context->state;
        bool edge = before.buttons != after.buttons || before.touching != after.touching || before.pointer_down != after.pointer_down;
        runtime.UpdateInput(after, edge);
    }
    catch (const std::exception& ex) { runtime.Error(ex.what()); }
}
}
