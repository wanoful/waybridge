# Waybridge 0.3.0

这一版修复越界菜单引起父窗口缩小：每个 `xdg_popup` 映射为独立的 Windows 无边框 owned window，不进入任务栏、不抢激活。父窗口的尺寸和画面范围保持不变；嵌套菜单、父窗口移动、DPI、鼠标抓取、Esc 和关闭均单独处理，弹出位置按 Windows 工作区约束。

默认 60 FPS，采用固定截止时间调度；完全不透明的帧跳过反预乘颜色计算。媒体与窗口控制/文件使用分开的认证 TCP 连接，并用呈现确认限制每窗口最多两个在途帧，防止旧帧堆进 socket 缓冲区，两条连接都经过原来的 SSH 转发端口，不需要新增端口。

`--codec auto` 在查看端支持时为顶层窗口启用 **NVENC H.264 超低延迟编码**。Windows 通过系统 Media Foundation/D3D11 解码为 GPU 纹理，再用 shader 结合无损 alpha 遮罩交给 DirectComposition。菜单继续使用无损 PNG。硬件不可用或硬解失败时按窗口回退 PNG；`--codec png` 可以强制使用无损图像。

Windows 客户端会移除旧服务端码流中的填充 NAL，避免已实测的 Media Foundation 解码器滞留末帧并触发 PNG 回退；新服务端直接从编码输出移除这种填充。PNG 和透明遮罩通过 WIC 直接解码到上传缓冲区，减少原先 GDI+ 的中间转换。

重复最大化/还原现在复用常用尺寸的编码器和解码器，切换后仍发送新 epoch 和完整 IDR。服务端闲置缓存全局最多两个、合计最多 4 Mi 像素；资源不足时清理缓存后重试。编码器冷启动期间出现更新尺寸时跳过已过时的原始帧。Windows 立即缩放现有画面，最大化/还原立即发送尺寸，交互拖动合并 resize。首次遇到新尺寸仍需初始化，不能据此保证完全无停顿。新服务端需要重新启动后才使用这些优化；新版客户端仍兼容之前的 v3 服务端。

**需要同时更新两端到 0.3.0。** 停止旧服务端，重新启动应用并复制新的 Windows `.exe`。协议已升级为 v3，连接端口和基本启动命令不变。客户端要求 Windows 10/11。已在 Snapdragon 8cx Gen 3 / Adreno 690 实机验证 ARM64 客户端硬解、纹理呈现及静止/动画不回退；实际输入到显示的端到端延迟尚未测量。Linux NVENC 比特流、透明度和参考帧连续性已在本机 NVIDIA 显卡上测试。

默认 CPU 路径仍存在同步 GPU 回读和 CPU→NVENC 输入拷贝；当前传输仍是 TCP/SSH，尚未接入 UDP/FEC 或 Sunshine/Moonlight 线协议。这是硬件媒体路径的第一版，不能据此承诺最终端到端延迟。

可用 `--codec hevc-nvenc` 或 `--codec av1-nvenc` 显式试用新编码；`auto` 仍使用
H.264。两种新编码保留独立无损 alpha、尺寸 epoch、关键帧和两帧呈现确认。
客户端通过系统 Media Foundation 枚举同步 D3D11-aware 解码器，并实际解码一个
小关键帧得到 NV12 GPU 纹理后才广告该能力；不可用、初始化/解码/呈现失败时回退
PNG。系统解码器安装情况、GPU/驱动和尺寸能力会影响可用性。新路径仅为 8-bit
4:2:0 SDR，不包含 HDR。Linux NVENC 与码流/alpha 已实际验证；Windows/WoA
新 codec 的硬解可用性、性能和应用体验尚待实机确认。

在实际 Windows 设备上先运行 `waybridge-client.exe --check-codecs` 查看能力。
诊断探针支持 `waybridge-decode-probe.exe BITSTREAM SIZES WIDTH HEIGHT hevc|av1`；
默认不指定 codec 时保持原 H.264 行为。Linux 可运行 `python tests/codecs_smoke.py`
验证真实 NVENC、alpha、缓存尺寸切换、协商和回退；`python tests/windows_codecs_smoke.py`
在私有 Wine/Xvfb 桌面验证未广告能力和安全 PNG 回退，不代替 Windows 硬解测试。

## 可选 CUDA GL 颜色输入

在同一 NVIDIA GPU 上，可用 `--gpu-input cuda` 显式启用 GL renderbuffer → CUDA
→ NVENC 输入；默认 `cpu` 保持原路径。实现通过公开 GL framebuffer attachment
查询取得资源，CUDA kernel 在 GPU 上反预乘并处理方向/奇数尺寸，再交给 FFmpeg
CUDA RGBA 帧。成功路径不把颜色读回 CPU，也不从 CPU 上传颜色；仍有 GPU 内部
复制、同步及 NVENC 色彩转换，因此不是整条管线零拷贝。

