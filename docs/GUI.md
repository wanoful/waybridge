# Waybridge Control Center

Linux 会话默认共享原有应用与钱包数据。创建前须注销同一用户的本地桌面，锁屏不够；自动连接优先接回已有 Waybridge，会拒绝第二个新会话。启动后再从其他 DM 登录交给该 DM，见 [会话策略](SESSION_POLICY.md)。TLS 使用服务端的新版启动助手；使用 SSH 模式时需重新构建控制中心，以更新内嵌的 `session-control.py`。

C# / .NET 10 + WinUI 3 控制中心，配套独立的 C++ 串流内核。提供 ARM64 与 x64 便携包。视频解码、DirectComposition、远端窗口、IME、音频和 OLE 仍由原生内核处理，GUI 只通过当前用户专用 named pipe 管理连接和设置。

## 开始使用

1. 解压 **整个 ZIP**，运行 `Waybridge.Control.exe`。WoA 使用 `win-arm64` 包。
2. .NET 与 Windows App SDK 已随包提供，无需另装。如果机器缺少 Visual C++ v14 运行库，先运行包内对应架构的 `vc_redist.arm64.exe` / `vc_redist.x64.exe`。来源为 [Microsoft 官方下载](https://learn.microsoft.com/cpp/windows/latest-supported-vc-redist)。
3. Linux 构建 `cargo build --release --workspace --locked`，启动 `./target/release/waybridge-service serve --compositor "$PWD/target/release/waybridge"`。需要 Python 3.11+、GIO 及合成器运行依赖；直连模式无需 Windows OpenSSH。标准组件生命周期的安装见 [桌面集成](INTEGRATION.md)。
4. 在 Linux 另一个可信终端运行 `./target/release/waybridge-service pair --expires 120`，取得一次性配对码与 `certificate_sha256` 指纹。不要把网络端自行报告的指纹当作信任依据。
5. GUI 选择「认证加密直连」，填写 LAN/VPN 地址、服务端口 `49092`、核对过的证书指纹和一次性配对码，点击「配对此设备」。客户端令牌保存在 Windows 凭据管理器；证书变化会拒绝连接。Linux 可用 `waybridge-service clients` / `revoke CLIENT_ID` 撤销信任，GUI 也可在断开后撤销当前设备。
6. 选择「已有会话优先」并连接。在「远端应用」刷新并启动应用；「启动终端」默认运行 `ghostty`。关闭查看端保留应用，重新连接可接回仍存活的会话。服务部署与授权范围见 [服务说明](SERVICE.md)。

兼容 SSH 模式：Windows 安装系统可选功能 OpenSSH 客户端，Linux 提供 SSH 服务。选择「内置 SSH」，填写 SSH 用户、私钥（或使用 ssh-agent）和 Linux 服务端绝对路径。使用 OpenSSH 密钥认证，首次核对主机 SHA256 指纹；没有交互式密码和键盘认证。加密私钥先执行 `ssh-add <私钥路径>`。
已有 SSH 隧道也可在 GUI 使用：选择「已有 SSH 隧道」、填写 `127.0.0.1`、本地转发端口和令牌。这一模式没有远端应用目录、启动或结束会话功能。可选保存令牌到 Windows 凭据管理器；JSON 配置不保存令牌。

内置 SSH 模式的会话端口可以填 `0` 自动分配；直连端口是 TLS 用户入口端口，通常为 `49092`。接入旧会话需要其服务端已更新到带连接描述信息的版本。一次只允许一个查看端。选择「创建独立会话」时自动使用唯一名称，成功后保存实际名称，供后续接入。

## 页面与行为

- **设备与连接**：保存多个连接，TLS 指纹与配对/撤销、可选 SSH 密钥认证、自动启动或接入会话、断线重连。
- **远端应用**：搜索 Linux 已安装的 desktop 应用并启动；自动应用用户明确配置的 [Waybridge desktop 覆写](CONFIGURATION.md)。查看当前远端窗口并切换焦点；启动请求已提交与窗口实际出现分别处理。
- **画面与声音**：编码、FPS、每窗口码率、跟随 Windows 或手动 DPI、音量、输出设备、UDP、实验性 HDR、CPU/CUDA 输入。编码/FPS/码率/缩放/音量实时应用；输出设备和 UDP 需要重连；HDR 与 GPU 输入还取决于新建服务端时的设置。
- **输入与共享**：远端图标角标、只读、IME、原生多点触控、剪贴板方向、剪贴板图片、文字拖放、文件目录共享。文字拖放默认开启，独立于剪贴板与文件共享；关闭后禁止跨端文字拖入/拖出。只读同时在服务端拒绝输入并释放按键、鼠标锁定和触点。
- **文件传输**：发送文件/目录到远端剪贴板，显示传输活动，打开最近完成的本地接收目录。可继续直接拖放。首版为活动记录，尚无逐文件进度、暂停/续传和统一取消按钮。
- **诊断**：实际编码、GPU 解码/呈现路径、网络 RTT、解码排队/解码/UI 排队/呈现时长、TCP 接收速率与窗口更新次数，支持导出。数据来自原生内核；不把这些分项之和称为端到端延迟。UDP 流量不计入 TCP 接收速率。

点击关闭按钮会收起到系统托盘；再次启动会唤回已有控制中心。托盘可连接/断开、切换静音、打开诊断和退出。**断开或退出仅停止本地 worker 与网络连接，远端应用保留。** 「结束远端会话」另外确认后结束同用户拥有且进程身份仍匹配的会话。

远端应用托盘使用 Windows 通知区域的独立图标；「远端应用」内的「后台应用」可打开／恢复应用或调用其菜单，菜单中的退出项由应用自己提供。没有 SNI 接口的后台进程不出现在这个列表，旧式 XEmbed 未接入。原生通知由 Windows 处理免打扰和通知中心；Linux 需显式选择 Waybridge 通知 provider，不能与另一个通知服务同时持有同名接口，详见 [桌面集成](INTEGRATION.md) 和 [托盘说明](DESKTOP-TRAY.md)。「输入与共享」可分别开关应用托盘和通知；只读模式禁止托盘/通知触发应用动作，仍可在本地关闭通知。

远端窗口保留 Linux 应用原图标，系统标题栏、任务栏和 Alt+Tab 的图标在右上角叠加 Waybridge 标记。没有原图时使用灰色应用占位图。应用自己绘制在内容区的图标不改动。任务栏按连接配置和应用设置独立身份；同应用窗口可归组，固定与重启动作暂未提供。控制中心及其托盘使用蓝底拱桥主图标。

在「输入与共享 → 远端图标角标」选择 PNG、JPEG、BMP 或 ICO 图片，可为当前连接替换默认角标；点击「应用到当前连接」立即生效。图片限 8 MiB、8192×8192 且不超过 1600 万像素，导入时保持比例并规范为 64×64 透明 PNG，存入连接配置，因此删除原图片不会影响角标。「使用 Waybridge 图标」恢复默认。这个外观设置只保留在 Windows，不改 Linux 主题或应用文件。品牌矢量源位于 `assets/branding/`；`python3 scripts/build-branding.py` 使用 rsvg-convert 与 Pillow 重建已提交的 PNG、ICO 与原生嵌入资源，常规构建无需这两项生成工具。

在「输入与共享 → 原生多点触控（实验）」启用 Windows 原生触摸。每个连接单独保存；「保存设置」可在断开时使用，「应用到当前连接」立即切换并保存。关闭会取消当前触点并恢复鼠标兼容，重新开启使用新的输入代次。旧配置缺少该字段时默认关闭，不再读取环境变量。旧服务端不支持实时切换时界面会提示重连。设备与应用范围见 [专业输入](INPUT_DEVICES.md)。

配置位于 `%LOCALAPPDATA%\Waybridge\profiles.json`，保存直连证书指纹与客户端标识，不保存令牌。客户端令牌和可选手动隧道令牌位于 Windows 凭据管理器；SSH 主机信任记录为同目录 `known_hosts`。Linux 会话日志在 `$XDG_RUNTIME_DIR/waybridge-control-center/<会话>.log`。诊断不导出会话令牌或剪贴板正文。

SSH config 的 User/IdentityFile/HostName 等设置由系统 OpenSSH 解析，界面端口会覆盖 SSH config 的 Port。首次指纹获取目前使用直接 `ssh-keyscan`，仅能经 ProxyJump/ProxyCommand 到达的主机需要先把核对过的主机密钥加入上述 `known_hosts`。UDP 媒体需要额外直达 Linux UDP 端口，并填写可直达地址；SSH 不转发 UDP。首次启用 UDP 或 HDR 请选择新建服务端会话。

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
python tests/gui_ssh_smoke.py  # 需本机 OpenSSH server，可用普通用户运行独立测试实例
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
