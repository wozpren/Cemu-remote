// SPDX-License-Identifier: GPL-3.0-only
#include "runtime.h"
#include "nvenc.h"
#include "config.h"
#include "globals.h"
#include "crypto.h"
#include "nvhttp.h"
#include "rtsp.h"
#include <nlohmann/json.hpp>
#include <algorithm>
#include <cstring>
#include <stdexcept>

namespace config { stream_t stream; sunshine_t sunshine; nvhttp_t nvhttp; }
namespace mail { safe::mail_t man; }
namespace http { std::string unique_id; }
thread_pool_util::ThreadPool task_pool;

namespace cgp
{
Runtime* active{};
void Require(bool condition, const char* message) { if (!condition) throw std::runtime_error(message); }
void Check(HRESULT status, const char* operation)
{
    if (FAILED(status)) throw std::runtime_error(std::string(operation) + ": HRESULT " + std::to_string(static_cast<uint32_t>(status)));
}
void WriteLog(int level, const std::string& message) noexcept
{
    if (active) active->Log(level, message);
}
bool TimingEnabled() noexcept { return active && active->traceEnabled; }
uint64_t Runtime::Now() const { return host.now_ns(host.user); }
void Runtime::Log(int level, const std::string& message) const noexcept
{
    host.log(host.user, static_cast<uint32_t>(level), message.c_str());
}
void Runtime::Error(const std::string& message)
{
    {
        std::lock_guard lock(errorMutex);
        // Teardown can trigger secondary failures on the other worker. Keep
        // the first cause visible instead of replacing it with a cleanup error.
        if (errorMessage.empty()) errorMessage = message;
    }
    Log(error, message);
    StopMedia();
}
void Runtime::Trace(const char* event, const CgpFrame& frame, uint64_t observed)
{
    if (!traceEnabled) return;
    std::lock_guard lock(traceMutex);
    trace << frame.generation << ',' << frame.sequence << ',' << event << ',' << (observed ? observed : Now()) << ',' << frame.timestamp_ns << '\n';
}

Runtime::Runtime(const CgpHost& callbacks, const std::filesystem::path& path) : host(callbacks)
{
    Require(!active, "Only one GamePad extension can be created per process");
    Require(std::filesystem::file_size(path) <= 65536, "GamePad configuration exceeds 64 KiB");
    std::ifstream file(path);
    Require(file.good(), "Cannot open GamePad configuration");
    auto settings = nlohmann::json::parse(file);
    enabled = settings.value("enabled", false);
    localAudio = settings.value("local_gamepad_audio", true);
    pointerTouch = settings.value("absolute_pointer_touch", true);
    traceEnabled = settings.value("trace_timeline", false);
    allowUnverified = settings.value("allow_unverified_backend", false);
    maxBitrateKbps = settings.value("max_bitrate_kbps", 100000u);
    Require(maxBitrateKbps >= 1000 && maxBitrateKbps <= 100000, "max_bitrate_kbps must be between 1000 and 100000");
    inputTimeoutMs = settings.value("input_timeout_ms", 1500u);
    Require(inputTimeoutMs >= 100 && inputTimeoutMs <= 10000, "input_timeout_ms must be between 100 and 10000");
    auto port = settings.value("port", 47989u);
    Require(port >= 1029 && port <= 65514, "GamePad base port must be between 1029 and 65514");
    config::sunshine.port = static_cast<uint16_t>(port);
    config::sunshine.bind_address = settings.value("bind_address", std::string("0.0.0.0"));
    config::nvhttp.sunshine_name = settings.value("name", std::string("Cemu GamePad"));
    dataDirectory = path.parent_path();
    std::filesystem::create_directories(dataDirectory / "pairing");
    std::filesystem::create_directories(dataDirectory / "logs");
    config::nvhttp.cert = (dataDirectory / "pairing/server.crt").string();
    config::nvhttp.pkey = (dataDirectory / "pairing/server.key").string();
    config::nvhttp.file_state = (dataDirectory / "pairing/clients.json").string();
    if (!std::filesystem::exists(config::nvhttp.cert) || !std::filesystem::exists(config::nvhttp.pkey))
    {
        const auto credentials = crypto::gen_creds("Cemu GamePad", 2048);
        std::ofstream cert(config::nvhttp.cert, std::ios::binary);
        std::ofstream key(config::nvhttp.pkey, std::ios::binary);
        cert << credentials.x509;
        key << credentials.pkey;
        Require(cert.good() && key.good(), "Could not store GamePad pairing credentials");
    }
    stopEvent = CreateEventW(nullptr, TRUE, TRUE, nullptr);
    workEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    controlStopEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!stopEvent || !workEvent || !controlStopEvent)
    {
        if (stopEvent) CloseHandle(stopEvent);
        if (workEvent) CloseHandle(workEvent);
        if (controlStopEvent) CloseHandle(controlStopEvent);
        throw std::runtime_error("Could not create GamePad worker events");
    }
    if (traceEnabled)
    {
        trace.open(dataDirectory / "logs/timeline.csv", std::ios::app);
        if (trace.tellp() == 0) trace << "generation,source_sequence,event,monotonic_ns,source_ns\n";
    }
    active = this;
}

