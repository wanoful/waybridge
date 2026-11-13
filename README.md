# Waybridge 0.3.0

配置归属、桌面集成边界及 Waybridge 专用 `.desktop` 追加/覆写的已确认设计见 [配置决策](CONFIGURATION_POLICY.md)；这是后续实施方案，当前还不能使用其中规划的配置入口。

这是 Linux → Windows 独立应用窗口原型。服务端是私有无头 Smithay Wayland 合成器，可选择启动 rootless Xwayland；不接入 KWin，不迁移已有桌面窗口。Windows 客户端为 Win32，提供 x64 和原生 ARM64 构建。功能来源见 [整合清单](PARALLEL_FEATURES.md)，实际验收范围见 [VALIDATION.md](VALIDATION.md)。

本目录是 Git 仓库根目录：`src/` 是 Linux 合成器，`native/` 是编解码与 GPU 适配，`client/` 是 Windows 原生内核，`gui/` 是控制中心，`scripts/` 和 `tests/` 分别提供构建与验证入口。构建产物写入已忽略的 `target/`、`build/` 和 .NET `bin/`、`obj/`。

现有能力与目标范围见 [功能规格](FEATURES.md)，技术边界见 [架构](ARCHITECTURE.md)，里程碑见 [Roadmap](ROADMAP.md)。默认采用[共享用户数据与单向会话检查](SESSION_POLICY.md)：先注销同一用户的本地桌面，再创建 Waybridge；已有会话可以重连。M0/M1 设备验证与本次策略变更分开记录，见 [VALIDATION.md](VALIDATION.md)。

同级 `../sunshine/` 和 `../moonlight/` 是未修改的本地参考源码，不纳入本仓库；参考版本见 [upstreams.json](upstreams.json)。完整 Git 历史与功能来源分支保留，早期提交使用 `waybridge/` 路径前缀。协议为独立 v3，普通 Moonlight 不能直接连接。

每个顶层窗口对应一个 Windows 窗口，标题栏只由一端拥有；越界/嵌套菜单映射为独立 owned popup，不改变父窗口尺寸。绘制跟随 Windows DPI，Qt 分数缩放、透明 alpha、父子对话框与原生输入分别处理。断线保留应用，重连恢复窗口；旧焦点的按键、组合和指针锁定不会继续作用于新目标。

默认 `--codec auto` 在 SDR 下选择 NVENC H.264，启用 HDR 偏好时选择 HEVC，菜单使用 PNG；实际路径仍取决于能力协商。可选 HEVC/AV1 保留独立无损 alpha。Windows 使用实际能力探测后的系统 Media Foundation/D3D11 解码和 DirectComposition；不可用或运行失败按窗口回退 PNG。`--codec png` 强制无损图像。默认 60 FPS，原始更新合并，编码引用帧有序，每窗口最多两个呈现确认在途帧。

控制/文件、媒体与可选音频使用独立认证连接，可经同一 TLS 入口或 SSH 转发端口承载。UDP 是额外可直达端口，保留原 TCP 控制/补帧。编码、解码和两端文件接收落盘在工作线程执行，Windows 输入发送使用有界有序队列。文件字节仍与控制共用通道，网络调度优化留到 M2。常见尺寸有界缓存，复用后仍强制新 epoch/独立关键帧；首次遇到新尺寸、资源限制和同步回读仍可能产生停顿。

**请同时更新并重启两端，再启动远端应用。** 协议保持独立 v3，不兼容普通 Moonlight。Windows 要求 Windows 10/11；历史 H.264/透明呈现的 WoA 实测不能替代新功能和整合版的实机验收。本版仍是开发原型，HDR 是明确标记的 scRGB 子集。

## Windows GUI 控制中心

**C# / WinUI 3 控制中心**提供原生 ARM64 和 x64 便携包。支持 TLS 直连配对、客户端撤销、可选 SSH、连接配置、自动启动/接入会话、应用目录、实时画面设置、输入共享、文件传输活动、托盘与诊断；视频继续由独立 C++ worker 处理。

解压 `build/Waybridge-Control-win-{arm64,x64}.zip` 的整个目录后运行 `Waybridge.Control.exe`。Linux 先构建并启动用户级入口，在另一个本机终端生成一次性配对码：

