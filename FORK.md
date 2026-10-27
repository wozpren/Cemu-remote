# Cemu Remote GamePad experimental fork

This is an unofficial development fork of [cemu-project/Cemu](https://github.com/cemu-project/Cemu).
Its implementation starts at Cemu commit `42876aa86d2b92f9c620a76959a792bf392b89d3`.
Development branch: [`codex/gamepad-moonlight`](https://github.com/wozpren/Cemu-remote/tree/codex/gamepad-moonlight).

The branch adds real GamePad GPU-frame capture and AX audio, an in-process
Moonlight-compatible host, direct remote VPAD input, an Options-menu streaming
settings/pairing dialog, and borderless fullscreen for the Windows main and
GamePad windows. Streaming requires Windows x64, Vulkan and the same NVIDIA
adapter for D3D11/NVENC. It does not start an external Sunshine server or capture
the Windows desktop.

The extension is disabled by default. The current backend supports one client,
H.264 SDR 4:2:0, up to 1920x1080 at 60 fps, and stereo Opus. Hardware acceptance
records remain empty. A user has observed streaming on an RTX 4070 SUPER, but
the complete lifecycle, audio/video, input and latency acceptance matrix has
not passed. Subsequent font-upload and fullscreen changes still need user
runtime verification. This is source for a development candidate, not a stable release.

## Build and use

Clone this branch with its Cemu submodules. Follow [BUILD.md](BUILD.md) for the
Cemu toolchain, then use [the extension build/usage guide](extensions/gamepad/README.zh-CN.md).
The protocol source generator and pinned upstream revisions are included;
`extensions/gamepad/packaging/prepare-sources.ps1` obtains the required transport
dependencies. The two projects use separate vcpkg baselines and build directories.

The latest local Windows Release builds succeeded. No binaries, local game
files, saves, pairing keys, personal configuration or build caches are included
in this source branch. The original design is in [the plan](docs/gamepad-moonlight-plan.zh-CN.md);
it is historical planning material, not a statement that all acceptance criteria passed.

## Licensing and distribution

Read [LICENSE-NOTICE.md](LICENSE-NOTICE.md) and the
[Chinese licensing notes](extensions/gamepad/LICENSING.zh-CN.md) before distribution.
Cemu's MPL-2.0 license and notices are preserved. The embedded Sunshine/Moonlight
transport and the complete enabled combination carry GPL-3.0 obligations;
a DLL boundary is not treated as a GPL exception. Third-party licenses remain
applicable. This branch publishes source only and does not publish a binary Release.

Discuss fork-specific problems with this fork's maintainer rather than represent
them as official Cemu defects. This fork claims no endorsement from Cemu, Sunshine or Moonlight.
