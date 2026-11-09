# 并行功能分支

14 个功能 worktree 的首版均已提交；HDR 为实验 scRGB 子集，完整 HDR 仍为 partial。尚未合并到 `main`，其基础提交保持 `7cad156`。根目录位于 `codex/windows-ime`，其余 13 个 worktree 位于 `.worktrees/`。

| 功能 | 分支 | 首版状态 | 代码提交 | 文档 |
|---|---|---|---|---|
| Windows 本地 IME | `codex/windows-ime` | 已提交 | `fc6cba41` | [README](/home/wano/workspace/streaming/waybridge/README.md) |
| 光标同步 | `codex/native-cursor` | 已提交 | `d838373` | [README](/home/wano/workspace/streaming/.worktrees/native-cursor/waybridge/README.md) |
| 应用图标 | `codex/app-icons` | 已提交 | `a9ea5d9` | [README](/home/wano/workspace/streaming/.worktrees/app-icons/waybridge/README.md) |
| 模态/窗口状态/输入区域 | `codex/window-semantics` | 已提交 | `3285963` | [README](/home/wano/workspace/streaming/.worktrees/window-semantics/waybridge/README.md) |
| Linux 到 Windows 原生文件拖出 | `codex/file-drag-out` | 已提交 | `999bfc9` | [README](/home/wano/workspace/streaming/.worktrees/file-drag-out/waybridge/README.md) |
| 目录传输 | `codex/directory-transfer` | 已提交 | `bec86e5` | [README](/home/wano/workspace/streaming/.worktrees/directory-transfer/waybridge/README.md) |
| 按需虚拟文件 | `codex/virtual-files` | 已提交 | `f5fda93` | [README](/home/wano/workspace/streaming/.worktrees/virtual-files/waybridge/README.md) |
| 相对鼠标/指针锁定 | `codex/relative-pointer` | 已提交 | `1b8090d` | [README](/home/wano/workspace/streaming/.worktrees/relative-pointer/waybridge/README.md) |
| X11 应用兼容 | `codex/xwayland` | 已提交 | `425f756e` | [README](/home/wano/workspace/streaming/.worktrees/xwayland/waybridge/README.md) |
| 音频串流 | `codex/audio` | 已提交 | `ad2021d4` | [README](/home/wano/workspace/streaming/.worktrees/audio/waybridge/README.md) |
| HEVC/AV1 | `codex/video-codecs` | 已提交 | `16a5d58` | [README](/home/wano/workspace/streaming/.worktrees/video-codecs/waybridge/README.md) |
| UDP/FEC | `codex/udp-fec` | 已提交 | `a44ce94` | [README](/home/wano/workspace/streaming/.worktrees/udp-fec/waybridge/README.md) |
| GPU 直接编码 | `codex/gpu-interop` | 已提交 | `d0350e7` | [README](/home/wano/workspace/streaming/.worktrees/gpu-interop/waybridge/README.md) |
| HDR 色彩链路 | `codex/hdr` | 实验 scRGB 子集 | `01d9150` | [README](/home/wano/workspace/streaming/.worktrees/hdr/waybridge/README.md) |

## 验证与边界

各分支均构建了 Windows x64、原生 ARM64 客户端。验证使用真实 Wayland/Qt、真实 NVIDIA NVENC/CUDA，以及隔离 Wine/Xvfb 中的 Windows PE/API；没有重启或替换已有 49090 服务。每个分支的详细场景和限制见其 README/VALIDATION。

