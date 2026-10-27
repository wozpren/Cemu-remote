// SPDX-License-Identifier: GPL-3.0-only
#include "nvenc.h"
#include <algorithm>
#include <stdexcept>

namespace cgp
{
void NvEncoder::Status(NVENCSTATUS status, const char* operation)
{
    if (status == NV_ENC_SUCCESS) return;
    const char* detail = m_encoder && m_api.nvEncGetLastErrorString ? m_api.nvEncGetLastErrorString(m_encoder) : "";
    throw std::runtime_error(std::string(operation) + " failed (" + std::to_string(status) + "): " + (detail ? detail : ""));
}

NvEncoder::NvEncoder(ID3D11Device* device, uint32_t width, uint32_t height, uint32_t fps,
                     uint32_t bitrate, uint32_t referenceFrames, uint32_t colorMode) : m_width(width), m_height(height)
{
    try
    {
        Require(width && height && width % 2 == 0 && height % 2 == 0 && fps && fps <= 60,
                "NVENC requires even, nonzero GamePad dimensions and at most 60 fps");
        Require(colorMode <= 3, "The GamePad backend supports SDR Rec.601/709, full or limited range");
        m_library = LoadLibraryExW(L"nvEncodeAPI64.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
        Require(m_library != nullptr, "NVIDIA NVENC driver is not installed");
        auto version = reinterpret_cast<NVENCSTATUS (NVENCAPI*)(uint32_t*)>(GetProcAddress(m_library, "NvEncodeAPIGetMaxSupportedVersion"));
        auto create = reinterpret_cast<NVENCSTATUS (NVENCAPI*)(NV_ENCODE_API_FUNCTION_LIST*)>(GetProcAddress(m_library, "NvEncodeAPICreateInstance"));
        Require(version && create, "NVENC driver entry points are unavailable");
        uint32_t supported{};
        Status(version(&supported), "NvEncodeAPIGetMaxSupportedVersion");
        Require(supported >= ((NVENCAPI_MAJOR_VERSION << 4) | NVENCAPI_MINOR_VERSION), "NVENC SDK requires a newer NVIDIA driver");
        m_api.version = NV_ENCODE_API_FUNCTION_LIST_VER;
        Status(create(&m_api), "NvEncodeAPICreateInstance");
        NV_ENC_OPEN_ENCODE_SESSION_EX_PARAMS open{};
        open.version = NV_ENC_OPEN_ENCODE_SESSION_EX_PARAMS_VER;
        open.device = device;
        open.deviceType = NV_ENC_DEVICE_TYPE_DIRECTX;
        open.apiVersion = NVENCAPI_VERSION;
        Status(m_api.nvEncOpenEncodeSessionEx(&open, &m_encoder), "NvEncOpenEncodeSessionEx");
        NV_ENC_CAPS_PARAM caps{};
        caps.version = NV_ENC_CAPS_PARAM_VER;
        caps.capsToQuery = NV_ENC_CAPS_ASYNC_ENCODE_SUPPORT;
        int asynchronous{};
        Status(m_api.nvEncGetEncodeCaps(m_encoder, NV_ENC_CODEC_H264_GUID, &caps, &asynchronous), "NvEncGetEncodeCaps");
        Require(asynchronous != 0, "This NVIDIA backend cannot provide asynchronous encode completion");
        uint32_t count{}, found{};
        Status(m_api.nvEncGetInputFormatCount(m_encoder, NV_ENC_CODEC_H264_GUID, &count), "NvEncGetInputFormatCount");
        std::vector<NV_ENC_BUFFER_FORMAT> formats(count);
        Status(m_api.nvEncGetInputFormats(m_encoder, NV_ENC_CODEC_H264_GUID, formats.data(), count, &found), "NvEncGetInputFormats");
        Require(std::find(formats.begin(), formats.begin() + found, NV_ENC_BUFFER_FORMAT_NV12) != formats.begin() + found,
                "The selected encoder does not accept GPU NV12 textures");
        NV_ENC_PRESET_CONFIG preset{};
        preset.version = NV_ENC_PRESET_CONFIG_VER;
        preset.presetCfg.version = NV_ENC_CONFIG_VER;
        Status(m_api.nvEncGetEncodePresetConfigEx(m_encoder, NV_ENC_CODEC_H264_GUID, NV_ENC_PRESET_P1_GUID,
               NV_ENC_TUNING_INFO_ULTRA_LOW_LATENCY, &preset), "NvEncGetEncodePresetConfigEx");
        auto config = preset.presetCfg;
        config.profileGUID = NV_ENC_H264_PROFILE_HIGH_GUID;
        config.frameIntervalP = 1;
        config.gopLength = NVENC_INFINITE_GOPLENGTH;
        config.rcParams.rateControlMode = NV_ENC_PARAMS_RC_CBR;
        config.rcParams.averageBitRate = bitrate;
        config.rcParams.maxBitRate = bitrate;
        config.rcParams.vbvBufferSize = std::max(1u, bitrate / fps);
        config.rcParams.vbvInitialDelay = config.rcParams.vbvBufferSize;
        config.rcParams.enableLookahead = 0;
        config.rcParams.lookaheadDepth = 0;
        config.rcParams.enableTemporalAQ = 0;
        config.rcParams.multiPass = NV_ENC_MULTI_PASS_DISABLED;
        auto& h264 = config.encodeCodecConfig.h264Config;
        h264.idrPeriod = NVENC_INFINITE_GOPLENGTH;
        h264.repeatSPSPPS = 1;
        h264.maxNumRefFrames = std::clamp(referenceFrames, 1u, 4u);
        h264.h264VUIParameters.videoSignalTypePresentFlag = 1;
        h264.h264VUIParameters.videoFormat = static_cast<NV_ENC_VUI_VIDEO_FORMAT>(5);
        h264.h264VUIParameters.videoFullRangeFlag = colorMode & 1;
        h264.h264VUIParameters.colourDescriptionPresentFlag = 1;
        h264.h264VUIParameters.colourPrimaries = static_cast<NV_ENC_VUI_COLOR_PRIMARIES>(colorMode >> 1 ? 1 : 6);
        h264.h264VUIParameters.transferCharacteristics = static_cast<NV_ENC_VUI_TRANSFER_CHARACTERISTIC>(colorMode >> 1 ? 1 : 6);
        h264.h264VUIParameters.colourMatrix = static_cast<NV_ENC_VUI_MATRIX_COEFFS>(colorMode >> 1 ? 1 : 6);
        NV_ENC_INITIALIZE_PARAMS init{};
        init.version = NV_ENC_INITIALIZE_PARAMS_VER;
        init.encodeGUID = NV_ENC_CODEC_H264_GUID;
        init.presetGUID = NV_ENC_PRESET_P1_GUID;
        init.tuningInfo = NV_ENC_TUNING_INFO_ULTRA_LOW_LATENCY;
        init.encodeWidth = init.darWidth = width;
        init.encodeHeight = init.darHeight = height;
        init.frameRateNum = fps;
        init.frameRateDen = 1;
        init.enablePTD = 1;
        init.enableEncodeAsync = 1;
        init.encodeConfig = &config;
        m_config = config;
        m_init = init;
        m_init.encodeConfig = &m_config;
        Status(m_api.nvEncInitializeEncoder(m_encoder, &m_init), "NvEncInitializeEncoder");
        m_complete = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        Require(m_complete != nullptr, "Could not create NVENC completion event");
        NV_ENC_EVENT_PARAMS event{};
        event.version = NV_ENC_EVENT_PARAMS_VER;
        event.completionEvent = m_complete;
        Status(m_api.nvEncRegisterAsyncEvent(m_encoder, &event), "NvEncRegisterAsyncEvent");
        m_eventRegistered = true;
        NV_ENC_CREATE_BITSTREAM_BUFFER output{};
        output.version = NV_ENC_CREATE_BITSTREAM_BUFFER_VER;
        Status(m_api.nvEncCreateBitstreamBuffer(m_encoder, &output), "NvEncCreateBitstreamBuffer");
        m_bitstream = output.bitstreamBuffer;

        device->GetImmediateContext(&m_context);
        Check(device->QueryInterface(IID_PPV_ARGS(&m_videoDevice)), "ID3D11VideoDevice");
        Check(m_context.As(&m_videoContext), "ID3D11VideoContext1");
        D3D11_VIDEO_PROCESSOR_CONTENT_DESC content{};
        content.InputFrameFormat = D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE;
        content.InputWidth = content.OutputWidth = width;
        content.InputHeight = content.OutputHeight = height;
        content.InputFrameRate = content.OutputFrameRate = {fps, 1};
        content.Usage = D3D11_VIDEO_USAGE_PLAYBACK_NORMAL;
        Check(m_videoDevice->CreateVideoProcessorEnumerator(&content, &m_videoEnumerator), "CreateVideoProcessorEnumerator");
        UINT support{};
        Check(m_videoEnumerator->CheckVideoProcessorFormat(DXGI_FORMAT_NV12, &support), "Check NV12 video conversion");
        Require((support & D3D11_VIDEO_PROCESSOR_FORMAT_SUPPORT_OUTPUT) != 0, "GPU video processor cannot output NV12");
        Check(m_videoEnumerator->CheckVideoProcessorFormat(DXGI_FORMAT_B8G8R8A8_UNORM, &support), "Check BGRA video conversion");
        Require((support & D3D11_VIDEO_PROCESSOR_FORMAT_SUPPORT_INPUT) != 0, "GPU video processor cannot read BGRA");
        Check(m_videoDevice->CreateVideoProcessor(m_videoEnumerator.Get(), 0, &m_videoProcessor), "CreateVideoProcessor");
        m_videoContext->VideoProcessorSetStreamAutoProcessingMode(m_videoProcessor.Get(), 0, FALSE);
        m_videoContext->VideoProcessorSetStreamFrameFormat(m_videoProcessor.Get(), 0, D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE);
        m_videoContext->VideoProcessorSetStreamColorSpace1(m_videoProcessor.Get(), 0, DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709);
        DXGI_COLOR_SPACE_TYPE outputSpace = colorMode >> 1 ?
            (colorMode & 1 ? DXGI_COLOR_SPACE_YCBCR_FULL_G22_LEFT_P709 : DXGI_COLOR_SPACE_YCBCR_STUDIO_G22_LEFT_P709) :
            (colorMode & 1 ? DXGI_COLOR_SPACE_YCBCR_FULL_G22_LEFT_P601 : DXGI_COLOR_SPACE_YCBCR_STUDIO_G22_LEFT_P601);
        m_videoContext->VideoProcessorSetOutputColorSpace1(m_videoProcessor.Get(), outputSpace);
        RECT rectangle{0, 0, static_cast<LONG>(width), static_cast<LONG>(height)};
        m_videoContext->VideoProcessorSetStreamSourceRect(m_videoProcessor.Get(), 0, TRUE, &rectangle);
        m_videoContext->VideoProcessorSetStreamDestRect(m_videoProcessor.Get(), 0, TRUE, &rectangle);
        m_videoContext->VideoProcessorSetOutputTargetRect(m_videoProcessor.Get(), TRUE, &rectangle);
        D3D11_TEXTURE2D_DESC texture{};
        texture.Width = width;
        texture.Height = height;
        texture.MipLevels = texture.ArraySize = texture.SampleDesc.Count = 1;
        texture.Format = DXGI_FORMAT_NV12;
        texture.Usage = D3D11_USAGE_DEFAULT;
        texture.BindFlags = D3D11_BIND_RENDER_TARGET;
        Check(device->CreateTexture2D(&texture, nullptr, &m_nv12), "Create GPU NV12 encoder input");
        D3D11_VIDEO_PROCESSOR_OUTPUT_VIEW_DESC view{};
        view.ViewDimension = D3D11_VPOV_DIMENSION_TEXTURE2D;
        Check(m_videoDevice->CreateVideoProcessorOutputView(m_nv12.Get(), m_videoEnumerator.Get(), &view, &m_outputView), "Create NV12 output view");
    }
    catch (...) { Close(); throw; }
}

void NvEncoder::Register(std::array<Slot, CGP_MAX_SLOTS>& slots)
{
    for (size_t i = 0; i < slots.size(); ++i)
    {
        D3D11_VIDEO_PROCESSOR_INPUT_VIEW_DESC view{};
        view.ViewDimension = D3D11_VPIV_DIMENSION_TEXTURE2D;
        Check(m_videoDevice->CreateVideoProcessorInputView(slots[i].texture.Get(), m_videoEnumerator.Get(), &view, &m_inputViews[i]), "Create BGRA video input view");
    }
    NV_ENC_REGISTER_RESOURCE resource{};
    resource.version = NV_ENC_REGISTER_RESOURCE_VER;
    resource.resourceType = NV_ENC_INPUT_RESOURCE_TYPE_DIRECTX;
    resource.width = m_width;
    resource.height = m_height;
    resource.resourceToRegister = m_nv12.Get();
    resource.bufferFormat = NV_ENC_BUFFER_FORMAT_NV12;
    resource.bufferUsage = NV_ENC_INPUT_IMAGE;
    Status(m_api.nvEncRegisterResource(m_encoder, &resource), "NvEncRegisterResource");
    m_registered = resource.registeredResource;
}

std::vector<uint8_t> NvEncoder::Encode(const CgpFrame& frame, uint64_t index, bool idr)
{
    if (active) active->Trace("gpu_conversion_submit", frame);
    D3D11_VIDEO_PROCESSOR_STREAM stream{};
    stream.Enable = TRUE;
    stream.pInputSurface = m_inputViews.at(frame.slot).Get();
    Check(m_videoContext->VideoProcessorBlt(m_videoProcessor.Get(), m_outputView.Get(), 0, 1, &stream), "GPU BGRA to NV12 conversion");
    m_context->Flush();
    NV_ENC_MAP_INPUT_RESOURCE mapped{};
    mapped.version = NV_ENC_MAP_INPUT_RESOURCE_VER;
    mapped.registeredResource = m_registered;
    Status(m_api.nvEncMapInputResource(m_encoder, &mapped), "NvEncMapInputResource");
    try
    {
        NV_ENC_PIC_PARAMS picture{};
        picture.version = NV_ENC_PIC_PARAMS_VER;
        picture.inputWidth = m_width;
        picture.inputHeight = m_height;
        picture.inputBuffer = mapped.mappedResource;
        picture.bufferFmt = mapped.mappedBufferFmt;
        picture.outputBitstream = m_bitstream;
        picture.completionEvent = m_complete;
        picture.pictureStruct = NV_ENC_PIC_STRUCT_FRAME;
        picture.inputTimeStamp = index;
        picture.encodePicFlags = idr ? NV_ENC_PIC_FLAG_FORCEIDR : 0;
        if (active) active->Trace("encode_submit", frame);
        Status(m_api.nvEncEncodePicture(m_encoder, &picture), "NvEncEncodePicture");
        // Only the encoder worker waits. No render-thread GPU wait, frame timer,
        // or next-frame callback is needed to publish completed compressed bytes.
        Require(WaitForSingleObject(m_complete, INFINITE) == WAIT_OBJECT_0, "NVENC completion wait failed");
        if (active) active->Trace("encode_complete", frame);
        NV_ENC_LOCK_BITSTREAM lock{};
        lock.version = NV_ENC_LOCK_BITSTREAM_VER;
        lock.outputBitstream = m_bitstream;
        lock.doNotWait = 1;
        Status(m_api.nvEncLockBitstream(m_encoder, &lock), "NvEncLockBitstream");
        auto* bytes = static_cast<uint8_t*>(lock.bitstreamBufferPtr);
        std::vector<uint8_t> result(bytes, bytes + lock.bitstreamSizeInBytes);
        Status(m_api.nvEncUnlockBitstream(m_encoder, m_bitstream), "NvEncUnlockBitstream");
        Status(m_api.nvEncUnmapInputResource(m_encoder, mapped.mappedResource), "NvEncUnmapInputResource");
        return result;
    }
    catch (...)
    {
        m_api.nvEncUnmapInputResource(m_encoder, mapped.mappedResource);
        throw;
    }
}

void NvEncoder::FinishInputReads()
{
    // A codec error does not prove that the earlier GPU video-processor read
    // has ended. Retire it before the shared BGRA lease can become writable.
    ComPtr<ID3D11Device> device;
    m_context->GetDevice(&device);
    if (FAILED(device->GetDeviceRemovedReason())) return; // Removed device cannot execute further reads.
    ComPtr<ID3D11Device5> device5;
    ComPtr<ID3D11DeviceContext4> context4;
    Check(device.As(&device5), "Error cleanup ID3D11Device5");
    Check(m_context.As(&context4), "Error cleanup ID3D11DeviceContext4");
    ComPtr<ID3D11Fence> fence;
    Check(device5->CreateFence(0, D3D11_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)), "Create GPU cleanup fence");
    HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    Require(event != nullptr, "Could not create GPU cleanup event");
    try
    {
        Check(context4->Signal(fence.Get(), 1), "Signal GPU input-read completion");
        Check(fence->SetEventOnCompletion(1, event), "Arm GPU cleanup event");
        m_context->Flush();
        Require(WaitForSingleObject(event, INFINITE) == WAIT_OBJECT_0, "GPU input-read retirement failed");
    }
    catch (...)
    {
        CloseHandle(event);
        if (FAILED(device->GetDeviceRemovedReason())) return;
        throw;
    }
    CloseHandle(event);
}

void NvEncoder::SetBitrate(uint32_t bitrate)
{
    m_config.rcParams.averageBitRate = m_config.rcParams.maxBitRate = bitrate;
    m_config.rcParams.vbvBufferSize = std::max(1u, bitrate / m_init.frameRateNum);
    m_config.rcParams.vbvInitialDelay = m_config.rcParams.vbvBufferSize;
    NV_ENC_RECONFIGURE_PARAMS update{};
    update.version = NV_ENC_RECONFIGURE_PARAMS_VER;
    update.reInitEncodeParams = m_init;
    Status(m_api.nvEncReconfigureEncoder(m_encoder, &update), "NvEncReconfigureEncoder bitrate");
}

void NvEncoder::Close() noexcept
{
    if (m_encoder)
    {
        if (m_registered) m_api.nvEncUnregisterResource(m_encoder, m_registered);
        if (m_bitstream) m_api.nvEncDestroyBitstreamBuffer(m_encoder, m_bitstream);
        if (m_eventRegistered)
        {
            NV_ENC_EVENT_PARAMS event{};
            event.version = NV_ENC_EVENT_PARAMS_VER;
            event.completionEvent = m_complete;
            m_api.nvEncUnregisterAsyncEvent(m_encoder, &event);
        }
        m_api.nvEncDestroyEncoder(m_encoder);
        m_encoder = nullptr;
    }
    if (m_complete) { CloseHandle(m_complete); m_complete = nullptr; }
    if (m_library) { FreeLibrary(m_library); m_library = nullptr; }
}
NvEncoder::~NvEncoder() { Close(); }
}
