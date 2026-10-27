// SPDX-License-Identifier: GPL-3.0-only
#include "runtime.h"
#include "logging.h"
#include "host_state.h"
#include <ws2tcpip.h>
#include <mswsock.h>
#include <mstcpip.h>
#include <iphlpapi.h>
#include <qos2.h>
#include <algorithm>
#include <iomanip>
#include <sstream>

using namespace std::literals;
namespace platf
{
void set_thread_name(std::string_view name)
{
    std::wstring wide(name.begin(), name.end());
    if (FAILED(SetThreadDescription(GetCurrentThread(), wide.c_str())))
        BOOST_LOG(warning) << "Could not name GamePad worker " << name;
}
void adjust_thread_priority(thread_priority_e priority)
{
    auto value = priority == thread_priority_e::critical ? THREAD_PRIORITY_HIGHEST :
                 priority == thread_priority_e::high ? THREAD_PRIORITY_ABOVE_NORMAL :
                 priority == thread_priority_e::low ? THREAD_PRIORITY_BELOW_NORMAL : THREAD_PRIORITY_NORMAL;
    if (!SetThreadPriority(GetCurrentThread(), value)) BOOST_LOG(warning) << "Could not set GamePad worker priority: " << GetLastError();
}
std::string from_sockaddr(const sockaddr* address)
{
    std::array<char, INET6_ADDRSTRLEN> text{};
    if (address->sa_family == AF_INET)
        inet_ntop(AF_INET, &reinterpret_cast<const sockaddr_in*>(address)->sin_addr, text.data(), text.size());
    else if (address->sa_family == AF_INET6)
        inet_ntop(AF_INET6, &reinterpret_cast<const sockaddr_in6*>(address)->sin6_addr, text.data(), text.size());
    return text.data();
}
std::pair<uint16_t, std::string> from_sockaddr_ex(const sockaddr* address)
{
    uint16_t port = address->sa_family == AF_INET ? reinterpret_cast<const sockaddr_in*>(address)->sin_port :
                    address->sa_family == AF_INET6 ? reinterpret_cast<const sockaddr_in6*>(address)->sin6_port : 0;
    return {port, from_sockaddr(address)};
}
std::string get_mac_address(const std::string_view& address)
{
    ULONG length{};
    GetAdaptersAddresses(AF_UNSPEC, 0, nullptr, nullptr, &length);
    std::vector<uint8_t> storage(length);
    auto* adapters = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(storage.data());
    if (GetAdaptersAddresses(AF_UNSPEC, 0, nullptr, adapters, &length) != NO_ERROR)
    {
        BOOST_LOG(warning) << "Could not enumerate host adapters for Moonlight discovery";
        return {};
    }
    for (auto* adapter = adapters; adapter; adapter = adapter->Next)
        for (auto* unicast = adapter->FirstUnicastAddress; unicast; unicast = unicast->Next)
            if (address == from_sockaddr(unicast->Address.lpSockaddr) && adapter->PhysicalAddressLength)
            {
                std::ostringstream result;
                result << std::hex << std::setfill('0');
                for (ULONG i = 0; i < adapter->PhysicalAddressLength; ++i)
                { if (i) result << ':'; result << std::setw(2) << unsigned(adapter->PhysicalAddress[i]); }
                return result.str();
            }
    return {};
}

// This is Sunshine's actual WSASendMsg/UDP segmentation path. Its sockets are
// owned by this DLL; neither raw frames nor encoded packets go to another host.
#include "windows_transport.inc"

class QosFlow final : public deinit_t
{
public:
    HANDLE handle{};
    SOCKET socket{};
    QOS_FLOWID flow{};
    ~QosFlow() override
    {
        if (flow) QOSRemoveSocketFromFlow(handle, socket, flow, 0);
        if (handle) QOSCloseHandle(handle);
    }
};
std::unique_ptr<deinit_t> enable_socket_qos(uintptr_t socket, boost::asio::ip::address& address,
                                         uint16_t port, qos_data_type_e type, bool tagging)
{
    auto result = std::make_unique<QosFlow>();
    result->socket = static_cast<SOCKET>(socket);
    QOS_VERSION version{1, 0};
    if (!QOSCreateHandle(&version, &result->handle))
    {
        BOOST_LOG(warning) << "Windows qWAVE QoS is unavailable: " << GetLastError();
        return {};
    }
    auto endpoint = boost::asio::ip::udp::endpoint(address, port);
    auto traffic = type == qos_data_type_e::audio ? QOSTrafficTypeVoice : QOSTrafficTypeAudioVideo;
    if (!QOSAddSocketToFlow(result->handle, result->socket, endpoint.data(), traffic, QOS_NON_ADAPTIVE_FLOW, &result->flow))
    {
        BOOST_LOG(warning) << "Could not apply Windows stream QoS: " << GetLastError();
        return {};
    }
    if (!tagging)
    {
        DWORD zero{};
        if (!QOSSetFlow(result->handle, result->flow, QOSSetOutgoingDSCPValue, sizeof(zero), &zero, 0, nullptr))
            BOOST_LOG(warning) << "Could not disable DSCP marking: " << GetLastError();
    }
    return result;
}
class PacketTimer final : public high_precision_timer
{
public:
    PacketTimer() : m_timer(CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS)) {}
    ~PacketTimer() override { if (m_timer) CloseHandle(m_timer); }
    explicit operator bool() override { return m_timer != nullptr; }
    void sleep_for(const std::chrono::nanoseconds& duration) override
    {
        LARGE_INTEGER due{};
        due.QuadPart = -std::max<int64_t>(1, duration.count() / 100);
        if (!SetWaitableTimerEx(m_timer, &due, 0, nullptr, nullptr, nullptr, 0) || WaitForSingleObject(m_timer, INFINITE) != WAIT_OBJECT_0)
            if (cgp::active) cgp::active->Error("GameStream packet pacing timer failed");
    }
private:
    HANDLE m_timer{};
};
std::unique_ptr<high_precision_timer> create_high_precision_timer() { return std::make_unique<PacketTimer>(); }
uint32_t get_capabilities() { return 0x01u | 0x02u; }
void streaming_will_start() { BOOST_LOG(info) << "Moonlight media connection is active"; }
void streaming_will_stop()
{
    if (cgp::active) cgp::active->StopMedia();
    BOOST_LOG(info) << "Moonlight media connection stopped; all GamePad input was released";
}
}
