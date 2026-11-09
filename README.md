# Waybridge

Linux 应用在 Windows 上显示为独立原生窗口的开发原型。服务端是自建的无头 Wayland 合成器，基于 Smithay，不连接 KWin，也不向宿主桌面注入输入。

项目在 [waybridge](waybridge/) 中。`sunshine/` 和 `moonlight/` 是本地参考用的上游源码，不纳入本仓库；版本记录在 [upstreams.json](upstreams.json)。**当前原型使用独立的 H.264＋alpha／PNG 协议，媒体与控制使用分开的 TCP 连接，还不能直接用普通 Moonlight 连接**。Sunshine/Moonlight 线协议集成尚未实现。

新增功能的首版已分别在独立 worktree 提交，见 [并行功能分支与测试清单](PARALLEL_FEATURES.md)。根目录当前是基础原型与 IME 分支，尚未合入其他功能；HDR 为实验 scRGB 子集。

已实现：

- 多个 Wayland 顶层窗口对应多个 Win32 窗口。
- XDG 应用 PNG/SVG 图标映射到 Windows 大小图标，跟随 DPI。
- 鼠标、键盘、滚轮、焦点、关闭及真实应用尺寸调整。
- 原生光标形状、隐藏、自定义及动画光标，跟随 DPI 和热点。
- 跟随 Windows 显示器 DPI 的应用绘制，Qt 分数缩放、菜单抓取与对话框父子关系。
- Windows D3D11/DirectComposition 保留帧呈现和透明度合成；编码、解码在后台线程。
- NVENC H.264 低延迟编码、Windows GPU 解码、独立越界菜单和有界尺寸缓存。
- Windows x64 和原生 ARM64 客户端构建。
- Windows 本地 IME 的 Wayland text-input-v3 桥接：预编辑、中文/emoji 提交及光标定位。
- 双向文本、PNG 图片剪贴板。
- 双向文件复制粘贴，64 KiB 分块传输。
- Windows 文件拖入远端窗口，在 Wayland 侧完成文件拖放。
- 会话断线保留应用，重连恢复窗口，断线释放按键。

这是开发原型，基础 ARM64 硬解和尺寸切换已在 Snapdragon 8cx Gen 3 / Adreno 690 实机验证。AV1、音频、X11 和 Linux → Windows 拖出等新增分支的实机验收仍待进行；XIM 与输入法重转换尚未实现。微软拼音实际候选面板、更广泛的设备兼容性和输入到显示的端到端延迟仍需验证。

详见 [运行说明与当前边界](waybridge/README.md)、[协议](waybridge/PROTOCOL.md)。
