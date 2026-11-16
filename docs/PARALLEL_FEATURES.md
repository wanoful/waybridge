# 功能整合清单

14 项功能已整合到同一套服务端和 Windows 客户端源码。下表保留功能来源；来源分支保留用于追溯，旧 worktree 已清理，不再需要分别选择不同功能客户端。统一说明见 [运行文档](README.md)、[协议](PROTOCOL.md) 和 [验证记录](VALIDATION.md)。

| 功能 | 来源分支 | 来源提交 | 当前范围 |
|---|---|---|---|
| Windows 本地 IME | `codex/windows-ime` | `fc6cba41` | text-input-v3；微软拼音候选面板待实机 |
| 光标同步 | `codex/native-cursor` | `d838373` | 系统/自定义/动画光标；物理多显示器待验 |
| 应用图标 | `codex/app-icons` | `a9ea5d9` | XDG PNG/SVG → Win32 大小图标 |
| 模态/窗口状态/输入区域 | `codex/window-semantics` | `3285963` | 原生窗口语义；点穿限同一 UI 线程 |
| Linux → Windows 文件拖出 | `codex/file-drag-out` | `999bfc9` | OLE 按需虚拟文件；COPY only |
| 目录传输 | `codex/directory-transfer` | `bec86e5` | 双向目录层级、空目录及严格路径校验 |
| Windows 虚拟文件导入 | `codex/virtual-files` | `f5fda93` | FILEDESCRIPTOR/FILECONTENTS；Linux 完成后发布 |
| 相对鼠标/锁定 | `codex/relative-pointer` | `1b8090d` | Raw Input + locked-pointer；未启用 confinement |
| X11 应用兼容 | `codex/xwayland` | `425f756e` | 私有 rootless Xwayland；无 XIM/XDND/PRIMARY |
| 会话音频 | `codex/audio` | `ad2021d4` | 私有 sink + PCM/WASAPI；无 Opus/严格音画同步 |
| HEVC/AV1 | `codex/video-codecs` | `16a5d58` | NVENC + GPU 能力探测；新 codec 实机待验 |
| UDP/FEC | `codex/udp-fec` | `a44ce94` | AES-GCM + 单片 XOR + TCP repair；固定节流 |
| GPU 颜色输入 | `codex/gpu-interop` | `d0350e7` | 同 NVIDIA GPU GL/CUDA；alpha 仍回读 |
| HDR 色彩链路 | `codex/hdr` | `01d9150` | 实验 scRGB/Main10/alpha16；整体 HDR 为 partial |

## 整合关系

目录传输保留原生拖出实现，虚拟文件再扩展 Windows 导入；GPU 颜色输入保留 HEVC/AV1，HDR 在其上提供 scRGB 子集。合并保留原分支历史。Smithay 的 Xwayland、colormap 生命周期及 SHM 格式缓存补丁共用同一份 vendor，来源与许可证见 [PATCHES.md](../vendor/smithay/PATCHES.md)。

媒体与交互共用 v3 能力协商。PNG/H.264/HEVC/AV1/Main10 的原始 kind 为 1/2/4/5/7/8，UDP 补帧容器为 6，独立 PCM 音频为 3。UDP 全局 8 条/64 MiB 传输 credit、每窗口两帧呈现 credit 和独立音频 credit 分开计数；PNG→视频恢复和缓存复用均保留新 epoch/独立关键帧。

光标更新在指针锁定期间不解除原生隐藏/裁剪，解锁取当前光标；IME/OLE/焦点切换保留 generation 和输入释放约束。共享退出处理先停止并等待视频编码线程，再回收 GPU、私有 Xwayland 资源和音频 sink。文件工作线程不阻塞合成器或窗口控制，Linux 只向应用发布已完成的本地路径。

## 构建和启用

在仓库根目录构建：

```bash
./scripts/build.sh
./scripts/build-windows-arm64.sh
```

产物为 `target/release/waybridge`、`build/windows/waybridge-client.exe` 和 `build/windows-arm64/waybridge-client.exe`。请同时更新两端，WoA 使用 ARM64 版本。默认 PNG/H.264、普通窗口与数据交换按能力自动协商；音频和 Xwayland 默认自动检测，其余扩展按下表启用。

| 功能 | 服务端开关 | Windows 端 |
|---|---|---|
| 音频 | 默认自动；`--audio` 要求成功，`--no-audio` 关闭 | 自动协商；`WAYBRIDGE_NO_AUDIO=1` 可禁用 |
| X11 | 已安装时自动；`--xwayland` 要求成功，`--no-xwayland` 关闭 | 无新增客户端依赖 |
| CUDA 颜色输入 | `--gpu-input cuda` | 保留原 GPU 解码能力探测 |
| HEVC / AV1 | `--codec hevc-nvenc` / `--codec av1-nvenc` | `--check-codecs` 查询实际支持 |
| UDP | `--udp-listen <地址>:49091` | 设置可直达的 `WAYBRIDGE_UDP_HOST`；SSH 仍承载 TCP |
| 实验 HDR | `--hdr experimental-scrgb --codec hevc-nvenc` 或 AV1 | `WAYBRIDGE_HDR=1`，且实际满足 Main10/HDR 条件 |

## 验证范围

真实 Wayland/Qt/GTK/X11、NVIDIA NVENC/CUDA 和私有 Wine/Xvfb 中的 Windows PE/API 已覆盖完整合并后的回归。结果包括全部 UDP 媒体 kind、音频、流控、文件/目录/OLE/21 种虚拟源，以及 X11/音频/UDP/GPU/HDR 同时启用时的 PNG-only 回退和共享退出清理。GPU 隐藏/重映射、持有编码缓存时退出也已通过；分层结果见 [VALIDATION.md](VALIDATION.md)。

Wine 的 Unicode 注入不代表微软拼音引擎、候选面板或选词验收；软件 D3D11 数值检查不代表物理 HDR/MFT。Explorer、Outlook、压缩软件、真实 WoA 新 codec、多显示器 DPI 和输入到显示延迟仍需相应实机测试。完整 HDR、XIM/XDND/PRIMARY、跨进程透明点穿、网络按需 FUSE、Opus、自适应拥塞控制与 Moonlight 线协议兼容未实现。

跨端只读文件交付已加入：Linux 使用完整缓存上的只读 FUSE 视图，Windows 实体剪贴板缓存使用 DACL。Linux/Dolphin、Windows 11 的文件与目录 Shell 复制及缓存生命周期通过验证；Wine 明确拒绝会传播 READONLY 属性的实体缓存，并验证拒绝后无公开路径或残留暂存，不能将其计作 NTFS 成功发布覆盖。虚拟文件流继续拒绝写入，验收记录见 [VALIDATION.md](VALIDATION.md#m2-read-only-file-delivery--2026-11-16)。
