// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "runtime.h"
#include <nvEncodeAPI.h>

namespace cgp
{
class NvEncoder
{
public:
    NvEncoder(ID3D11Device* device, uint32_t width, uint32_t height, uint32_t fps,
              uint32_t bitrate, uint32_t referenceFrames, uint32_t colorMode = 2);
    ~NvEncoder();
    NvEncoder(const NvEncoder&) = delete;
    NvEncoder& operator=(const NvEncoder&) = delete;
    void Register(std::array<Slot, CGP_MAX_SLOTS>& slots);
    std::vector<uint8_t> Encode(const CgpFrame& frame, uint64_t index, bool idr);
    void SetBitrate(uint32_t bitrate);
    void FinishInputReads(); // Error cleanup only; normal completion uses NVENC's event.
private:
    void Status(NVENCSTATUS status, const char* operation);
    void Close() noexcept;
    HMODULE m_library{};
    NV_ENCODE_API_FUNCTION_LIST m_api{};
    NV_ENC_CONFIG m_config{};
    NV_ENC_INITIALIZE_PARAMS m_init{};
    void* m_encoder{};
    uint32_t m_width{}, m_height{};
    NV_ENC_REGISTERED_PTR m_registered{};
    ComPtr<ID3D11DeviceContext> m_context;
    ComPtr<ID3D11VideoDevice> m_videoDevice;
    ComPtr<ID3D11VideoContext1> m_videoContext;
    ComPtr<ID3D11VideoProcessorEnumerator> m_videoEnumerator;
    ComPtr<ID3D11VideoProcessor> m_videoProcessor;
    ComPtr<ID3D11Texture2D> m_nv12;
    ComPtr<ID3D11VideoProcessorOutputView> m_outputView;
    std::array<ComPtr<ID3D11VideoProcessorInputView>, CGP_MAX_SLOTS> m_inputViews;
    NV_ENC_OUTPUT_PTR m_bitstream{};
    HANDLE m_complete{};
    bool m_eventRegistered{};
};
}
