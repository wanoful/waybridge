# Waybridge Client Launcher 与 Server TUI 重构设计规格

当前实现包括 WinUI 主启动器、八类客户端设置、TLS 单连接路径和 Linux 管理 TUI。主启动器具备全局栏、应用墙、应用菜单和 TLS 批量图标。原型阶段可同步更新两端，使用新版时可重新添加设备并配对。Avalonia 迁移、粘贴配对口令和快捷启动窗按后续阶段推进。

## 1. 目标与核心决策

本次重构旨在改善 Waybridge 日常连接、应用启动与服务端管理的操作直觉与视觉体验，同时消除 WinUI 3 无 XAML 交叉编译的复杂构建链路。

| 决策项 | 选型与边界 |
|---|---|
| Client UI 技术栈 | 优先验证 **Avalonia UI（.NET 10 + FluentAvalonia）**，通过便携包与系统集成验收后再替换 WinUI 3 与 Windows App SDK |
| Client 构建方式 | 保持在 Linux 上交叉发布 `win-x64` 与 `win-arm64` 便携包。迁移验收后清理原有 PRI 提取与 manifest 拼接脚本 |
| Client 核心复用 | 复用 `Waybridge.Control.Core`、Windows 凭据管理器和系统托盘，按新版连接流程更新回归测试 |
| Client 连接方式 | 仅通过认证加密的 Waybridge TLS 接入 Linux 用户服务。移除内置 SSH 和原始会话隧道模式 |
| Client 形态 | **双形态 App Launcher**：完整应用墙主窗口（Full Launcher）+ 随呼随用悬浮启动窗（Quick Launcher） |
| 图标与目录 | 扩展 `applications` 元数据，新增 `app_icons` 批量 PNG 接口，控制中心缓存并显示应用原图 |
| 配对体验 | 首版沿用一次性配对码与完整带外指纹核对，后续支持粘贴可信来源的 `wbpair://` 口令 |
| Server 管理界面 | **`waybridge-service tui`**（Rust `ratatui` + `crossterm`），支持在线/离线双态运行 |

---

## 2. 实现边界与验收基线

首批实现验证以下分工和用户流程。

1. **媒体与控制职责边界**（[ARCHITECTURE.md](ARCHITECTURE.md)）：视频解码、DirectComposition、远端窗口、IME、音频和 OLE 继续留在 C++ 原生内核 `waybridge-client.exe`；C# 仅通过当前用户专用 named pipe 收发控制/状态 JSON，不转发媒体字节。
2. **原生引擎 named pipe 协议**（[client/main.cpp](../client/main.cpp)、[EngineChannel.cs](../gui/Waybridge.Control.Core/EngineChannel.cs)）负责连接生命周期、窗口激活、文件传输、托盘、通知、设置与诊断。协议字段可以随 GUI 和 worker 一起更新。
3. **配置与凭据**（[Profiles.cs](../gui/Waybridge.Control.Core/Profiles.cs)）采用仅含 TLS 连接和客户端偏好的配置结构。使用新版时可重新添加设备并配对。持久凭据存入 Windows 凭据管理器，诊断内容与配置 JSON 排除令牌及配对码。
4. **Core 与服务测试**覆盖 `Waybridge.Control.Tests`、`tests/gui_direct_smoke.py` 和 `service/tests/direct_service_smoke.py` 的新版直连流程。随旧连接模式退役专用 SSH Agent 和 `tests/gui_ssh_smoke.py`。
5. **UI 自动化**（[Probe.cs](../gui/Waybridge.GuiFocus.Probe/Probe.cs)）随新版界面更新。覆盖触控、手写笔和手柄，验证离线保存与实际应用状态可区分，以及保存设置不会意外建立连接。

---

## 3. 跨端协议与服务端扩展

### 3.1 扩展 `op: "applications"` 目录元数据

