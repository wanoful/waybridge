# Windows 麦克风回传

在控制中心的「声音」页面开启「转发麦克风」，再点「应用到当前连接」。该设置按工作站保存，默认关闭。它使用 Windows 默认输入设备，远端声音的播放开关可以独立设置。

采用 PulseAudio 或 PipeWire-Pulse 的 Linux 应用会从会话环境取得 `PULSE_SOURCE`。有设备选择器的应用可以选择 **Waybridge Microphone**。服务端在会话启动时创建输入源，切换转发设置时保持设备名称和应用的设备选择。现有会话需要使用支持该功能的合成器重新启动。服务端音频路径未启用或 `module-pipe-source` 不可用时，控制中心会显示当前会话没有麦克风输入。

麦克风通过 WASAPI 采集默认输入设备。设备失效或麦克风通道中断后，客户端会在同一连接内每两秒重试。关闭转发、启用只读、或断开连接都会释放当前采集并清理待发的样本。重连使用重新协商的权限。参见 [Microsoft WASAPI 采集说明](https://learn.microsoft.com/en-us/windows/win32/coreaudio/capturing-a-stream) 和 [PipeWire-Pulse 的 Pipe Source](https://docs.pipewire.org/page_pulse_module_pipe_source.html)。

## 传输

- v3 双方通过 `microphone_v1:true` 协商能力。控制连接发送 `microphone_enabled`（含单调递增的 `request_id` 与布尔 `enabled`）。服务端在 `microphone_state` 中返回相同的 `request_id` 与布尔 `allowed`。只读或控制连接关闭会撤销许可。
- 独立音频连接先按现有方式通过 TLS 用户入口授权，再发送带当前会话 token、`media_key`、`channel:"microphone"` 和已获准 `request_id` 的 v3 hello。匹配当前连接且同一许可下没有麦克风通道时返回 `microphone_ready`。
- 上行载荷沿用音频包种类 3 和 28 字节头。固定为 48 kHz、单声道、有符号 PCM16 LE、480 样本（10 ms）。每个入站块独立检查尺寸、参数和序号。回执为种类 3 加八字节大端序号。Windows 保留最多四块待发和八块待确认。Linux 通过仅本 UID 可访问的 FIFO 提供独立虚拟输入，缓冲满时丢弃旧数据。

## 验证

```sh
cargo test --locked microphone::tests
python3 tests/session_fixture.py python3 tests/microphone_smoke.py
python3 tests/windows_microphone_smoke.py
```

Linux 测试把受控信号送入真实的 PipeWire-Pulse 输入，再用 `parec` 录取。测试也覆盖未经许可的连接、错误凭据、重复通道、过期请求、超长参数，以及只读、停用、掉线和退出清理。Windows 测试使用实际 x64 worker，在隔离的 Wine / PipeWire 中通过 WASAPI 采集合成的默认麦克风。额外的 Schannel/rustls 实测也经过服务入口成功录到同一信号。物理 Windows 麦克风和具体会议应用的验收仍需在设备上进行。
