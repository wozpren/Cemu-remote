// SPDX-License-Identifier: MPL-2.0 OR GPL-3.0-only
#ifndef CEMU_GAMEPAD_EXTENSION_H
#define CEMU_GAMEPAD_EXTENSION_H

#include <stdint.h>

#ifdef _WIN32
#define CGP_CALL __cdecl
#ifdef CGP_BUILD_PLUGIN
#define CGP_EXPORT __declspec(dllexport)
#else
#define CGP_EXPORT
#endif
#else
#define CGP_CALL
#define CGP_EXPORT
#endif

#ifdef __cplusplus
extern "C" {
#endif

#define CGP_ABI_MAJOR 1u
#define CGP_ABI_MINOR 1u
#define CGP_INPUT_MOTION 3u
#define CGP_MAX_MOTION_SAMPLES 512u
#define CGP_MAX_SLOTS 3u
#define CGP_CAP_TOUCH 1u
#define CGP_CAP_GYRO 2u
#define CGP_CAP_ACCEL 4u
#define CGP_CAP_RUMBLE 8u

typedef struct CgpInstance CgpInstance;
typedef int32_t CgpResult;
enum { CGP_OK = 0, CGP_IDLE = 1, CGP_ERROR = -1, CGP_INCOMPATIBLE = -2 };

#pragma pack(push, 8)
typedef struct CgpHost
{
    uint32_t size;
    uint32_t major;
    uint32_t minor;
    void *user;
    uint64_t (CGP_CALL *now_ns)(void *user);
    void (CGP_CALL *log)(void *user, uint32_t level, const char *message);
} CgpHost;

typedef struct CgpDevice
{
    uint32_t size;
    uint32_t vendor_id;
    uint8_t luid[8];
    uint32_t vulkan_driver_version;
} CgpDevice;

/* Handles are borrowed Win32 NT handles in this process. The adapter imports them
 * as D3D11_TEXTURE and D3D12_FENCE, never as opaque VkImage values. */
typedef struct CgpFrame
{
    uint32_t size;
    uint32_t slot;
    uint64_t generation;
    uint64_t sequence;
    uint64_t timestamp_ns;
    uint64_t texture_handle;
    uint64_t ready_fence_handle;
    uint64_t ready_value;
    uint32_t width;
    uint32_t height;
    uint32_t vk_format;
    uint32_t color_primaries; /* ISO/IEC 23091-2 values; RGB input uses Rec.709. */
    uint32_t transfer_function;
    uint32_t color_matrix; /* 0 = RGB identity. */
    uint32_t full_range;
    uint32_t rotation;
    float content_x;
    float content_y;
    float content_width;
    float content_height;
} CgpFrame;

typedef struct CgpAudio
{
    uint32_t size;
    uint32_t sample_rate;
    uint32_t channels;
    uint32_t frames;
    uint64_t generation;
    uint64_t timestamp_ns;
    const int16_t *samples;
} CgpAudio;

typedef struct CgpInput
{
    uint32_t size;
    uint32_t connected;
    uint64_t generation;
    uint64_t timestamp_ns;
    uint32_t capabilities;
    uint32_t buttons; /* Wii U VPAD label semantics. */
    float left_x, left_y, right_x, right_y;
    float left_trigger, right_trigger;
    uint32_t touching;
    float touch_x, touch_y; /* Normalized within the DRC content rectangle. */
    uint32_t pointer_down;
    float pointer_x, pointer_y; /* Normalized video coordinates for Cemu overlays. */
    float gyro_x, gyro_y, gyro_z; /* deg/s, right-handed client controller axes. */
    float accel_x, accel_y, accel_z; /* m/s^2, same axes as gyro. */
    uint64_t motion_timestamp_ns;
    uint64_t accel_timestamp_ns;
} CgpInput;

typedef struct CgpApi
{
    uint32_t size;
    uint32_t major;
    uint32_t minor;
    CgpResult (CGP_CALL *create)(const CgpHost *, const char *config_path, CgpInstance **);
    void (CGP_CALL *destroy)(CgpInstance *);
    CgpResult (CGP_CALL *attach_device)(CgpInstance *, const CgpDevice *);
    void (CGP_CALL *detach_device)(CgpInstance *);
    void (CGP_CALL *set_game)(CgpInstance *, uint32_t available);
    CgpResult (CGP_CALL *acquire_frame)(CgpInstance *, CgpFrame *);
    CgpResult (CGP_CALL *submit_frame)(CgpInstance *, const CgpFrame *);
    void (CGP_CALL *cancel_frame)(CgpInstance *, const CgpFrame *);
    CgpResult (CGP_CALL *submit_audio)(CgpInstance *, const CgpAudio *);
    /* consumer: 0 = observe, 1 = VPAD edges, 2 = overlay pointer edges.
     * ABI 1.1: 3 = consume one motion sample in arrival order, CGP_IDLE if empty.
     * Motion is independent of button/touch edges; only the bound VPAD consumes it. */
    CgpResult (CGP_CALL *read_input)(CgpInstance *, CgpInput *, uint32_t consumer);
    void (CGP_CALL *set_input_owner)(CgpInstance *, uint32_t bound);
    void (CGP_CALL *set_overlay_focus)(CgpInstance *, uint32_t focus);
    CgpResult (CGP_CALL *rumble)(CgpInstance *, float strength);
    const char *(CGP_CALL *last_error)(CgpInstance *);
} CgpApi;
#pragma pack(pop)

typedef CgpResult (CGP_CALL *CgpGetApi)(uint32_t major, uint32_t table_size, CgpApi *api);
CGP_EXPORT CgpResult CGP_CALL CemuGamePad_GetApi(uint32_t major, uint32_t table_size, CgpApi *api);

#ifdef __cplusplus
}
#endif
#endif
