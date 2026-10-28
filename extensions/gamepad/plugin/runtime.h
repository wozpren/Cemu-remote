// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include <winsock2.h>
#include <windows.h>
#include <d3d11_4.h>
#include <dxgi1_6.h>
#include <wrl/client.h>
#include <array>
#include <atomic>
#include <condition_variable>
#include <deque>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <optional>
#include <thread>
#include "gamepad_extension.h"
#include "MotionInputQueue.h"
#include "platform/common.h"

namespace cgp
{
using Microsoft::WRL::ComPtr;
class NvEncoder;
struct Slot
{
    enum class State { Free, Writing, Queued, Encoding };
    State state{State::Free};
    ComPtr<ID3D11Texture2D> texture;
    ComPtr<ID3D11Fence> fence;
    HANDLE textureHandle{}, fenceHandle{}, readyEvent{};
    CgpFrame frame{};
};

class Runtime
{
public:
    Runtime(const CgpHost& host, const std::filesystem::path& config);
    ~Runtime();
    void StartHost();
    void Attach(const CgpDevice& device);
    void Detach();
    void SetGame(bool available);
    void StartMedia();
    void StopMedia();
    void CreatePool(uint32_t width, uint32_t height);
    void DrainPool();
    bool Acquire(CgpFrame& frame);
    void Submit(const CgpFrame& frame);
    void Cancel(const CgpFrame& frame);
    bool NextFrame(Slot*& slot);
    void Release(Slot& slot);
    bool Audio(const CgpAudio& audio);
    bool BeginAudio(uint64_t sessionGeneration);
    bool NextAudio(std::vector<int16_t>& samples, size_t count);
    void UpdateInput(CgpInput state, bool edge);
    CgpInput ReadInput(uint32_t consumer);
    void ResetInput();
    void Rumble(float strength);
    void RunControl();
    uint64_t Now() const;
    void Log(int level, const std::string& message) const noexcept;
    void Error(const std::string& message);
    void Trace(const char* event, const CgpFrame& frame, uint64_t observed = 0);

    CgpHost host;
    std::filesystem::path dataDirectory;
    std::atomic_bool gameAvailable{false}, appActive{false}, deviceReady{false};
    std::atomic_bool hostStarted{false};
    std::atomic_bool mediaStopped{true}, controlStopped{true};
    std::atomic_bool inputOwned{false};
    std::atomic_bool overlayFocus{false};
    std::atomic<uint64_t> clientActivityNs{}, mediaStartNs{};
    bool enabled{}, localAudio{true}, pointerTouch{true};
    bool traceEnabled{};
    bool allowUnverified{}, backendValidated{};
    std::string backendKey; // Protected by lifecycleMutex, including management reads.
    uint32_t inputTimeoutMs{1500};
    uint32_t maxBitrateKbps{100000}; // Startup setting; Moonlight still negotiates the stream format.
    std::string errorMessage;
    std::mutex errorMutex;
    std::mutex lifecycleMutex;
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11Device5> device5;
    HANDLE stopEvent{}, workEvent{}, controlStopEvent{};
    std::mutex mediaMutex;
    std::mutex drainMutex; // Session teardown and renderer detach can both drain.
    std::condition_variable mediaChanged;
    bool accepting{};
    std::atomic<uint64_t> generation{};
    uint64_t sequence{};
    std::array<Slot, CGP_MAX_SLOTS> slots;
    std::optional<uint32_t> pending;
    CgpFrame viewport{};
    std::deque<int16_t> pcm;
    uint64_t audioStartNs{}; // Protected by mediaMutex; zero until the consumer is ready.
    std::mutex inputMutex;
    CgpInput input{};
    std::string clientIdentity;
    MotionInputQueue motionSamples; // Protected by inputMutex, separate from input edges.
    std::deque<CgpInput> edges;
    std::deque<CgpInput> pointerEdges;
    platf::feedback_queue_t feedback;
    uint16_t controllerId{};
    std::jthread httpThread, rtspThread, controlThread;
    HANDLE controlPipe{};
    std::ofstream trace;
    std::mutex traceMutex;
};
extern Runtime* active;
void Require(bool condition, const char* message);
void Check(HRESULT status, const char* operation);
}

struct CgpInstance { std::unique_ptr<cgp::Runtime> runtime; };
