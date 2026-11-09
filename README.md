# Waybridge

Linux 应用在 Windows 上显示为独立原生窗口的开发原型。服务端是基于 Smithay 的自建无头 Wayland 合成器，不依赖 KWin，也不向宿主桌面注入输入。

14 项并行功能已整合到同一套源码和构建中，覆盖 Windows IME、光标、应用图标、窗口语义、双向文件拖放、目录、虚拟文件、相对鼠标、Xwayland、音频、HEVC/AV1、UDP/FEC、GPU 颜色输入和实验 HDR。来源与验收范围见 [功能整合清单](PARALLEL_FEATURES.md)。

- Wayland 与可选 X11 应用分别映射为 Win32 窗口；越界菜单独立显示，跟随 Windows DPI，支持透明度、模态、全屏和尺寸约束。
- Windows 本地 IME 桥接 Wayland text-input-v3；原生光标、应用图标和受控指针锁定。
- 双向文本、PNG、文件及目录剪贴板；Windows 普通/虚拟文件拖入，Linux 文件按 Windows 目标读取请求拖出。
- 默认 H.264 超低延迟 NVENC，支持可选 HEVC/AV1、独立无损 alpha、Windows GPU 解码、CUDA GL 颜色输入及实验 scRGB Main10 HDR。
- 可选加密 UDP/XOR FEC 与 TCP 补帧；私有会话音频通过独立 PCM/WASAPI 通道播放。
- Windows x64 和原生 ARM64 客户端；断线保留应用，重连恢复窗口并释放旧输入状态。

项目在 [waybridge](waybridge/) 中；[运行与构建说明](waybridge/README.md)、[协议](waybridge/PROTOCOL.md)、[验证记录及未验证范围](waybridge/VALIDATION.md)。协议为独立 v3，普通 Moonlight 不能直接连接。`sunshine/` 和 `moonlight/` 是未修改的本地参考源码，不纳入仓库；版本见 [upstreams.json](upstreams.json)。

历史 ARM64 H.264/透明呈现已在 Snapdragon 8cx Gen 3 / Adreno 690 验证；这不等于已验收整合版的所有新功能。微软拼音实际候选面板、WoA HEVC/AV1/Main10、物理 HDR 显示与 Explorer/Outlook 等兼容性仍需实机验证。HDR 目前只支持显式标记的 Wayland scRGB 子集。此原型不保证所有应用、设备兼容性或相对 Moonlight 的端到端延迟。
