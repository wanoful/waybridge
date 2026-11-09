# Waybridge 0.3.0

这一版修复越界菜单引起父窗口缩小：每个 `xdg_popup` 映射为独立的 Windows 无边框 owned window，不进入任务栏、不抢激活。父窗口的尺寸和画面范围保持不变；嵌套菜单、父窗口移动、DPI、鼠标抓取、Esc 和关闭均单独处理，弹出位置按 Windows 工作区约束。

默认 60 FPS，采用固定截止时间调度；完全不透明的帧跳过反预乘颜色计算。媒体与窗口控制/文件使用分开的认证 TCP 连接，并用呈现确认限制每窗口最多两个在途帧，防止旧帧堆进 socket 缓冲区，两条连接都经过原来的 SSH 转发端口，不需要新增端口。

`--codec auto` 在查看端支持时为顶层窗口启用 **NVENC H.264 超低延迟编码**。Windows 通过系统 Media Foundation/D3D11 解码为 GPU 纹理，再用 shader 结合无损 alpha 遮罩交给 DirectComposition。菜单继续使用无损 PNG。硬件不可用或硬解失败时按窗口回退 PNG；`--codec png` 可以强制使用无损图像。

Windows 客户端会移除旧服务端码流中的填充 NAL，避免已实测的 Media Foundation 解码器滞留末帧并触发 PNG 回退；新服务端直接从编码输出移除这种填充。PNG 和透明遮罩通过 WIC 直接解码到上传缓冲区，减少原先 GDI+ 的中间转换。

重复最大化/还原现在复用常用尺寸的编码器和解码器，切换后仍发送新 epoch 和完整 IDR。服务端闲置缓存全局最多两个、合计最多 4 Mi 像素；资源不足时清理缓存后重试。编码器冷启动期间出现更新尺寸时跳过已过时的原始帧。Windows 立即缩放现有画面，最大化/还原立即发送尺寸，交互拖动合并 resize。首次遇到新尺寸仍需初始化，不能据此保证完全无停顿。新服务端需要重新启动后才使用这些优化；新版客户端仍兼容之前的 v3 服务端。

**需要同时更新两端到 0.3.0。** 停止旧服务端，重新启动应用并复制新的 Windows `.exe`。协议已升级为 v3，连接端口和基本启动命令不变。客户端要求 Windows 10/11。已在 Snapdragon 8cx Gen 3 / Adreno 690 实机验证 ARM64 客户端硬解、纹理呈现及静止/动画不回退；实际输入到显示的端到端延迟尚未测量。Linux NVENC 比特流、透明度和参考帧连续性已在本机 NVIDIA 显卡上测试。

Linux 仍存在同步 GPU 回读和 CPU→NVENC 输入拷贝；当前传输仍是 TCP/SSH，尚未接入 UDP/FEC 或 Sunshine/Moonlight 线协议。这是硬件媒体路径的第一版，不能据此承诺最终端到端延迟。

## 直接运行

本机已构建 Linux 服务端和 x64、ARM64 Windows 客户端：

- Linux：`target/release/waybridge`
- Windows x64：`build/windows/waybridge-client.exe`。
- Windows on ARM 原生 ARM64：`build/windows-arm64/waybridge-client.exe`。

两个 Windows 版本均只依赖系统 DLL，启动参数相同。WoA 设备可选用 ARM64 版，避免 x64 仿真开销。

在 Linux 启动测试应用：

```bash
cd /home/wano/workspace/streaming/waybridge
./target/release/waybridge -- weston-terminal
```

服务端默认创建 `waybridge-0` socket，监听 `127.0.0.1:49090`，与 Sunshine 的 47990 管理端口分开。首次运行在 `$XDG_RUNTIME_DIR/waybridge.token` 创建 0600 权限的认证 token。可以在另一个 Linux 终端读取这个文件，将 token 用于 Windows 客户端：

```bash
cat "$XDG_RUNTIME_DIR/waybridge.token"
```

