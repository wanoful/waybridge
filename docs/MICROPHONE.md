# Windows Microphone Forwarding

Enable **Forward microphone** on the Control Center's **Audio** page and click **Apply to current connection**. This setting is saved per workstation and defaults to off. It captures from the default Windows input device; remote audio playback can be toggled independently.

Linux applications using PulseAudio or PipeWire-Pulse receive `PULSE_SOURCE` from the session environment. Applications with a device selector can choose **Waybridge Microphone**. The server creates the input source at session startup and keeps the device name and application selection intact when forwarding is toggled. Existing sessions must be restarted with a compositor that supports this feature. If server audio is disabled or `module-pipe-source` is unavailable, the Control Center reports that the current session has no microphone input.

Microphone capture uses WASAPI on the default input device. If the device becomes invalid or the microphone channel drops, the client retries every two seconds within the same connection. Disabling forwarding, enabling read-only mode, or disconnecting releases the capture device and clears queued samples. Reconnecting uses freshly negotiated permissions. See [Microsoft WASAPI stream capture](https://learn.microsoft.com/en-us/windows/win32/coreaudio/capturing-a-stream) and [PipeWire-Pulse Pipe Source](https://docs.pipewire.org/page_pulse_module_pipe_source.html).

## Transport

- v3 peers negotiate support via `microphone_v1:true`. The control connection sends `microphone_enabled` (containing a monotonically increasing `request_id` and boolean `enabled`). The server replies with `microphone_state` carrying the same `request_id` and boolean `allowed`. Read-only mode or closing the control connection revokes permission.
- The dedicated audio connection first authenticates through the TLS user entry point as usual, then sends a v3 `hello` with the current session token, `media_key`, `channel:"microphone"`, and the approved `request_id`. If it matches the active connection and no microphone channel is attached for that grant, the server replies `microphone_ready`.
- Uplink payloads reuse audio frame kind `3` and its 28-byte header, fixed to 48 kHz, mono, signed PCM16 LE, 480 samples (10 ms). Every inbound chunk is independently validated for size, parameters, and sequence number. Acknowledgements use kind `3` plus an 8-byte big-endian sequence number. Windows buffers at most 4 chunks pending send and 8 chunks awaiting acknowledgement. Linux exposes the virtual input through a UID-private FIFO and drops old data when the buffer fills.

## Verification

```sh
cargo test --locked microphone::tests
python3 tests/session_fixture.py python3 tests/microphone_smoke.py
python3 tests/windows_microphone_smoke.py
```

The Linux tests feed a controlled signal into a real PipeWire-Pulse input and record it back with `parec`. They also cover unauthorized connections, bad credentials, duplicate channels, stale requests, oversized parameters, read-only mode, disabling, disconnects, and shutdown cleanup. The Windows test runs the actual x64 worker under isolated Wine / PipeWire, capturing a synthetic default microphone via WASAPI. Additional Schannel/rustls tests confirm recording the same signal through the TLS user service entry point. Physical Windows microphone hardware and real conferencing application validation still need on-device testing.
