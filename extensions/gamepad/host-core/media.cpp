// SPDX-License-Identifier: GPL-3.0-only
#include "runtime.h"
#include "nvenc.h"
#include "host_state.h"
#include "globals.h"
#include "stream.h"
#include "rtsp.h"
#include <opus/opus_multistream.h>

namespace cemu_host
{
const std::array<app_t, 1>& apps()
{
    static const std::array<app_t, 1> registry{{{"Cemu GamePad", "1"}}};
    return registry;
}
int app_running() { return cgp::active && cgp::active->appActive ? 1 : 0; }
int launch(int id)
{
    if (!cgp::active || id != 1) return 404;
    auto& runtime = *cgp::active;
    if (!runtime.gameAvailable || !runtime.deviceReady) return 503;
    bool expected = false;
    return runtime.appActive.compare_exchange_strong(expected, true) ? 0 : 409;
}
void cancel()
{
    if (!cgp::active) return;
    cgp::active->appActive = false;
    cgp::active->StopMedia();
}
void release_failed_launch() { cancel(); }
void report_negotiation_error(const std::string& message)
{
    if (!cgp::active) return;
    // Rejecting a new handshake must not tear down an existing media session.
    // Surface the rejection in both log.txt and the settings window instead.
    {
        std::lock_guard lock(cgp::active->errorMutex);
        cgp::active->errorMessage = message;
    }
    cgp::active->Log(warning, message);
}
void start_media() { cgp::Require(cgp::active != nullptr, "GamePad host is not initialized"); cgp::active->StartMedia(); }
void stop_media() { if (cgp::active) cgp::active->StopMedia(); }
void input_heartbeat() { if (cgp::active) cgp::active->clientActivityNs = cgp::active->Now(); }
namespace
{
thread_local CgpFrame sending{};
thread_local uint64_t lastSubmit{};
thread_local bool firstSubmit{};
}
void begin_send(uint64_t generation, uint64_t sequence, uint64_t timestamp)
{
    sending = {};
    sending.generation = generation;
    sending.sequence = sequence;
    sending.timestamp_ns = timestamp;
    lastSubmit = 0;
    firstSubmit = true;
    if (cgp::active) cgp::active->Trace("packetizer_begin", sending);
}
void packet_submitted(bool success)
{
    if (!cgp::active) return;
    if (!success) { cgp::active->Error("Direct GameStream UDP submission failed; session stopped"); return; }
    if (!sending.sequence) return;
    lastSubmit = cgp::active->Now();
    if (firstSubmit)
    {
        cgp::active->Trace("first_packet_submitted", sending, lastSubmit);
        firstSubmit = false;
    }
}
void end_send()
{
    if (cgp::active && sending.sequence && lastSubmit) cgp::active->Trace("last_packet_submitted", sending, lastSubmit);
    sending = {};
}
}

namespace video
{
int active_hevc_mode = 1, active_av1_mode = 1;
bool last_encoder_probe_supported_ref_frames_invalidation = false;
std::array<bool, 3> last_encoder_probe_supported_yuv444_for_codec{};
std::array<bool, 3> last_encoder_probe_supported_dynamic_range_for_codec{};
std::array<bool, 3> last_encoder_probe_supported_dynamic_range_yuv444_for_codec{};
int probe_encoders()
{
    if (!cgp::active) return -1;
    std::lock_guard lifecycle(cgp::active->lifecycleMutex);
    if (!cgp::active->deviceReady || !cgp::active->gameAvailable) return -1;
    try { cgp::NvEncoder encoder(cgp::active->device.Get(), 854, 480, 60, 8000000, 1); return 0; }
    catch (const std::exception& ex) { cgp::active->Error(ex.what()); return -1; }
}
void capture(safe::mail_t sessionMail, config_t config, stream::session_t* session)
{
    auto& runtime = *cgp::active;
    auto shutdown = sessionMail->event<bool>(mail::shutdown);
    try
    {
        cgp::Require(config.videoFormat == 0 && config.dynamicRange == 0 && config.chromaSamplingType == 0,
                     "Only H.264 SDR 4:2:0 is supported by the first GamePad backend");
        cgp::Require(config.bitrate > 0 && config.bitrate <= 100000, "Requested video bitrate is outside the supported range");
        // Sending fewer bits preserves the negotiated dimensions and frame rate.
        // Never substitute a different format behind the client's decoder.
        uint32_t bitrate = std::min(static_cast<uint32_t>(config.bitrate), runtime.maxBitrateKbps) * 1000;
        cgp::NvEncoder encoder(runtime.device.Get(), config.width, config.height, config.framerate,
                               bitrate, config.numRefFrames, config.encoderCscMode);
        runtime.CreatePool(config.width, config.height);
        encoder.Register(runtime.slots);
        auto packets = mail::man->queue<packet_t>(mail::video_packets);
        auto idr = sessionMail->event<bool>(mail::idr);
        auto invalidation = sessionMail->event<std::pair<int64_t, int64_t>>(mail::invalidate_ref_frames);
        uint64_t index = 1;
        cgp::Slot* slot{};
        while (!shutdown->peek() && runtime.NextFrame(slot))
        {
            try
            {
                runtime.Trace("gpu_ready_observed", slot->frame);
                bool force = index == 1 || idr->peek() || invalidation->peek();
                if (idr->peek()) idr->pop();
                if (invalidation->peek()) invalidation->pop();
                if (packets->pending_size() && bitrate > 250000)
                {
                    bitrate = std::max(250000u, bitrate * 3 / 4);
                    encoder.SetBitrate(bitrate);
                    runtime.Log(warning, "Network sender backlog; reducing GamePad bitrate to " + std::to_string(bitrate));
                }
                auto bytes = encoder.Encode(slot->frame, index++, force);
                runtime.Trace("bitstream_ready", slot->frame);
                auto packet = std::make_unique<packet_raw_generic>(std::move(bytes), index - 1, force);
                packet->channel_data = session;
                packet->source_sequence = slot->frame.sequence;
                packet->source_timestamp_ns = slot->frame.timestamp_ns;
                packet->source_generation = slot->frame.generation;
                // Translate the host clock to this module's chrono epoch; C ABI
                // timestamps do not assume identical C++ runtimes across DLLs.
                packet->frame_timestamp = std::chrono::steady_clock::now() - std::chrono::nanoseconds(runtime.Now() - slot->frame.timestamp_ns);
                cgp::Require(packets->raise(std::move(packet)), "Encoded-video sender backlog exceeded two frames; stopping instead of dropping reference frames");
                runtime.Trace("packetizer_queued", slot->frame);
                runtime.Release(*slot);
            }
            catch (...)
            {
                encoder.FinishInputReads();
                runtime.Release(*slot);
                throw;
            }
        }
        runtime.StopMedia();
        runtime.DrainPool();
    }
    catch (const std::exception& ex)
    {
        runtime.Error(ex.what());
        runtime.DrainPool();
    }
    shutdown->raise(true);
}
}

