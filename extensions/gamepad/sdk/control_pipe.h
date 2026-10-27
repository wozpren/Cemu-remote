// SPDX-License-Identifier: MPL-2.0 OR GPL-3.0-only
#pragma once
#include <windows.h>
#include <sddl.h>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <vector>

namespace cgp_control
{
inline std::wstring UserSid()
{
    HANDLE token{};
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) throw std::runtime_error("Could not read current user token");
    DWORD count{};
    GetTokenInformation(token, TokenUser, nullptr, 0, &count);
    std::vector<uint8_t> storage(count);
    if (!GetTokenInformation(token, TokenUser, storage.data(), count, &count))
    { CloseHandle(token); throw std::runtime_error("Could not read current user SID"); }
    CloseHandle(token);
    LPWSTR text{};
    if (!ConvertSidToStringSidW(reinterpret_cast<TOKEN_USER*>(storage.data())->User.Sid, &text))
        throw std::runtime_error("Could not format current user SID");
    std::wstring sid(text);
    LocalFree(text);
    return sid;
}
inline std::wstring PipeName() { return L"\\\\.\\pipe\\CemuRemoteGamePad." + UserSid(); }
}
