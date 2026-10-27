# 发布与许可证说明

本分支为非官方 Cemu Remote GamePad 开发分支。修改日期：2026-10-27。
具体分发声明以仓库根目录 [LICENSE-NOTICE.md](../../LICENSE-NOTICE.md) 和各许可证原文为准。

| 部分 | 许可及处理 |
| --- | --- |
| Cemu 原有源码及对其修改 | 保留 MPL-2.0、作者署名和原有第三方许可；组合分发时依 MPL 3.3 同时提供 GPL-3.0 条款下的许可 |
| 新的 Cemu 适配层、C ABI SDK 和 GUI 文件 | MPL-2.0 OR GPL-3.0-only，保留回馈 MPL 上游的可能 |
| Sunshine 衍生协议、插件运行时、主机替代模块、管理工具和打包脚本 | GPL-3.0-only，完整文本见本目录 LICENSE.txt |
| Dear ImGui Vulkan 后端及修改 | 保留原 MIT 许可和声明 |
| Moonlight-common-c、ENet、nanors、Simple-Web-Server 和构建依赖 | 分别保留上游许可，见 THIRD_PARTY_NOTICES.md 和 licenses/ |

**不能只按 Cemu 的 MPL 看待本组合。** 插件与 Cemu 在同一进程内互调、共享数据结构和 GPU 资源，拆 DLL 并不自动排除 GPL 义务。本分支以 GPL-3.0-only 分发启用插件的组合，同时依 MPL 2.0 第 3.3 条保留 MPL 部分的原许可和额外 GPL 许可；没有把 Sunshine 改成 MPL，也没有删除 Cemu 的 MPL。

源码上传保留修改说明、固定依赖提交、生成脚本及构建方法。今后若发布二进制，需要提供与二进制对应的完整源码及构建/安装材料，并明确下载位置。只链接 Cemu 原仓库、只放插件源码、或只上传不含子模块的 GitHub 自动源码压缩包，都不能直接证明已满足整个组合的源码分发义务。

发布包还应携带适用许可和第三方声明，保留静态链接依赖各自要求的材料。不能声称是 Cemu 官方版本，不能把 GPL 代码设为闭源或附加禁止再分发的条款。游戏、固件、密钥、配对私钥、存档和本机配置不随源码上传。

本次只推送源码分支，不创建二进制 Release。

依据：[Mozilla MPL FAQ](https://www.mozilla.org/en-US/MPL/2.0/FAQ/)、[MPL 与 GPL 组合指引](https://www.mozilla.org/en-US/MPL/2.0/combining-mpl-and-gpl/)、[GNU 插件说明](https://www.gnu.org/licenses/gpl-faq.en.html#GPLPlugins)、[GPL-3.0 原文](https://www.gnu.org/licenses/gpl-3.0.en.html)。
