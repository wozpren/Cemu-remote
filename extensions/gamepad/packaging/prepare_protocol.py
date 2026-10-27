# SPDX-License-Identifier: GPL-3.0-only
"""Materialize the pinned Sunshine transport fork. This does not compile or run it."""
from pathlib import Path
import re
import shutil

ROOT = Path(__file__).resolve().parents[1]
SOURCE = ROOT / "host-core/upstream/src"
DEST = ROOT / "host-core/protocol/src"
DEST.mkdir(parents=True, exist_ok=True)

def write_generated(path, text):
    # Preserve timestamps for unchanged generated headers so incremental builds
    # do not recompile every transport unit after a one-file compatibility fix.
    if not path.exists() or path.read_text(encoding="utf-8") != text:
        path.write_text(text, encoding="utf-8")

FILES = ["crypto.cpp", "crypto.h", "network.cpp", "network.h", "nvhttp.cpp", "nvhttp.h",
         "rtsp.cpp", "rtsp.h", "stream.cpp", "stream.h", "file_handler.cpp", "file_handler.h",
         "utility.h", "uuid.h", "string_hash.h", "sync.h", "thread_safe.h", "thread_pool.h",
         "task_pool.h", "stat_trackers.h", "move_by_copy.h"]

def replace_once(text, old, new):
    if text.count(old) != 1:
        raise RuntimeError(f"Pinned upstream context changed: {old[:100]!r}")
    return text.replace(old, new, 1)

def replace_function(text, signature, replacement):
    start = text.index(signature)
    opening = text.index("{", start)
    # These entry points contain ordinary braces; tokenize strings/comments first
    # so format strings and protocol literals cannot terminate the function early.
    tokens = re.finditer(r'"(?:\\.|[^"\\])*"|\'(?:\\.|[^\'\\])*\'|//[^\n]*|/\*[\s\S]*?\*/|[{}]', text[opening:])
    depth = 0
    for token in tokens:
        if token.group() == "{":
            depth += 1
        elif token.group() == "}":
            depth -= 1
            if depth == 0:
                return text[:start] + replacement + text[opening + token.end():]
    raise RuntimeError(f"Unterminated upstream function: {signature}")