```bash
cargo build --release --workspace --locked
./target/release/waybridge-service serve --compositor "$PWD/target/release/waybridge"
# 另一个 Linux 终端：
./target/release/waybridge-service pair --expires 120
```

GUI 选择「认证加密直连」，填写 Linux LAN/VPN 地址及端口 `49092`，从可信 Linux 终端核对并填写 `certificate_sha256` 指纹和一次性配对码，再配对、连接。客户端令牌保存到 Windows 凭据管理器，指纹变化会拒绝连接。默认服务监听 `0.0.0.0:49092`，可用 `--listen` 绑定具体 LAN/VPN 地址。安装可选用户单元、信任撤销及协议细节见 [service README](service/README.md)，GUI 与 SSH 兼容模式见 [GUI README](gui/README.md)。

## 直接运行

构建产物：

- Linux：`target/release/waybridge`、`target/release/waybridge-service`
- Windows x64：`build/windows/waybridge-client.exe`。
- Windows on ARM 原生 ARM64：`build/windows-arm64/waybridge-client.exe`。

两个 Windows 版本均只依赖系统 DLL，启动参数相同。WoA 设备可选用 ARM64 版，避免 x64 仿真开销。

在 Linux 启动测试应用：

```bash
cd /path/to/waybridge
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

以上是低层客户端的 SSH 兼容用法。原始 v3 TCP 协议自身不加密，需通过 TLS 入口或 SSH 等受保护传输；可选 UDP 媒体另有 AES-GCM 认证加密。认证握手和版本检查在任何窗口数据发送前完成；每个会话一次只允许一个查看端连接。

升级后先结束旧版 Waybridge 会话，再创建新版会话。创建前请注销同一用户的本地桌面，锁屏不够。可先运行 `./target/release/waybridge --check-session` 查看结果；GUI/TLS/SSH 和直接启动均执行相同检查。已有 Waybridge 会话优先接回，第二个会话会被拒绝；启动后其他 DM 的登录交给该 DM。

在 Waybridge 打开的终端内启动应用会继承正确的环境。从另一个 SSH 终端加入正在运行的会话，请使用：

```bash
./target/release/waybridge --launch -- dolphin --new-window
./target/release/waybridge --launch -- hmcl-bin
./target/release/waybridge --launch -- vlc
./target/release/waybridge --launch -- mpv "/path/to/video.mkv"
# 默认使用原有 Chrome 数据；显式 --user-data-dir 仍可选：
./target/release/waybridge --launch -- google-chrome-stable
```

自定义 socket 的会话同时指定 `--socket <名称>`。`--launch` 读取 0600 权限描述文件，带上 Waybridge 的显示、用户 D-Bus、IME 和音频环境。过期描述会被拒绝，正常退出时移除。已有本地窗口不会迁入 Waybridge。

会话使用标准用户 D-Bus、原有钱包及应用配置和数据。Waybridge 设置当前显示与服务激活环境，退出时恢复；钱包解锁和 Portal 文件选择使用现成原生界面。服务状态见 `WAYBRIDGE_DESKTOP_SERVICES`。不要求运行 KWin 或其他 DE；Flatpak 文档授权待逐项验证，窗口共享尚未实现。

控制中心、管理 API 和 `--launch` 均不自动添加 Chrome 的 `--user-data-dir` 或密码后端参数，默认使用原有数据，显式参数原样保留。旧 `~/.local/share/waybridge/sessions/` 隔离数据保留，不自动迁移或覆盖。数据目录占用沿用应用原有机制；现有 Steam 跨会话启动检查保留，不终止冲突进程。

Chrome 会按桌面身份选择密码后端。原来使用 KDE/KWallet 6 的用户可运行 `waybridge --launch -- google-chrome-stable --password-store=kwallet6`，或将该选项保存在浏览器自身的用户 flags 中；这仍使用原有 profile。Waybridge 不自动转换密码存储后端，见 [Chromium 说明](https://chromium.googlesource.com/chromium/src/+/HEAD/docs/linux/password_storage.md)。

正常启动 `./target/release/waybridge -- ghostty` 会自动启用已安装的 Xwayland，以及可用的私有音频 sink。`--no-xwayland` / `--no-audio` 可分别关闭；`--xwayland` / `--audio` 则要求该功能初始化成功，失败时直接报错。可选功能不可用时默认保留原生 Wayland 窗口。

可用 `--render-node /dev/dri/renderD129` 指定 GPU，`--fps 60` 指定帧回调上限（现在默认 60）；`--codec auto|png|h264-nvenc|hevc-nvenc|av1-nvenc` 选择编码，`--bitrate 30000` 指定每个视频流的 kbps；旧 `--quality` 参数保留兼容，但无损 PNG 不使用它；`--help` 列出完整选项。正常操作不需要 root、DRM master 或显示器。

## 光标、图标与窗口语义

光标、XDG 应用图标和窗口语义按 v3 能力自动协商。支持系统形状、隐藏、自定义/动画光标及 DPI 热点；自定义动画独立于窗口画面，最多 30 FPS。应用 PNG/SVG 图标在后台解析为 Windows 大小图标，找不到时使用默认图标；不执行 desktop 文件中的 Exec。

已实现真正的 xdg-dialog 模态提示、动态 owner、最小/最大尺寸、全屏恢复、unmap/remap 与隐藏/最小化后的 suspended/低频回调。仅有 parent 关系不代表模态。输入区域与视觉透明度分别映射；排除的客户区像素使用 `HTTRANSPARENT`，只保证同一 UI 线程中的 Waybridge 窗口间穿透，不能点穿到其他进程或桌面。未实现 KDE 毛玻璃。

尊重 `xdg-decoration` 的客户端/服务端请求，Chrome 自绘标签栏时不再叠加 Windows 标题栏。原生 Wayland Chrome 支持 `xdg-toplevel-drag-v1`：标签可拖出成独立 Windows 窗口、拖回另一个窗口合并，Esc 取消。拖拽期间保持 Wayland data-device grab，按本地窗口遮挡和实际画面坐标选择目标；拖动中的窗口不参与目标命中。需要更新并重启两端，应用启动时发现新的 Wayland 接口；显式 X11 Chrome 的 XDND 路径尚未实现。

## 相对鼠标与指针锁定

实现 `zwp_relative_pointer_manager_v1` 与 locked-pointer 子集，Windows 使用 Raw Input、隐藏光标和裁剪。只有目标已映射、具有焦点、指针位于有效区域且无 popup grab 时才申请锁定，原生建立成功确认后才向应用报告 locked；消息使用 target/generation 拒绝过时输入。

**Ctrl+Alt+Shift+F12** 紧急解锁。焦点丢失、弹出菜单、移动/缩放、约束移除、断线和退出均释放原生锁定并恢复当前光标。confined-pointer、未锁定的独立相对运动、popup 锁定及光标位置提示未启用。

## Windows 本地输入法

本版提供 Windows 本地 IME → Wayland `text-input-v3` 通道。需要更新并重新启动服务端及客户端，让应用发现新的 Wayland 全局接口；连接命令和协议版本仍为 v3。

在远端应用输入框获得焦点后，可用 Windows 的 `Win+Space` 切换到微软拼音。候选窗口显示在 Windows；预编辑文字由远端应用绘制，确认后才提交文本。候选位置跟随应用报告的光标矩形，并考虑 Windows DPI、画面缩放及裁剪原点。切换窗口会取消旧预编辑，迟到的提交由焦点 generation 拒绝。英文文本走同一文本通道，Ctrl/Alt/Win 快捷键与导航按键继续转发。

快捷键不会再额外提交其翻译字符，例如 Ctrl+Space 不会额外插入空格。组合输入结束或取消后，仍按住的 Ctrl/Shift 等修饰键会恢复同步，已经松开的键保持松开；AltGr 继续用于本地键盘布局的文本输入。

已验证真实 Qt 和 Ghostty/GTK 在 175% 下的中文、emoji、预编辑/取消及焦点保护。2026-11-10 的 WoA 实机测试已覆盖微软拼音原生候选框、`ni` 选词提交“你”、不切焦点立即退格和回车；Ghostty 的 Linux 真终端回归也检查提交后立即收到退格/回车。其他输入法、GTK 实机候选行为及更多切换场景仍需扩展验收；Wine Unicode 注入与实机测试分别记录。

仅支持实现 `text-input-v3` 的 Wayland 应用；X11/XIM、文档重转换和周边文本查询暂未支持。应用周边文本不传到 Windows。启动命令和 `--launch` 自动清除宿主 `GTK_IM_MODULE` / `QT_IM_MODULE` 等覆盖，Qt/GTK 优先选择原生 Wayland，避免将远端输入交给宿主的 fcitx。

## 会话音频（可选）

Linux 安装 `pactl` 和 `parec` 并有可用 PulseAudio/PipeWire-Pulse 时，音频默认自动启用。服务端创建私有 sink，通过 `PULSE_SINK` 路由 PulseAudio 应用，并通过 `PIPEWIRE_NODE`/流属性路由 mpv 等原生 PipeWire 应用；不修改系统默认输出。后续应用使用 `--launch` 自动加入同一 sink，无需手工抄 PID。原生 PipeWire 流禁止在目标消失时回退到宿主输出；直接使用硬件 ALSA 的应用仍需选择兼容后端。

新版 Windows 客户端自动协商独立音频连接，经 TLS 入口或原来的 SSH 端口，使用 WASAPI 播放到默认输出设备。出现 `WASAPI 48 kHz stereo audio playback active` 表示音频设备已启动；设置 `WAYBRIDGE_NO_AUDIO=1` 可关闭客户端音频，`WAYBRIDGE_AUDIO_DEVICE` 可指定 WASAPI endpoint ID。无声卡或音频连接失败不影响画面。

首版为 48 kHz、16-bit 双声道 PCM，每秒约 1.536 Mbit，不含协议开销；每包 10 ms，服务端待发队列最多 40 ms、在途最多 40 ms，客户端待播放队列最多 100 ms，另有系统缓冲。拥堵时丢弃旧 PCM，优先恢复实时播放。尚未接入 Opus、跨设备时钟同步、严格音画同步、麦克风或输出设备热切换。正常退出/SIGINT/SIGTERM 清理自身 sink；强制 SIGKILL 无法清理，遗留模块可按日志确认后手动卸载。音频应用应随会话退出。

## 可选 X11 应用

安装 `Xwayland` 后默认自动启用。Java/JavaFX、选择 X11 后端的 Chrome 等应用可直接从会话终端或 `--launch` 启动。例如在独立端口和 socket 强制要求 X11：

```bash
./target/release/waybridge --xwayland \
  --listen 127.0.0.1:49101 --socket waybridge-x11 \
  --token-file "$XDG_RUNTIME_DIR/waybridge-x11.token" -- glxgears