Runtime::~Runtime()
{
    try { Detach(); }
    catch (const std::exception& ex) { Log(error, std::string("GamePad shutdown: ") + ex.what()); }
    if (hostStarted)
    {
        controlStopped = true;
        SetEvent(controlStopEvent);
        mail::man->event<bool>(mail::shutdown)->raise(true);
        if (httpThread.joinable()) httpThread.join();
        if (rtspThread.joinable()) rtspThread.join();
        if (controlThread.joinable()) controlThread.join();
        task_pool.stop();
        task_pool.join();
        // Timer services can join helper threads during destruction. Retire
        // them here, before destroy returns and FreeLibrary takes the loader lock.
        rtsp_stream::deinitialize();
    }
    active = nullptr;
    CloseHandle(controlStopEvent);
    CloseHandle(workEvent);
    CloseHandle(stopEvent);
}

void Runtime::StartHost()
{
    Require(enabled, "Remote GamePad is disabled in config.json");
    mail::man = std::make_shared<safe::mail_raw_t>();
    // LoadLibrary has returned before this API is called. Never construct an
    // Asio timer in a DLL global initializer: it synchronously starts a thread.
    rtsp_stream::initialize();
    hostStarted = true;
    task_pool.start(1);
    controlStopped = false;
    auto run = [this](auto entry)
    {
        try { entry(); }
        catch (const std::exception& ex) { Error(ex.what()); mail::man->event<bool>(mail::shutdown)->raise(true); }
    };
    httpThread = std::jthread([run] { run(nvhttp::start); });
    rtspThread = std::jthread([run] { run(rtsp_stream::start); });
    controlThread = std::jthread([this]
    {
        try { RunControl(); }
        catch (const std::exception& ex) { Error(ex.what()); mail::man->event<bool>(mail::shutdown)->raise(true); }
    });
    Log(info, "Remote GamePad host workers created; backend acceptance remains unverified");
}