把 `.exe` 复制到 Windows。在 PowerShell 中建立 SSH 隧道并保持运行：

```powershell
ssh -N -L 49090:127.0.0.1:49090 wano@<Linux-IP>
```

再开一个 PowerShell，启动客户端：

```powershell
.\waybridge-client.exe 127.0.0.1 49090 "<token>"
```

原型协议自身不加密，默认使用本地监听和 SSH 隧道。认证握手和版本检查在任何窗口数据发送前完成；一次只允许一个查看端连接。

应用可在服务端运行期间继续加入：

```bash
WAYLAND_DISPLAY=waybridge-0 weston-terminal
```

Qt、Electron、浏览器等应选择 Wayland 后端。单实例应用可能通过已有 D-Bus 服务打开到原桌面，需要使用应用的新实例参数，或用 `dbus-run-session -- <应用>` 启动独立实例。已有 KWin 窗口不会迁移到这个会话。

可用 `--render-node /dev/dri/renderD129` 指定 GPU，`--fps 60` 指定帧回调上限（现在默认 60）；`--codec auto|png|h264-nvenc` 选择编码，`--bitrate 30000` 指定每个视频流的 kbps；旧 `--quality` 参数保留兼容，但无损 PNG 不使用它；`--help` 列出完整选项。正常操作不需要 root、DRM master 或显示器。

## 可选 UDP/FEC 媒体通道

默认继续使用 TCP/SSH。显式启用 UDP 时，在 Linux 增加 `--udp-listen <可达监听地址>:49091`；`--udp-mbps 200` 控制含校验片的发送速率上限，范围 1–1000 Mbit/s。它是固定速率节流，尚无自适应拥塞控制；应低于可用带宽。Windows 保留原来的 SSH 隧道和 TCP 启动参数，另设置：

```powershell
$env:WAYBRIDGE_UDP_HOST = "<Windows 能直接访问的 Linux 地址>"
# 可选：NAT/代理暴露的端口与服务端监听端口不同时设置
$env:WAYBRIDGE_UDP_PORT = "49091"
.\waybridge-client.exe 127.0.0.1 49090 "<token>"
```

SSH `-L` 只转发 TCP，不能承载这个 UDP 通道；可使用局域网或已有 VPN 的地址，需允许该 UDP 端口。握手和媒体密钥仍走原控制连接，应保留 SSH。UDP 每会话、每方向使用独立随机 AES-256-GCM 密钥、唯一计数 nonce、认证分片和重放检查；认证成功前不向 UDP 地址发送画面。

画面分为 1024-byte 数据片，每 8 片增加一片 XOR 校验，可恢复该组单片丢失；乱序重组后按原顺序送入解码器。其余丢包通过原 TCP 媒体连接补同一帧，保留 H.264 引用链。每窗口的两帧呈现 credit 保持有效。UDP 不通时继续 TCP，已建立的 UDP 中断时用 TCP 补帧；当前不支持 UDP endpoint 漫游，需重连。协议仍为 v3，未协商的客户端不受影响。

此分支尚未实现动态码率、Reed–Solomon、多丢包低延迟 IDR 恢复或音频 UDP。重组最多 16 条消息/96 MiB，UDP 仅用于不超过 8 MiB 的帧，服务端修复缓存最多 16 条/32 MiB；超限新帧走 TCP，异常重组溢出断开会话而非破坏参考链。传输层按全局消息顺序交付，丢失一帧可能暂时阻塞其他窗口，不能据此保证弱网体验或端到端延迟优于 TCP。

## 使用数据交换