透明窗口仍将无损 alpha 以每像素 1 字节读回 CPU 并压为 PNG；完全不透明时只读
4 字节透明度标志，不回读整张 alpha。GL/CUDA 交接和 alpha 回读目前是同步的。
PNG、弹出窗口和不支持的 codec/尺寸继续走 CPU。初始化/注册失败保留 CPU 输入；
GPU 编码失败保留当前帧的 PNG，触发一次新的 CPU 捕获，并以新 epoch 的 CPU
NVENC 关键帧恢复。每个查看会话/窗口发生失败后不再反复尝试 GPU 路径。

正常构建仅在 `WAYBRIDGE_CUDA_ROOT`（默认 `/opt/cuda`）存在 CUDA 开发头文件及 GL 开发头文件时
编入可选 interop；运行时动态加载系统 `libcuda.so.1`，不新增 libcudart/libnvrtc 运行依赖。
缺失头文件的构建仍可正常运行，指定 `cuda` 时会记录原因并回退。PTX 来自仓库内
kernel 源码，可用 `bash scripts/build-gpu-kernel.sh` 重新生成；预生成 PTX 需要
兼容的 NVIDIA 驱动。此版仅支持 RGBA8 SDR，同 GPU 限制由 CUDA GL 查询校验。

`python tests/gpu_smoke.py --benchmark` 检查真实 GL/CUDA/NVENC、每像素/alpha 和
编码错误恢复，并输出 Release CPU/CUDA 对照到 `build/diagnostics/gpu-input-benchmark.json`。
本机 RTX 5070 Ti 上，颜色输入及编码阶段在 1080p 从约 6.65/9.20 ms（不透明/透明）
降到 1.67/2.40 ms；1440p 从约 10.80/15.35 ms 降到 2.40/3.37 ms。这些是本机
合成测试的阶段耗时，不是物理输入到屏幕延迟，也不代表其他设备性能。

## 实验 scRGB HDR 子集（partial）

默认仍为 8-bit BT.709 SDR。服务端显式使用
`--hdr experimental-scrgb --codec hevc-nvenc`（或 `av1-nvenc`）；Windows
PowerShell 设置 `$env:WAYBRIDGE_HDR="1"` 后运行客户端。此模式要求同一 NVIDIA
GPU 执行 GL/CUDA/NVENC，自动为 HDR 开启 CUDA 输入；`auto` 仍保持 H.264 SDR。

只有通过 `wp_color_manager_v1.create_windows_scrgb` 明确标记的 Wayland surface
才进入 HDR。RGB 是线性 Rec.709/scRGB，每单位为 80 nits。surface tree 在真正的
RGBA16F target 合成；未标记的 SDR 子表面先从 sRGB 转线性再混合。CUDA 反预乘、
转换 BT.2020/PQ/P010，NVENC 输出 HEVC/AV1 Main10。透明度独立以 16-bit PNG
存储 **原始 FP16 alpha 位**，没有量化成 8 位；成功路径不回读颜色，透明帧仍回读
每像素 2 字节 alpha，并保留一张 GPU SDR 备用图。不是全程零拷贝。

查看端须实际解码 Main10 探针得到 P010 GPU 纹理，验证 P010 plane views、精度和
FP16/scRGB composition swapchain，才广告 HDR。每个窗口仅在其显示器已处于 HDR
模式时请求 HDR；窗口移动/显示设置变化会重新检查。P010/PQ shader 转回线性 scRGB
并以预乘 FP16 交给 DirectComposition。不更改系统 HDR 设置。显示器 SDR、缺解码器、
GPU/编码错误或不支持的色域/非有限源值都会回退 SDR：线性 Rec.709 按
`rgb/(1+luminance)` 做 Reinhard 映射后转 sRGB。解码/呈现错误按窗口回退 PNG。

这是可验证的首个 HDR 子集，**整体 HDR 仍是 partial**。ICC、任意 parametric
BT.2020/PQ 描述、广义色域映射、HDR X11 应用和物理 Windows/WoA HDR 显示/MFT
仍未完成验证。超出 BT.2020 正值 0–10000 nit 的 scRGB 不会被裁剪后冒充正确 HDR。
一般应用若未明确标记 scRGB，仍按 SDR 处理；SDR 首选/output 描述保持真实。

`waybridge --check-hdr` 离屏测量 RGB10/FP16 和真实 Main10 编解码，不创建
会话/监听。`waybridge-client.exe --check-hdr` 只读查询显示模式及 FP16 能力；
`--check-hdr-shaders` 离屏执行实际 P010/PQ/scRGB shader 数值测试。Linux 可运行
`python tests/hdr_smoke.py`、`python tests/hdr_video_smoke.py`；私有 Wine/Xvfb 数值
与原始 alpha 检查为 `python tests/hdr_windows_smoke.py`，不代替物理 HDR 显示验收。
协议 v3 为 HDR 使用 kind7/8 与 `hevc_main10_pq_alpha16` /
`av1_main10_pq_alpha16` 能力，保留 60-byte 视频 header；kind6 留给 UDP repair。
测试需要 NumPy、Pillow、CUDA 开发头及真实 NVENC。源码内 vendor Smithay 保留 Xwayland 补丁，并修复同尺寸 SHM 格式切换的纹理缓存。

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

