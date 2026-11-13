# 配置归属与桌面集成决策

决策日期：2026-11-14。状态：已确认的设计，待实施。本文补充 [会话策略](SESSION_POLICY.md)，收敛桌面服务和应用兼容的职责边界，不代表以下配置入口已经可用。

## 职责边界

Waybridge 负责合成、远程窗口、输入、媒体，以及可靠的图形会话入口和通用应用启动行为。会话环境发布、就绪和退出顺序属于项目责任；钱包、通知、认证代理等外部组件的选择由用户或可选集成配置决定。

- 使用标准用户 D-Bus 和用户原有数据，不按已安装的软件猜测钱包或浏览器密码后端。
- 通过 systemd 用户 unit、target 和声明的依赖接入服务，不在核心建立按 KWallet、GNOME Keyring 等名称区分的服务管理框架。
- 使用 `.desktop`、D-Bus、Portal 等既有契约。需要合成器配合的能力，例如 Waybridge 的 ScreenCast 后端，仍由项目实现。
- 不建立按 Chrome、Steam、HMCL 等程序名称自动修改行为的核心兼容引擎；用户显式的启动定制使用下文的通用机制。
- 项目可维护推荐组件组合、unit/drop-in 示例和应用配置说明，不自动接管用户桌面配置。
- 单向创建检查和共享用户数据的决策保持不变，启动后的其他 DM 登录仍不拦截。

会话结束应先处理归属当前会话的图形服务，再销毁显示并撤销环境。用户总线、持久数据以及用户自行保留的后台任务按各自生命周期处理，不扫描或强杀所有同 UID 进程。服务存在不等于它已经连接当前显示；此类状态需要诊断，而不是按程序名自动修复。

## 配置所有权与位置

实现时尊重有效的 XDG 环境变量，不整体替换应用的 HOME 或 XDG 数据路径。

| 内容 | 建议位置 | 所有者 |
|---|---|---|
| Linux 服务、会话默认值、资源限制 | `$XDG_CONFIG_HOME/waybridge/config.toml` | Waybridge 用户配置 |
| Waybridge 专用应用启动追加/覆写 | `$XDG_CONFIG_HOME/waybridge/desktop-overrides/`，按完整 desktop ID 区分 | Waybridge 用户配置 |
| Windows 连接、画质及设备偏好 | 现有 `%LOCALAPPDATA%\Waybridge\profiles.json` | Windows 控制中心 |
| Waybridge 服务和会话生命周期 | 标准 systemd 用户 unit、target 及用户 drop-in | systemd；包提供默认，用户提供覆盖 |
| Portal 默认后端选择 | `/usr/share/xdg-desktop-portal/waybridge-portals.conf` | 可选集成包 |
| 用户 Portal 覆盖 | `$XDG_CONFIG_HOME/xdg-desktop-portal/waybridge-portals.conf` | 用户，遵循 Portal 原生规则 |
| 全桌面通用的应用配置和 desktop 覆盖 | 应用原生配置、`$XDG_DATA_HOME/applications/` | 用户及对应应用 |
| 钱包、浏览器等持久数据 | 各应用原有位置 | 对应应用 |
| 配对身份、信任记录、日志等持久状态 | `$XDG_STATE_HOME/waybridge/` | Waybridge；身份材料单独限制权限 |
| 可重建缓存 | `$XDG_CACHE_HOME/waybridge/` | Waybridge，可重建 |
| socket、锁、会话描述及派生启动文件 | `$XDG_RUNTIME_DIR/waybridge/` | Waybridge 运行期资源 |

用户文件和第三方文件不因 Waybridge 启动或升级而被改写。凭据、私钥、钱包内容不进入普通配置导出；Windows 凭据继续使用凭据管理器。

## Waybridge 自身配置

- 首版采用 TOML，一份用户主配置，不引入隐式脚本执行、工作目录自动发现或复杂 include 体系。
- 优先级为：内置默认值 → 用户配置文件 → 显式启动参数。`--config` 指定文件时替代默认用户文件位置，不再叠加隐藏配置层。
- Windows 保存客户端偏好，Linux 保存服务端设置和允许范围。客户端只请求允许动态调整的项目，服务端返回生效值及限制或回退原因，不通过请求改写 Linux 配置文件。
- 临时运行时修改不自动持久化；保存为默认值必须是明确操作。
- 每项设置标明立即生效、下一会话生效或需要重启服务。无效字段和取值明确报错，不静默退回另一套行为。
- 会话层管理显示和激活所需变量；工具包偏好、应用参数等不因核心默认选择而一并导入用户全局环境。用户显式的环境覆盖应注明作用范围。

## Waybridge 专用 desktop 追加与覆写

