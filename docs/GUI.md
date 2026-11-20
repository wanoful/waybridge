# Waybridge Control Center

Linux 会话默认共享原有应用与钱包数据。创建前须注销同一用户的本地桌面，锁屏不够。自动连接优先接回已有 Waybridge，会拒绝第二个新会话。启动后再从其他 DM 登录交给该 DM，见 [会话策略](SESSION_POLICY.md)。

C# / .NET 10 + WinUI 3 控制中心，配套独立的 C++ 串流内核。提供 ARM64 与 x64 便携包。视频解码、DirectComposition、远端窗口、IME、音频和 OLE 仍由原生内核处理，GUI 只通过当前用户专用 named pipe 管理连接和设置。

## 开始使用

1. 解压 **整个 ZIP**，运行 `Waybridge.Control.exe`。WoA 使用 `win-arm64` 包。
2. .NET 与 Windows App SDK 已随包提供，无需另装。如果机器缺少 Visual C++ v14 运行库，先运行包内对应架构的 `vc_redist.arm64.exe` / `vc_redist.x64.exe`。来源为 [Microsoft 官方下载](https://learn.microsoft.com/cpp/windows/latest-supported-vc-redist)。
3. Linux 构建 `cargo build --release --workspace --locked`，启动 `./target/release/waybridge-service serve --compositor "$PWD/target/release/waybridge"`。需要 Python 3.11+、GIO 及合成器运行依赖。标准组件生命周期的安装见 [桌面集成](INTEGRATION.md)。
4. 在 Linux 另一个可信终端运行 `./target/release/waybridge-service pair --expires 120`，取得一次性配对码与 `certificate_sha256` 指纹。不要把网络端自行报告的指纹当作信任依据。
5. 在 Windows 添加设备，填写 LAN/VPN 地址、服务端口 `49092`、核对过的完整证书指纹和一次性配对码。客户端令牌保存在 Windows 凭据管理器。换证书后需用可信来源重新核对和配对。Linux 可用 `waybridge-service clients` / `revoke CLIENT_ID` 撤销信任，GUI 也可在断开后撤销当前设备。
6. 使用默认会话策略连接。搜索并启动一个应用，或从正在运行的窗口列表切回。关闭查看端会保留应用，重新连接可接回仍存活的会话。服务部署与授权范围见 [服务说明](SERVICE.md)。

Windows 控制中心通过 TLS 接入用户服务。仅能通过 SSH 到达 Linux 时，可用系统 SSH 转发 TLS 入口，再在控制中心填写本地转发地址和端口，核对 Linux 服务的完整指纹。例如在服务也监听 Linux 本地 `49092` 时，使用 `ssh -N -L 127.0.0.1:49092:127.0.0.1:49092 user@linux-host`。UDP 媒体需要独立可达。

## 页面与行为

- **顶部工作站栏**显示工作站及连接状态，底部显示当前会话名。顶部可切换只读、调整远端音量、查看 RTT 和实际画面路径，也可打开传输面板或设置。切换工作站会断开当前连接，远端应用继续运行。
- **全局搜索**用 `Ctrl+K` 或 `/` 聚焦，输入应用名、关键字或窗口标题后按 Enter 启动或切回。以 `>` 开头在当前工作站运行命令，可指定 Linux 工作目录，并以 Tab 补全最近命令。参数作为字面值传给 Linux 进程。
- **应用链接与快捷方式**在应用卡片的 `…` 菜单中。可以复制 `waybridge://launch/…` 链接、添加到「开始 → 所有应用 → Waybridge」，或创建桌面快捷方式。快捷方式带应用图标和工作站名称。可在 Windows「运行」里打开应用链接。
- **画面预设**在应用卡片的 `…` 菜单中，按工作站和应用保存。「文字清晰」优先 PNG 无损并把该窗口限制到最高 30 FPS。「交互低延迟」优先 H.264，并让该窗口在后台窗口中优先获取捕获资源。「跟随连接」恢复连接的统一设置。编解码能力和服务端限制仍决定实际路径。切换只需要刷新受影响的窗口。
- **文件传输**从文件抽屉看进度、取消或打开接收目录。「共享与权限」可设置传输限速，0 表示不限速。[文件传输说明](FILE_TRANSFERS.md)
- **麦克风回传**在「声音」页开启，默认关闭。Windows 默认麦克风送到远端的 **Waybridge Microphone**。[麦克风说明](MICROPHONE.md)
- 应用链接绑定本机的配对配置和远端应用 ID。打开时复用当前工作站连接，或自动连接所选工作站。切换正在查看的工作站时会先确认，并保留原工作站上的远端应用。应用成功启动时控制中心可以留在托盘。重新配对或移除工作站后，可从新配置重新创建链接。
- **远端应用**首页从固定与最近一直滚动到全部应用。滚过目录标题后，刷新、分类和布局切换留在视口顶部。网格和紧凑列表都展示原始应用图标，应用名称最多两行，超长文本在第二行末尾显示省略号。点击已配对工作站的应用会在需要时先接入会话。应用卡片的 `…` 和右键菜单提供固定、Desktop Actions、触控策略和自定义启动预览。应用启动沿用 Linux 上的 [Waybridge desktop 覆写](CONFIGURATION.md)。
- **运行中**在固定左侧导航栏单独显示远端窗口和后台应用。可以切回窗口、恢复后台应用或打开其菜单。
- **文件传输**从顶部打开抽屉，可发送文件和目录、查看活动及打开接收目录。关闭抽屉时，顶部徽标显示未查看的传输活动。
- **诊断**显示正在使用的编码、解码和呈现路径，以及 RTT、TCP 接收速率、窗口更新次数和各阶段耗时。可导出诊断。TCP 接收速率仅统计 TCP，耗时指标分别反映对应阶段。

左侧导航的 **设置** 分为「画面」「声音」「输入设备」「共享与权限」「网络」「连接」「桌面集成」「常规」。在「常规」切换浅色、深色或跟随 Windows 后，控制中心与标题栏立即更新。常规设置作用于整个控制中心，其他设置按工作站分别保存。支持实时变更的页面提供「应用到当前连接」。实际帧率、码率和编码参数在「画面」页更新。输出设备等依赖重连的设置提供「保存并重新连接」。首次启用 UDP、HDR 或更换服务端颜色输入时，可能需要建立适用的新会话。

点击关闭按钮会收起到系统托盘；再次启动会唤回已有控制中心。托盘可连接/断开、切换静音、打开诊断和退出。**断开或退出仅停止本地 worker 与网络连接，远端应用保留。** 「结束远端会话」另外确认后结束同用户拥有且进程身份仍匹配的会话。

远端应用托盘使用 Windows 通知区域的独立图标；「远端应用」内的「后台应用」可打开／恢复应用或调用其菜单，菜单中的退出项由应用自己提供。没有 SNI 接口的后台进程不出现在这个列表，旧式 XEmbed 未接入。原生通知由 Windows 处理免打扰和通知中心；Linux 需显式选择 Waybridge 通知 provider，不能与另一个通知服务同时持有同名接口，详见 [桌面集成](INTEGRATION.md) 和 [托盘说明](DESKTOP-TRAY.md)。「输入与共享」可分别开关应用托盘和通知；只读模式禁止托盘/通知触发应用动作，仍可在本地关闭通知。

远端窗口保留 Linux 应用原图标，系统标题栏、任务栏和 Alt+Tab 的图标在右上角叠加 Waybridge 标记。没有原图时使用灰色应用占位图。应用自己绘制在内容区的图标不改动。任务栏按连接配置和应用设置独立身份；同应用窗口可归组，固定与重启动作暂未提供。控制中心及其托盘使用蓝底拱桥主图标。

在「桌面集成 → 远端图标角标」选择 PNG、JPEG、BMP 或 ICO 图片，可为当前连接的 Windows 窗口和通知区托盘图标更换角标。点击「应用到当前连接」立即生效。「使用 Waybridge 图标」恢复默认。图片限 8 MiB、8192×8192 且不超过 1600 万像素，导入时保持比例并规范为 64×64 透明 PNG，存入连接配置。品牌矢量源位于 `assets/branding/`。需要更新品牌时，`python3 scripts/build-branding.py` 使用 rsvg-convert 与 Pillow 重建 PNG、ICO 与原生嵌入资源。

在「输入设备」启用原生多点触控、压感笔或手柄。「保存设置」可在断开时使用，「应用到当前连接」立即请求切换并保存。触控默认自动检测应用是否接入原生触控，其余使用鼠标兼容。展开「震动、压感曲线和应用兼容」可按应用调整策略，鼠标兼容支持长按右键与拖选。设备与应用范围见 [专业输入](INPUT_DEVICES.md)。

配置位于 `%LOCALAPPDATA%\Waybridge\profiles.json`，保存 TLS 证书指纹与客户端标识。客户端令牌位于 Windows 凭据管理器。Linux 会话日志在 `$XDG_RUNTIME_DIR/waybridge-control-center/<会话>.log`。诊断排除会话令牌和剪贴板正文。首次启用 UDP 或 HDR 时请选择新建服务端会话。

高级输入设置还包括笔压曲线（0.25–4，默认 1）、震动和摇杆死区（0–50%，默认 15%）。设备状态区区分本机发现、远端支持、请求开启、前台输入与实际转发，并显示后端错误。范围见 [笔输入](PEN.md) 和 [手柄](GAMEPADS.md)。

## 从源码构建

Linux 需 .NET 10 SDK、CMake、Ninja、MinGW x64 / LLVM-MinGW ARM64：

```bash
./scripts/build-control-center.sh arm64  # 也可 x64 / all
```

生成 `build/Waybridge-Control-win-arm64.zip` 和 `build/control-center-arm64/`。脚本验证 GUI、原生 worker、WinUI 与 .NET runtime 的 PE 架构，并生成包内 SHA256 清单。Windows 可在装有 VS C++ 工具、Windows SDK 和 .NET 10 SDK 的开发终端执行 `scripts/build-control-center.ps1 -Architecture arm64`。

GUI 使用 C# 创建真实 WinUI 控件，没有自定义 XAML 资源。程序入口手动初始化 `XamlControlsXamlMetaDataProvider`，并通过 Application 的 `IXamlMetadataProvider` 向解析器提供控件类型。WinUI 自身的主题和本地化字符串仍需要默认 `resources.pri`：发布任务从同版本、同架构的微软 framework MSIX 复制完整预编译索引，并核对 WinUI / AppRuntime / MRT DLL 与包内文件逐字节一致。Linux 因而无需执行 XAML/PRI 编译器；后续若增加应用 XAML/RESW，必须改成包含应用资源的标准 PRI 合并流程。跨编译仍使用 Windows App SDK 自动生成完整 WinRT 注册 manifest，再通过仓库脚本合并应用 manifest；Windows 使用 SDK 自带的 mt.exe。

自动测试：

```bash
dotnet run --project gui/Waybridge.Control.Tests -c Release
python tests/gui_session_smoke.py
python tests/gui_direct_smoke.py  # 实际 TLS 服务与 .NET 管理、私有 worker 启动契约
python tests/gui_native_smoke.py  # 需 Wine / Xvfb；测试真实 C++ named-pipe 链路
```

Linux 测试无法替代 WinUI 在真实 Windows/WoA 上的布局、托盘、凭据管理器、音频设备和文件选择器验收。

## 双击后无窗口或立即退出

运行解压目录中的 **`Diagnose-Startup.cmd`**。也可把仓库的 `scripts/Diagnose-Startup.ps1` 单独放在旧版 `Waybridge.Control.exe` 旁，在 PowerShell 执行：

```powershell
powershell.exe -NoProfile -ExecutionPolicy Bypass -File .\Diagnose-Startup.ps1
```

脚本会验证包内 SHA256，记录 Windows 版本、程序与依赖架构、进程退出码，启用 [.NET host tracing](https://github.com/dotnet/runtime/blob/main/docs/design/features/host-tracing.md)，收集本次启动相关的 Application Error / .NET Runtime / SideBySide 事件，并生成 `%LOCALAPPDATA%\Waybridge\startup-reports\<时间戳>.zip`。无需管理员权限；不会修改全局执行策略、注册表或结束已有进程。若进程仍运行，最多等待 15 秒后保留它，便于检查弹窗/托盘。

新版还在 `%LOCALAPPDATA%\Waybridge\startup.log` 记录启动阶段和托管异常，错误时显示普通 Win32 错误框。诊断脚本运行时，该日志写入同一报告目录。若报告没有 `startup.log`，重点检查 `dotnet-host.txt`、`stderr.txt` 和 `events.txt`，可能在进入 C# 入口前失败。报告不收集 profiles.json、SSH 密钥、凭据管理器或剪贴板；分享报告时其中可能含 Windows 用户目录和应用安装路径。

优先提供报告 ZIP，或粘贴 `report.txt` 的退出码、`startup.log` 的最后一个阶段和异常、`events.txt`。例如 `0xE0434352` 本身仅表示托管异常，需要堆栈；不要仅凭“无窗口”反复重装运行库。