可用 `--render-node /dev/dri/renderD129` 指定 GPU，`--fps 60` 指定帧回调上限（现在默认 60）；`--codec auto|png|h264-nvenc|hevc-nvenc|av1-nvenc` 选择编码，`--bitrate 30000` 指定每个视频流的 kbps；旧 `--quality` 参数保留兼容，但无损 PNG 不使用它；`--help` 列出完整选项。正常操作不需要 root、DRM master 或显示器。

## 使用数据交换

- 文本和 PNG 图片在两端自动同步剪贴板，单种格式上限 12 MiB。图片剪贴板与图片文件分别处理。
- Windows 资源管理器中复制文件，传输完成后可在支持文件粘贴的 Linux 应用里粘贴；Linux 应用提供 `text/uri-list` 时，文件下载后可粘贴到 Windows 资源管理器。
- 从 Windows 资源管理器拖文件到远端窗口，先上传文件，再由合成器向目标应用协商 Copy 拖放。当前不会在上传期间显示远端拖动预览。
- 单次最多 128 个普通文件，单文件最多 1 GiB。暂不支持目录、剪切/移动及 Linux → Windows 原生拖出。
- Linux 收到的文件保留在 `$XDG_RUNTIME_DIR/waybridge-inbox/<transfer-id>/`；Windows 文件保留在 `%TEMP%\Waybridge-*`。不在粘贴后立即删除，因为应用可能延迟读取。临时文件可在不再需要后手动清理。

应用对粘贴和拖入的数据类型有自己的限制；私有应用格式不做跨平台转换。

## 构建

本机已有所需工具。Rust 依赖版本由 `Cargo.lock` 固定。Linux 需要 EGL/GLES、libdrm、Wayland、xkbcommon、FFmpeg（libavcodec/libavutil）开发文件及 pkg-config；使用 NVENC 还需要系统 FFmpeg 的对应 h264_nvenc/hevc_nvenc/av1_nvenc 编码器和 NVIDIA 驱动；构建原生 Windows 客户端需要 MinGW-w64、CMake 和 Ninja。

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
```

`tests/smoke.py` 启动隔离的真实合成器和两个原生 Wayland 测试窗口，检查图像颜色/方向、尺寸、输入、断线、重连、双向文本/PNG/文件、文件拖放和路径校验。依赖 `python-pillow`、`wl-clipboard`、`wayland-scanner`、C 编译器。`tests/qt_smoke.py` 需要 Qt6Widgets 开发文件与 C++ 编译器，验证真实 Qt 的 175%/125% 动态缩放、精确透明度、文字输入、菜单和模态对话框；有 Dolphin 时还会在独立 D-Bus/配置目录中验证路径输入及导航。

`tests/windows_smoke.py` 使用项目自己的 Wine prefix 与 Xvfb 显示，检查 `.exe` 的窗口像素、真实尺寸调整、原生剪贴板和文件拖放，不操作用户桌面。Wine 测试明确使用双缓冲 GDI 备用路径，不能代替在实际 Windows 机器上验证 DirectComposition 的透明合成和呈现效果。

## 在 Windows 实机检查这一版

连接后应出现 `D3D11/DirectComposition presentation active`。硬件媒体路径成功时，Linux 另有 `NVENC ultra-low-latency encoder active` 和 `codec="H.264"`，Windows 有 `D3D11 hardware H.264 decoder active for window ...`。客户端启动时会打印 H.264 能力；若不可用，将继续使用 PNG。`GDI fallback` 代表备用绘制，不支持逐像素透明。

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

- 扩展可选 CUDA GL 输入的驱动/多 GPU 兼容性与异步同步；增加 UDP/FEC 和拥塞控制。
- 视频路径为 8-bit 4:2:0 SDR，彩色文字可能不如 PNG 清晰；默认 H.264，可选 HEVC/AV1 的 Windows 硬解仍需实机验证。4:4:4 和高刷新率仍需扩展；上方实验 scRGB HDR 子集仍为 partial。
- 每个视频顶层窗口使用一个 NVENC 会话；会话数量受硬件/驱动限制，不能分配时回退 PNG。视频包尺寸范围为 48×48 至 4096×2304，各 codec 的实际最小尺寸另受驱动限制，不支持时走 PNG。
- 完善模态协议、窗口状态和光标/输入区域映射。
- 透明区域的视觉 alpha 与鼠标穿透分别处理；尚未向 Windows 完整映射 Wayland 输入区域，也未实现 KDE 毛玻璃。
- 接入 Xwayland，完善输入法、相对鼠标、指针形状和显式同步。
- 实现 Linux → Windows 原生拖出、目录和按需文件访问。
- 完善音频、断点续传、文件传输进度/取消、端到端加密及会话管理。

普通 `wl_shm` 客户端和 DMA-BUF 导入已接通；各类 GPU 应用、驱动及大型应用组合仍需逐一验证。此版本不保证 “All 应用” 的兼容性。
