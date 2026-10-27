# 本机构建记录

2026-10-27 已完成 Windows x64 Release 构建。用户已确认启动死锁修复后窗口可以打开；本轮加入 Cemu 内置串流设置界面并重新构建。助手未启动新版界面，未执行 GPU 与 Android Moonlight 联调。

| 产物 | 结果 |
| --- | --- |
| 接入版 `Cemu.exe` | 完整重编译、链接成功；源码基线 `42876aa86d2b92f9c620a76959a792bf392b89d3`，包含工作区接入改动 |
| `extensions/gamepad/CemuGamePad.dll` | 独立工程完整重编译、链接成功；导出 `CemuGamePad_GetApi` |
| `extensions/gamepad/gamepadctl.exe` | 编译、链接成功 |

工具链：Visual Studio Community 2026，MSVC 19.51.36260.0，CMake 4.3.1-msvc1，Windows SDK 10.0.26100.0，Ninja，静态 MSVC 运行库。Cemu 与插件分别保留自己的 vcpkg 基线和安装目录，临时构建目录也分开。

本轮修复了协议 fork 在 MSVC 下把标准库迭代器当作裸指针使用的编译问题，以及拆分后的 RTSP 头文件声明依赖。中文编译器的 include 输出曾被 CMake 按错误编码识别；现在配置阶段采集实际前缀，完整重编译后的 Ninja 记录已包含真实头文件依赖。

用户首次启用插件后遇到“进程存在但主窗口不出现”。只读检查发现 DLL 已加载、主窗口尚未创建，插件实例的配置目录初始化也尚未执行。源码中的 RTSP 全局对象会在 DLL 加载阶段构造 Asio 定时器，定时器创建线程并等待进入事件，与 Windows DLL 加载锁冲突。修复后由插件实例显式创建 RTSP 对象，工作线程退出后显式销毁，避免在 DLL 加载和卸载锁内启动或等待线程；并处理线程池部分启动失败时的清理。修正版已重新编译、链接，未由助手启动程序复测。

静态检查确认：DLL 的 C ABI 入口已导出，三个程序的常规导入动态库均为 Windows 系统组件；NVIDIA 编码 API 和 Vulkan 驱动仍由运行时加载，本轮没有验证它们。构建中保留了第三方代码的编译警告，没有将警告当成运行通过的证据。

新增入口：选项（Options）→ GamePad 串流设置…。窗口支持真实状态与错误展示、指定设备的 PIN 配对、已配对设备移除、停止当前流及启动配置保存。管理通信有等待上限与关闭取消，且校验进程，防止操作另一 Cemu。新增码率上限实际用于 NVENC 初始码率；仍由 Moonlight 协商分辨率、帧率和音频格式。保存现有配置时保留未涉及的字段，损坏配置不以默认值覆盖。

用户随后报告安卓端 `RTSP handshake (error 400)`。现有用户日志显示 NVIDIA 编码器初始化成功，但旧版没有记录具体握手拒绝原因，尚不能判断失败参数。本轮插件增加 RTSP 错误状态与 CSeq 记录；格式拒绝会包含客户端请求的分辨率、帧率、编码、HDR、色度和声道，缺少必需参数时列出字段名，并显示到设置窗口的错误栏。插件已重新编译；此改动用于定位问题，不代表 400 已修复。手机端可先用 1280×720、60 fps、H.264、SDR、双声道复测。

用户再次连接后的日志显示已通过协商并建立会话，随后先报 `DRC audio encoder fell behind its 40 ms safety bound`，再报 `Media session was stopped before GPU pool creation`。原因是会话启动后立即收集 PCM，而音频线程还在等待 UDP 握手及初始化 Opus，尚无消费者，启动前积压触发了保护。修正版仅在本代音频编码器就绪后开始接收 PCM，重连和停止时清除就绪状态，并继续检查时间戳和代际；运行中的 40 ms 积压保护保持有效。每次会话重新开始时清除旧错误，停止后的次生错误保留在日志中但不覆盖首个原因。该修复已完成增量 Release 编译、链接；助手未启动游戏，音画恢复仍待用户复测。

