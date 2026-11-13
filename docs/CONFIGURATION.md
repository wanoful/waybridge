# 配置与应用启动

所有命令在仓库根目录执行。设计边界见 [配置决策](CONFIGURATION_POLICY.md)，外部服务的接入见 [桌面集成](INTEGRATION.md)。

## Linux 主配置

默认读取 `$XDG_CONFIG_HOME/waybridge/config.toml`；未设置有效的绝对 XDG 路径时使用 `~/.config/waybridge/config.toml`。默认文件不存在时采用内置默认值，不在当前工作目录查找。显式 `--config /absolute/path.toml` 替代默认文件，文件不存在也会报错。

优先级为内置默认值 → 配置文件 → 显式命令行参数。未知字段、错误类型和越界值均报错。先检查，再启动：

```sh
./target/release/waybridge --check-config
./target/release/waybridge --print-config
./target/release/waybridge --config /absolute/path.toml --fps 45 --print-config
./target/release/waybridge-service --config /absolute/path.toml --check-config
```

`--print-config` 的 JSON 包含 `values`、逐字段 `sources` 和生效阶段 `apply`，不包含身份私钥、配对令牌或环境变量转储。这两个检查入口不会创建图形会话、启动钱包或监听网络。

首版配置示例，省略字段采用内置默认值：

```toml
[service]
listen = "0.0.0.0:49092"
compositor = "/absolute/path/to/waybridge"
python = "python3"

[session]
listen = "127.0.0.1:49090"
socket = "waybridge-0"
fps = 60
bitrate = 30000 # 每个视频流的 kbps
codec = "auto"
gpu_input = "cpu"
hdr = "off"
layout = "us"

[limits]
max_fps = 120
max_bitrate = 200000
allowed_codecs = ["png", "h264", "hevc", "av1"]
```

`codec` 接受 `auto`、`png`、`h264-nvenc`、`hevc-nvenc`、`av1-nvenc`；`gpu_input` 为 `cpu` 或 `cuda`；`hdr` 为 `off` 或 `experimental-scrgb`。`auto` 的 SDR 策略为 H.264，HDR 策略为 HEVC。`allowed_codecs` 使用编码族名称，`auto` 解析后也必须符合限制。安全回退及硬件能力仍以实际运行路径为准。

帧率支持 1–120，码率支持 1000–200000 kbps；用户限额可以收紧这些范围。会话默认值也必须符合限额，例如将 `max_fps` 改成 30 时需同时设置 `session.fps = 30`。

修改 `[service]` 后重启 TLS 服务；会话设置和限额用于新建会话，不重配已有进程。客户端可请求帧率、码率、编码和 HDR 等允许实时调整的项目，Linux 校验后返回生效值或拒绝原因。Windows `profiles.json` 保存客户端偏好，Linux 配置保存服务端默认值与限制；请求不会改写 TOML。

`session.listen`、`session.socket` 是直接启动合成器的默认值；TLS/SSH 管理入口会为会话分配监听地址和 socket，并在来源报告标为 `session_manager`。`auto`/`attach` 接回已有会话时不读取新的媒体配置。服务的 `pair`、`status`、`clients`、`revoke` 同样直接连接运行中的入口，`--config` 只影响 `serve` 和配置检查。

`allowed_codecs` 限制编码请求。运行期硬件失败或会话未启用视频时，仍可回退到 PNG；实际设置回复通过 `fallback_reason` 说明，不把允许请求的编码族等同于硬件保证。

## Waybridge 专用 desktop 覆写

在 `$XDG_CONFIG_HOME/waybridge/desktop-overrides/` 创建 `<完整 desktop ID>.toml`，例如 `google-chrome.desktop.toml`。ID 来自应用目录接口的 `id` 字段。它与可执行文件名、窗口标题并非同一个标识；子目录安装的应用仍使用标准折叠后的完整 ID。