void Runtime::Attach(const CgpDevice& context)
{
    std::lock_guard lifecycle(lifecycleMutex);
    Require(enabled, "Remote GamePad is disabled in config.json");
    Require(context.vendor_id == 0x10de, "The first GamePad backend requires the Cemu Vulkan device to be NVIDIA");
    Require(!deviceReady, "A GamePad graphics device is already attached");
    ComPtr<IDXGIFactory1> factory;
    Check(CreateDXGIFactory1(IID_PPV_ARGS(&factory)), "CreateDXGIFactory1");
    ComPtr<IDXGIAdapter1> selected;
    for (UINT i = 0; ; ++i)
    {
        ComPtr<IDXGIAdapter1> adapter;
        if (factory->EnumAdapters1(i, &adapter) == DXGI_ERROR_NOT_FOUND) break;
        DXGI_ADAPTER_DESC1 desc{};
        Check(adapter->GetDesc1(&desc), "IDXGIAdapter1::GetDesc1");
        if (std::memcmp(&desc.AdapterLuid, context.luid, 8) == 0) { selected = adapter; break; }
    }
    Require(selected != nullptr, "No D3D11 adapter matches Cemu's Vulkan device LUID");
    DXGI_ADAPTER_DESC1 selectedDesc{};
    Check(selected->GetDesc1(&selectedDesc), "IDXGIAdapter1::GetDesc1");
    LARGE_INTEGER driver{};
    Check(selected->CheckInterfaceSupport(__uuidof(IDXGIDevice), &driver), "DXGI driver version");
    backendKey = std::to_string(selectedDesc.VendorId) + ":" + std::to_string(selectedDesc.DeviceId) + ":" +
        std::to_string(driver.QuadPart) + ":" + std::to_string(context.vulkan_driver_version) + ":" +
        std::to_string(NVENCAPI_MAJOR_VERSION) + "." + std::to_string(NVENCAPI_MINOR_VERSION) + ":0.1.0";
    backendValidated = false;
    HMODULE module{};
    Require(GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
        reinterpret_cast<LPCWSTR>(&CemuGamePad_GetApi), &module) != FALSE, "Could not locate the GamePad DLL");
    std::wstring modulePath(32768, L'\0');
    const auto pathLength = GetModuleFileNameW(module, modulePath.data(), static_cast<DWORD>(modulePath.size()));
    Require(pathLength && pathLength < modulePath.size(), "Could not locate the GamePad compatibility records");
    modulePath.resize(pathLength);
    const auto recordsPath = std::filesystem::path(modulePath).parent_path() / "compat/validated.json";
    if (std::filesystem::exists(recordsPath))
    {
        Require(std::filesystem::file_size(recordsPath) <= 1048576, "GamePad compatibility records exceed 1 MiB");
        std::ifstream recordsFile(recordsPath);
        const auto records = nlohmann::json::parse(recordsFile);
        Require(records.at("schema") == 1, "Unsupported GamePad compatibility record schema");
        for (const auto& record : records.at("validated_backends"))
            if (record.at("key") == backendKey && record.at("status") == "passed") backendValidated = true;
    }
    Require(backendValidated || allowUnverified,
        "This GPU/driver backend has no P0 acceptance record. Use gamepadctl enable-experimental only for explicit validation.");
    if (!backendValidated) Log(warning, "Experimental GPU backend; P0 acceptance is pending: " + backendKey);
    D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};
    Check(D3D11CreateDevice(selected.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT,
          levels, 2, D3D11_SDK_VERSION, &device, nullptr, nullptr), "D3D11CreateDevice");
    Check(device.As(&device5), "ID3D11Device5 (shared GPU fences)");
    // Initialize the real encoder before advertising availability. No software
    // encoder or adapter substitution is accepted if this fails.
    { NvEncoder probe(device.Get(), 854, 480, 60, 8000000, 1); }
    deviceReady = true;
    { std::lock_guard lock(errorMutex); errorMessage.clear(); }
    Log(info, "GamePad encoder initialized on the Cemu Vulkan device's NVIDIA adapter");
}

void Runtime::Detach()
{
    {
        std::lock_guard lifecycle(lifecycleMutex);
        deviceReady = false;
        gameAvailable = false;
    }
    StopMedia();
    appActive = false;
    if (hostStarted) rtsp_stream::terminate_sessions();
    DrainPool();
    {
        std::lock_guard lifecycle(lifecycleMutex);
        device5.Reset();
        device.Reset();
    }
}

void Runtime::SetGame(bool available)
{
    { std::lock_guard lifecycle(lifecycleMutex); gameAvailable = available; }
    if (!available)
    {
        StopMedia();
        appActive = false;
        if (deviceReady) rtsp_stream::terminate_sessions();
    }
}

void Runtime::StartMedia()
{
    std::lock_guard lifecycle(lifecycleMutex);
    Require(gameAvailable && deviceReady, "No Cemu game with a compatible Vulkan renderer is running");
    Require(mediaStopped, "Only one remote GamePad media session is supported");
    { std::lock_guard lock(errorMutex); errorMessage.clear(); }
    { std::lock_guard lock(mediaMutex); ++generation; pcm.clear(); audioStartNs = 0; pending.reset(); }
    ResetEvent(stopEvent);
    mediaStopped = false;
    mediaStartNs = Now();
    clientActivityNs = Now();
    overlayFocus = false;
    ResetInput();
}
void Runtime::StopMedia()
{
    mediaStopped = true;
    { std::lock_guard lock(mediaMutex); accepting = false; audioStartNs = 0; pcm.clear(); }
    SetEvent(stopEvent);
    mediaChanged.notify_all();
    ResetInput();
}