最新修正版交付目录为源码工程的 `extensions/gamepad/out/gamepad-stage-fixed`，包含共享纹理、《风之杖 HD》选择 GamePad 时的字体刷新修复，以及 Windows 无边框全屏窗口。`extensions/gamepad/compat/build.local.json` 记录实际二进制 SHA256 与构建时间；此前的 `gamepad-stage` 和 `gamepad-stage-ui` 保留。使用时先关闭旧 Cemu，再打开新目录的 `Cemu.exe`。默认不启用插件，已有用户配置继续沿用。首次进行硬件验证时，在界面勾选启用与允许未验证组合、保存并重启，选择 RTX 4070 SUPER 并记录安卓机型、系统及 Moonlight 版本。

用户随后报告 `Create shared encoder input texture: HRESULT 2147942487`（`0x80070057`，无效参数）及安卓 Moonlight 整个应用退出。用户新日志显示会话建立后在共享纹理创建处失败，未再出现此前的启动音频队列错误。代码只设置了 `SHARED_NTHANDLE`，缺少实际启用资源共享的标记。已改为 `D3D11_RESOURCE_MISC_SHARED | D3D11_RESOURCE_MISC_SHARED_NTHANDLE`，保留共享 fence 和槽租约同步，不引入 keyed mutex 协议；同时记录实际创建尺寸并检查边界。这与 Chromium 的共享 fence 路径使用的[标记组合](https://raw.githubusercontent.com/chromium/chromium/main/gpu/command_buffer/service/shared_image/d3d_image_backing_factory.cc)一致，标记语义见[微软文档](https://learn.microsoft.com/en-us/windows/win32/api/d3d11/ne-d3d11-d3d11_resource_misc_flag)。插件已重新编译、链接；共享纹理创建是否成功及安卓端是否仍闪退待用户复测。用户说明为安卓 15、改版 Moonlight，具体改版名称与版本待确认，不能将客户端闪退认定为已修复。

`compat/validated.json` 仍为空。用户日志提供了编码器初始化、连接与故障的局部运行证据；P0 的真实 GPU 资源互操作、P1 生命周期、P2 音画、P3 输入及 P4 性能/升级均没有完整通过记录。本次构建结果不代表这些项目已通过。

用户已确认可以串流，但在《风之杖 HD》游戏内选择 GamePad 时退出。用户日志记录的是电脑端 Cemu 在 NVIDIA Vulkan 驱动中的 `0xc0000005`，不是普通断流。为定位旧版偏移，先对未修改渲染代码的 Release 程序生成链接地址表，确认其 `.text` SHA256 与用户运行的 `Cemu.exe` 完全一致（`18b483d8c338762726c49930258b9f5e0ce94cd22fbd87c0d2c005ca2affa7f3`）。调用栈中 Cemu 偏移 `0x5cbcb5` 对应 `ImGui_ImplVulkan_CreateFontsTexture+0x555`，上层 `0x2b0ef7` 对应 `GamePadVulkanBridge::Draw+0x217`。

接入代码在 `ImGui_PrecacheFonts()` 前保存命令缓冲；该函数处理新字体时会执行 `Flush(true)`，提交旧缓冲并开始另一缓冲，随后字体上传却仍使用旧句柄。修复将命令缓冲获取移至字体缓存准备之后，绘制用的 uniform 上传和源图像/图像视图使用标记也移到刷新之后。Release 编译、链接已通过；助手未启动游戏，需用户按相同控制器选择步骤复测。新增 `compat/CemuGamePad.map` 与本次程序配套，用于继续定位崩溃偏移，不作为运行验收通过的证据。

按用户要求将 Windows 主窗口、独立 GamePad 窗口的全屏入口改为普通无边框窗口。旧路径使用 wxWidgets 的弹出式全屏窗口；源码未发现主动申请独占显示模式的接口。新路径去掉窗口装饰及 `WS_POPUP`，覆盖当前显示器，保留正常 Z 顺序，不改变分辨率/刷新率。使用 `WINDOWPLACEMENT` 保存/恢复正常位置和最大化状态；切换过程的中间尺寸事件不覆盖已保存的普通窗口位置。主窗口继续管理全屏菜单显示，原快捷键沿用；其他系统仍走原生 wxWidgets 全屏。Release 编译、链接通过，未由助手启动程序。用户应确认进入/退出全屏、Alt+Tab、从最大化状态切换，以及独立 GamePad 窗口的行为。