for name in FILES:
    text = (SOURCE / name).read_text(encoding="utf-8")
    if name in ("nvhttp.cpp", "stream.cpp"):
        for header in ("display_device.h", "process.h", "system_tray.h"):
            text = text.replace(f'#include "{header}"', '')
        text = text.replace('#include "config.h"', '#include "config.h"\n#include "host_state.h"')
        text = text.replace("proc::proc.running()", "cemu_host::app_running()")
    if name == "nvhttp.cpp":
        text = text.replace(".config.reuse_address = true;", ".config.reuse_address = false;")
        text = text.replace("    proc::refresh(config::stream.file_apps);", "")
        text = text.replace("proc::proc.get_apps()", "cemu_host::apps()")
        text = text.replace("proc::proc.execute((int) appid, launch_session)", "cemu_host::launch((int) appid)")
        text = text.replace("proc::proc.terminate();", "cemu_host::cancel();")
        text = text.replace("display_device::revert_configuration();", "cemu_host::release_failed_launch();")
        text = text.replace("display_device::configure_display(config::video, *launch_session);", "")
        text = text.replace("Failed to initialize video capture/encoding. Is a display connected and turned on?",
                            "Cemu GamePad GPU backend is not available; consult the Cemu log")
        text = replace_once(text, "    if (appid > 0) {", "    if (appid != 1) {\n      tree.put(\"root.<xmlattr>.status_code\", 404);\n      tree.put(\"root.gamesession\", 0);\n      return;\n    }\n    if (appid > 0) {")
        text = replace_once(text, "    if (current_appid == 0) {", "    if (current_appid == 0 || rtsp_stream::session_count() != 0) {")
        text = replace_function(text, "  void appasset(resp_https_t response, req_https_t request)",
            '  void appasset(resp_https_t response, req_https_t request)\n  {\n'
            '    response->write(SimpleWeb::StatusCode::client_error_not_found);\n'
            '    response->close_connection_after_response = true;\n  }')
        # The embedded core is LAN-only. Reject before accepting certificates,
        # authorizing a launch, or disclosing paired-client state.
        for needle in ("    print_req<T>(request);", "    print_req<SunshineHTTPS>(request);"):
            text = text.replace(needle, needle + "\n    if (net::from_address(request->remote_endpoint().address().to_string()) == net::WAN) { response->write(SimpleWeb::StatusCode::client_error_forbidden); return; }")
    if name == "stream.cpp":
        text = text.replace('#include "stream.h"', '#include "stream.h"\n#include "rtsp.h"')
        text = text.replace("std::begin(payload)", "payload.data()")
        text = text.replace("std::end(payload)", "(payload.data() + payload.size())")
        begin = text.index("      // Current Nvidia drivers have a bug")
        end = text.index('      BOOST_LOG(debug) << "Waiting for video to end', begin)
        text = text[:begin] + "      // The host application owns process lifetime; never terminate Cemu on a codec timeout.\n" + text[end:]
        begin = text.index("        bool revert_display_config")
        end = text.index("        platf::streaming_will_stop();", begin)
        text = text[:begin] + "        input::terminate_gamepads();\n" + text[end:]
        text = text.replace("session.shutdown_event->raise(true);", "session.shutdown_event->raise(true);\n      cemu_host::stop_media();")
        text = text.replace("session->shutdown_event->raise(true);", "session->shutdown_event->raise(true);\n      cemu_host::stop_media();")
        text = replace_once(text, "      session.input = input::alloc(session.mail, session.input_session_id);",
            '      try { cemu_host::start_media(); }\n      catch (const std::exception& ex) { session.state.store(state_e::STOPPED); BOOST_LOG(error) << ex.what(); return -1; }\n      session.input = input::alloc(session.mail, session.input_session_id);')
        text = replace_once(text, "    void join(session_t &session) {", '    void join(session_t &session) {\n      if (session.state.load() == state_e::STOPPED && !session.videoThread.joinable() && !session.audioThread.joinable()) { input::reset(session.input); return; }')
        text = replace_once(text, "      if (!session.broadcast_ref) {\n        return -1;\n      }",
            "      if (!session.broadcast_ref) {\n        cemu_host::stop_media();\n        session.state.store(state_e::STOPPED);\n        return -1;\n      }")
        text = text.replace("session->pingTimeout = std::chrono::steady_clock::now() + config::stream.ping_timeout;",
            "session->pingTimeout = std::chrono::steady_clock::now() + config::stream.ping_timeout;\n      cemu_host::input_heartbeat();")
        # Keep protocol packet pacing, but never delay the first packet of a
        # frame to a previous frame's bitrate budget.
        text = re.sub(r"auto ratecontrol_frame_start = std::max\([^;]+;",
                      "auto ratecontrol_frame_start = std::chrono::steady_clock::now();", text)
        text = replace_once(text, "      frame_network_latency_logger.first_point_now();",
            "      cemu_host::begin_send(packet->source_generation, packet->source_sequence, packet->source_timestamp_ns);\n      frame_network_latency_logger.first_point_now();")
        text = replace_once(text, "        session->video.lowseq = lowseq;", "        session->video.lowseq = lowseq;\n        cemu_host::end_send();")
        text = text.replace('        std::this_thread::sleep_for(100ms);', '        cemu_host::stop_media();\n        break;')
    if name == "rtsp.cpp":
        # An Asio timer starts a helper thread and waits for its entry event.
        # A DLL global constructor runs under the Windows loader lock, which
        # prevents that helper from entering. Own the server explicitly instead.
        text = replace_once(text,
            "  rtsp_server_t server {};  ///< Singleton RTSP server used by GameStream launch sessions.",
            '  static std::unique_ptr<rtsp_server_t> server_owner;\n\n'
            '  void initialize() {\n'
            '    if (server_owner) throw std::logic_error("RTSP host is already initialized");\n'
            '    server_owner = std::make_unique<rtsp_server_t>();\n'
            '  }\n\n'
            '  void deinitialize() { server_owner.reset(); }\n\n'
            '  static rtsp_server_t &host_server() {\n'
            '    if (!server_owner) throw std::logic_error("RTSP host is not initialized");\n'
            '    return *server_owner;\n  }')
        text = re.sub(r"\bserver\.", "host_server().", text)
        text = text.replace("std::begin(payload)", "payload.data()")
        text = text.replace("std::end(payload)", "(payload.data() + payload.size())")
        text = text.replace("std::end(msg_buf)", "(msg_buf.data() + msg_buf.size())")
        text = text.replace("std::end(socket->msg_buf)", "(socket->msg_buf.data() + socket->msg_buf.size())")
        text = text.replace("std::ranges::find_if(content,", "std::find_if(content.data(), content.data() + content.size(),")
        text = text.replace("std::end(content)", "(content.data() + content.size())")
        text = replace_once(text,
            "    auto begin = std::ranges::find(target, '=') + 1;\n    auto end = std::find(begin, std::end(target), '/');\n    std::string_view type {begin, (size_t) std::distance(begin, end)};",
            '    const auto separator = target.find(\'=\');\n'
            '    if (separator == std::string_view::npos) {\n'
            '      respond(sock, session, &seqn, 400, "BAD REQUEST", req->sequenceNumber, {});\n'
            '      return;\n    }\n'
            '    const auto value_begin = separator + 1;\n'
            '    const auto type = target.substr(value_begin, target.find(\'/\', value_begin) - value_begin);')
        text = text.replace("x < audio::MAX_STREAM_CONFIG", "x < 2")
        text = text.replace('#include "config.h"', '#include "config.h"\n#include "host_state.h"')
        marker = "    if (config.monitor.videoFormat == 1 && video::active_hevc_mode == 1) {"
        text = replace_once(text, marker,
            '    if (config.audio.channels != 2 || config.monitor.videoFormat != 0 || config.monitor.dynamicRange != 0 || config.monitor.chromaSamplingType != 0 || config.monitor.width <= 0 || config.monitor.height <= 0 || config.monitor.width > 1920 || config.monitor.height > 1080 || config.monitor.framerate <= 0 || config.monitor.framerate > 60) {\n'
            '      const auto reason = std::format("ANNOUNCE unsupported GamePad format: {}x{} at {} fps, codec={} (0=H264), HDR={}, chroma={} (0=420), audio={} channels. Select H264 SDR 420, stereo, at most 1920x1080 and 60 fps in Moonlight.", config.monitor.width, config.monitor.height, config.monitor.framerate, config.monitor.videoFormat, config.monitor.dynamicRange, config.monitor.chromaSamplingType, config.audio.channels);\n'
            '      respond(sock, session, &option, 400, reason.c_str(), req->sequenceNumber, {});\n'
            '      return;\n    }\n' + marker)
        # A client-facing 400 alone hides which of the format guards failed.
        # Persist every RTSP rejection, without recording pairing keys or bodies.
        signature = '  void respond(tcp::socket &sock, launch_session_t &session, POPTION_ITEM options, int statuscode, const char *status_msg, int seqn, const std::string_view &payload) {'
        text = replace_once(text, signature, signature + '\n    if (statuscode >= 400) {\n      cemu_host::report_negotiation_error(std::format("RTSP {} (CSeq {}): {}", statuscode, seqn, status_msg));\n    }')
        missing = '    } catch (std::out_of_range &) {\n      respond(sock, session, &option, 400, "BAD REQUEST", req->sequenceNumber, {});\n      return;\n    }'
        text = replace_once(text, missing,
            '    } catch (std::out_of_range &) {\n'
            '      std::string missing;\n'
            '      for (auto key : {"x-nv-audio.surround.numChannels"sv, "x-nv-audio.surround.channelMask"sv, "x-nv-audio.surround.AudioQuality"sv, "x-nv-video[0].packetSize"sv, "x-nv-video[0].clientViewportHt"sv, "x-nv-video[0].clientViewportWd"sv, "x-nv-video[0].maxFPS"sv, "x-nv-vqos[0].bw.maximumBitrateKbps"sv, "x-nv-video[0].videoEncoderSlicesPerFrame"sv, "x-nv-video[0].maxNumReferenceFrames"sv}) {\n'
            '        if (!args.contains(key)) missing += std::string(key) + "; ";\n'
            '      }\n'
            '      const auto reason = "ANNOUNCE missing required parameters: " + missing;\n'
            '      respond(sock, session, &option, 400, reason.c_str(), req->sequenceNumber, {});\n'
            '      return;\n    }')
        text = replace_once(text, "    auto stream_session = stream::session::alloc(config, session);",
            '    if (rtsp_stream::session_count() != 0) {\n      respond(sock, session, &option, 453, "One GamePad client is already connected", req->sequenceNumber, {});\n      return;\n    }\n    auto stream_session = stream::session::alloc(config, session);')
    if name == "utility.h":
        # MSVC string_view iterators are checked iterator objects, not pointers.
        text = replace_once(text, "return from_chars(std::begin(number), std::end(number));",
            "return number.empty() ? 0 : from_chars(number.data(), number.data() + number.size());")
    if name == "stream.h":
        text = replace_once(text, "namespace stream {", "namespace rtsp_stream { struct launch_session_t; }\n\nnamespace stream {")
    if name == "rtsp.h":
        text = replace_once(text, "namespace rtsp_stream {",
            "namespace rtsp_stream {\n  // Called by the DLL instance, outside DllMain and the loader lock.\n  void initialize();\n  void deinitialize();")
    if name == "uuid.h":
        text = text.replace("std::uniform_int_distribution<std::uint8_t>", "std::uniform_int_distribution<unsigned int>")
    if name == "thread_safe.h":
        text = text.replace("std::mutex _lock;", "mutable std::mutex _lock;")
        text = text.replace("bool peek() const {\n      return", "bool peek() const {\n      std::lock_guard guard(_lock);\n      return")
        text = text.replace("bool peek() {\n      return _continue && !_queue.empty();", "std::size_t pending_size() const { std::lock_guard guard(_lock); return _queue.size(); }\n\n    bool peek() {\n      std::lock_guard guard(_lock);\n      return _continue && !_queue.empty();")
        text = text.replace("bool running() const {\n      return", "bool running() const {\n      std::lock_guard guard(_lock);\n      return")
        text = replace_once(text, "      auto post = std::make_shared<typename queue_t<T>::element_type>(shared_from_this(), 32);",
            '      using queue_type = ::safe::queue_t<T>;\n'
            '      const bool encoded = id == "video_packets" || id == "audio_packets";\n'
            '      const auto policy = encoded ? queue_type::overflow_policy_e::reject : queue_type::overflow_policy_e::drop_oldest;\n'
            '      const auto limit = id == "video_packets" ? 2u : id == "audio_packets" ? 8u : 32u;\n'
            '      auto post = std::make_shared<typename queue_t<T>::element_type>(shared_from_this(), limit, policy);')
    if name == "thread_pool.h":
        text = text.replace("bool _continue;", "std::atomic_bool _continue;")
        text = replace_once(text, "        t.join();", "        if (t.joinable()) t.join();")
    write_generated(DEST / name, "// SPDX-License-Identifier: GPL-3.0-only\n// Derived from Sunshine d0b4a88ef727f8dabf80cc970a14739b55c885de.\n// Modified for Cemu Remote GamePad, 2026-10-27; upstream copyrights retained.\n#include <cstdint>\n#include <limits>\n#include <ctime>\n#include <cmath>\n#include <cstring>\n#include <sstream>\n" + text)