void Runtime::CreatePool(uint32_t width, uint32_t height)
{
    std::lock_guard lock(mediaMutex);
    Require(!mediaStopped, "Media session was stopped before GPU pool creation");
    Require(width && height && width <= 1920 && height <= 1080 && width % 2 == 0 && height % 2 == 0,
            "Shared GamePad textures require even, nonzero dimensions within 1920x1080");
    Log(info, "Creating shared GamePad textures: " + std::to_string(width) + "x" + std::to_string(height) +
              ", BGRA8, SHARED | SHARED_NTHANDLE, fence synchronization");
    for (uint32_t i = 0; i < slots.size(); ++i)
    {
        auto& slot = slots[i];
        D3D11_TEXTURE2D_DESC desc{};
        desc.Width = width;
        desc.Height = height;
        desc.ArraySize = desc.MipLevels = desc.SampleDesc.Count = 1;
        desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        desc.Usage = D3D11_USAGE_DEFAULT;
        desc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
        // NTHANDLE selects the handle representation; SHARED enables sharing.
        // Ownership already uses the exported GPU fence and slot leases, so
        // this resource must not introduce the separate keyed-mutex protocol.
        desc.MiscFlags = D3D11_RESOURCE_MISC_SHARED | D3D11_RESOURCE_MISC_SHARED_NTHANDLE;
        Check(device->CreateTexture2D(&desc, nullptr, &slot.texture), "Create shared encoder input texture");
        ComPtr<IDXGIResource1> resource;
        Check(slot.texture.As(&resource), "IDXGIResource1");
        Check(resource->CreateSharedHandle(nullptr, DXGI_SHARED_RESOURCE_READ | DXGI_SHARED_RESOURCE_WRITE,
              nullptr, &slot.textureHandle), "Export encoder texture NT handle");
        Check(device5->CreateFence(0, D3D11_FENCE_FLAG_SHARED, IID_PPV_ARGS(&slot.fence)), "Create shared ready fence");
        Check(slot.fence->CreateSharedHandle(nullptr, GENERIC_ALL, nullptr, &slot.fenceHandle), "Export ready fence NT handle");
        slot.readyEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        Require(slot.readyEvent != nullptr, "Could not create GPU readiness event");
        slot.frame = {};
        slot.frame.size = sizeof(CgpFrame);
        slot.frame.slot = i;
        slot.frame.generation = generation;
        slot.frame.texture_handle = reinterpret_cast<uint64_t>(slot.textureHandle);
        slot.frame.ready_fence_handle = reinterpret_cast<uint64_t>(slot.fenceHandle);
        slot.frame.width = width;
        slot.frame.height = height;
        slot.frame.vk_format = 44; // VK_FORMAT_B8G8R8A8_UNORM, fixed by the D3D11 export contract.
        slot.frame.color_primaries = 1;
        slot.frame.transfer_function = 4; // RGB gamma 2.2 presentation input.
        slot.frame.color_matrix = 0;
        slot.frame.full_range = 1;
        slot.state = Slot::State::Free;
    }
    accepting = true;
}

bool Runtime::Acquire(CgpFrame& frame)
{
    std::lock_guard lock(mediaMutex);
    if (!accepting || mediaStopped) return false;
    for (auto& slot : slots)
    {
        if (slot.state != Slot::State::Free) continue;
        slot.state = Slot::State::Writing;
        ++slot.frame.ready_value;
        slot.frame.sequence = ++sequence;
        slot.frame.timestamp_ns = Now();
        frame = slot.frame;
        Trace("source_acquired", frame);
        return true;
    }
    return false;
}

