# 桌面交互：窗口、托盘和拖放

更新日期：2026-11-17。本文区分当前实现、自动回归和真实应用验收；不将一种应用成功视为全部工具包兼容。

## 本轮问题及机制

- **托盘点击**：Windows Shell 协商 `NOTIFYICON_VERSION_4` 后，一次左键可同时产生传统 `WM_LBUTTONUP` 与 `NIN_SELECT`，右键也可能同时产生传统抬键与 `WM_CONTEXTMENU`。重复转发会让切换显示的应用先打开再隐藏，也会让第一次菜单的 token 失效。当前按实际协商版本只处理一套回调；SNI/DBusMenu 动作回到原注册对象，验证拥有者、代际、菜单 token 与节点。菜单时间戳不使用另一个系统的 uptime。
- **窗口图标**：托盘图标与窗口图标属于不同协议对象。窗口优先使用运行时图标（Wayland `xdg-toplevel-icon-v1`、X11 `_NET_WM_ICON`），再按 desktop ID、`StartupWMClass`、X11 class/instance 等通用关联查找。运行时更新、删除、异步旧结果和重连都有明确失效规则。来源角标在客户端统一添加，不改应用图像。
- **激活和焦点**：用户在控制中心或托盘发起操作时建立短期激活上下文，经实际启动 PID 或标准 `xdg-activation-v1` token 关联到窗口，再请求 Windows 前台。过期、重复和后台请求不能无限继承这次输入。对话框隐藏/销毁前，先恢复父窗的可用状态与焦点。应用窗口不设置永久 TOPMOST；后台请求可提示用户。
- **拖放**：原来主要桥接文件字节，遗漏文本 OLE 格式、跨 HWND 鼠标捕获、返回原会话的交接、X11 XDND 目标和 DND icon。现在由通用 Source／DndFocus／OfferData 路由原 Wayland/X11 拖拽，协调外部 MIME 交付与原生 OLE 读取，通过连接、source/offer、拖拽代际和 OLE attempt 约束生命周期，不使用应用名规则。

Chrome 实机复测还发现“立即松手”会早于远端接受，Qt 成功并不能证明该场景。当前补齐 Pending/Accepted/Rejected、目标 revision、可重入的有界 OLE 等待和目标完成回执，具体职责、750 ms／2 秒边界及测试入口见 [拖放设计](DRAG_AND_DROP.md)。文字拖放已从文件共享中分离，在控制中心有独立开关。

## 当前支持矩阵

| 方向/场景 | 当前行为 | 边界 |
|---|---|---|
| 同一 Linux 会话、Wayland/X11 窗口内及不同串流窗口间 | 保留原数据源、原 MIME 和目标协商的 COPY/MOVE；跨 HWND 路由鼠标 | 依赖源与目标本身支持该 MIME/动作；不是任意控件都必须接受 |
| Windows → Wayland/X11 文字 | `CF_UNICODETEXT` 转 UTF-8 MIME，先 hover/协商再 COPY | 1 MiB 上限；不导出临时文本文件，不改剪贴板 |
| Wayland/X11 → Windows 文字 | UTF-8 MIME 转 OLE `CF_UNICODETEXT` | 纯文本；HTML/RTF/图像拖放尚未桥接 |
| Windows → Wayland/X11 文件/目录 | 完成传输后，通过只读文件视图交付 URI | 当前仍在 Windows drop 后传输/交付；没有传输前的文件 hover、自动展开或滚动协商 |
| Wayland/X11 → Windows 文件/目录 | OLE virtual-file descriptor + 延迟只读 IStream，支持异步读取 | 目标必须接受虚拟文件；只接受 CF_HDROP 的 Windows 应用尚未覆盖 |
| 离开远程窗口进入 Windows OLE，再返回远程窗口 | 结束本次 OLE attempt，继续原 Wayland/X11 source；延迟旧 IStream/异步结果不能修改新 attempt | 保留原会话拖放语义，不把文件重新上传给自己 |
| Windows → Xwayland 目标 | 纯文本、URI 文件 COPY；XDND Status/Finished、TARGETS 和 INCR | 保留外部只读 COPY 策略；不提升为 MOVE |
| Wayland ↔ X11；X11 ↔ X11 | 原 selection/MIME、COPY/MOVE、独立坐标与源/目标完成确认；Windows 出入复用现有 OLE 路径 | Qt 协议矩阵及 Windows 实机夹具验收；QQ/Dolphin 等实际工作流需独立记录，详见 XDND.md |
| 拖拽虚影 | 使用源的真实 Wayland DND icon，支持动画、透明度及 hotspot/attach offset | 源必须提供图标；Windows 临时层穿透鼠标且不激活，尺寸、像素、帧率和队列有界；X11 源图标未接入 |

跨设备文件保持 COPY 和只读交付：应用可读/导入，用户希望编辑持久文件时先复制到 Dolphin/Explorer 所选目录。复制出的文件可以正常写入。不做应用专用 Save As 适配、随机可写工程副本或自动写回原文件。此限制不改变同一 Linux 会话内原文件的正常 MOVE/编辑语义。

## 状态归属

- `src/data_offer.rs`：不可变 MIME 数据快照；`src/data_drop.rs`：会话协调、Wayland/Xwayland 目标选择、接受状态、drop 截止时间与取消、源导出租约。
- `src/drag.rs`：Wayland 源读取、受限文件导出和私有协议 sink；数据源的完成与取消由原协议处理。
- `src/input_focus.rs`、`src/dnd_lifecycle.rs`：统一焦点路由、源身份及反馈；Smithay `input/dnd`、data-device 与 XWM 负责协议。XWM 查询仍在 calloop 线程，不能声称所有 X11 往返均有独立线程/截止时间；迁移来源和边界见 [XDND](XDND.md)。
- `client/file_drag.h`、`client/text_drag.h`：原生 OLE 数据表示、虚拟文件流、外部文本及原会话返回；窗口销毁、COM 重入和旧异步结果不得复活已取消手势。
- `src/drag_visual.rs`、`client/drag_visual.h`：只负责视觉反馈；不拥有文件、焦点或数据传输的完成状态。
- `src/activation.rs`、`client/window_activation.h`：激活授权、连接代际与前台切换；图标/托盘不自行设置窗口永久层级。

## 验证和后续

通用真实 Qt 控件回归覆盖输入框、不同窗口、文件管理模型 MOVE 和外部文字；真实生产 Windows worker 回归覆盖 OLE 出入及离开后返回；私有 Xwayland 回归覆盖 Unicode、INCR 和只读 URI 文件复制。托盘回归使用真实 SNI/QAction，图标测试使用真实 Wayland/X11 协议对象。设备测试及其限制见 [VALIDATION.md](VALIDATION.md)。

真实 Windows Chrome 与原生 Wayland Chrome 的专用页面另外覆盖双向文字、立即松手、延迟、取消、拒绝、跨窗口与换目标，源端完成结果和目标文字同时验收。此结果限定于纯文本和该应用组合。

下一步扩展真实应用矩阵，并补文件 hover/进度/取消、CF_HDROP-only 消费者及更多拖放格式。`XEmbed` 托盘、全局菜单等独立协议仍按 [ROADMAP.md](ROADMAP.md) 分项推进。用户反馈的 HMCL、QQ、Dolphin、Blender 应分别记录版本、后端和实际操作结果，不能以名称特判替代协议支持。
