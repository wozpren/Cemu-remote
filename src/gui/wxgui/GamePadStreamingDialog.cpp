// SPDX-License-Identifier: MPL-2.0 OR GPL-3.0-only
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include "GamePadStreamingDialog.h"
#include "../../../extensions/gamepad/sdk/control_pipe.h"
#include <iphlpapi.h>
#include <ws2tcpip.h>
#include <wx/button.h>
#include <wx/checkbox.h>
#include <wx/listbox.h>
#include <wx/msgdlg.h>
#include <wx/notebook.h>
#include <wx/panel.h>
#include <wx/scrolwin.h>
#include <wx/display.h>
#include <wx/sizer.h>
#include <wx/spinctrl.h>
#include <wx/stattext.h>
#include <wx/textctrl.h>
#include <wx/thread.h>
#include <rapidjson/document.h>
#include <rapidjson/prettywriter.h>
#include <rapidjson/stringbuffer.h>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <thread>
#include <string>
#include <type_traits>
#include <vector>
#include <algorithm>
#include <cmath>

namespace
{
using Json = rapidjson::Document;
wxDEFINE_EVENT(EVT_GAMEPAD_REPLY, wxThreadEvent);
wxString U(const char* text) { return wxString::FromUTF8(text); }
std::string Utf8(const wxString& text) { return text.ToStdString(wxConvUTF8); }
struct Handle
{
    HANDLE value;
    ~Handle() { if (value && value != INVALID_HANDLE_VALUE) CloseHandle(value); }
};
std::string Encode(const Json& doc)
{
    rapidjson::StringBuffer buffer;
    rapidjson::PrettyWriter<rapidjson::StringBuffer> writer(buffer);
    doc.Accept(writer);
    return {buffer.GetString(), buffer.GetSize()};
}
Json Decode(const std::string& text)
{
    Json doc;
    doc.Parse<rapidjson::kParseValidateEncodingFlag>(text.data(), text.size());
    if (doc.HasParseError() || !doc.IsObject()) throw std::runtime_error("无效的 JSON 数据");
    return doc;
}
std::string Text(const rapidjson::Value& doc, const char* key, const char* fallback = "")
{
    if (!doc.HasMember(key)) return fallback;
    if (!doc[key].IsString()) throw std::runtime_error(std::string(key) + " 必须是文本");
    return {doc[key].GetString(), doc[key].GetStringLength()};
}
bool Flag(const rapidjson::Value& doc, const char* key, bool fallback = false)
{
    if (!doc.HasMember(key)) return fallback;
    if (!doc[key].IsBool()) throw std::runtime_error(std::string(key) + " 必须是开关");
    return doc[key].GetBool();
}
unsigned Number(const rapidjson::Value& doc, const char* key, unsigned fallback)
{
    if (!doc.HasMember(key)) return fallback;
    if (!doc[key].IsUint()) throw std::runtime_error(std::string(key) + " 必须是正整数");
    return doc[key].GetUint();
}
template<class T> void Set(Json& doc, const char* key, T value)
{
    rapidjson::Value name(key, doc.GetAllocator());
    rapidjson::Value item;
    if constexpr (std::is_same_v<T, std::string>) item.SetString(value.data(), static_cast<rapidjson::SizeType>(value.size()), doc.GetAllocator());
    else if constexpr (std::is_same_v<T, bool>) item.SetBool(value);
    else item.SetUint(static_cast<unsigned>(value));
    if (doc.HasMember(key)) doc[key] = std::move(item);
    else doc.AddMember(name, item, doc.GetAllocator());
}
std::filesystem::path ConfigPath()
{
    const auto* local = _wgetenv(L"LOCALAPPDATA");
    if (!local) throw std::runtime_error("无法找到 Windows 用户配置目录");
    return std::filesystem::path(local) / L"CemuRemoteGamePad/config.json";
}
Json LoadConfig()
{
    const auto path = ConfigPath();
    if (!std::filesystem::exists(path)) { Json doc; doc.SetObject(); return doc; }
    if (std::filesystem::file_size(path) > 65536) throw std::runtime_error("config.json 超过 64 KiB，未读取或改写");
    std::ifstream file(path, std::ios::binary);
    if (!file) throw std::runtime_error("无法读取 config.json");
    return Decode(std::string(std::istreambuf_iterator<char>(file), {}));
}
void StoreConfig(const Json& doc)
{
    auto path = ConfigPath();
    std::filesystem::create_directories(path.parent_path());
    auto temporary = path;
    temporary += L".gui." + std::to_wstring(GetCurrentProcessId()) + L"." + std::to_wstring(GetTickCount64());
    const auto body = Encode(doc) + "\n";
    if (body.size() > 65536) throw std::runtime_error("配置超过 64 KiB，未保存");
    bool created{};
    try
    {
        {
            Handle file{CreateFileW(temporary.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr)};
            if (file.value == INVALID_HANDLE_VALUE) throw std::runtime_error("无法创建配置临时文件");
            created = true;
            DWORD written{};
            if (!WriteFile(file.value, body.data(), static_cast<DWORD>(body.size()), &written, nullptr) ||
                written != body.size() || !FlushFileBuffers(file.value)) throw std::runtime_error("无法完整保存配置，原文件未改写");
        }
        if (!MoveFileExW(temporary.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
            throw std::runtime_error("无法替换 config.json，原文件未改写");
    }
    catch (...) { if (created) DeleteFileW(temporary.c_str()); throw; }
}

std::string Request(const std::string& body, HANDLE stopping)
{
    const auto name = cgp_control::PipeName();
    Handle pipe{INVALID_HANDLE_VALUE};
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    do
    {
        pipe.value = CreateFileW(name.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, FILE_FLAG_OVERLAPPED, nullptr);
        if (pipe.value != INVALID_HANDLE_VALUE) break;
        const auto error = GetLastError();
        if (error != ERROR_PIPE_BUSY && error != ERROR_FILE_NOT_FOUND)
            throw std::runtime_error("无法访问串流管理服务，Windows 错误 " + std::to_string(error));
        if (WaitForSingleObject(stopping, 50) == WAIT_OBJECT_0) throw std::runtime_error("已关闭设置窗口");
    } while (std::chrono::steady_clock::now() < deadline);
    if (pipe.value == INVALID_HANDLE_VALUE)
        throw std::runtime_error("串流管理服务未连接。请启用插件并重启 Cemu；如果仍不可用，请查看 log.txt 的 Remote GamePad 错误。");
    DWORD mode = PIPE_READMODE_MESSAGE;
    if (!SetNamedPipeHandleState(pipe.value, &mode, nullptr, nullptr)) throw std::runtime_error("无法设置管理通信模式");
    Handle completed{CreateEventW(nullptr, TRUE, FALSE, nullptr)};
    if (!completed.value) throw std::runtime_error("无法创建管理通信事件");
    // Overlapped I/O keeps both a stalled service and a long PIN handshake off
    // the GUI thread. Cancellation is drained before its buffers are released.
    auto transfer = [&](void* bytes, DWORD count, bool writing)
    {
        ResetEvent(completed.value);
        OVERLAPPED io{};
        io.hEvent = completed.value;
        DWORD transferred{};
        BOOL success = writing ? WriteFile(pipe.value, bytes, count, nullptr, &io) : ReadFile(pipe.value, bytes, count, nullptr, &io);
        if (!success && GetLastError() != ERROR_IO_PENDING) throw std::runtime_error("串流管理通信中断，请刷新后重试");
        HANDLE events[] = {stopping, completed.value};
        const auto result = success ? WAIT_OBJECT_0 + 1 : WaitForMultipleObjects(2, events, FALSE, 20000);
        if (result != WAIT_OBJECT_0 + 1)
        {
            CancelIoEx(pipe.value, &io);
            GetOverlappedResult(pipe.value, &io, &transferred, TRUE);
            throw std::runtime_error("管理操作已取消或等待超时；操作结果请以刷新后的状态为准");
        }
        if (!GetOverlappedResult(pipe.value, &io, &transferred, FALSE)) throw std::runtime_error("串流管理通信失败，请刷新后重试");
        return transferred;
    };
    if (transfer(const_cast<char*>(body.data()), static_cast<DWORD>(body.size()), true) != body.size())
        throw std::runtime_error("管理请求未完整发送");
    char buffer[16384];
    const auto count = transfer(buffer, sizeof(buffer), false);
    char acknowledgement = 6;
    transfer(&acknowledgement, 1, true);
    return {buffer, count};
}

wxString LocalAddresses()
{
    ULONG size = 16384;
    std::vector<unsigned char> storage(size);
    auto adapters = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(storage.data());
    ULONG result = GetAdaptersAddresses(AF_INET, GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER, nullptr, adapters, &size);
    if (result == ERROR_BUFFER_OVERFLOW)
    {
        storage.resize(size);
        adapters = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(storage.data());
        result = GetAdaptersAddresses(AF_INET, GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER, nullptr, adapters, &size);
    }
    if (result != NO_ERROR) return U("无法读取本机地址，请在 Windows 网络设置中查看 IPv4 地址。");
    wxString output;
    for (auto* adapter = adapters; adapter; adapter = adapter->Next)
    {
        if (adapter->OperStatus != IfOperStatusUp || adapter->IfType == IF_TYPE_SOFTWARE_LOOPBACK) continue;
        for (auto* address = adapter->FirstUnicastAddress; address; address = address->Next)
        {
            const auto* ip = reinterpret_cast<sockaddr_in*>(address->Address.lpSockaddr);
            const auto host = ntohl(ip->sin_addr.s_addr);
            // Exclude link-local and common proxy/TUN testing addresses. List
            // every remaining adapter by name rather than guessing a LAN route.
            if ((host >> 16) == 0xA9FE || (host & 0xFFFE0000) == 0xC6120000) continue;
            wchar_t value[INET_ADDRSTRLEN]{};
            if (InetNtopW(AF_INET, &ip->sin_addr, value, INET_ADDRSTRLEN))
                output += wxString(adapter->FriendlyName) + ": " + value + "\n";
        }
    }
    return output.empty() ? U("未找到可用的 IPv4 地址，请连接局域网后重新打开此窗口。"): output;
}

struct Reply { std::string command, body, error; };
class StreamingDialog final : public wxDialog
{
public:
    explicit StreamingDialog(wxWindow* parent);
    ~StreamingDialog() override;
private:
    void Work();
    void Enqueue(Json request);
    void Receive(wxThreadEvent& event);
    void Save();
    void Pair();
    void Unpair();
    void FillList(wxListBox* list, const rapidjson::Value& values, bool pending);
    void Buttons(bool online);
    wxStaticText *m_status{}, *m_operation{}, *m_saved{};
    wxTextCtrl *m_error{}, *m_pin{}, *m_clientName{}, *m_name{}, *m_address{};
    wxCheckBox *m_enabled{}, *m_experimental{}, *m_localAudio{}, *m_touch{}, *m_trace{};
    wxSpinCtrl *m_port{}, *m_timeout{};
    wxSpinCtrlDouble* m_bitrate{};
    wxListBox *m_pending{}, *m_clients{};
    wxButton *m_pair{}, *m_unpair{}, *m_stop{};
    std::vector<std::string> m_pendingIds, m_clientIds;
    bool m_busy{}, m_online{};
    std::string m_commandError;
    Handle m_stopping{CreateEventW(nullptr, TRUE, FALSE, nullptr)};
    std::mutex m_mutex;
    std::condition_variable m_changed;
    std::deque<std::string> m_requests;
    std::jthread m_worker;
};

StreamingDialog::StreamingDialog(wxWindow* parent) : wxDialog(parent, wxID_ANY, U("GamePad 串流设置"), wxDefaultPosition, wxDefaultSize, wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER)
{
    if (!m_stopping.value) throw std::runtime_error("无法创建管理线程退出事件");
    auto* outer = new wxBoxSizer(wxVERTICAL);
    auto* tabs = new wxNotebook(this, wxID_ANY);
    auto* connection = new wxScrolledWindow(tabs);
    auto* settings = new wxScrolledWindow(tabs);
    auto* help = new wxScrolledWindow(tabs);
    for (auto* page : {connection, settings, help}) page->SetScrollRate(0, FromDIP(12));
    tabs->AddPage(connection, U("连接与配对"));
    tabs->AddPage(settings, U("串流设置"));
    tabs->AddPage(help, U("使用说明"));
    auto note = [](wxWindow* page, wxSizer* sizer, const char* text)
    {
        auto* label = new wxStaticText(page, wxID_ANY, U(text));
        label->Wrap(page->FromDIP(620));
        sizer->Add(label, 0, wxEXPAND | wxALL, page->FromDIP(6));
    };
    auto* content = new wxBoxSizer(wxVERTICAL);
    m_status = new wxStaticText(connection, wxID_ANY, U("正在连接当前 Cemu 的串流服务…"), wxDefaultPosition, FromDIP(wxSize(-1, 95)));
    content->Add(m_status, 0, wxEXPAND | wxALL, FromDIP(6));
    m_error = new wxTextCtrl(connection, wxID_ANY, wxEmptyString, wxDefaultPosition, FromDIP(wxSize(-1, 65)), wxTE_MULTILINE | wxTE_READONLY);
    content->Add(m_error, 0, wxEXPAND | wxALL, FromDIP(6));
    auto* stopRow = new wxBoxSizer(wxHORIZONTAL);
    m_stop = new wxButton(connection, wxID_ANY, U("停止当前串流"));
    auto* refresh = new wxButton(connection, wxID_ANY, U("刷新状态"));
    stopRow->Add(m_stop, 0, wxALL, FromDIP(6));
    stopRow->Add(refresh, 0, wxALL, FromDIP(6));
    content->Add(stopRow);
    note(connection, content, "手机与电脑连接同一局域网。在安卓 Moonlight 中手动添加下方地址；多个地址时，选手机能访问的以太网或 Wi-Fi 地址。");
    auto* addresses = new wxTextCtrl(connection, wxID_ANY, LocalAddresses(), wxDefaultPosition, FromDIP(wxSize(-1, 55)), wxTE_MULTILINE | wxTE_READONLY);
    content->Add(addresses, 0, wxEXPAND | wxALL, FromDIP(6));
    note(connection, content, "待配对请求：在手机上发起配对，选中对应设备，再输入手机显示的四位 PIN。");
    m_pending = new wxListBox(connection, wxID_ANY, wxDefaultPosition, FromDIP(wxSize(-1, 65)));
    content->Add(m_pending, 1, wxEXPAND | wxALL, FromDIP(6));
    auto* pairRow = new wxBoxSizer(wxHORIZONTAL);
    m_clientName = new wxTextCtrl(connection, wxID_ANY, U("安卓设备"));
    m_clientName->SetHint(U("配对后显示的设备名称"));
    m_pin = new wxTextCtrl(connection, wxID_ANY, wxEmptyString, wxDefaultPosition, FromDIP(wxSize(85, -1)), wxTE_PASSWORD);
    m_pin->SetMaxLength(4);
    m_pin->SetHint("PIN");
    m_pair = new wxButton(connection, wxID_ANY, U("确认配对"));
    pairRow->Add(m_clientName, 1, wxALL, FromDIP(6));
    pairRow->Add(m_pin, 0, wxALL, FromDIP(6));
    pairRow->Add(m_pair, 0, wxALL, FromDIP(6));
    content->Add(pairRow, 0, wxEXPAND);
    note(connection, content, "已配对设备：");
    m_clients = new wxListBox(connection, wxID_ANY, wxDefaultPosition, FromDIP(wxSize(-1, 65)));
    content->Add(m_clients, 1, wxEXPAND | wxALL, FromDIP(6));
    m_unpair = new wxButton(connection, wxID_ANY, U("移除选中设备的配对"));
    content->Add(m_unpair, 0, wxALL, FromDIP(6));
    m_operation = new wxStaticText(connection, wxID_ANY, wxEmptyString);
    content->Add(m_operation, 0, wxEXPAND | wxALL, FromDIP(6));
    connection->SetSizer(content);

    auto* config = new wxBoxSizer(wxVERTICAL);
    m_enabled = new wxCheckBox(settings, wxID_ANY, U("启用 GamePad 串流（保存后重启 Cemu）"));
    m_experimental = new wxCheckBox(settings, wxID_ANY, U("允许使用尚未通过验证的显卡 / 驱动组合（实验性）"));
    config->Add(m_enabled, 0, wxALL, FromDIP(6));
    config->Add(m_experimental, 0, wxALL, FromDIP(6));
    note(settings, config, "当前为候选版本，硬件与安卓端串流仍需实机验证。仅在你准备参与验证时勾选实验性选项。");
    auto* grid = new wxFlexGridSizer(2, FromDIP(8), FromDIP(10));
    grid->AddGrowableCol(1);
    auto field = [&](const char* label, wxWindow* control)
    {
        grid->Add(new wxStaticText(settings, wxID_ANY, U(label)), 0, wxALIGN_CENTER_VERTICAL);
        grid->Add(control, 1, wxEXPAND);
    };
    m_name = new wxTextCtrl(settings, wxID_ANY); field("Moonlight 中显示的名称", m_name);
    m_address = new wxTextCtrl(settings, wxID_ANY); field("监听 IPv4 地址", m_address);
    m_port = new wxSpinCtrl(settings, wxID_ANY); m_port->SetRange(1029, 65514); field("基础端口", m_port);
    m_bitrate = new wxSpinCtrlDouble(settings, wxID_ANY); m_bitrate->SetRange(1, 100); m_bitrate->SetDigits(3); field("视频码率上限（Mbps）", m_bitrate);
    m_timeout = new wxSpinCtrl(settings, wxID_ANY); m_timeout->SetRange(100, 10000); field("输入超时（毫秒）", m_timeout);
    config->Add(grid, 0, wxEXPAND | wxALL, FromDIP(10));
    note(settings, config, "监听地址通常保持 0.0.0.0。默认基础端口 47989；其他端口沿用 Moonlight 协议偏移。不会自动修改防火墙。若电脑同时运行 Sunshine，请避免端口冲突。");
    note(settings, config, "画质：分辨率和帧率在手机 Moonlight 中设置，最高 1920×1080、60 fps；选择 H.264、SDR、双声道。实际初始视频码率取客户端请求和此处上限中的较小值，网络拥塞时还会下降。");
    m_localAudio = new wxCheckBox(settings, wxID_ANY, U("电脑也播放 GamePad 声音"));
    m_touch = new wxCheckBox(settings, wxID_ANY, U("把绝对鼠标点击映射为 GamePad 触摸"));
    m_trace = new wxCheckBox(settings, wxID_ANY, U("记录逐帧延迟日志（用于排查问题）"));
    for (auto* control : {m_localAudio, m_touch, m_trace}) config->Add(control, 0, wxALL, FromDIP(6));
    auto* save = new wxButton(settings, wxID_ANY, U("保存设置"));
    config->Add(save, 0, wxALL, FromDIP(6));
    m_saved = new wxStaticText(settings, wxID_ANY, U("设置保存后，下次启动 Cemu 时生效。停止串流按钮只结束当前连接。"));
    m_saved->Wrap(FromDIP(620));
    config->Add(m_saved, 0, wxEXPAND | wxALL, FromDIP(6));
    settings->SetSizer(config);
    auto* instructions = new wxBoxSizer(wxVERTICAL);
    note(help, instructions, "1. 在“串流设置”中启用插件，保存并重启 Cemu。当前候选版本需要明确允许未验证的显卡 / 驱动组合。");
    note(help, instructions, "2. 在安卓 Moonlight 中添加电脑局域网地址并发起配对，回到“连接与配对”，选中请求并输入手机上的 PIN。配对不需要先启动游戏。");
    note(help, instructions, "3. Cemu 选项 → 常规设置（General settings）→ 图形：选择 Vulkan 和 NVIDIA 显卡。");
    note(help, instructions, "4. Cemu 选项 → 输入设置（Input settings）：选择一个 Wii U GamePad，添加 Remote GamePad → Moonlight GamePad，使用默认映射。该槽只能有这一个输入源；远程源只能绑定一个槽。此窗口不会改写已有手柄配置。");
    note(help, instructions, "5. 在 Cemu 中启动游戏，再在 Moonlight 中打开 Cemu GamePad。手机接收的是 GamePad 画面；实际连接和输入能力会显示在状态中。");
    note(help, instructions, "6. 手机触摸需使用绝对触控模式。体感、震动是否可用，取决于 Moonlight 实际上报的能力，请结合状态和游戏实测确认。");
    note(help, instructions, "停止当前串流后，可从 Moonlight 重新打开应用。禁用插件需取消“启用 GamePad 串流”、保存并重启 Cemu。");
    help->SetSizer(instructions);
    outer->Add(tabs, 1, wxEXPAND | wxALL, FromDIP(8));
    outer->Add(CreateButtonSizer(wxCLOSE), 0, wxALIGN_RIGHT | wxALL, FromDIP(8));
    SetSizer(outer);
    const auto available = wxDisplay(parent).GetClientArea().GetSize();
    const auto preferred = FromDIP(wxSize(720, 760));
    SetSize(wxSize(std::min(preferred.x, available.x), std::min(preferred.y, available.y)));
    SetMinSize(FromDIP(wxSize(680, 400)));
    Layout();
    for (auto* page : {connection, settings, help}) page->FitInside();
    CentreOnParent();
    try
    {
        auto doc = LoadConfig();
        m_enabled->SetValue(Flag(doc, "enabled"));
        if (!m_enabled->GetValue()) tabs->SetSelection(1);
        m_experimental->SetValue(Flag(doc, "allow_unverified_backend"));
        m_localAudio->SetValue(Flag(doc, "local_gamepad_audio", true));
        m_touch->SetValue(Flag(doc, "absolute_pointer_touch", true));
        m_trace->SetValue(Flag(doc, "trace_timeline"));
        m_name->SetValue(U(Text(doc, "name", "Cemu GamePad").c_str()));
        m_address->SetValue(U(Text(doc, "bind_address", "0.0.0.0").c_str()));
        const unsigned port = Number(doc, "port", 47989), bitrate = Number(doc, "max_bitrate_kbps", 100000), timeout = Number(doc, "input_timeout_ms", 1500);
        if (port < 1029 || port > 65514 || bitrate < 1000 || bitrate > 100000 || timeout < 100 || timeout > 10000)
            throw std::runtime_error("配置中的端口、码率或输入超时超出范围，请先修复 config.json");
        m_port->SetValue(static_cast<int>(port));
        m_bitrate->SetValue(bitrate / 1000.0);
        m_timeout->SetValue(static_cast<int>(timeout));
    }
    catch (const std::exception& ex)
    {
        save->Disable();
        tabs->SetSelection(1);
        m_saved->SetLabel(U("配置读取失败，保存已禁用，以免覆盖原设置：") + U(ex.what()));
        m_saved->Wrap(FromDIP(620));
    }
    Bind(EVT_GAMEPAD_REPLY, &StreamingDialog::Receive, this);
    Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { EndModal(wxID_CLOSE); }, wxID_CLOSE);
    save->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { Save(); });
    refresh->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { Json doc; doc.SetObject(); Set(doc, "command", std::string("overview")); Enqueue(std::move(doc)); });
    m_pair->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { Pair(); });
    m_unpair->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { Unpair(); });
    m_stop->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { Json doc; doc.SetObject(); Set(doc, "command", std::string("stop")); Enqueue(std::move(doc)); });
    m_pending->Bind(wxEVT_LISTBOX, [this](wxCommandEvent&) { Buttons(m_online); });
    m_clients->Bind(wxEVT_LISTBOX, [this](wxCommandEvent&) { Buttons(m_online); });
    Buttons(false);
    m_worker = std::jthread([this] { Work(); });
}
StreamingDialog::~StreamingDialog()
{
    SetEvent(m_stopping.value);
    m_changed.notify_all();
    if (m_worker.joinable()) m_worker.join();
    // No worker may queue another event after children begin destruction.
    DeletePendingEvents();
}
void StreamingDialog::Buttons(bool online)
{
    m_online = online;
    m_pair->Enable(online && !m_busy && m_pending->GetSelection() != wxNOT_FOUND);
    m_unpair->Enable(online && !m_busy && m_clients->GetSelection() != wxNOT_FOUND);
    m_stop->Enable(online && !m_busy);
}
void StreamingDialog::Enqueue(Json request)
{
    const auto command = Text(request, "command");
    if (m_busy) return;
    Set(request, "process_id", GetCurrentProcessId());
    if (command != "overview")
    {
        m_busy = true;
        m_commandError.clear();
        m_operation->SetLabel(U("正在处理，请稍候…"));
        Buttons(m_online);
    }
    {
        std::lock_guard lock(m_mutex);
        // Repeated refresh clicks need only one pending snapshot.
        if (command == "overview" && !m_requests.empty()) return;
        m_requests.push_back(Encode(request));
    }
    m_changed.notify_one();
}
void StreamingDialog::Work()
{
    while (WaitForSingleObject(m_stopping.value, 0) != WAIT_OBJECT_0)
    {
        Reply reply;
        try
        {
            std::string request;
            {
                std::lock_guard lock(m_mutex);
                if (!m_requests.empty()) { request = std::move(m_requests.front()); m_requests.pop_front(); }
            }
            if (request.empty())
            {
                Json doc; doc.SetObject();
                Set(doc, "command", std::string("overview"));
                Set(doc, "process_id", GetCurrentProcessId());
                request = Encode(doc);
            }
            reply.command = Text(Decode(request), "command");
            reply.body = Request(request, m_stopping.value);
        }
        catch (const std::exception& ex) { reply.error = ex.what(); }
        if (WaitForSingleObject(m_stopping.value, 0) == WAIT_OBJECT_0) break;
        auto* event = new wxThreadEvent(EVT_GAMEPAD_REPLY);
        event->SetPayload(reply);
        wxQueueEvent(this, event);
        std::unique_lock lock(m_mutex);
        // Immediately refresh after a mutation; otherwise poll once a second.
        if (reply.command == "overview")
            m_changed.wait_for(lock, std::chrono::seconds(1), [this] { return !m_requests.empty() || WaitForSingleObject(m_stopping.value, 0) == WAIT_OBJECT_0; });
    }
}
void StreamingDialog::FillList(wxListBox* list, const rapidjson::Value& values, bool pending)
{
    if (!values.IsArray()) throw std::runtime_error("串流服务返回了无效的设备列表");
    auto& ids = pending ? m_pendingIds : m_clientIds;
    const auto selection = list->GetSelection();
    const std::string selected = selection != wxNOT_FOUND ? ids.at(selection) : "";
    ids.clear(); list->Clear();
    for (const auto& value : values.GetArray())
    {
        if (!value.IsObject()) throw std::runtime_error("串流服务返回了无效的设备信息");
        const auto id = Text(value, pending ? "id" : "uuid");
        if (id.empty()) throw std::runtime_error("串流设备缺少标识符");
        ids.push_back(id);
        auto label = U(Text(value, "name", "未命名设备").c_str());
        label += "  (" + U((pending ? Text(value, "address") : id).c_str()) + ")";
        if (!pending && !Flag(value, "enabled", true)) label += U(" [已禁用]");
        list->Append(label);
        if (selected == id) list->SetSelection(static_cast<int>(ids.size() - 1));
    }
}
void StreamingDialog::Receive(wxThreadEvent& event)
{
    const auto reply = event.GetPayload<Reply>();
    const bool mutation = reply.command != "overview";
    if (mutation) m_busy = false;
    try
    {
        if (!reply.error.empty()) throw std::runtime_error(reply.error);
        auto doc = Decode(reply.body);
        if (!Flag(doc, "ok")) throw std::runtime_error(Text(doc, "error", "操作未成功：PIN 错误、请求已过期或设备已移除，请刷新后重试。"));
        if (Number(doc, "process_id", 0) != GetCurrentProcessId()) throw std::runtime_error("管理服务属于另一个 Cemu，请关闭其他 Cemu 后重试。");
        if (mutation)
        {
            if (reply.command == "pair") m_pin->Clear();
            m_operation->SetLabel(reply.command == "pair" ? U("配对成功。") : reply.command == "stop" ? U("当前串流已停止，可从 Moonlight 重新连接。") : U("设备配对已移除。"));
            Buttons(true);
            return;
        }
        const bool stopped = Flag(doc, "network_stopped");
        auto yes = [](bool value) { return value ? U("是") : U("否"); };
        wxString status = stopped ? U("网络服务已停止，需要重启 Cemu。\n") : U("串流服务已连接。\n");
        status += U("游戏：") + yes(Flag(doc, "game_running")) + U("    显卡就绪：") + yes(Flag(doc, "device_ready")) + U("    输入已绑定：") + yes(Flag(doc, "input_bound"));
        status += U("\n串流：") + yes(Flag(doc, "streaming")) + U("    客户端输入在线：") + yes(Flag(doc, "connected"));
        status += U("    生效码率上限：") + wxString::Format("%.3g Mbps", Number(doc, "max_bitrate_kbps", 100000) / 1000.0);
        status += U("\n触摸：") + yes(Flag(doc, "touch")) + U("    陀螺仪：") + yes(Flag(doc, "gyro")) + U("    加速度：") + yes(Flag(doc, "accelerometer")) + U("    震动：") + yes(Flag(doc, "rumble"));
        status += Text(doc, "backend_key").empty() ? U("\n硬件组合：等待 Vulkan 游戏启动后识别。") :
            Text(doc, "backend_validation") == "passed" ? U("\n硬件组合：已有验证记录。") : U("\n硬件组合：暂无验证记录，仍需实机验证。");
        m_status->SetLabel(status);
        auto error = Text(doc, "last_error");
        if (!m_commandError.empty()) error = m_commandError + (error.empty() ? "" : "\n" + error);
        m_error->ChangeValue(U(error.c_str()));
        if (!doc.HasMember("requests") || !doc.HasMember("clients")) throw std::runtime_error("串流管理接口不完整，请使用同一构建包中的 Cemu 和插件。");
        FillList(m_pending, doc["requests"], true);
        FillList(m_clients, doc["clients"], false);
        Buttons(!stopped);
        Layout();
    }
    catch (const std::exception& ex)
    {
        if (mutation)
        {
            m_commandError = ex.what();
            m_operation->SetLabel(U("操作未完成，请查看上方错误并刷新。"));
        }
        else m_status->SetLabel(U("当前 Cemu 的串流管理服务不可用。"));
        m_error->ChangeValue(U(ex.what()));
        Buttons(false);
    }
}
void StreamingDialog::Save()
{
    try
    {
        auto name = Utf8(m_name->GetValue().Strip(wxString::both));
        auto address = Utf8(m_address->GetValue().Strip(wxString::both));
        IN_ADDR parsed{};
        if (name.empty() || name.size() > 128 || name.find('\0') != std::string::npos) throw std::runtime_error("主机名称需要 1–128 个 UTF-8 字节");
        if (InetPtonA(AF_INET, address.c_str(), &parsed) != 1) throw std::runtime_error("请输入有效的监听 IPv4 地址，例如 0.0.0.0");
        // Re-read immediately before saving to retain unrelated configuration
        // keys. Malformed existing files are never replaced by defaults.
        auto doc = LoadConfig();
        Set(doc, "enabled", m_enabled->GetValue()); Set(doc, "name", name);
        Set(doc, "bind_address", address); Set(doc, "port", m_port->GetValue());
        Set(doc, "max_bitrate_kbps", static_cast<unsigned>(std::lround(m_bitrate->GetValue() * 1000)));
        Set(doc, "input_timeout_ms", m_timeout->GetValue());
        Set(doc, "allow_unverified_backend", m_experimental->GetValue());
        Set(doc, "local_gamepad_audio", m_localAudio->GetValue());
        Set(doc, "absolute_pointer_touch", m_touch->GetValue());
        Set(doc, "trace_timeline", m_trace->GetValue());
        StoreConfig(doc);
        m_saved->SetLabel(U("设置已保存。请关闭并重新打开 Cemu 后生效；当前串流继续使用启动时的设置。"));
        m_saved->Wrap(FromDIP(620));
        Layout();
    }
    catch (const std::exception& ex) { wxMessageBox(U(ex.what()), U("设置未保存"), wxOK | wxICON_ERROR, this); }
}
void StreamingDialog::Pair()
{
    const auto selection = m_pending->GetSelection();
    if (!m_online || m_busy || selection == wxNOT_FOUND) return;
    const auto pin = Utf8(m_pin->GetValue());
    const auto name = Utf8(m_clientName->GetValue().Strip(wxString::both));
    if (pin.size() != 4 || pin.find_first_not_of("0123456789") != std::string::npos || name.empty() || name.size() > 128)
    { wxMessageBox(U("请输入四位数字 PIN，以及 1–128 个 UTF-8 字节的设备名称。"), U("检查配对信息"), wxOK | wxICON_INFORMATION, this); return; }
    Json doc; doc.SetObject(); Set(doc, "command", std::string("pair"));
    Set(doc, "id", m_pendingIds.at(selection)); Set(doc, "pin", pin); Set(doc, "name", name);
    Enqueue(std::move(doc));
}
void StreamingDialog::Unpair()
{
    const auto selection = m_clients->GetSelection();
    if (!m_online || m_busy || selection == wxNOT_FOUND) return;
    // The confirmation dialog pumps GUI events, including list refreshes.
    // Keep the selected identity, not an index into a changing device list.
    const auto id = m_clientIds.at(selection);
    if (wxMessageBox(U("移除此设备的配对？该设备再次连接时需要重新输入 PIN。"), U("移除配对"), wxYES_NO | wxNO_DEFAULT | wxICON_QUESTION, this) != wxYES) return;
    Json doc; doc.SetObject(); Set(doc, "command", std::string("unpair"));
    Set(doc, "id", id); Enqueue(std::move(doc));
}
}

void ShowGamePadStreamingSettings(wxWindow* parent)
{
    try { StreamingDialog dialog(parent); dialog.ShowModal(); }
    catch (const std::exception& ex) { wxMessageBox(U(ex.what()), U("无法打开串流设置"), wxOK | wxICON_ERROR, parent); }
}
