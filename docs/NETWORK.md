# 网络体验：UDP 节流与编码码率联动

更新日期：2026-11-19。认证 UDP 的交付反馈同时驱动发送节流和 NVENC 编码目标。线协议仍为 v3 / `udp_fec_v1`，现有 Windows 客户端即可使用。

## 当前链路与范围

控制、媒体及音频继续使用独立认证连接。UDP 需要显式配置可达地址；未启用、尚未绑定、保活过期、载荷过大或缓存不足时保留 TCP 路径。每窗口呈现 ACK、UDP 全局在途额度和原始捕获背压分别承担不同职责。

UDP 全局最多八个未确认消息、64 MiB 在途字节阈值（保留单条较大消息的既有例外），修复缓存为 32 MiB。发送、修复和逐窗口解码沿用既有顺序。

## 反馈与节流

- `--udp-mbps` 仍是发送上限，初始按这个上限发送。每个数据报按加密载荷及 UDP/IP 基本头计入节流，包括认证头、认证标签和 XOR 校验包；链路层及 VPN 额外封装开销不在此估算中。节流时刻跨消息延续，阻塞后的调度不补发一串追赶突发。
- 最后一个 DATA 数据报提交发送前记本机 `Instant`（不等待尾随校验包），以认证控制通道上该消息的 `udp_ack` 计算交付确认耗时。它包括接收端组装、调度及反馈通道延迟，不是网络 RTT，更不是物理输入到显示延迟。
- 只使用仍在途、已到达最终 DATA 发送边界且未请求 TCP 修复的消息训练恢复；未知、重复、提前到达的 ACK 和 TCP 回退交付不训练。累积 ACK 继续正常释放额度。
- 对到达最终 DATA 发送边界至少 20 ms 后的首次有效修复请求，速率减为当前的 75%，250 ms 内不重复降低。更早的请求立即走 TCP 修复，但只将它计为修复请求，因为接收端的周期检查可能与仍在路上的 DATA 交叉。
- 用交付确认耗时的 EWMA 与十秒窗口内最小值比较；超过基线加 `max(基线/2, 50 ms)` 时保守降低速率，最多每秒一次。该余量容纳接收端及反馈调度抖动；延迟升高也可能来自接收端，不声称准确定位网络拥塞。
- 稳定反馈下至少四次有效 ACK、距离上次增长至少 500 ms、距离修复至少一秒，才增长 `max(当前速率的5%, 250 kbit/s)`，始终不超过配置上限。下限为 `min(配置上限, 2 Mbit/s)`。

周期基线会重新建立，避免路径/接收端的永久延迟变化永远阻止恢复。单个丢包若被 XOR 直接恢复，现有协议不会报告它，因此本轮没有独立丢包率估计。较大画面在低速节流时可能先触发既有 100 ms 接收器修复计时器而经 TCP 交付，本批未调整该计时器。这是有界、保守的交付反馈节流，不是对任意共享网络的公平性或最优吞吐承诺。

## 编码码率

- 已认证且在三秒内收到保活的 UDP 会话，将当前节流速率的 75% 分给过去两秒有视频编码活动的窗口。每个窗口的目标受 `--bitrate` 或客户端应用的码率上限约束，NVENC 最低目标为 1 Mbit/s。
- 编码器运行至少 500 ms 后可处理 10% 或更大的降幅，小幅变化等到 2 秒后再处理。目标高于当前码率持续至少 2 秒后才恢复，每步最多增加当前目标的 20% 或 500 kbit/s，取较大值。TCP 会话用配置码率，UDP 保活过期后已有窗口按同样的恢复步长升回配置码率。
- 实际切换时重新初始化该窗口的编码器，从新的解码 epoch 和 IDR 帧开始。同码率下仍可复用已有的窗口尺寸缓存。重建期间有初始化开销，已编码的引用帧仍按顺序交付。
- 预留带宽用于校验、头部、透明度数据和编码突发。该策略控制视频目标码率，实际流量还受 PNG、透明度数据、音频、关键帧和最低编码目标影响。分辨率、FPS 和媒体资源调度沿用各自配置。

## 诊断

使用 `--stats` 每两秒报告：

- `udp_delivery_ack`：本机时钟上的交付确认耗时及分位数。
- `udp_pacing_kbps`：当前速率和区间最高值。
- `udp_pending_messages` / `udp_pending_bytes`：在途消息数/载荷字节的最近值和区间最高值。
- `udp_pacing_backoff_repair` / `udp_pacing_backoff_delay` / `udp_pacing_recovery`：实际速率调整次数。
- `udp_tcp_repair_requests` / `udp_tcp_send_fallback`：有效修复请求及数据报发送失败后的 TCP 回退次数。
- `udp_repair_feedback_grace`：最后一个 DATA 发送边界之后 20 ms 内的修复请求。传输立即修复，节流估计忽略这类样本。
- `video_target_kbps`：成功编码帧的最近目标与区间最高值。NVENC 启动日志也记录窗口、目标码率、UDP 节流速率和解码 epoch。