- 文本和 PNG 图片在两端自动同步剪贴板，单种格式上限 12 MiB。图片剪贴板与图片文件分别处理。
- Windows 资源管理器中复制文件，传输完成后可在支持文件粘贴的 Linux 应用里粘贴；Linux 应用提供 `text/uri-list` 时，文件下载后可粘贴到 Windows 资源管理器。
- 从 Windows 资源管理器拖文件到远端窗口，先上传文件，再由合成器向目标应用协商 Copy 拖放。当前不会在上传期间显示远端拖动预览。
- 单次最多 128 个普通文件，单文件最多 1 GiB。暂不支持目录、剪切/移动及 Linux → Windows 原生拖出。
- Linux 收到的文件保留在 `$XDG_RUNTIME_DIR/waybridge-inbox/<transfer-id>/`；Windows 文件保留在 `%TEMP%\Waybridge-*`。不在粘贴后立即删除，因为应用可能延迟读取。临时文件可在不再需要后手动清理。

应用对粘贴和拖入的数据类型有自己的限制；私有应用格式不做跨平台转换。

## 构建

本机已有所需工具。Rust 依赖版本由 `Cargo.lock` 固定。Linux 需要 EGL/GLES、libdrm、Wayland、xkbcommon、FFmpeg（libavcodec/libavutil）开发文件及 pkg-config；使用 NVENC 还需要系统 FFmpeg 的 h264_nvenc 编码器和 NVIDIA 驱动；构建原生 Windows 客户端需要 MinGW-w64、CMake 和 Ninja。

```bash
./scripts/build.sh
```

原生 Windows ARM64 客户端使用 LLVM-MinGW 的 `aarch64-w64-mingw32-clang++` 交叉编译，输出目录为 `build/windows-arm64/`，不需要重编 Linux 服务端：

```bash
./scripts/build-windows-arm64.sh
# 若工具链不在 PATH 或本机默认位置，指定其根目录：
LLVM_MINGW_ROOT=/path/to/llvm-mingw ./scripts/build-windows-arm64.sh
```

也可在 Windows 上使用 MSVC/CMake 构建 `client/`。客户端使用 Win32、D3D11、DXGI、DirectComposition、GDI+、系统 Media Foundation 和系统剪贴板 API，无 WSL 或 Qt 运行时依赖。

## 验证

```bash
./scripts/test.sh
# 可选：安装了 Wine 和 Xvfb 时，验证真正的 Windows .exe
python tests/windows_smoke.py
python tests/windows_smoke.py --csd
python tests/windows_smoke.py --terminal
python tests/windows_smoke.py --qt
# 可选：需要 NVIDIA/NVENC；真实硬件编码、P 帧、alpha、奇数尺寸及回退
python tests/hardware_smoke.py
# 可选：真实 NVENC 尺寸切换、缓存淘汰、IDR/P 引用与回环耗时
python tests/resize_smoke.py
# 可选：安装了 Ghostty 时，验证真实 GTK 越界菜单
python tests/ghostty_smoke.py
# 可选：实际 PE/CNG UDP 解密、丢包/乱序/重复/损坏、XOR/TCP 修复、真实 NVENC P 链
python tests/windows_udp_smoke.py
python tests/windows_smoke.py --udp --qt
```

`tests/smoke.py` 启动隔离的真实合成器和两个原生 Wayland 测试窗口，检查图像颜色/方向、尺寸、输入、断线、重连、双向文本/PNG/文件、文件拖放和路径校验。依赖 `python-pillow`、`wl-clipboard`、`wayland-scanner`、C 编译器。`tests/qt_smoke.py` 需要 Qt6Widgets 开发文件与 C++ 编译器，验证真实 Qt 的 175%/125% 动态缩放、精确透明度、文字输入、菜单和模态对话框；有 Dolphin 时还会在独立 D-Bus/配置目录中验证路径输入及导航。

`tests/windows_smoke.py` 使用项目自己的 Wine prefix 与 Xvfb 显示，检查 `.exe` 的窗口像素、真实尺寸调整、原生剪贴板和文件拖放，不操作用户桌面。Wine 测试明确使用双缓冲 GDI 备用路径，不能代替在实际 Windows 机器上验证 DirectComposition 的透明合成和呈现效果。

## 在 Windows 实机检查这一版

