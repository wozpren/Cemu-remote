// SPDX-License-Identifier: GPL-3.0-only
#include "runtime.h"
#include "control_pipe.h"
#include "nvhttp.h"
#include "rtsp.h"
#include "globals.h"
#include <nlohmann/json.hpp>

namespace cgp
{
void Runtime::RunControl()
{
    const auto pipeName = cgp_control::PipeName();
    const auto acl = L"D:P(A;;GA;;;" + cgp_control::UserSid() + L")";
    PSECURITY_DESCRIPTOR descriptor{};
    Require(ConvertStringSecurityDescriptorToSecurityDescriptorW(acl.c_str(), SDDL_REVISION_1, &descriptor, nullptr) != FALSE,
            "Could not restrict GamePad management to the current user");
    SECURITY_ATTRIBUTES security{sizeof(security), descriptor, FALSE};
    while (!controlStopped)
    {
        HANDLE pipe = CreateNamedPipeW(pipeName.c_str(), PIPE_ACCESS_DUPLEX | FILE_FLAG_FIRST_PIPE_INSTANCE | FILE_FLAG_OVERLAPPED,
            PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS,
            1, 16384, 16384, 0, &security);
        if (pipe == INVALID_HANDLE_VALUE) { LocalFree(descriptor); throw std::runtime_error("Could not open GamePad control pipe (another Cemu host may already own it)"); }
        HANDLE ioEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        if (!ioEvent) { CloseHandle(pipe); LocalFree(descriptor); throw std::runtime_error("Could not create control I/O event"); }
        auto perform = [&](auto operation, DWORD& transferred, bool connecting = false)
        {
            ResetEvent(ioEvent);
            OVERLAPPED io{};
            io.hEvent = ioEvent;
            BOOL immediate = operation(&io);
            if (!immediate && connecting && GetLastError() == ERROR_PIPE_CONNECTED) return true;
            if (!immediate && GetLastError() != ERROR_IO_PENDING) return false;
            HANDLE events[] = {controlStopEvent, ioEvent};
            auto wait = WaitForMultipleObjects(2, events, FALSE, INFINITE);
            if (wait != WAIT_OBJECT_0 + 1)
            {
                CancelIoEx(pipe, &io);
                GetOverlappedResult(pipe, &io, &transferred, TRUE);
                return false;
            }
            return GetOverlappedResult(pipe, &io, &transferred, FALSE) != FALSE;
        };
        DWORD connectedBytes{};
        bool connected = perform([&](OVERLAPPED* io) { return ConnectNamedPipe(pipe, io); }, connectedBytes, true);
        if (!connected) { CloseHandle(ioEvent); CloseHandle(pipe); if (controlStopped) break; continue; }
        std::array<char, 16384> bytes{};
        DWORD read{};
        nlohmann::json response;
        if (perform([&](OVERLAPPED* io) { return ReadFile(pipe, bytes.data(), static_cast<DWORD>(bytes.size()), nullptr, io); }, read))
        {
            try
            {
                auto request = nlohmann::json::parse(bytes.data(), bytes.data() + read);
                auto command = request.at("command").get<std::string>();
                // A second Cemu may share this user's pipe name. A GUI must never
                // approve a pairing or stop a stream owned by that other process.
                if (request.contains("process_id") && request.at("process_id").get<DWORD>() != GetCurrentProcessId())
                    throw std::runtime_error("The GamePad service belongs to another Cemu process. Close the other Cemu first.");
                response["ok"] = true;
                response["process_id"] = GetCurrentProcessId();
                if (command == "status" || command == "overview")
                {
                    auto state = ReadInput(false);
                    response["device_ready"] = deviceReady.load();
                    response["network_stopped"] = mail::man->event<bool>(mail::shutdown)->peek();
                    response["input_bound"] = inputOwned.load();
                    response["game_running"] = gameAvailable.load();
                    response["streaming"] = !mediaStopped.load();
                    response["connected"] = state.connected != 0;
                    response["touch"] = (state.capabilities & CGP_CAP_TOUCH) != 0;
                    response["gyro"] = (state.capabilities & CGP_CAP_GYRO) != 0;
                    response["accelerometer"] = (state.capabilities & CGP_CAP_ACCEL) != 0;
                    response["rumble"] = (state.capabilities & CGP_CAP_RUMBLE) != 0;
                    response["max_bitrate_kbps"] = maxBitrateKbps;
                    {
                        std::lock_guard lifecycle(lifecycleMutex);
                        response["backend_validation"] = backendValidated ? "passed" : "not_recorded";
                        response["backend_key"] = backendKey;
                    }
                    {
                        std::lock_guard lock(errorMutex);
                        response["last_error"] = errorMessage;
                    }
                    if (command == "overview")
                    {
                        response["requests"] = nlohmann::json::array();
                        for (const auto& pending : nvhttp::get_pending_pairings())
                            response["requests"].push_back({{"id", pending.id}, {"name", pending.name}, {"address", pending.address}});
                        response["clients"] = nvhttp::get_all_clients();
                    }
                }
                else if (command == "pending")
                {
                    response["requests"] = nlohmann::json::array();
                    for (const auto& pending : nvhttp::get_pending_pairings())
                        response["requests"].push_back({{"id", pending.id}, {"name", pending.name}, {"address", pending.address}});
                }
                else if (command == "pair")
                    response["ok"] = nvhttp::pin(request.at("id").get<std::string>(), request.at("pin").get<std::string>(), request.at("name").get<std::string>());
                else if (command == "clients") response["clients"] = nvhttp::get_all_clients();
                else if (command == "unpair") response["ok"] = nvhttp::unpair_client(request.at("id").get<std::string>());
                else if (command == "stop")
                {
                    StopMedia();
                    rtsp_stream::terminate_sessions();
                    appActive = false;
                }
                else throw std::runtime_error("Unknown GamePad management command");
            }
            catch (const std::exception& ex) { response = {{"ok", false}, {"error", ex.what()}}; }
            auto message = response.dump();
            if (message.size() > 16384) message = R"({"ok":false,"error":"Management response exceeds 16 KiB"})";
            DWORD written{};
            if (perform([&](OVERLAPPED* io) { return WriteFile(pipe, message.data(), static_cast<DWORD>(message.size()), nullptr, io); }, written))
            {
                // DisconnectNamedPipe discards unread data. Wait for the client's
                // acknowledgement using cancellable I/O rather than a blocking flush.
                char acknowledgement{};
                DWORD acknowledged{};
                perform([&](OVERLAPPED* io) { return ReadFile(pipe, &acknowledgement, 1, nullptr, io); }, acknowledged);
            }
            else if (!controlStopped) Log(warning, "Could not reply to the GamePad management client");
        }
        DisconnectNamedPipe(pipe);
        CloseHandle(ioEvent);
        CloseHandle(pipe);
    }
    LocalFree(descriptor);
}
}
