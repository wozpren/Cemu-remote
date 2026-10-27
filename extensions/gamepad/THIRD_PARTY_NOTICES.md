# Remote GamePad third-party notices

This fork preserves the original authors' rights and makes no claim of
authorship over their work. The exact transport revisions are in
[`host-core/upstream.lock.json`](host-core/upstream.lock.json).

| Component | Source / fixed revision | License text |
| --- | --- | --- |
| Cemu | cemu-project/Cemu, `42876aa86d2b92f9c620a76959a792bf392b89d3` | [MPL-2.0](../../LICENSE.txt) and existing per-component licenses |
| Sunshine transport fork | LizardByte/Sunshine, `d0b4a88ef727f8dabf80cc970a14739b55c885de` | [GPL-3.0](licenses/Sunshine-LICENSE) |
| Moonlight-common-c RTSP parser / transport definitions | moonlight-stream/moonlight-common-c, `04053843c90c4268583f2496b997d54d59f03957` | [GPL-3.0](licenses/Moonlight-common-c-LICENSE) |
| ENet (Moonlight fork) | moonlight-stream/enet, `aca87840b57f045a1f7f9299e4b1b9b8e2a5e2f1` | [MIT](licenses/ENet-LICENSE), Lee Salzman and contributors |
| nanors and bundled OBLAS routines | sleepybishop/nanors, `b1e3c22ca0cdc0bb83e3cd6ed1a2fc77869ed99a` | [MIT](licenses/nanors-LICENSE), Joseph Calderon and contributors |
| Simple-Web-Server | eidheim/Simple-Web-Server, `546895a93a29062bb178367b46c7afb72da9881e` | [MIT](licenses/Simple-Web-Server-LICENSE), Ole Christian Eidheim and contributors |
| Dear ImGui and Vulkan renderer backend | Cemu's pinned Dear ImGui submodule and existing backend files | [MIT](../../dependencies/imgui/LICENSE.txt), Omar Cornut and contributors |

`host-core/protocol/src/` contains selected Sunshine files and extracted Windows
transport/wire code, modified for the embedded GamePad host on 2026-10-27.
`packaging/prepare_protocol.py` records those transformations; regeneration
preserves the derived-source notice. The unchanged source checkout is obtained
by `packaging/prepare-sources.ps1` rather than committed as a build/cache tree.

The extension's vcpkg manifest pins its own baseline. Its build dependencies
include Boost (Boost Software License 1.0), OpenSSL 3 (Apache-2.0), Opus
(BSD-style), nlohmann/json (MIT), and ffnvcodec headers (MIT-style). Consult
the actual pinned package copyright files for complete notices and bundled
subcomponents. Installation collects those files into `licenses/vcpkg/`.
Cemu's separate vcpkg dependency notices and source obligations also apply to
any complete Cemu binary distribution.

The NVIDIA/Vulkan driver is provided by the user's system; this fork does not
redistribute the driver or an NVIDIA runtime DLL. License compatibility does
not establish hardware interoperability or patent clearance for every use.