# Preserve the wire structures and transport metadata verbatim, without bringing
# Sunshine's desktop capture, FFmpeg encoder, or OS input interfaces into the DLL.
common = (SOURCE / "platform/common.h").read_text(encoding="utf-8")
prefix = common[common.index("namespace platf {"):common.index("  enum class mem_type_e")]
send = common[common.index("  struct buffer_descriptor_t {"):common.index("  /**\n   * @brief Open a url")]
platform = DEST / "platform"
platform.mkdir(exist_ok=True)
write_generated(platform / "wire_types.inc", "// SPDX-License-Identifier: GPL-3.0-only\n// Derived from Sunshine platform/common.h; modified 2026-10-27 for Cemu Remote GamePad.\n" + prefix + "\n" + send + "\n}\n")
shutil.copyfile(ROOT / "host-core/upstream/LICENSE", ROOT / "host-core/protocol/LICENSE")

windows = (SOURCE / "platform/windows/misc.cpp").read_text(encoding="utf-8")
start = windows.index("  SOCKADDR_IN to_sockaddr(")
end = windows.index("  /**\n   * @brief Owns platform QoS", start)
transport = windows[start:end]
transport = replace_once(transport,
    "    return WSASendMsg((SOCKET) send_info.native_socket, &msg, 0, &bytes_sent, nullptr, nullptr) != SOCKET_ERROR;",
    "    bool success = WSASendMsg((SOCKET) send_info.native_socket, &msg, 0, &bytes_sent, nullptr, nullptr) != SOCKET_ERROR;\n    if (success) cemu_host::packet_submitted(true);\n    return success;")
transport = replace_once(transport,
    "      return false;\n    }\n\n    return true;",
    "      cemu_host::packet_submitted(false);\n      return false;\n    }\n\n    cemu_host::packet_submitted(true);\n    return true;")
write_generated(DEST / "windows_transport.inc", "// SPDX-License-Identifier: GPL-3.0-only\n// Sunshine Windows direct UDP sender; modified 2026-10-27 for Cemu Remote GamePad.\n" + transport)