void Runtime::Submit(const CgpFrame& frame)
{
    std::lock_guard lock(mediaMutex);
    auto& slot = slots.at(frame.slot);
    Require(slot.state == Slot::State::Writing && frame.generation == slot.frame.generation &&
            frame.sequence == slot.frame.sequence && frame.ready_value == slot.frame.ready_value,
            "Invalid or stale GPU frame lease");
    slot.frame = frame;
    viewport = frame;
    slot.state = Slot::State::Queued;
    // Replacement changes only the unencoded candidate. A discarded candidate
    // remains leased until its own GPU completion event retires it below.
    pending = frame.slot;
    Check(slot.fence->SetEventOnCompletion(frame.ready_value, slot.readyEvent), "Arm GPU readiness event");
    SetEvent(workEvent);
    mediaChanged.notify_all();
}
void Runtime::Cancel(const CgpFrame& frame)
{
    std::lock_guard lock(mediaMutex);
    auto& slot = slots.at(frame.slot);
    if (slot.state == Slot::State::Writing && slot.frame.generation == frame.generation && slot.frame.sequence == frame.sequence)
    {
        Trace("source_cancelled", slot.frame);
        slot.state = Slot::State::Free;
    }
    mediaChanged.notify_all();
}

bool Runtime::NextFrame(Slot*& result)
{
    HANDLE events[CGP_MAX_SLOTS + 2] = {stopEvent, workEvent};
    for (size_t i = 0; i < slots.size(); ++i) events[i + 2] = slots[i].readyEvent;
    for (;;)
    {
        {
            std::lock_guard lock(mediaMutex);
            if (mediaStopped) return false;
            for (uint32_t i = 0; i < slots.size(); ++i)
            {
                auto& slot = slots[i];
                if (slot.state != Slot::State::Queued || slot.fence->GetCompletedValue() < slot.frame.ready_value) continue;
                if (pending && *pending == i)
                {
                    pending.reset();
                    slot.state = Slot::State::Encoding;
                    result = &slot;
                    return true;
                }
                Trace("gpu_candidate_retired", slot.frame);
                slot.state = Slot::State::Free;
            }
        }
        auto wait = WaitForMultipleObjects(CGP_MAX_SLOTS + 2, events, FALSE, INFINITE);
        Require(wait != WAIT_FAILED, "GPU frame event wait failed");
    }
}
void Runtime::Release(Slot& slot)
{
    std::lock_guard lock(mediaMutex);
    slot.state = Slot::State::Free;
    mediaChanged.notify_all();
}

void Runtime::DrainPool()
{
    std::lock_guard drain(drainMutex);
    std::unique_lock lock(mediaMutex);
    accepting = false;
    mediaChanged.wait(lock, [this]
    {
        return std::none_of(slots.begin(), slots.end(), [](const Slot& slot) { return slot.state == Slot::State::Writing || slot.state == Slot::State::Encoding; });
    });
    for (auto& slot : slots)
    {
        if (slot.state == Slot::State::Queued)
        {
            lock.unlock();
            Check(slot.fence->SetEventOnCompletion(slot.frame.ready_value, slot.readyEvent), "Drain GPU lease");
            Require(WaitForSingleObject(slot.readyEvent, INFINITE) == WAIT_OBJECT_0, "GPU drain failed");
            lock.lock();
        }
        if (slot.textureHandle) CloseHandle(slot.textureHandle);
        if (slot.fenceHandle) CloseHandle(slot.fenceHandle);
        if (slot.readyEvent) CloseHandle(slot.readyEvent);
        slot = {};
    }
    pending.reset();
}