修改 [scripts/session-control.py](../scripts/session-control.py) 的 `app_catalog()`，在保留既有字段 `id`、`name`、`icon`（以及内部使用的 `path`）的基础上，补充解析 `[Desktop Entry]` 中的标准轻量字段：

```json
{
  "id": "google-chrome.desktop",
  "name": "Google Chrome",
  "icon": "google-chrome",
  "generic_name": "Web Browser",
  "comment": "Access the Internet",
  "categories": ["Network", "WebBrowser"],
  "keywords": ["Internet", "Web", "Browser"],
  "actions": [
    { "id": "new-window", "name": "New Window" },
    { "id": "new-private-window", "name": "New Incognito Window" }
  ]
}
```

约束：
* 每个字符串字段需做长度截断（例如 `name`/`generic_name` ≤ 256，`comment` ≤ 512，`categories`/`keywords`/`actions` ≤ 32 项），继续受 `.desktop` 单文件 `65536` 字节与最多 `1024` 个应用的上限保护。
* 目录接口仅返回展示和启动所需的公开元数据，`Exec`、环境变量及文件系统路径留在服务端。
* `actions` 列表直接复用 `session-control.py` 已支持的 `{"op": "launch", "app": "<id>", "action": "<action_id>"}` 契约。

### 3.2 新增 `op: "app_icons"` 批量图标接口

当前窗口图标由 Linux 合成器在运行时经 [src/app_icon.rs](../src/app_icon.rs) 解析，而未启动的应用目录缺少实际位图。Launcher 通过认证后的 TLS 管理接口获取这些图标，独立于串流会话状态。

* **TLS 管理请求**
  ```json
  {
    "id": 10,
    "op": "app_icons",
    "apps": ["google-chrome.desktop", "blender.desktop"]
  }
  ```
* **响应格式**：
  ```json
  {
    "icons": {
      "google-chrome.desktop": {
        "sha256": "<64-char hex>",
        "png": "<base64 of 128x128 RGBA PNG>"
      },
      "blender.desktop": null
    },
    "omitted": []
  }
  ```
* **尺寸与安全预算**：
  * 复用 [src/app_icon.rs](../src/app_icon.rs) 的 `Resolver`、常规文件检查、`rsvg-convert` 子进程 `rlimit` 隔离以及 `128x128`（`EDGE = 128`、`MAX_PNG = 96 * 1024`）缩放规范。建议将纯无头图标解析逻辑抽为共享 workspace crate（或提供 `waybridge --resolve-app-icons` 命令行工具，供 `waybridge-service` 与 `session-control.py` 共用），避免在 Python 中重复实现 XDG 图标主题扫描。
  * 单次请求最多接受 24 个应用。按 Base64 和响应封装在内的最终 JSON 行大小分页，确保满足 TLS 管理接口的 4 MiB 限额。剩余项放入 `omitted`，由客户端分批拉取。
  * 仅解析桌面目录中实际存在的受信任应用图标名/绝对图标路径，禁止接受任意路径穿越。

### 3.3 简化配对体验与安全边界

保持 [SERVICE.md](SERVICE.md) 的核心安全原则：**绝不自动信任来自同一未信网络响应的证书指纹**，必须保留人类或带外通道核对。

1. **首版使用现有配对机制。** `waybridge-service pair` 生成一次性、限时配对码，输出完整证书指纹与监听信息。失败请求没有可验证的配对码归属，失败限流另行设计。
2. **地址、指纹、配对码按步骤录入。** 用户从可信来源复制完整的 64 个十六进制字符的 SHA-256 指纹。若提供 IP 探测，展示 TLS 握手实际看到的叶证书指纹供比对。分组用于可读性，确认时核对完整指纹。客户端持该指纹建立固定证书的 TLS 连接后才发送配对码，并把 `client_token` 存入 Windows 凭据管理器。
3. **后续支持粘贴配对口令。** TUI 可用可达主机地址生成 `wbpair://v1?host=<host>&port=<port>&fp=<certificate_sha256>&code=<code>`。向导把可信渠道传来的口令解析为地址、完整指纹和配对码，先显示目标并由用户确认，再进行同样的固定证书校验。配对口令含一次性秘密，只用于本次配对。