```

Windows 通过对应的 SSH 转发连接 49101，使用该 token 文件。Windows 客户端无新增运行时依赖，已有 v3 客户端可显示 X11 窗口。

日志 `private rootless Xwayland ready display=":1000"` 给出实际 DISPLAY，后续 X11 应用使用此值。显示号从 1000–4095 的空闲范围选取，同时避开现有文件和 abstract socket；只监听权限 0700 的文件系统 Unix socket，限同一 Linux 用户连接。启动命令自动获得 DISPLAY，原生 Wayland 应用仍可加入相同会话。例如（按日志替换 `:1000`）：

```bash
DISPLAY=:1000 QT_QPA_PLATFORM=xcb ./build/tests/qt-app
WAYLAND_DISPLAY=waybridge-x11 QT_QPA_PLATFORM=wayland ./build/tests/qt-app
```

已接入独立顶层窗口、transient owner、无边框 override-redirect 菜单/嵌套菜单、原生移动后的点击路由、resize/关闭、键盘焦点、文本/PNG/URI 剪贴板及大文本 INCR。X11 菜单在 Windows 捕获鼠标时仍能越出父窗口，Tooltip 不建立输入捕获。剪贴板按代际丢弃过时读取，缓存写出最多 16 worker、每次最多 10 秒。

Xft/DPI 和 X11 客户端密度为这个 X 服务器共享，初始跟随 Windows 连接 DPI；各 Windows 窗口的呈现仍独立缩放。实测 Qt/XCB 在 175% 创建 1680×1120 画面，改成 125% 呈现为 1000×625，但应用共享 Xft 密度仍为 175%。忽略 Xft/DPI 的旧软件可能需要应用自己的字体设置。X11 resize 仍由应用自行布局，不能等同原生 Wayland 的逐窗口密度协议。

所有 X11 窗口共用一个稳定的 RandR 输出，原生 Wayland 的逐窗口输出对 Xwayland 隐藏。调整 MC 等单个窗口的尺寸/呈现 DPI 或关闭游戏，不再创建、移除或缩放整个 X11 会话的显示器；真实 GLFW 回归覆盖缩放后菜单坐标，以及禁用光标的游戏窗口关闭后再次点击启动器。

当前 X11 子集不含 PRIMARY、XDND 文件拖放桥、XIM/Windows IME、完整 EWMH fullscreen/尺寸提示及自动 Xwayland 崩溃重启。崩溃/关闭 X11 应用不会关闭其余 Wayland 窗口；正常退出/SIGINT/SIGTERM 回收私有 X socket 和锁。可以与本版的原生 Wayland、音频和媒体功能共用会话。

## 使用数据交换

- 文本和 PNG 图片在两端自动同步剪贴板，单种格式上限 12 MiB。图片剪贴板与图片文件分别处理。
- Windows 资源管理器中复制文件或目录，传输完成后可在支持文件粘贴的 Linux 应用里粘贴；Linux 应用提供 `text/uri-list` 时，目录树下载后可粘贴到 Windows 资源管理器。两端会协商 `file_tree` 能力，保留层级、空目录、零字节文件和名称；旧端仍只支持普通文件。
- 从 Windows 资源管理器拖文件或目录到远端窗口，先上传完整数据，再由合成器向目标应用协商 Copy 拖放。当前不会在上传期间显示远端拖动预览。
- Windows 应用仅提供 `FILEDESCRIPTORW/A` + `FILECONTENTS`、没有 `CF_HDROP` 路径时，也可复制或拖入远端窗口。独立 COM worker 按文件获取并以 64 KiB 分块读取流；支持 Clone/Seek、顺序流、未知大小流的 Stat，以及最多 12 MiB 的已知大小 HGLOBAL。异步源在 Linux 提交后收到完成通知，同步拖入等完整接收后返回，等待期间 Esc 可取消。Linux 应用读取的是完成后保留的本地文件，尚未提供 FUSE 按需文件系统；IStorage 和未知大小 HGLOBAL 暂不支持。
- Linux 应用提供标准文件/目录拖放时，可按住左键拖出远端窗口，到支持 Windows 虚拟文件的原生目标释放。文件内容按目标读取请求传输，无需先下载完整文件；支持异步提取、取消以及断线清理，始终使用 Copy，保留 Linux 源文件并保持剪贴板内容。仅接受 CF_HDROP 的目标暂不支持这一拖出路径。
- 目录批次最多 128 个顶层项、512 个条目、16 层路径、总文件内容 8 GiB，单文件最多 1 GiB；相对路径最多 240 个 UTF-16 单元，单个名称最多 240 UTF-8 字节/UTF-16 单元。Windows 临时目录加完整相对路径还必须小于 MAX_PATH，超限会报错。仅 Copy，不保留权限、时间戳和硬链接关系。
- 符号链接、NTFS reparse point（含 junction/云占位文件）、特殊文件、非本地 URI、Windows 保留名/ADS/尾点空格及同一父目录下的大小写冲突会拒绝整批，绝不静默覆盖或改名。Linux 拖出要求应用使用 Wayland data-device v3 并支持 Copy；一次一个出站操作，空闲超过 120 秒取消。
- 未完成的私有目标树会在取消、错误、替换剪贴板或断线时清理。通过新协议完成的文件/目录批次保留在 Linux `$XDG_RUNTIME_DIR/waybridge-inbox/tree-<batch>/` 或 Windows `%TEMP%\Waybridge-tree-*`；旧普通文件路径分别为 `<transfer-id>/` 和 `%TEMP%\Waybridge-*`。应用可能延迟读取，成功后不立即删除；确认无消费者后可手动清理。原生拖出目标自己创建的文件由目标管理，Waybridge 不删除用户目标目录或源文件。

应用对粘贴和拖入的数据类型有自己的限制；私有应用格式不做跨平台转换。

## 可选 UDP/FEC 媒体通道

默认使用 TLS 或 SSH 内的 TCP。显式启用 UDP 时，在 Linux 增加 `--udp-listen <可达监听地址>:49091`；`--udp-mbps 200` 控制含校验片的发送速率上限，范围 1–1000 Mbit/s。它是固定速率节流，尚无自适应拥塞控制；应低于可用带宽。控制中心可直接配置 UDP；低层客户端的 SSH 兼容模式保留隧道和 TCP 启动参数，另设置：

```powershell
$env:WAYBRIDGE_UDP_HOST = "<Windows 能直接访问的 Linux 地址>"
# 可选：NAT/代理暴露的端口与服务端监听端口不同时设置
$env:WAYBRIDGE_UDP_PORT = "49091"
.\waybridge-client.exe 127.0.0.1 49090 "<token>"
```

SSH `-L` 只转发 TCP，不能承载这个 UDP 通道；可使用局域网或已有 VPN 的地址，需允许该 UDP 端口。握手和媒体密钥仍走受保护的 TLS 或 SSH 控制连接。UDP 每会话、每方向使用独立随机 AES-256-GCM 密钥、唯一计数 nonce、认证分片和重放检查；认证成功前不向 UDP 地址发送画面。

画面分为 1024-byte 数据片，每 8 片增加一片 XOR 校验，可恢复该组单片丢失；乱序重组后按原顺序送入解码器。其余丢包通过原 TCP 媒体连接补同一帧，保留 H.264/HEVC/AV1/Main10 引用链。每窗口的两帧呈现 credit 保持有效。UDP 不通时继续 TCP，已建立的 UDP 中断时用 TCP 补帧；当前不支持 UDP endpoint 漫游，需重连。协议仍为 v3，未协商的客户端不受影响。

当前尚未实现动态码率、Reed–Solomon、多丢包低延迟 IDR 恢复或音频 UDP。全局最多 8 条未确认媒体消息、原始帧体合计最多 64 MiB（单条超大 TCP 帧只允许独占），包括 UDP 和 TCP fallback；补帧优先，不受这个新帧门限阻塞。接收端重组最多 16 条/96 MiB，缺失的头部 TCP 补帧可直接交付并排空后续消息。UDP 仅用于不超过 8 MiB 的帧，服务端修复缓存最多 16 条/32 MiB；超限新帧走 TCP。断线取消会逐包中断慢速发送。传输层按全局消息顺序交付，丢失一帧可能暂时阻塞其他窗口，不能据此保证弱网体验或端到端延迟优于 TCP。

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
兼容的 NVIDIA 驱动。常规 CUDA 输入处理 RGBA8 SDR；下面的 HDR 模式另有 FP16→P010 路径。同 GPU 限制由 CUDA GL 查询校验。

`python tests/gpu_smoke.py --benchmark` 检查真实 GL/CUDA/NVENC、每像素/alpha 和
编码错误恢复，并输出 Release CPU/CUDA 对照到 `build/diagnostics/gpu-input-benchmark.json`。
本机 RTX 5070 Ti 上，颜色输入及编码阶段在 1080p 从约 6.65/9.20 ms（不透明/透明）
降到 1.67/2.40 ms；1440p 从约 10.80/15.35 ms 降到 2.40/3.37 ms。这些是本机
合成测试的阶段耗时，不是物理输入到屏幕延迟，也不代表其他设备性能。

## 实验 scRGB HDR 子集（partial）

默认仍为 8-bit BT.709 SDR。服务端显式使用
`--hdr experimental-scrgb --codec hevc-nvenc`（或 `av1-nvenc`）；Windows
PowerShell 设置 `$env:WAYBRIDGE_HDR="1"` 后运行客户端。此模式要求同一 NVIDIA
GPU 执行 GL/CUDA/NVENC，自动为 HDR 开启 CUDA 输入；启用 HDR 时 `--codec auto` 也会选择 HEVC。

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

## 构建

Rust 依赖版本由 `Cargo.lock` 固定。Linux 需要 EGL/GLES、libdrm、Wayland、xkbcommon、FFmpeg（libavcodec/libavutil）开发文件及 pkg-config；使用 NVENC 还需要系统 FFmpeg 的对应 h264_nvenc/hevc_nvenc/av1_nvenc 编码器和 NVIDIA 驱动；构建原生 Windows 客户端需要 MinGW-w64、CMake 和 Ninja。

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

运行需要 systemd-logind 和可用的用户 D-Bus（dbus-daemon 或 dbus-broker），可用 `systemctl --user start dbus.service` 启动用户总线。管理入口需要 Python 3，应用目录需要 GIO。钱包与文件选择需安装所选 Secret Service/KWallet、`xdg-desktop-portal` 与 GTK 后端；缺少可选 provider 时状态文件报告不可用。

按需功能依赖：X11 需要 `Xwayland`，音频需要 `pactl`/`parec` 和可用的 PulseAudio 或 PipeWire-Pulse；SVG 图标需要 `rsvg-convert`。CUDA/HDR 需要兼容 NVIDIA GPU/驱动及构建时 CUDA/GL 开发头；缺少 CUDA 头时仍可构建 CPU/PNG 路径。测试另需 Pillow、wl-clipboard、Wayland scanner、Qt6Widgets、Wine/Xvfb/MinGW；HDR 数值测试需要 NumPy。

## 构建后的测试

以下命令均在 `waybridge` 目录执行。组合脚本自动使用临时用户总线、模拟 logind 和临时用户数据，避免操作当前桌面的钱包或激活环境。单独运行旧图形回归时使用 `python tests/session_fixture.py python tests/<脚本>.py`；新的 admission 和 desktop-services 测试自带测试环境。

```bash
./scripts/test.sh
./scripts/test-m0-m1.sh --with-wine  # M0/M1 完整 Linux/Wine 回归；需 NVIDIA/CUDA/NVENC
python tests/session_integration_smoke.py
```

组合脚本同时启用 X11/音频/UDP/GPU/HDR，验证能力、独立音频认证、PNG-only 回退、Wayland+X11 共存、控制响应及共享退出清理；它不代表所有 14 项功能在一个真实 Windows 会话中完成验收。完整文件整合后的组合脚本、原生文件/IME 和媒体回归已通过，分层结果见 [VALIDATION.md](VALIDATION.md)。

按所需功能选择额外测试；每条命令都需其对应依赖。

| 范围 | 测试入口（`python tests/<名称>.py`） |
|---|---|
| 原生 Windows 窗口/剪贴板/菜单 | `windows_smoke`，可加 `--csd` / `--terminal` / `--qt` / `--udp` |
| 光标与图标 | `cursor_smoke`、`windows_cursor_smoke`、`app_icon_smoke`、`windows_app_icon_smoke` |
| 模态/全屏/输入区域 | `window_semantics_smoke`、`windows_semantics_smoke` |
| IME 与快捷键 | `ime_smoke`、`windows_ime_keys_smoke`、`ghostty_smoke --ime` |
| 相对鼠标 | `relative_smoke`、`windows_relative_smoke` |
| 原生拖出/目录/虚拟源 | `drag_out_smoke`、`tree_smoke`、`windows_drag_out_smoke`（可加 `--tree`）、`windows_virtual_smoke` |
| X11/GLX/剪贴板 | `x11_smoke`、`x11_clipboard_smoke` |
| 游戏式 X11 缩放/点击、稳定 RandR | `x11_pointer_smoke`（需要 GLFW ≥3.4、GL、xrandr） |
| Chrome 标签拆分/合并/取消 | `chrome_drag_smoke`、`windows_chrome_smoke`（独立测试 profile） |
| PCM/WASAPI | `audio_smoke`、`windows_audio_smoke` |
| 真实 NVENC/缓存/回退 | `hardware_smoke`、`codecs_smoke`、`resize_smoke`、`windows_codecs_smoke` |
| CUDA 与故障恢复 | `gpu_window_lifecycle_smoke`、`gpu_smoke --binary target/debug/waybridge`；Release 对照使用 `--benchmark` |
| UDP 故障/所有 codec/全局流控 | `windows_udp_smoke`、`udp_flow_smoke` |
| HDR 数值/视频/Windows shader | `hdr_smoke`、`hdr_video_smoke`、`hdr_windows_smoke`、`hdr_preflight` |

Wine GUI 测试使用 GDI 备用呈现，不能据此验收真实 Windows GPU/透明显示。HDR 的离屏软件 D3D11 数值测试只验证 shader/WIC，不证明物理 HDR。测试输出位于各工作目录的 `build/tests/` 和 `build/diagnostics/`，不是随仓库提交的固定产物。

## Windows 实机检查与延迟日志

先运行 `waybridge-client.exe --check-codecs`。正常 GPU 呈现日志包含 `D3D11/DirectComposition presentation active`，视频解码日志标明具体 codec；`GDI fallback` 不支持逐像素透明。HDR 另用 `--check-hdr` 查询当前显示/能力，并在实际 HDR 显示器上检查 Main10、透明度及切回 SDR。程序不会替你启用系统 HDR。

请重点验收 175% DPI、Ghostty 越界菜单、真实微软拼音候选/选词/焦点切换、Explorer 双向目录和拖出、Outlook/压缩软件虚拟源、锁定后紧急解锁、Windows 新 codec 及多显示器切换。Linux→Windows 拖出始终 Copy，目标必须接受虚拟文件；先用小文件、空目录和 Unicode 名称核对哈希与保留源文件。

Linux：

```bash
./target/release/waybridge --stats --codec auto -- ghostty
```

Windows PowerShell：

```powershell
$env:WAYBRIDGE_STATS = "1"
.\waybridge-client.exe 127.0.0.1 49090 "<token>"
```

Linux 记录合成、回读、编码排队、转换、编码、发送及呈现确认往返；Windows 记录解码排队/处理、UI 排队、呈现提交及控制 RTT。各端使用自身单调时钟；`present` 不含显示器扫描输出，`send` 只是 socket 写入，`display_roundtrip` 包含往返，均不等于物理输入到显示延迟。尺寸切换另有 `encoder_init`、`encoder_reuse`、`resize_first_encoded`、`resize_first_sent`、`decoder_create`、`decoder_reuse` 和 `resize_to_present`。

开发诊断变量：`WAYBRIDGE_DPI=168` 强制 175%，`WAYBRIDGE_GDI=1` 强制备用呈现，`WAYBRIDGE_PNG=1` 禁用视频能力协商；清除变量恢复自动选择。新旧客户端、编译配置、窗口尺寸及网络路径应在性能对照时保持一致；当前没有输入到光子的测量或相对 Moonlight 的性能结论。

## 当前限制

- 每个视频顶层窗口使用 NVENC 会话，尺寸/并发受硬件与驱动限制；申请失败回退 PNG。视频一般限制在 48×48–4096×2304，驱动可能拒绝更小尺寸；更大窗口走 PNG。SDR 视频是 8-bit 4:2:0，彩色文字可能不如 PNG 清晰；无 4:4:4。
- CUDA/HDR 需要兼容的同一 NVIDIA GPU；透明 alpha、同步和 GPU 内复制仍存在。HDR 仅显式 Wayland scRGB，ICC、通用 parametric 色彩管理、HDR X11 和物理 Main10/HDR 验收未完成。
- IME 仅 text-input-v3，不含 XIM、周边文本查询/重转换。X11 还缺 PRIMARY、XDND、完整 EWMH 及自动崩溃重启；忽略 Xft 的程序需自己的密度设置。
- 指针锁定和输入区域为有限子集；未支持通用跨进程透明点穿、KDE 模糊或 confined-pointer。
- 文件只 Copy，不保留权限/时间戳/硬链接，无断点续传、FUSE 或 IStorage。Explorer/Outlook/压缩软件与不响应 COM 取消的源仍需实机兼容性验证。
- 音频无 Opus、麦克风、严格音画同步和设备热切换；UDP 为固定节流与全局有序流，无自适应拥塞控制或 endpoint 漫游，弱网可能跨窗口阻塞。
- 原始 v3 TCP 需经内建 TLS 入口或外部 SSH 等受保护传输；与 Sunshine/Moonlight 线协议不兼容。TLS 入口重启保留会话，合成器崩溃后的应用状态恢复未实现；SIGKILL 无法执行资源清理。

普通 SHM、DMA-BUF 和可选 Xwayland 已接通；大型应用、驱动、多 GPU 与物理 Windows 组合仍需逐项测试，不能保证“All 应用”。