bool Runtime::BeginAudio(uint64_t sessionGeneration)
{
    std::lock_guard lock(mediaMutex);
    if (mediaStopped || generation != sessionGeneration) return false;
    Require(audioStartNs == 0, "A GamePad audio consumer is already active");
    pcm.clear();
    audioStartNs = Now();
    return true;
}
bool Runtime::Audio(const CgpAudio& block)
{
    if (mediaStopped || block.generation != generation || block.timestamp_ns < mediaStartNs) return false;
    Require(block.sample_rate == 48000 && (block.channels == 1 || block.channels == 2 || block.channels == 6),
            "Unexpected Cemu DRC PCM format (the AX output hook must supply 48 kHz PCM)");
    Require(block.samples && block.frames <= 1440, "Invalid DRC PCM block");
    bool overflow{};
    {
        std::lock_guard lock(mediaMutex);
        if (mediaStopped || block.generation != generation) return false;
        // RTSP starts the session before UDP discovery and Opus initialization.
        // Capture begins only when that session's consumer can drain real PCM;
        // pre-roll must not fill the bounded queue while no consumer exists.
        if (!audioStartNs || block.timestamp_ns < audioStartNs) return false;
        overflow = pcm.size() + block.frames * 2 > 3840;
        if (!overflow)
            for (uint32_t i = 0; i < block.frames; ++i)
            {
                pcm.push_back(block.samples[i * block.channels]);
                pcm.push_back(block.samples[i * block.channels + (block.channels == 1 ? 0 : 1)]);
            }
    }
    if (overflow) Error("DRC audio encoder fell behind its 40 ms safety bound; stopping the session");
    mediaChanged.notify_all();
    return !overflow;
}
bool Runtime::NextAudio(std::vector<int16_t>& samples, size_t count)
{
    std::unique_lock lock(mediaMutex);
    mediaChanged.wait(lock, [this, count] { return mediaStopped || pcm.size() >= count; });
    if (mediaStopped) return false;
    samples.resize(count);
    for (auto& sample : samples) { sample = pcm.front(); pcm.pop_front(); }
    return true;
}

void Runtime::ResetInput()
{
    std::lock_guard lock(inputMutex);
    input = {};
    input.size = sizeof(CgpInput);
    input.generation = generation;
    edges.clear();
    pointerEdges.clear();
    feedback.reset();
    clientIdentity.clear();
}
void Runtime::UpdateInput(CgpInput state, bool edge)
{
    bool overflow{};
    {
        std::lock_guard lock(inputMutex);
        if (mediaStopped || state.generation != generation) return;
        state.size = sizeof(CgpInput);
        state.timestamp_ns = Now();
        clientActivityNs = state.timestamp_ns;
        state.connected = 1;
        if (input.pointer_down != state.pointer_down)
        {
            overflow = pointerEdges.size() >= 256;
            if (!overflow) pointerEdges.push_back(state);
        }
        input = state;
        if (edge && inputOwned)
        {
            overflow |= edges.size() >= 256;
            if (edges.size() < 256) edges.push_back(state);
        }
    }
    if (overflow) Error("Remote input edge queue overflowed; releasing input and stopping the session");
}
CgpInput Runtime::ReadInput(uint32_t consumer)
{
    std::lock_guard lock(inputMutex);
    if (consumer == 1 && !inputOwned)
    {
        CgpInput neutral{};
        neutral.size = sizeof(neutral);
        neutral.generation = generation;
        return neutral;
    }
    if (mediaStopped || !gameAvailable || (clientActivityNs && Now() - clientActivityNs > uint64_t(inputTimeoutMs) * 1000000))
    {
        input = {};
        input.size = sizeof(CgpInput);
        input.generation = generation;
        edges.clear();
        pointerEdges.clear();
    }
    auto state = input;
    if (consumer == 1 && !edges.empty())
    {
        auto edge = edges.front();
        edges.pop_front();
        state.buttons = edge.buttons;
        state.touching = edge.touching;
        state.touch_x = edge.touch_x;
        state.touch_y = edge.touch_y;
        state.pointer_down = edge.pointer_down;
        state.pointer_x = edge.pointer_x;
        state.pointer_y = edge.pointer_y;
    }
    if (consumer == 2 && !pointerEdges.empty())
    {
        const auto edge = pointerEdges.front();
        pointerEdges.pop_front();
        state.pointer_down = edge.pointer_down;
        state.pointer_x = edge.pointer_x;
        state.pointer_y = edge.pointer_y;
    }
    if (consumer == 1 && overlayFocus) state.touching = 0;
    return state;
}
void Runtime::Rumble(float strength)
{
    std::lock_guard lock(inputMutex);
    if (feedback && input.connected && (input.capabilities & CGP_CAP_RUMBLE))
    {
        auto amplitude = static_cast<uint16_t>(std::clamp(strength, 0.f, 1.f) * 65535.f);
        feedback->raise(platf::gamepad_feedback_msg_t::make_rumble(controllerId, amplitude, amplitude));
    }
}
}
