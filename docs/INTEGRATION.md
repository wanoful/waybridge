# 桌面组件与会话生命周期

Waybridge 提供显示、应用启动环境和会话生命周期。钱包、通知、认证代理等组件通过它们自己的 D-Bus、systemd 和配置文件接入。核心不会按已安装的软件猜测钱包后端，不启动整个 Plasma，也不改写浏览器或第三方配置。

应用沿用原有 HOME、配置、数据和用户 D-Bus。原生组件可以在需要时被应用通过 D-Bus 激活；需要随 Waybridge 开始和结束的组件，则显式声明下面的 unit 依赖。用户数据共享和单向创建检查见 [会话策略](SESSION_POLICY.md)。

## 安装标准集成文件

仓库的 [integration/](../integration/) 保存 systemd target 和静态 Portal 默认配置。先构建，再明确安装：

```sh
cargo build --release --workspace --locked
python3 service/install-user-service.py
systemctl --user daemon-reload
systemctl --user enable --now waybridge.service
```

安装器复制用户二进制、TLS 入口 unit 及三个 Waybridge session target；安装步骤本身不启动服务。已有修改过的 unit 拒绝覆盖，需要用户合并。`waybridge.service` 是 TLS 入口，重启它不结束已有合成器会话。会话 target 由合成器在显示和环境准备后启动，无需手动 enable 或 start。

默认安装不写 Portal 配置。需要使用仓库提供的 GTK 文件选择组合时，可在上述安装命令增加明确的目标目录：

```sh
python3 service/install-user-service.py \
  --portal-directory "${XDG_CONFIG_HOME:-$HOME/.config}/xdg-desktop-portal"
```

这会安装 `waybridge-portals.conf`，遇到不同的已有文件同样拒绝覆盖。发行版集成可把该默认文件安装到 `/usr/share/xdg-desktop-portal/`，用户文件仍按 Portal 的原生查找规则覆盖它。运行合成器不会在临时目录生成另一份 Portal 选择配置，也不会改动 `XDG_CONFIG_DIRS`。

该组合使用 `xdg-desktop-portal` 与 GTK 后端，并将尚未实现的 ScreenCast、Screenshot、RemoteDesktop 和 GlobalShortcuts 后端设为 `none`。它不表示 Waybridge 已支持这些功能，也不保证所有 Flatpak 文档授权工作流已经验收。

## 接入一个会话组件

用户可以为选定组件编写标准 user unit，例如 `~/.config/systemd/user/example-waybridge-agent.service`：

```ini
[Unit]
Description=My Waybridge session component
After=waybridge-session-pre.target
Before=waybridge-session.target
PartOf=waybridge-session.target

[Service]
Type=simple
ExecStart=/absolute/path/to/component

[Install]
WantedBy=waybridge-session.target
```

然后执行：

```sh
systemctl --user daemon-reload
systemctl --user enable example-waybridge-agent.service
```

下一次创建 Waybridge 时启动该组件，结束会话时通过 `PartOf` 停止它。`WantedBy` 使它被会话拉起，`PartOf` 声明结束归属，两者作用不同。组件需要的参数、环境或授权方式由该 unit 和组件原生配置决定。已有发行版 unit 可使用标准 drop-in；具体服务名和启动方式以安装版本为准。

共享的持久用户服务可以继续采用自己的生命周期，不必全部归属 Waybridge。未声明归属的后台进程不会因名称或 UID 被扫描、终止。是否将一个现成 provider 绑定会话，应由用户根据其生命周期选择。

如果组件已经提供 systemd unit 和带 `SystemdService=` 的 D-Bus 激活文件，应沿用该 unit 名称，通过 drop-in 接入。不要复制一个不同名称的 `Type=dbus` 包装服务来占用相同 `BusName`；即使两个服务没有同时运行，systemd 也可能因名称分配冲突拒绝激活。

例如，已安装的 Portal 可通过 `~/.config/systemd/user/waybridge-session.target.d/portals.conf` 拉起：

```ini
[Unit]
Wants=xdg-desktop-portal.service xdg-desktop-portal-gtk.service
```

再分别为这两个原生 unit 添加 `waybridge.conf` drop-in：

```ini
[Unit]
PartOf=waybridge-session.target
After=waybridge-session-pre.target
Before=waybridge-session.target
```

Portal 前端可额外声明 `After=xdg-desktop-portal-gtk.service`，明确所选后端的启动顺序。修改后运行 `systemctl --user daemon-reload`。这保持 D-Bus 激活文件、systemd unit 与实际服务一致；钱包等组件同样应避免重复分配名称。

## 就绪与退出顺序

1. 合成器完成单向准入检查、创建显示，并发布本会话显示与身份环境。
2. 异步启动 `waybridge-session.target` 和它声明的依赖，主线程继续处理 Wayland，允许需要显示交互的 `Type=notify`/D-Bus 服务完成启动。
3. target 进入 active 后，生命周期诊断报告 `ready`。具体组件是否成功仍由它的 unit 状态判断；`Wants` 的组件失败可以不阻止 target 激活，应用是否已显示窗口也另行判断。
4. 正常结束时，启动 shutdown target，停止通过 `PartOf` 声明归属的服务；这段时间继续处理显示请求。
5. 服务停止后撤销本会话仍拥有的激活环境，再销毁显示。后登录桌面已经改写的环境予以保留，注销前已失效的显示地址不恢复。

这三个 target 独立于 `graphical-session.target`、`plasma-workspace.target` 和其他 DE target。标准 systemd 用户管理器及已安装的集成文件提供声明式生命周期；缺失时诊断标记 `unavailable`，不假装已经管理了外部服务。

systemd 的环境支持真正删除未设置变量，原先用户显式设置的空值则保持为空。D-Bus daemon 的 `UpdateActivationEnvironment` 没有删除接口；只能以空值禁用失效地址，诊断会说明这一限制。清理先更新 D-Bus，再让 systemd 恢复或删除变量，避免 daemon 的 systemd 激活转发把未设置变量变为空字符串。没有可查询的用户管理器环境时，也无法安全判断 daemon 环境是否已被后登录桌面接管。会话不将 Qt/GDK 后端偏好、每应用音频设置或数据目录导入全局用户服务环境。

## 钱包、浏览器与诊断

钱包内容和解锁方式仍由原生组件负责。SSH 密钥登录不会自动提供钱包解锁密码；已有解锁服务也不意味着它选择了浏览器所用的密码存储后端。需要保留原 Chrome 后端时，使用用户明确配置的 [desktop 覆写](CONFIGURATION.md)，不会由核心自动追加参数。

`WAYBRIDGE_DESKTOP_SERVICES` 指向会话状态 JSON，包含生命周期状态、标准接口的只读观测及未支持能力。D-Bus name 存在仅代表接口有 owner，不能证明其窗口连接了当前显示；Waybridge 不据此自动重启共享服务。

可在 Waybridge 终端查看状态，并用标准工具查询声明的组件：

```sh
cat "$WAYBRIDGE_DESKTOP_SERVICES"
systemctl --user status waybridge-session.target
systemctl --user status example-waybridge-agent.service
journalctl --user -u example-waybridge-agent.service
```

应用目录的启动方案与环境覆写诊断见 [配置与应用启动](CONFIGURATION.md)。实际验证范围和未完成项见 [验证记录](VALIDATION.md)。