下面的例子仅在 Waybridge 应用目录启动 Chrome 时加上用户明确选择的 KWallet 6 参数，沿用原始 `Exec` 和原 profile。是否选用该后端取决于原有数据使用的后端：

```toml
version = 1

[default]
launch_mode = "exec"
extra_argv = ["--password-store=kwallet6"]
```

`extra_argv` 是字面参数数组，插在原始命令的可执行文件后、原有参数前，因此位于原来的 `--` 和文件字段之前。它不经过 shell；字面 `%` 也不会变成新的字段代码。需要 shell 时必须在 `exec` 中明确调用 shell。

可用字段：

| 字段 | 行为 |
|---|---|
| `exec` | 替换 Desktop Entry `Exec` 字符串，仍由 GIO 解析引号、转义和字段代码 |
| `extra_argv` | 按上述位置插入字面参数 |
| `cwd` | 指定绝对工作目录 |
| `env` | 设置环境变量的字符串表 |
| `unset_env` | 取消环境变量的名称数组 |
| `launch_mode` | `desktop` 保留标准激活行为；`exec` 显式选择命令启动 |
| `name`、`icon`、`terminal` | 覆写对应的显示元数据或终端属性 |
| `clear` | 删除 `exec`、`cwd` 或 `icon` 对应的原始字段 |

默认动作使用 `[default]`；具名动作使用 `[actions."ActionID"]`，两者互不继承：

```toml
version = 1

[default]
cwd = "/home/alice/Projects"
extra_argv = ["--example-option"]
unset_env = ["EXAMPLE_OLD_SETTING"]

[default.env]
EXAMPLE_MODE = "work"

[actions."NewWindow"]
extra_argv = ["--example-option"]
```

应用声明 `DBusActivatable=true` 时，D-Bus 激活可能把请求交给现有实例，不能保证应用进程使用新的参数、目录或环境。出现这类覆写时必须显式选择 `launch_mode = "exec"`，否则启动报错；即使选用 Exec，应用自己的单实例机制仍可能转交请求。

原始 `.desktop` 和标准用户应用目录保持不变。派生入口只存在于会话运行目录，随会话清理；`%k` 仍指向原文件。非 Waybridge 的启动和直接 argv 启动均不套用这些覆写。

## 工作目录与诊断

桌面启动使用生效的 `Path`/`cwd`，未指定时使用 HOME，避免 HMCL 等把相对数据目录建在服务或仓库目录。直接命令同样默认 HOME，可用 `--cwd` 指定目录：

```sh
./target/release/waybridge --launch --cwd /home/alice/Projects -- blender
```

终端内后续命令正常继承 shell 当前目录。

`--launch` 省略 `--socket` 时会读取配置以确定会话名称；显式 `--launch --socket NAME` 则只接入该会话，不解析新配置，配置文件暂时写坏也可继续使用已有会话。

管理助手通过一行 JSON 接受请求；TLS 和 SSH 管理入口复用相同契约。先查询方案不会执行应用：

```json
{"id":1,"op":"launch-plan","session":"waybridge-0","app":"google-chrome.desktop"}
{"id":2,"op":"launch","session":"waybridge-0","app":"google-chrome.desktop"}
{"id":3,"op":"launch","session":"waybridge-0","app":"example.desktop","action":"NewWindow"}
{"id":4,"op":"launch","session":"waybridge-0","app":"example.desktop","uris":["file:///home/alice/example.txt"]}
```

本地可将请求写给 `python3 scripts/session-control.py`；TLS 请求另加已配对的身份字段，见 [服务接口](SERVICE.md)。GIO 的 action 契约不接受文件/URI，因此不能在同一请求中同时指定 `action` 和非空 `uris`。

启动方案显示来源、目录、动作和激活方式；环境定制仅显示 `env_keys`、`unset_env`，不导出环境值或会话令牌。桌面启动返回的 `dispatched` 或 `activation-requested` 只表示已提交请求，直接命令的 `spawned` 表示已创建进程，均不能证明应用已创建窗口。