---

## 四、Server 管理界面：`waybridge-service tui`

### 4.1 运行架构与双态边界

在 [service/](../service) 中新增 `waybridge-service tui` 子命令（基于 Rust `ratatui` + `crossterm`），适配本地 Linux TTY 与系统 SSH 终端。只通 SSH 的网络可由系统 SSH 转发 Waybridge 的 TLS 入口，Windows 端按同一套直连方式配对和连接。UDP 媒体仍需要独立可达。

* **在线态（`waybridge-service serve` 正在运行）**：
  * 避免直接读写 `trust.json` 与 `service.lock`（[service/src/main.rs#L151](../service/src/main.rs#L151)）冲突，TUI 通过同 UID `0o600` 的 `$XDG_RUNTIME_DIR/waybridge-service/control.sock` 与守护进程通信。
  * 在 `control.sock` 上扩展**仅限本地管理 socket** 的操作（绝不向公网 TLS `remote()` 暴露）：`sessions`（列举当前活跃会话）、`end_session`（安全终止指定会话）、`preflight`（环境自检）等；也可复用现有 `helper()` 执行 `applications` 与 `launch_plan`。
* **离线态（`waybridge-service serve` 未启动）**：
  * TUI 仍可正常进入，展示“服务未运行”状态与快捷操作（一键尝试 `systemctl --user start waybridge.service` 或显示前台运行命令），同时完整支持环境前置检查、配置编辑与 Desktop Overrides 生成。

### 4.2 界面结构（4 个键盘 Tab）

支持 `1–4`、`Tab`/`Shift+Tab`、方向键、`Enter`、`Esc` 全键盘操作，不依赖鼠标：

1. **`[1] 总览与配对`**：
   * 显示 `waybridge.service` / 进程运行状态、TLS 监听地址、分段证书指纹、已加载配置文件路径。
   * **前置环境自检面板**：
     * 调用等价于 `waybridge --check-session` 的检测，提前告知同一 UID 下是否有本地 X11/Wayland 桌面占用（需先注销才能建会话）；
     * 检查 NVENC/CUDA 可用性、Xwayland、`pactl`/`parec`、`rsvg-convert`、`fusermount3`、Portal 配置与 `waybridge-notifications.service` 冲突状态。
   * **配对操作区**：按快捷键（如 `p`）生成一次性配对码，显示可复制的完整证书指纹和剩余有效时间。粘贴口令随对应向导交付。
2. **`[2] 会话与日志`**：
   * 列表展示当前运行中的 Waybridge 会话（名称、PID、监听端口、运行时长）。
   * 下方/右侧窗格实时 tail `$XDG_RUNTIME_DIR/waybridge-control-center/<session>.log`（支持暂停滚动、关键字过滤、上下翻页）。
   * 选中会话支持安全结束操作（二次确认后走 `pidfd` 校验发 `SIGTERM`）。
3. **`[3] 已信任设备`**：
   * 列表展示 `trust.json` 中已配对设备（`label`、`client_id`、配对时间）。
   * 支持选中设备并二次确认后调用 `revoke` 撤销信任（同时立即关闭该设备的活动 TLS 隧道）。
4. **`[4] 设置`**：
   * **监听与服务、会话默认值、性能与资源**覆盖 `[service]`、`[session]`、`[limits]` 和 `[media_budget]`。以服务实际加载的配置路径为准，支持 XDG 默认路径和显式 `--config`。在线态区分当前运行值、已保存值和草稿，显示内置默认值、文件或启动参数等来源。离线态显示下次启动预计值。沿用 [config/src/lib.rs](../config/src/lib.rs) 的配置校验与生效阶段报告。
   * **检查并保存**先校验整个草稿并展示变更和生效时机，再原子保存。若原文件在编辑期间发生变化，提示重新读取。需要重启的项目提供独立的重启操作，明确连接影响。新会话配置在下次创建时生效。
   * **应用启动**按完整 Desktop ID 编辑 Waybridge 专用覆写的默认项及具体 Desktop Action。保存前复用现有启动解析逻辑，用未保存草稿预览有效命令和 `field_sources`。没有运行会话时可基于已安装条目与选定配置做离线预览。预览标明 `DBusActivatable` 与进程覆写冲突，并显示下一次启动的预期行为。
   * **桌面组件**管理 Waybridge 负责的 Portal 与通知等集成配置，展示实际配置来源、用户单元以及可读取的运行状态。修改写入相应的标准配置和 Waybridge 用户单元 `drop-in`，沿用 [桌面集成](INTEGRATION.md) 的归属规则。

---

## 五、Client 重构：UI 技术栈与双形态 Launcher

### 5.1 工程结构与 WinUI 3 移除清单

先制作最小 Avalonia 便携包。从 Linux 交叉发布 x64 与 ARM64，在 Windows 实机验证解压启动、托盘、UI 自动化、原生通知及通知动作回传。确认通知身份与激活注册方案适用于便携包后，再开始移除 WinUI 和 App SDK 打包步骤。

1. **移除依赖与黑魔法**：
   * 移除 `UseWinUI`、`WindowsPackageType`、`WindowsAppSDKSelfContained`、`Microsoft.WindowsAppSDK`、`AppxGeneratePriEnabled`、`EnablePriGenTooling`、`DISABLE_XAML_GENERATED_MAIN`、自定义 MSBuild task `WaybridgeExtractResourceIndex` 以及 `TrackCrossHostManifestTool`。
   * 删除 [scripts/extract-runtime-resources.cs](file:///home/wano/workspace/streaming/waybridge/scripts/extract-runtime-resources.cs) 与 [scripts/merge-windows-manifest.py](file:///home/wano/workspace/streaming/waybridge/scripts/merge-windows-manifest.py)（及对应的 build 校验逻辑）；更新 [scripts/build-control-center.sh](file:///home/wano/workspace/streaming/waybridge/scripts/build-control-center.sh) 为标准 `dotnet publish -r win-$arch --self-contained true` + 拷贝 `waybridge-client.exe`、文档与许可证。
   * [Program.cs](../gui/Waybridge.Control/Program.cs) 保留单实例 `Mutex`（`Local\Waybridge.ControlCenter`）和启动日志诊断，入口改为标准 Avalonia `AppBuilder.Configure<App>().UsePlatformDetect()`。
2. **建议模块拆分**：
   * `Waybridge.Control.Core/`（保持可跨平台测试）：
     * 新增 `LauncherModels.cs` & `LauncherStore.cs`（管理 `launcher.json` 与本地图标磁盘缓存）；
     * 新增 `PairingHelper.cs`（完整指纹输入和核对，后续加入 `wbpair://v1` 解析）；
     * 扩展 `SessionController.cs`（暴露 `AppIconsAsync`、`LaunchPlanAsync`、`ClickToLaunchAsync` 等协调方法，并支持带 `action`/`uris`/`argv`/`cwd` 的启动）。
   * `Waybridge.Control/`（Avalonia 桌面项目）：
     * `Views/MainWindow.axaml(.cs)`：完整应用墙与控制中心；
     * `Views/QuickLauncherWindow.axaml(.cs)`：随呼随用悬浮启动窗；
     * `Views/PairingWizardDialog.axaml(.cs)`：分步 TLS 配对；
     * `Views/SettingsView.axaml(.cs)`：分栏设置中心；
     * `Views/ActivityDrawer.axaml(.cs)`：文件传输活动与诊断延迟面板；
     * `ViewModels/`：严格分离状态、命令与跨线程调度（UI Dispatcher）；
     * `Services/DesktopNotifications.cs`：先验证通知替代方案，包括便携包应用身份、激活注册以及通知动作回到正确会话。迁移时复用现有通知协议与生命周期规则；
     * `Services/GlobalHotkeyService.cs`：Win32 `RegisterHotKey` 封装与冲突检测；
     * `IconBranding.cs`：将自定义角标规范化为 64×64。原生 Windows 窗口和托盘在目标像素尺寸合成角标。

### 5.2 本地存储与缓存策略

TLS 连接配置、Launcher 状态和图标缓存各自持久化。

* **`%LOCALAPPDATA%\Waybridge\profiles.json`**：保存 TLS 地址、证书指纹、设备标识和连接偏好，按需简化 `ConnectionProfile` / `ClientSettings`。
* **`%LOCALAPPDATA%\Waybridge\launcher.json`**：
  * 全局字段：`hotkey`（默认 `"Ctrl+Alt+Space"`，可为空表示禁用）、`view_mode`（`"grid"` / `"list"`）、`close_to_tray`、`quick_launcher_autohide`。
  * 按 `profile_id` 隔离：`pinned_apps`（有序 `desktop_id` 数组）、`recent_apps`（最近启动与时间戳，上限 32 条）、`command_history`（自定义 `argv` + `cwd` 历史，上限 20 条）、`cached_applications`（上次拉取的扩展应用目录快照）。
* **`%LOCALAPPDATA%\Waybridge\cache\icons\`**：
  * 原始图标缓存：`raw/<sha256>.png`，由启动器各页共享。

### 5.3 主窗口（Full Launcher）布局与交互

1. **顶部全局栏（Omnibar）**：
   * 左侧：**主机切换胶囊**——显示连接状态灯（绿=已连、黄=连接中、灰=断开）、主机名与当前会话名；下拉可切换主机、一键连接/断开、打开“添加/管理主机”、或触发需二次确认的“结束远端会话”。
   * 中间：**全局搜索框**（`Ctrl+K` / `/` 聚焦）：
     * 普通输入实时过滤已安装应用（匹配 `name`、`generic_name`、`id`、`keywords`、`comment`）以及当前活动窗口；
     * 输入以 `>` 开头切换为**命令运行模式**（支持自动补全历史命令、可选工作目录，回车即通过 `argv` 启动到会话）。
   * 右侧：**快捷胶囊**——「只读模式」一键切换、「声音/音量」Flyout、「状态/延迟（RTT + 实际编解码路径）」Flyout、「文件传输」抽屉（带活动徽章）、以及「设置」入口。
2. **启动器与运行中页面**：
   * **运行中**由固定左侧导航进入，集中显示远端窗口和 SNI 后台应用：
     * 活动窗口卡片显示图标 + 标题 + 编码/解码标签（如 `HEVC · GPU`），单击立即调用 `ActivateWindowAsync` 切回前台；
     * SNI 后台应用卡片提供“恢复窗口”与“应用菜单”入口。
   * **Pinned & Recent**：置顶与最近启动的应用网格卡片，视觉权重最高。
   * **All Apps** 与固定和最近应用同在一个滚动区。继续向下滚动时，目录标题、刷新、布局切换和分类筛选停留在视口顶部。全部应用支持网格和紧凑列表。
   * **应用名称**在网格和紧凑列表中均最多显示两行，超长文本在第二行末尾使用省略号，悬停可查看全名。
   * **点击与上下文操作**：
     * **单击卡片即启动**：已连接直接 `LaunchAsync`；未连接且选中有效已配对主机时自动触发 **Click-to-Launch**（显示轻量连接进度，不跳页打断），连接就绪后自动发射启动请求。
     * **右键 / 悬停 `...` 菜单**：
       * 启动；
       * 各个 Desktop Actions（如有）；
       * 固定到顶部 / 取消固定；
       * 触控模式（自动选择 / 优先原生触控 / 鼠标兼容）。按连接和完整 Desktop ID 保存应用级覆写，将已确认的目录身份映射到原生内核实际使用的窗口 `app_id`。身份未知时可从运行中窗口设置，并继续使用自动触控判定作为默认行为。
       * 自定义启动（修改/追加工作目录或参数，支持调用 `launch-plan` 预览生效方案）。
   * **首次使用冷启动**：当本地尚无已配对/可用的主机配置时，主区域展示清晰的「添加第一台 Linux 主机」引导卡片，直接唤起配对向导。
3. **二级视图**：
   * **设备与配对向导**填写 TLS 地址、核对完整指纹并输入一次性配对码。后续增加从可信渠道粘贴配对口令。已配对连接的日常视图显示状态摘要和「撤销此设备配对」，证书指纹与设备标识按需查看。
   * **设置中心**分「画面」「声音」「输入设备」「共享与权限」「网络」「连接」「桌面集成」「常规」。显示当前正在编辑的连接或全局范围，并按实际能力标注实时、重连或新建会话生效。触控、笔、手柄的总开关与本机识别、远端支持、焦点和实际转发状态直接可见。压感、死区和逐应用兼容覆盖放入高级设置。离线编辑可保存，在线应用在收到确认后显示生效结果，保留自动化探针依赖的行为。
   * **文件传输与诊断**：文件发送按钮、接收目录快捷打开与传输记录；RTT / TCP 接收速率 / 呈现更新数 / 解码与呈现阶段耗时仪表，以及日志导出（不导出令牌、剪贴板或通知正文）。

### 5.4 Quick Launcher（悬浮启动窗）规格

* **窗口机制**：随应用生命周期创建并保活（`ShowInTaskbar = false`、`Topmost = true`、无系统边框圆角 + Windows 11 `Mica`/`Acrylic` 背景）；弹出时定位到当前鼠标指针所在显示器的水平居中偏上位置（约工作区顶部 25% 处），支持隐藏/显示零闪烁。
* **触发与关闭**：
  * **全局热键**：默认 `Ctrl + Alt + Space`（Win32 `RegisterHotKey`），支持在设置页修改组合键或关闭；若热键被系统或其他软件占用，不报错弹窗，而在设置页热键旁显示“当前热键已被占用”。
  * **系统托盘联动**：左键单击托盘图标呼出 Quick Launcher；右键托盘弹出菜单（打开完整控制台 / 呼出快速启动器 / 连接或断开 / 切换静音 / 诊断 / 退出）。
  * **交互**：呼出自动聚焦顶部搜索框，展示「Running 窗口」+「搜索结果或 Pinned/Recent Top 8」；支持上下方向键高亮、回车启动（同步触发 Click-to-Launch）并立即隐藏；按 `Esc` 或窗口失焦时自动隐藏；右上角提供“展开完整控制台”图标按钮。

---

## 6. 建议实施顺序与验收点

1. **客户端主窗口、设置与便携包验证。** 收敛为单一 TLS 路径，清理内置 SSH、原始会话隧道及其配置。使用现有配对和应用目录，先交付接回会话、搜索启动、切回窗口和完整设置。并行验证最小 Avalonia 便携包，通过后再迁移 Windows 系统集成与构建脚本。运行新版直连链路和 Windows UI 自动化回归。
2. **服务端 TUI 和配置编辑并行推进。** 验收本机和 SSH 终端里的在线／离线切换、配对、受控会话操作、日志、配置来源与生效阶段、草稿校验与应用启动预览。
3. **主启动器和目录。** 交付顶部全局栏、网格和紧凑列表、固定与最近、运行窗口与后台区、应用上下文操作。通过 TLS 扩展目录元数据和批量图标，验证响应预算与缺失图标回退。粘贴配对口令单独验收完整指纹匹配、一次性使用和过期。
4. **Quick Launcher 与托盘／热键打磨。**
   * 实现 `QuickLauncherWindow` 隐藏保活、多显示器跟随居中、Win32 全局热键及冲突降级、左键托盘触发与键盘导航。
   * 验收：Windows 实机验证热键响应、失焦自动隐藏、未连接状态 Click-to-Launch 连贯性及高 DPI 呈现。