确认提供这一能力，作用范围仅限通过 Waybridge 的桌面应用启动入口，不改变本地 KDE、niri 等桌面的同名应用启动行为。

这是用户显式提供的通用启动配置。例如，用户可以为自己的 Chrome desktop ID 增加密码后端参数，但核心不会识别 Chrome 并猜测应添加什么参数。

### 查找与覆盖顺序

1. 按标准应用目录和用户覆盖规则解析原始 `.desktop`，以完整 desktop ID 作为匹配键，不按可执行文件名或显示名称匹配。
2. 应用该 ID 的 Waybridge 专用追加/覆写，得到仅供当前启动使用的有效视图。
3. 交给通用启动器及 GIO 等标准实现处理，保留字段转义、文件/URI 展开和 desktop action 语义。

原文件和 `$XDG_DATA_HOME/applications/` 中的用户文件均保持不变。派生文件如有需要放在运行目录，记录原始 ID、来源和生效配置；不能因临时文件命名破坏应用身份和激活语义。

### 行为约束

- 区分字段替换与参数追加，允许用户显式定制启动命令、工作目录、终端行为、显示元数据，以及环境变量的设置和取消。
- 额外参数采用字面参数数组，插入位置、`--` 边界以及 `%f/%F/%u/%U` 等字段展开的先后必须明确，不通过 shell 字符串拼接实现。
- 默认启动动作与具名 desktop action 分别处理，不把一个动作的覆盖隐式传播到其他动作。
- 提供取消/删除覆盖值的表达方式，不能用空字符串统一代替未设置。
- 替换 `Exec` 时仍按 Desktop Entry 规则解析；只有用户显式使用 shell 命令时才具有对应 shell 语义。
- `DBusActivatable=true` 与进程参数、工作目录或环境覆写可能存在语义冲突。不能静默忽略覆写或静默关闭 D-Bus 激活；需要校验，并让用户显式选择保持标准激活或采用 `Exec` 启动。
- 请求交给已有应用实例时，不承诺覆写能改变该实例的环境、目录或窗口。诊断区分新进程启动与请求转交。
- 直接 argv 启动保持显式命令语义，不根据可执行程序名称自动套用 desktop 覆写。

通用工作目录规则：桌面应用采用有效覆写后的 `Path`，未指定时默认用户 HOME；直接命令采用显式 `cwd`，未指定时默认 HOME；应用终端内的子程序正常继承 shell 当前目录。

专用覆盖文件的精确 schema、参数插入语法和 action 寻址形式在实施时设计并验证。本次确认作用范围和行为约束，不把未定语法当作已可用接口。

## 标准配置与推荐组合

项目可在仓库 `integration/` 集中维护 unit、target、Portal 默认配置和接入示例；安装后由对应标准组件读取，不新增统一配置同步守护进程。

- 包提供的文件由包更新，用户覆盖始终归用户所有。
- 推荐配置通过明确安装/启用步骤接入，不在每次启动时重新生成或覆盖用户文件。
- 第三方配置保留原格式和覆盖规则：systemd drop-in、标准 desktop 覆盖和 Portal 配置不套用同一个合并算法。
- 钱包/provider 的选择以及应用数据迁移留在显式用户配置和组件工具中，不自动修改或合并钱包。
- 专用配置优先使用 Waybridge 独立命名空间，避免无意改变其他桌面。

## 诊断、升级与验收

提供配置检查、生效值和来源查询、desktop 有效启动方案查询，以及标准桌面接口检查。显示实际目录、启动方式和配置来源；敏感字段脱敏，不导出钱包或登录数据。检查工具主要报告问题，不自动重写第三方配置。

升级只更新包拥有的默认文件；用户配置需要迁移时显式报告变化，不静默覆盖。旧隔离应用数据继续按 [会话策略](SESSION_POLICY.md) 保留。

M1 收尾覆盖：默认和覆写后的工作目录、参数/字段展开与 shell 边界、D-Bus 激活冲突、不同动作的覆盖、环境变量取消、用户文件不被修改、非 Waybridge 启动不受影响，以及图形服务随声明的会话依赖正常结束。实际 systemd 用户会话集成需要独立验证，不能仅依靠模拟 D-Bus/logind 的结果。

当前代码仍有硬编码的钱包激活/监控和运行期 Portal 配置，主配置及专用 desktop 覆写也未实现；这些属于下一轮调整范围。本次仅记录决策，不修改运行行为。

参考：[XDG 目录](https://specifications.freedesktop.org/basedir/latest/)、[Desktop Entry](https://specifications.freedesktop.org/desktop-entry/latest/)、[Portal 配置](https://flatpak.github.io/xdg-desktop-portal/docs/portals.conf.html)、[systemd unit](https://www.freedesktop.org/software/systemd/man/latest/systemd.unit.html)。
