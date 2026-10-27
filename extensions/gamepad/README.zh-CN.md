# Cemu Remote GamePad 构建候选版

这是非官方开发分支。发布许可、MPL/GPL 组合条款和二进制源码义务见 [LICENSING.zh-CN.md](LICENSING.zh-CN.md) 与根目录 [LICENSE-NOTICE.md](../../LICENSE-NOTICE.md)。

本实现按 `docs/gamepad-moonlight-plan.zh-CN.md` 接入真实 DRC 画面、DRC PCM 与 VPAD。当前交付为 **0.1.0 构建候选版**：已在本机完成 Windows x64 Release 构建，生成接入版 Cemu、独立 DLL 和管理工具，并检查导出入口及系统动态库依赖。用户已确认初始化死锁修复后可打开窗口；本轮加入“GamePad 串流设置”界面。GPU、游戏及安卓联调尚未完成，没有 P0–P4 通过记录。

## 范围与结构

首个目标是 Windows x64、Cemu Vulkan、同一 NVIDIA 显卡上的 D3D11/NVENC，以及原版 Android Moonlight。候选设备是 RTX 4070 SUPER / 驱动 617.14；安卓具体系统、Moonlight 版本与输入能力尚待记录。首版协商 H.264 SDR 4:2:0、最多 1920×1080 / 60 fps、双声道 48 kHz Opus、一个客户端。HEVC、AV1、HDR、环绕声和其他 GPU 后端不在本次实现范围。

Windows 主窗口和独立 GamePad 窗口的全屏操作现在使用普通无边框窗口，铺满进入全屏时所在的显示器，不改变显示模式或刷新率，也不设为永久置顶。原来的菜单、全屏快捷键及退出键继续使用；退出时恢复窗口位置、大小和最大化状态。此版本包含前述字体刷新崩溃修复，具体窗口切换效果仍需用户实测。

| 目录 | 内容 |
| --- | --- |
| `sdk/` | C ABI 1.0、GPU 租约约定、当前用户管理管道 |
| `adapter/` | Cemu DLL 加载器、直接输入 Provider、运动数据转换 |
| `plugin/` | 会话生命周期、三槽共享 GPU 资源、D3D11 GPU 转换和 NVENC |
| `host-core/` | 固定版本协议主机、配对、音频编码、输入与直接网络发送 |
| `manager/` | `gamepadctl` 配置/配对工具，不承载音画 |
| `compat/` | 源码候选信息与独立的已验证后端表 |
| `packaging/` | 源码准备、默认配置和时间线汇总工具 |

Cemu 内接入点包括常规 DRC scan buffer、OSScreen DRC、AX 当前 PCM 块、VPADRead、Vulkan 共享图像和离屏交互叠层。视频在 GPU 内采样、绘制、转换并进入 NVENC；CPU 只取得压缩码流。视频与音频由 DLL 自己的套接字直接发出。本地 GamePad 窗口开关不作为采集条件。

需要本仓库的 **接入版 Cemu**。把 DLL 放进官方原版 Cemu 不会自动提供接入接口；官方更新覆盖可执行文件后也不保证保留功能。建议与现有 Cemu 并列安装，保留现有版本及配置。

## 构建（由用户执行）

需要 Git、Python 3、CMake 3.25 以上、支持 C++23 的 MSVC、Windows SDK、Vulkan 构建环境。插件和 Cemu 分别构建，插件工程不包含 Cemu 内部头文件。以下在仓库根目录的 Visual Studio 开发者 PowerShell 中执行；使用当前已安装的 Visual Studio 默认生成器。

先准备固定版本主机源码及 Cemu 子模块：

```powershell
git submodule update --init --recursive
./extensions/gamepad/packaging/prepare-sources.ps1
```

若本机 Git 代理不可用但网络可直连，可给准备脚本加 `-Direct`；它只对本次 Git 调用关闭代理，不修改全局 Git 配置。协议 fork 源码已物化在 `host-core/protocol`；首次在新目录构建仍需取得锁定的上游依赖。

单独配置、构建、安装插件（`out/gamepad-stage` 为临时安装目录）：

```powershell
cmake -S extensions/gamepad -B extensions/gamepad/build -A x64 `
  -DCMAKE_TOOLCHAIN_FILE="$PWD/dependencies/vcpkg/scripts/buildsystems/vcpkg.cmake" `
  -DVCPKG_TARGET_TRIPLET=x64-windows-static
cmake --build extensions/gamepad/build --config Release
cmake --install extensions/gamepad/build --config Release --prefix "$PWD/extensions/gamepad/out/gamepad-stage"
```

按根目录 `BUILD.md` 准备 Cemu 的正常依赖，配置时增加以下开关；该开关默认关闭，只允许 Windows x64 且启用 Vulkan：

```powershell
cmake -S . -B build-gamepad -A x64 -DENABLE_GAMEPAD_EXTENSION=ON -DENABLE_VULKAN=ON
cmake --build build-gamepad --config Release
```

