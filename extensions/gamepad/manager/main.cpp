// SPDX-License-Identifier: GPL-3.0-only
#include <windows.h>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <vector>
#include <nlohmann/json.hpp>
#include "../sdk/control_pipe.h"

int wmain(int argc, wchar_t** wideArguments)
{
    try
    {
        std::vector<std::string> arguments;
        for (int i = 0; i < argc; ++i)
        {
            const int count = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, wideArguments[i], -1, nullptr, 0, nullptr, nullptr);
            if (!count) throw std::runtime_error("Could not decode a command-line argument");
            std::string value(count, '\0');
            if (!WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, wideArguments[i], -1, value.data(), count, nullptr, nullptr))
                throw std::runtime_error("Could not decode a command-line argument");
            value.resize(count - 1);
            arguments.push_back(std::move(value));
        }
        if (argc < 2) throw std::runtime_error("Usage: gamepadctl init|enable|enable-experimental|disable|status|pending|pair ID PIN NAME|clients|unpair ID|stop");
        const std::string command(arguments[1]);
        if (command == "init" || command == "enable" || command == "enable-experimental" || command == "disable")
        {
            wchar_t* appData{};
            size_t length{};
            _wdupenv_s(&appData, &length, L"LOCALAPPDATA");
            if (!appData) throw std::runtime_error("LOCALAPPDATA is unavailable");
            auto directory = std::filesystem::path(appData) / L"CemuRemoteGamePad";
            free(appData);
            std::filesystem::create_directories(directory);
            auto path = directory / L"config.json";
            nlohmann::json config;
            if (std::filesystem::exists(path)) { std::ifstream file(path); file >> config; }
            else config = {{"enabled", false}, {"port", 47989}, {"bind_address", "0.0.0.0"}, {"name", "Cemu GamePad"},
                           {"input_timeout_ms", 1500}, {"local_gamepad_audio", true}, {"absolute_pointer_touch", true},
                           {"trace_timeline", false}, {"allow_unverified_backend", false}};
            if (command != "init")
            {
                config["enabled"] = command != "disable";
                config["allow_unverified_backend"] = command == "enable-experimental";
            }
            auto temporary = path;
            temporary += L".tmp";
            { std::ofstream file(temporary); file << config.dump(2) << '\n'; if (!file) throw std::runtime_error("Could not save GamePad configuration"); }
            if (!MoveFileExW(temporary.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
                throw std::runtime_error("Could not replace GamePad configuration");
            std::cout << "Configuration saved. Restart Cemu to apply enable/disable changes.\n";
            return 0;
        }
        nlohmann::json request{{"command", command}};
        if (command == "pair")
        {
            if (argc != 5) throw std::runtime_error("Usage: gamepadctl pair REQUEST_ID FOUR_DIGIT_PIN CLIENT_NAME");
            request["id"] = arguments[2]; request["pin"] = arguments[3]; request["name"] = arguments[4];
        }
        else if (command == "unpair")
        {
            if (argc != 3) throw std::runtime_error("Usage: gamepadctl unpair CLIENT_ID");
            request["id"] = arguments[2];
        }
        const auto pipeName = cgp_control::PipeName();
        HANDLE pipe = CreateFileW(pipeName.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
        if (pipe == INVALID_HANDLE_VALUE) throw std::runtime_error("GamePad host is unavailable. Enable the extension and start a Vulkan game in the adapted Cemu build.");
        DWORD pipeMode = PIPE_READMODE_MESSAGE;
        if (!SetNamedPipeHandleState(pipe, &pipeMode, nullptr, nullptr)) { CloseHandle(pipe); throw std::runtime_error("Could not set management message mode"); }
        auto body = request.dump();
        DWORD count{};
        if (!WriteFile(pipe, body.data(), static_cast<DWORD>(body.size()), &count, nullptr)) { CloseHandle(pipe); throw std::runtime_error("Could not send management request"); }
        std::string reply;
        char buffer[4096];
        for (;;)
        {
            BOOL success = ReadFile(pipe, buffer, sizeof(buffer), &count, nullptr);
            reply.append(buffer, count);
            if (reply.size() > 65536) { CloseHandle(pipe); throw std::runtime_error("Management response exceeds 64 KiB"); }
            if (success || GetLastError() == ERROR_BROKEN_PIPE) break;
            if (GetLastError() != ERROR_MORE_DATA) { CloseHandle(pipe); throw std::runtime_error("Could not read management response"); }
        }
        const char acknowledgement = 6;
        WriteFile(pipe, &acknowledgement, 1, &count, nullptr);
        CloseHandle(pipe);
        auto response = nlohmann::json::parse(reply);
        std::cout << response.dump(2) << '\n';
        return response.value("ok", false) ? 0 : 1;
    }
    catch (const std::exception& ex) { std::cerr << ex.what() << '\n'; return 1; }
}