- IME：原生 SendInput、Qt/Ghostty、Unicode/emoji、组合边界及快捷键通过；审查修复了 Ctrl+Space 重复字符和组合后的修饰键恢复。微软拼音的实际候选面板、选词和切换仍待 WoA 实测。
- 数据交换：原生拖出、双向目录和空目录，以及无 CF_HDROP 的跨进程虚拟源已验证，包含 21 项虚拟文件场景。Linux 完整落盘后发布文件，尚无 FUSE；Explorer/Outlook/压缩软件与真实 COM 中断需实机验证。
- 窗口/输入：光标、图标、全屏/模态/尺寸约束/隐藏/输入区域、相对鼠标和锁定均有真实 Wayland/Win32 测试；HTTRANSPARENT 仅覆盖同一 UI 线程，尚非跨进程点穿。
- X11：真实 GLX 动画、175% Qt/XCB、精确透明度、嵌套/越界菜单和 Tooltip 捕获、父窗口移动、重连、共存及退出清理通过。XIM/XDND/PRIMARY、完整 EWMH 和自动崩溃重启未实现。
- 音频：私有 sink → 独立 PCM 通道 → 实际 PE/WASAPI → 私有输出回录通过；首版无 Opus、麦克风、严格音画同步或设备热切换。
- 编码：H.264/HEVC/AV1 实际 NVENC 比特流和引用链通过。CUDA 颜色输入成功路径省去 CPU 颜色回读/上传；透明遮罩仍回读，需显式 opt-in，不能称整条链路零拷贝。
- UDP：加密认证、单片 XOR、乱序/重复/损坏、TCP 补帧通过。审查修复了满缓存头补帧拒绝和断线残留发送；20 真实窗口/40 帧、600 ms 延迟补帧、全局 8 条上限与断线后 0 包均有回归。尚无自适应拥塞控制。
- HDR：scRGB FP16 → 线性合成 → P010/PQ → HEVC/AV1 Main10 已验证，两个 codec 各 15 帧且 277 级解码亮度；alpha16、缓存、回退和 Windows shader 数值通过。ICC、任意 parametric、HDR X11 与物理 WoA HDR/MFT 不在已验收范围。

物理 WoA 上的新功能与组合效果仍待实测；Wine 结果不代表其驱动、显示效果或真实输入到显示延迟。当前没有包含所有功能的集成客户端。

## 依赖与整合

- `directory-transfer` 继承 `file-drag-out`；`virtual-files` 继承目录传输。
- `gpu-interop` 继承 `video-codecs`；HDR 继承 GPU 分支，并使用同一份 Smithay 修复。
- `xwayland` 包含 Smithay 修复提交 `ea515b2` 与 colormap 修正 `c9f16b5`；来源、许可证和修改原因见 [PATCHES.md](/home/wano/workspace/streaming/.worktrees/xwayland/waybridge/vendor/smithay/PATCHES.md)。HDR 已同步相同 vendor 内容。
- 协议保持 v3，以能力字段协商。二进制 kind：1 PNG、2 H264、3 PCM 音频、4 HEVC、5 AV1、6 UDP TCP 补帧容器、7/8 实验 Main10。
- 合并 UDP 与 codecs/HDR 时，允许原始 kind `1/2/4/5/7/8`；`6` 只用于补帧容器。保留全局消息流控、视频参考顺序与每窗口两帧呈现 credit，音频使用独立连接/credit。
- 光标与相对鼠标：锁定须覆盖 `WM_SETCURSOR` 和光标刷新；解锁重新取得当前光标，不能恢复动画期间已经释放的 HCURSOR。
- IME、OLE 和窗口状态：保留焦点 generation、组合取消、迟到字符路由和按键释放；原生拖放接管前清理 IME 键状态，避免恢复旧目标的修饰键。
- X11 使用键盘焦点枚举；IME 从中取得底层 wl_surface，窗口状态读取需区分 X11 与 Wayland toplevel。
- 音频与 X11 的信号处理需要统一退出标志，确保两种资源都清理。网络 listener 的 audio/UDP/HDR 参数也需统一。
- 保留 HDR 分支对 PNG→任意视频恢复的 epoch/IDR 修复，不能只在 HDR 切换时生效。

## 单独构建和测试

进入对应 worktree 的 `waybridge` 目录，先阅读该分支 README 的开关与测试命令。每个 worktree 有自己的 `target/` 和 `build/`，测试时请使用独立端口、Wayland socket 和 token 文件。

```bash
cargo build --release --locked
./scripts/build-windows-arm64.sh
```

原生 ARM64 客户端位于各自 worktree 的 `waybridge/build/windows-arm64/waybridge-client.exe`。选择相同分支的服务端与客户端测试；单个分支不包含其他所有并行功能。

| 需显式启用的功能 | 服务端开关 |
|---|---|
| 音频 | `--audio` |
| X11 | `--xwayland` |
| GPU 颜色输入 | `--gpu-input cuda` |
| HEVC / AV1 | `--codec hevc-nvenc` / `--codec av1-nvenc` |
| UDP | `--udp-listen <地址>:<端口>`，Windows 另设 `WAYBRIDGE_UDP_HOST` |
| 实验 HDR | `--hdr experimental-scrgb --codec hevc-nvenc` 或 AV1；客户端必须实际满足 HDR/硬解条件 |

其他交互功能主要通过能力协商自动启用。每个分支的 README 包含其实际支持范围与回归入口。