本机 Visual Studio 2026 也可使用封装脚本，自动载入 x64 编译环境、分别构建插件和 Cemu，并将日志保存在根目录 `out/build-logs/gamepad-Release`：

```powershell
./extensions/gamepad/packaging/build-windows.ps1 -UseSystemTools -UsePowerShellDownloads
```

`-Component plugin` 或 `-Component cemu` 可分别构建；默认两者都构建。`-UseSystemTools` 使用现有 CMake/Ninja/7-Zip，`-UsePowerShellDownloads` 通过 PowerShell 获取依赖，保留 HTTPS 证书及锁定 SHA512 校验。默认沿用当前代理；只有明确需要直连时才加 `-Direct`。这些选项仅影响本次构建进程，不修改全局 Git 或代理配置。脚本不启动 Cemu 或执行游戏联调。

将安装目录中的整个 `extensions/gamepad` 文件夹复制到这次构建的 `Cemu.exe` 旁边。预期布局：

```text
Cemu.exe
extensions/gamepad/
  CemuGamePad.dll
  gamepadctl.exe
  config.example.json
  upstream.lock.json
  compat/validated.json
  compat/candidate.json
  docs/
  licenses/
  tools/summarize-timeline.py
```

依赖使用静态 vcpkg triplet 和静态 MSVC 运行库；系统 D3D11/DXGI、Vulkan 和 NVIDIA 编码驱动由本机提供。不要用其他版本的 `nvEncodeAPI64.dll` 覆盖驱动自带文件。发布二进制时还需提供对应源码、生成脚本和许可证材料。

## 启用、配对和输入绑定

接入版 Cemu 的 **选项（Options）→ GamePad 串流设置…** 提供三个页面，即使插件尚未启用也能打开：

- **串流设置**：启用插件、允许未验证的硬件组合、主机名称、监听地址、端口、视频码率上限、本地 GamePad 声音、绝对点击触摸与延迟日志。保存后重启 Cemu 生效，不会中断当前游戏或改写已有手柄映射。
- **连接与配对**：显示本机 IPv4 地址、游戏/显卡/输入/串流状态及错误。手机发起配对后，选择对应请求并输入四位 PIN；也可移除已配对设备或停止当前串流。状态自动刷新；配对与管理通信在后台执行，关闭窗口会取消等待。
- **使用说明**：查看图形、输入绑定和安卓 Moonlight 的接入步骤。分辨率与帧率由 Moonlight 选择；电脑端 `max_bitrate_kbps`（1000–100000，默认 100000）限制实际初始编码码率，网络拥塞时仍会继续降码率。

首次配置默认禁用，当前验证表为空，准备参与候选版验证时需同时勾选“启用”和“允许使用尚未通过验证的显卡 / 驱动组合”。配置和配对数据位于 `%LOCALAPPDATA%/CemuRemoteGamePad`。管理服务只接受当前用户的本地连接，界面还检查服务所属的 Cemu 进程，避免操作另一个 Cemu 实例。

命令行工具仍可使用，但不需要常驻或单独启动。也可这样完成启用：

```powershell
./extensions/gamepad/gamepadctl.exe init
./extensions/gamepad/gamepadctl.exe enable-experimental
```

当前 `compat/validated.json` 为空，普通 `enable` 会要求已验证的 GPU/驱动组合并拒绝本候选。`enable-experimental` 明确允许用户执行尚未完成的 P0 验证；它不把候选写成已兼容。`disable` 关闭插件。错误和状态中的 `backend_key` 用于之后记录真实验收组合。

1. 在接入版 Cemu 中选择 Vulkan 和 RTX 4070 SUPER，启动游戏。初次 GPU 附加会初始化真实 NVENC；失败原因写入 Cemu 日志。
2. Android Moonlight 中手动添加电脑的局域网 IPv4 地址。本实现没有提供 mDNS 发现服务。主机名称默认为 `Cemu GamePad`，应用列表只有 GamePad。使用同一电脑运行 Sunshine 等主机时会产生默认端口冲突，应先停止冲突主机或给本插件配置另一端口。
3. 在 Moonlight 发起配对并保留 PIN 界面，在 Cemu 的“GamePad 串流设置 → 连接与配对”中选择对应名称/地址的请求、填写 PIN、点击“确认配对”。配对不需要先启动游戏。命令行等效操作为 `gamepadctl pending` 和 `gamepadctl pair REQUEST_ID FOUR_DIGIT_PIN CLIENT_NAME`。
4. Cemu 输入设置中选择一个 **Wii U GamePad** 槽，输入 API 选 `Remote GamePad`，设备选 `Moonlight GamePad`，使用默认映射并保存。该远程源只允许绑定至一个 VPAD，且是该槽唯一物理输入源。不会自动改写已有手柄配置。
5. Moonlight 选择 H.264、SDR、双声道、60 fps 以内，启动 `Cemu GamePad`。ABXY 按标签对应 Wii U 按钮。触摸默认接受实际绝对指针点击，原生触摸和控制器触摸板也直接接入；安卓端若使用相对鼠标模式，需要切换绝对触控模式。
6. 在“连接与配对”或通过 `gamepadctl status` 查看会话、绑定、触摸、陀螺仪、加速度和震动状态。手机自身传感器或连接的手柄是否被原版 Moonlight 上报，以此及实际操作结果为准，不能仅凭安卓平台推定有体感。输入设置中的运动/震动选项仍由用户控制。