## 验证

`cargo test --locked udp::` 验证退避冷却、上下限、恢复、延迟变化、跨消息节流，以及无效/重复/提前 ACK、即时修复与延迟采样、在途字节归还。真实本地 UDP 套接字的低速夹具在最终 DATA 和校验包之间留出约 500 ms，要求在校验包仍等待发送时的 ACK 已具备有效采样边界，防止低延迟接收端永久错过恢复反馈。

```sh
python3 tests/session_fixture.py python3 tests/udp_adaptive_smoke.py
python3 tests/session_fixture.py python3 tests/udp_adaptive_smoke.py --nvenc
python3 tests/session_fixture.py python3 tests/udp_adaptive_smoke.py --two-windows
python3 tests/session_fixture.py python3 tests/udp_flow_smoke.py
WAYBRIDGE_UDP_TEST_KINDS=1,2 python3 tests/session_fixture.py python3 tests/windows_udp_smoke.py
```

自适应回归使用生产 PE/CNG 接收器、私有 Wine/Xvfb、真实合成器，以及只连接本地临时端口的代理。它丢弃首个 UDP 消息全部分片，经 TCP 修复；注入 200 ms 控制反馈延迟后恢复；要求 PNG 逐帧内容、序号一致，诊断能证明速率下降后回升并保持在上限内。现有接收器每 100 ms 请求缺失消息，200 ms 反馈延迟可能先触发修复，因此该实测按修复反馈分类；无修复时的纯延迟退避另由确定性时钟测试覆盖。它不会更改宿主路由、qdisc、网络服务或显示配置。

2026-11-17 实测：七项 UDP/控制器测试通过；生产 PE/CNG 经本地代理完整接收 180 帧，首消息 16 个分片被丢弃，10 条 ACK 注入 200 ms 延迟；80 Mbit/s 上限下记录到约 8.0 Mbit/s 的退避与随后高于 24 Mbit/s 的恢复，逐帧序号及像素一致。这些速率是该夹具中的调节轨迹，不是吞吐或延迟改进幅度。

既有 20 窗口／40 帧、600 ms 首次修复延迟下的八消息额度，及 1 Mbit/s 大帧断开后 100 ms 内停止发送回归通过；生产 PNG/H.264 的丢包、损坏、重放与 UDP 黑洞转 TCP 修复检查也通过，引用链与 alpha 保持正确。

生成证据：`build/tests/udp-adaptive.log`、`build/tests/udp-adaptive-report.json`。窗口更多时的全局顺序、修复和断开取消沿用 `udp_flow_smoke.py`；H.264 引用链和精确 alpha 沿用生产 PE 回归。

`--nvenc` 使用同一生产接收器和代理，丢弃第一个 UDP 消息后恢复正常反馈，验证降码率及分步恢复、逐帧顺序、每次切换从 IDR 开始及实际解码。`--two-windows` 检查两个活跃窗口各按同一 UDP 预算分配码率并分别解码。报告写入 `build/tests/udp-adaptive-nvenc-report.json` 和 `build/tests/udp-adaptive-nvenc-two-report.json`。

2026-11-19 本地注入丢包实测：单窗口收到 270 帧，编码目标从 6 降到 4.5 Mbit/s，再经 5.4 恢复到 6 Mbit/s。双窗口收到 360 帧，两个窗口在修复后各分得 2.25 Mbit/s，恢复后各为 3 Mbit/s。每个窗口的解码 epoch 均可从首帧独立解码。同轮 PNG 回归收到 180 帧，逐帧序号及像素一致。

同日 Windows x64 实机在有线 LAN 和 Tailscale 各收到 360 帧。媒体 UDP 分别走两条实际链路，控制和 TCP 媒体连接经 SSH 隧道。UDP 上限 5 Mbit/s、配置码率 6 Mbit/s，两次 H.264 均保持 3.75 Mbit/s。Tailscale 的七次即时修复均处于 20 ms 采样宽限内，两条链路均未触发节流退避。报告见 `build/tests/udp-native-paths-report.json`。Wi-Fi 与持续容量、公平性测量待完成。

## 后续

后续工作包括 FPS 与传输预算联动、按流交付、文件独立数据通道、真实 LAN/VPN 容量与公平性测量。