连接后应出现 `D3D11/DirectComposition presentation active`。硬件媒体路径成功时，Linux 另有 `NVENC H.264 ultra-low-latency encoder active`，Windows 有 `D3D11 hardware H.264 decoder active for window ...`。客户端启动时会打印 H.264 能力；若不可用，将继续使用 PNG。`GDI fallback` 代表备用绘制，不支持逐像素透明。

1. Windows 保持 175% 缩放，打开 Ghostty 的越界菜单；检查父窗口大小和文字比例保持不变，菜单可正常操作，Esc、点击外部和嵌套菜单正常。
2. 滚动终端、连续输入，检查两端实际启用的编码日志。对比 `--codec png` 与 `--codec auto`，使用相同窗口尺寸和内容。
3. 自动测试构建后，可用 `WAYLAND_DISPLAY=waybridge-0 ./build/tests/qt-app` 打开验证应用。`Ctrl+O` 打开靠近右下角的越界/嵌套菜单；透明窗口左侧不透明红色、中间半透明绿色、右侧透出 Windows 背景。

诊断延迟时，在 Linux 启动：

```bash
./target/release/waybridge --stats --codec auto -- ghostty
```

Windows PowerShell：

```powershell
$env:WAYBRIDGE_STATS = "1"
.\waybridge-client.exe 127.0.0.1 49090 "<token>"
```

Linux 每两秒记录合成、回读、编码排队、颜色转换、NVENC/PNG、发送及呈现确认往返的平均/最大耗时。Windows 记录解码排队、解码、UI 排队、提交呈现、GDI 备用绘制，以及控制通道 RTT。均为各端自身时钟上的阶段耗时，**不等于输入到显示的端到端延迟**；`present` 是提交耗时，不含最终显示器扫描输出。`send` 是写入 socket 的耗时；`display_roundtrip` 包含传输、客户端处理和呈现确认返回，也不等于单向显示延迟。

尺寸切换另记录服务端 `encoder_init`、`encoder_reuse`（命中次数）、`resize_first_encoded`、`resize_first_sent`。后两项从开始处理 resize 到首个匹配目标像素尺寸的帧完成编码/写入 socket，跨越了应用重新布局、Wayland 提交、捕获及编码；不包含 Windows 解码与显示。Windows 的 `decoder_create`、`decoder_reuse` 和 `resize_to_present` 用于对照，其中 `resize_to_present` 从发送 resize 到匹配逻辑尺寸的帧提交呈现，也不包含最终屏幕扫描输出。

开发诊断环境变量：`WAYBRIDGE_DPI=168` 强制 175%，`WAYBRIDGE_GDI=1` 强制备用绘制，`WAYBRIDGE_PNG=1` 禁用查看端 H.264 协商。正常使用跟随 Windows 的实际 DPI。清除环境变量恢复自动选择。

## 当前边界与后续工作

- 改进 GPU 直接送入编码器与异步回读，减少 CPU 往返；完善 UDP 自适应拥塞控制和多丢包恢复。
- 首版硬件路径为 H.264 4:2:0 SDR，彩色文字可能不如 PNG 清晰；4:4:4、HEVC/AV1、HDR 和高刷新率仍需扩展、验证。
- 每个视频顶层窗口使用一个 NVENC 会话；会话数量受硬件/驱动限制，不能分配时回退 PNG。视频支持范围为 48×48 至 4096×2304，其他尺寸走 PNG。
- 完善模态协议、窗口状态和光标/输入区域映射。
- 透明区域的视觉 alpha 与鼠标穿透分别处理；尚未向 Windows 完整映射 Wayland 输入区域，也未实现 KDE 毛玻璃。
- 接入 Xwayland，完善输入法、相对鼠标、指针形状和显式同步。
- 实现 Linux → Windows 原生拖出、目录和按需文件访问。
- 完善音频、断点续传、文件传输进度/取消、端到端加密及会话管理。

普通 `wl_shm` 客户端和 DMA-BUF 导入已接通；各类 GPU 应用、驱动及大型应用组合仍需逐一验证。此版本不保证 “All 应用” 的兼容性。