默认 HTTP 基准端口为 47989；沿用协议偏移，HTTPS 47984、RTSP TCP 48010、UDP 视频 47998 / 控制 47999 / 音频 48000。仅局域网使用，不自动修改防火墙或建立路由映射；允许这些端口的本地网络访问由用户配置。管理使用只允许当前 Windows 用户访问的本地管道，不监听管理网页端口。

遇到 `RTSP handshake (error 400)`，先在手机 Moonlight 选择 1280×720、60 fps、H.264、关闭 HDR、双声道再连接。400 表示请求遭到拒绝，不能仅凭提示认定防火墙有问题。最新版插件会在“连接与配对”的错误栏和 Cemu `log.txt` 中记录拒绝详情；格式不支持时列出实际请求值，缺少协商字段时列出字段名。不要为了越过错误而直接删除后端格式限制。

此前启动时可能先出现音频 40 ms 积压，再出现 `Media session was stopped before GPU pool creation`。后者是会话已停止后的次生错误；修正版会在 Opus 就绪后才接收当前代音频，避免启动前填满队列，且状态栏保留首个失败原因。运行期间真实的音频或发送积压仍会明确停止会话，不以静音代替。

`gamepadctl clients` 列出配对设备，`unpair CLIENT_ID` 撤销配对，`stop` 结束当前流而不退出 Cemu。关闭管理工具不影响推流。`local_gamepad_audio=false` 仅控制本地 DRC 播放，远程 PCM 接入独立于本地设备和四块聚合。退出游戏/客户端、超时与重新连接会清空远程按键及触摸。

## 验收记录与延迟数据

`compat/candidate.json` 记录已完成构建但尚未进行运行验收的候选，P0–P4 全部待用户验证。构建包另含 `compat/build.local.json`，记录工具链及二进制 SHA256。建议按规划文档的验证矩阵逐项执行，并记录安卓机型/系统、Moonlight 版本、Cemu 提交、DLL 来源、驱动版本、会话设置和日志。至少覆盖真实 DRC 与 TV 不同画面、两条输出路径、本地窗口关闭、无本地 DRC 音频设备、触摸黑边、快速按下/释放、断线、游戏切换、重连、设备重建和编码错误。

已验证后端表的 schema 为 1，`validated_backends` 中的记录格式为 `{"key":"status 输出的 backend_key","status":"passed","evidence":"验收记录位置"}`。仅在对应硬件组合实际完成 P0 后写入；是否达到完整首版仍由 P0–P4 的整套证据决定。键包含 PCI vendor/device、DXGI 驱动版本、Vulkan 驱动版本、NVENC 头文件 API 及插件版本，驱动或插件升级会重新要求记录。

把配置 `trace_timeline` 设为 `true` 并重启后，日志写入 `logs/timeline.csv`，按代际、源帧序号和源时间关联采集、就绪观察、编码提交/完成、码流取出、首末包提交。可由用户执行：

```powershell
python ./extensions/gamepad/tools/summarize-timeline.py "$env:LOCALAPPDATA/CemuRemoteGamePad/logs/timeline.csv"
```

输出实际样本的 p50/p95/p99、缺失和逆序事件数；不会为未完成的帧补数据。当前时间线全部来自同一个主机单调时钟：源时间是 DRC 钩子内取得租约的时刻，GPU 就绪和编码完成是 **CPU 观察时刻**，网络时间是套接字提交时刻。它不代表 GPU 执行时长、网卡发出时刻、安卓解码或屏幕呈现，也未完成 GPU timestamp 到 CPU 的校准。真实 GPU 分阶段测量、等价直接嵌入基线、空载/忙载/拥塞场景及跨官方提交升级回退演练仍属 P4 待办，不能由该脚本单独宣称通过。

默认关闭跟踪，避免同步日志写入影响日常延迟。跟踪启用时需要另外量化其开销。不承诺固定毫秒数、数学零延迟或已经验证“无额外整帧等待”。

## 升级与回退

保留上一套接入版 Cemu 与对应插件目录。新官方提交需要重放接入改动、重新构建并执行规划中的升级矩阵，不能仅凭合并无冲突认定兼容。ABI 不匹配会拒绝加载；配置/配对保存在独立用户目录，覆盖它们之前先备份。当前尚未执行升级演练，也没有发布已验证兼容组合。

接口租约详见 `sdk/ABI.zh-CN.md`；固定上游来源及 fork 变更详见 `host-core/README.md` 与 `upstream.lock.json`。DLL 与 Cemu 同进程，驱动挂起或插件崩溃没有进程隔离保证；设备故障恢复仍需真实联调。