namespace audio
{
const std::array<opus_stream_config_t, MAX_STREAM_CONFIG> stream_configs{{
    {48000, 2, 1, 1, platf::speaker::map_stereo.data(), 96000},
    {48000, 2, 1, 1, platf::speaker::map_stereo.data(), 512000},
    {48000, 6, 4, 2, platf::speaker::map_surround51.data(), 256000},
    {48000, 6, 6, 0, platf::speaker::map_surround51.data(), 1536000},
    {48000, 8, 5, 3, platf::speaker::map_surround71.data(), 450000},
    {48000, 8, 8, 0, platf::speaker::map_surround71.data(), 2048000}
}};
void capture(safe::mail_t sessionMail, config_t config, stream::session_t* session)
{
    auto& runtime = *cgp::active;
    auto shutdown = sessionMail->event<bool>(mail::shutdown);
    try
    {
        const auto sessionGeneration = runtime.generation.load();
        cgp::Require(config.channels == 2 && !config.flags[config_t::CUSTOM_SURROUND_PARAMS], "GamePad audio requires the negotiated standard stereo layout");
        cgp::Require(config.packetDuration == 5 || config.packetDuration == 10 || config.packetDuration == 20,
                     "Unsupported Moonlight Opus packet duration");
        int status{};
        std::unique_ptr<OpusMSEncoder, decltype(&opus_multistream_encoder_destroy)> encoder(
            opus_multistream_encoder_create(48000, 2, 1, 1, platf::speaker::map_stereo.data(), OPUS_APPLICATION_RESTRICTED_LOWDELAY, &status),
            opus_multistream_encoder_destroy);
        cgp::Require(status == OPUS_OK && encoder, "Could not initialize the GamePad Opus encoder");
        auto check = [](int result) { if (result < 0) throw std::runtime_error(opus_strerror(result)); };
        check(opus_multistream_encoder_ctl(encoder.get(), OPUS_SET_BITRATE(config.flags[config_t::HIGH_QUALITY] ? 512000 : 96000)));
        check(opus_multistream_encoder_ctl(encoder.get(), OPUS_SET_VBR(0)));
        int lookahead{};
        check(opus_multistream_encoder_ctl(encoder.get(), OPUS_GET_LOOKAHEAD(&lookahead)));
        runtime.Log(info, "DRC Opus packet duration=" + std::to_string(config.packetDuration) + " ms, algorithmic lookahead=" + std::to_string(lookahead) + " samples");
        auto packets = mail::man->queue<packet_t>(mail::audio_packets);
        const auto frames = config.packetDuration * 48;
        std::vector<int16_t> pcm;
        const bool ready = !shutdown->peek() && runtime.BeginAudio(sessionGeneration);
        while (ready && !shutdown->peek() && runtime.NextAudio(pcm, frames * 2))
        {
            buffer_t packet(1400);
            int count = opus_multistream_encode(encoder.get(), pcm.data(), frames, packet.begin(), 1400);
            check(count);
            packet.fake_resize(count);
            cgp::Require(packets->raise(session, std::move(packet)), "Encoded-audio sender backlog exceeded its safety bound");
        }
    }
    catch (const std::exception& ex) { runtime.Error(ex.what()); }
    shutdown->raise(true);
    runtime.StopMedia();
}
}
