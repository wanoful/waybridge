# 游戏手柄（实验）

更新日期：2026-11-18。Windows 的 XInput 手柄通过认证连接转发为 Linux 会话拥有的 uinput 虚拟手柄。默认关闭，不采集后台手柄输入。

## GUI 与配置

连接配置的输入设置中提供以下字段；保存到 profile 的 `settings`，不需要环境变量：

| GUI 设置 | Profile 字段 | 默认值 | 范围与含义 |
| --- | --- | --- | --- |
| 游戏手柄 | `gamepads` | `false` | 启用手柄转发 |
| 手柄震动 | `gamepad_rumble` | `true` | 允许当前会话的游戏回传震动 |
| 摇杆死区（%） | `gamepad_deadzone` | `15` | 整数 `0–50`；当前同时作用于摇杆和模拟扳机 |

摇杆使用径向死区并重新缩放剩余行程，扳机独立应用阈值并重新缩放。初始 hello 协商 `gamepad_v1`、`gamepad_enabled`；支持 `gamepad_settings_v1` 的服务端可通过 `gamepad_enabled` 实时切换。旧端缺少能力时显示状态，不把保存偏好误报为已生效。

`gamepad_enabled` 表示协商后的启用偏好；`gamepad_active` 表示当前前台窗口取得了输入租约。另报告 `gamepad_supported`、`gamepad_devices`、`rumble_supported`、`gamepad_error` 和动态加载的 `gamepad_backend`。

## 输入、焦点与震动

Windows 仅从 System32 动态加载 `xinput1_4.dll`，失败后依次尝试 `xinput1_3.dll`、`xinput9_1_0.dll`，没有静态 XInput DLL 依赖。最多四个槽位，转发标准按钮、双摇杆、双扳机和方向键；普通 HID/DirectInput 设备不在本批范围内。活跃设备轮询上限为 125 Hz，空槽位每 500 ms 探测一次；实际频率受 UI 定时器和调度影响。

只有本地前台远端窗口、服务端焦点和非只读输入策略同时匹配才取得租约。输入快照绑定窗口 ID、服务端 generation、槽位、设备 incarnation 和递增序号；过期、重复及越界状态不产生输入。失焦、只读、关闭功能、拔出或断开会清空按钮与轴，撤销对应虚拟设备，并停止 Waybridge 自己启动的震动。默认关闭时不会轮询手柄或清零其他应用的电机。

Linux 使用标准 Xbox 360 的 BTN/ABS/HAT 布局，按 evdev 约定反转摇杆 Y 轴。游戏的 `FF_RUMBLE` 上传、播放、停止和有限时长结束，经当前租约回传到原 Windows 槽位；旧设备反馈不能作用于重连后的设备。持续震动每 250 ms 续发，反馈租期为 750 ms，客户端在轮询中检查超时并归零；它不是独立于 UI 调度的实时硬截止。

当前每次焦点租约变化都会重建虚拟节点，SDL/Steam 可能重新分配玩家索引。虚拟设备由本会话创建并随生命周期删除，但内核设备节点对系统可见，不提供进程级隔离。

## 后端与验收

Linux 检查 `/dev/uinput` 的读写访问及版本（需要支持设备设置的 v5 及以上），只有有效、已连接且持有焦点租约的输入才创建设备。缺少节点、权限不足或 ioctl 失败会通过 `gamepad_error` 报告并停止手柄功能，不修改系统权限，也不影响其他输入功能。

已通过严格 x64/ARM64 交叉编译，以及以下定向验证：

```sh
cargo test --locked gamepad::tests
cargo test --locked gamepad::tests::uinput_roundtrip_and_real_rumble -- --ignored
python3 tests/windows_gamepad_smoke.py
python3 tests/session_fixture.py python3 tests/gamepad_smoke.py
```

真实 Linux 测试只操作由唯一名称、phys 和 sysname 确认的临时虚拟设备，验证 evdev 按钮/轴/方向键、真实 `EVIOCSFF` 上传和震动回传、效果到期归零、热插拔隔离及设备删除。负例覆盖缺失/无权限后端、非 uinput 节点、非法数值、旧租约和重放。Windows 测试在私有 Wine/Xvfb 中运行生产适配器与模拟 XInput，覆盖默认关闭、轮询边界、死区、焦点、重连和精确槽位震动。

真实合成器协议到 evdev 的端到端测试也已通过默认关闭、双端焦点匹配、按钮/轴/HAT、非法/过期/重复输入拒绝、失焦/只读/断线中和销毁和恢复后归零。只读取由本次合成器 PID、name、phys 和 sysname 确认的临时节点，权限不足明确跳过，不修改权限。

实体 Windows 手柄输入与电机、真实 SDL/Steam 游戏的识别和玩家索引仍待验收。Windows x64 的实际 WinUI 设置保存、重载和状态已通过私有 provider 测试；模拟适配器和 evdev/FF 通过不等于实体输入已通过。
