# Embedded transport fork

This directory contains the in-process GameStream host used by CemuGamePad.dll.
It does not launch Sunshine, capture a desktop, inject OS input, or pass media to
another process. The DLL owns the HTTP/HTTPS pairing, RTSP, ENet control, UDP video
and audio sockets. Cemu's render adapter supplies DRC frames and AX supplies PCM.

`upstream.lock.json` records the exact Sunshine, Moonlight and web-server commits.
`upstream/` is an ignored source checkout. `protocol/src/` contains the materialized
Sunshine-derived fork plus the small native replacement interfaces. The plugin
build uses those sources directly; it does not link a separately running host.

`packaging/prepare-sources.ps1` obtains the locked checkout and submodules, then
`packaging/prepare_protocol.py` copies the selected source files and applies the
tracked transformations. Regeneration replaces only files named by that script;
the custom config, audio, video, logging, input and platform headers remain native
sources. Change the transformation script when editing generated protocol files.
Context-sensitive changes fail if the pinned source no longer matches.

MSVC portability changes use contiguous data pointers in RTSP/FEC parsing rather
than assuming standard-library iterators are raw pointers. Standalone transport
headers declare their RTSP types explicitly, without the original app's implicit
include order. The source generator preserves timestamps of unchanged outputs.

The fork retains upstream pairing/authentication, RTSP negotiation, control
encryption, RTP/FEC packetization and direct Windows UDP submission. Changes remove
desktop/display/process control, web administration, external media paths, and
upstream process-kill recovery. They replace the app registry with a single Cemu
GamePad, reject WAN HTTP requests and unsupported media formats, preserve input
edges, bound sender queues, and add source-frame tracing. A GPU backend failure
ends the session instead of substituting a software encoder. Unsupported Windows
UDP segmentation may use the upstream individual-datagram sender on the same
socket, without routing media to another process.

Runtime media support is H.264 SDR 4:2:0, standard stereo Opus, one active client,
one explicitly bound VPAD, and the Windows NVIDIA Vulkan/D3D11/NVENC backend.
HEVC/AV1/HDR/surround/custom mappings/desktop streaming are rejected or not
advertised. Source availability is not a record of successful interoperability.

See the repository's `LICENSE-NOTICE.md` and the extension's
`LICENSING.zh-CN.md` for the complete combination's distribution terms. The DLL
boundary is not a GPL linking exception. The enabled combination is distributed
under GPL-3.0-only, with MPL-covered files additionally offered under GPL-3.0
under MPL section 3.3 while retaining their MPL availability and notices.
The Cemu repository license is MPL-2.0 and remains applicable to Cemu's sources.
Sunshine-derived code is GPL-3.0-only; the complete upstream license is preserved
in `protocol/LICENSE`. Moonlight-common-c, ENet, nanors and Simple-Web-Server retain
their upstream licenses. CMake installation includes these and the vcpkg package
copyright notices. Binary distribution must include the corresponding source,
these modifications, lock data, generation script and build instructions; the
ignored checkout must be obtainable at the locked revisions. This candidate has
been compiled with MSVC on Windows x64. Hardware, Android interoperability and
latency acceptance remain pending; compilation is not a P0–P4 acceptance result.
