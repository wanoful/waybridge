# Network Experience: UDP Pacing and Encoder Bitrate Coordination

Updated: 2026-11-19. Delivery feedback from authenticated UDP drives both transmit pacing and NVENC encoder targets. The wire protocol remains v3 / `udp_fec_v1`, compatible with existing Windows clients.

## Current Paths and Scope

Control, media, and audio continue using independently authenticated connections. UDP requires an explicitly configured reachable address; TCP remains the fallback when UDP is disabled, not yet bound, keepalive-expired, or when payloads exceed size or buffer limits. Per-window presentation ACKs, global UDP in-flight credits, and raw capture backpressure each serve distinct roles.

UDP allows at most 8 unacknowledged messages globally and a 64 MiB in-flight byte threshold (keeping the existing exception for a single larger message), with a 32 MiB repair cache. Transmission, repair, and per-window decoding preserve existing ordering.

## Feedback and Pacing

- `--udp-mbps` remains the ceiling rate and the initial transmit rate. Each datagram accounts for its encrypted payload plus base UDP/IP headers, including auth headers, auth tags, and XOR parity packets; link-layer and VPN encapsulation overhead is not estimated here. Pacing deadlines carry across messages so blocked scheduling does not emit a catch-up burst.
- The server records a local `Instant` right before submitting the final DATA datagram (without waiting for trailing parity packets) and computes delivery-ACK elapsed time from the matching `udp_ack` on the authenticated control channel. This includes receiver reassembly, scheduling, and feedback-path delay; it is neither network RTT nor physical input-to-display latency.
- Only messages still in flight that have reached the final DATA boundary and have not requested TCP repair train recovery; unknown, duplicate, early ACKs, and TCP fallback deliveries do not train the controller. Cumulative ACKs still release credits normally.
- On the first valid repair request arriving at least 20 ms after the final DATA boundary, the rate drops to 75% of the current rate, with a 250 ms cooldown against repeated drops. Earlier requests still trigger immediate TCP repair but are only counted as repairs, because the receiver's periodic check can race with DATA still in flight.
- An EWMA of delivery-ACK time is compared against a 10-second rolling minimum; exceeding `baseline + max(baseline / 2, 50 ms)` conservatively reduces the rate at most once per second. The margin absorbs receiver and feedback scheduling jitter; rising delay may also originate on the receiver and does not claim precise network congestion attribution.
- Under stable feedback—at least 4 valid ACKs, at least 500 ms since the last increase, and at least 1 s since any repair—the rate grows by `max(5% of current rate, 250 kbit/s)` up to the configured cap. The floor is `min(configured cap, 2 Mbit/s)`.

Periodic baseline re-anchoring prevents permanent path or receiver delay shifts from blocking recovery indefinitely. Single-packet losses recovered directly by XOR parity are not reported by the current protocol, so there is no independent loss-rate estimate in this pass. Larger frames under low pacing rates can hit the existing 100 ms receiver repair timer first and complete via TCP; that timer was not changed here. This is a bounded, conservative delivery-feedback pacer rather than a fairness or optimal-throughput guarantee on arbitrary shared networks.

## Encoder Bitrate

- An authenticated UDP session that has received a keepalive within the last 3 seconds allocates 75% of the current pacing rate across windows that encoded video within the last 2 seconds. Each window's target is capped by `--bitrate` or the client-applied bitrate limit, with an NVENC minimum target of 1 Mbit/s.
- Once an encoder has run for at least 500 ms, drops of 10% or more are applied immediately, while smaller changes wait 2 seconds. Increases require the target to stay above the current bitrate for at least 2 seconds and step up by at most `max(20% of current target, 500 kbit/s)`. TCP sessions use the configured bitrate; after a UDP keepalive expires, existing windows ramp back up to the configured bitrate using the same step limits.
- Actual bitrate switches reinitialize that window's encoder, starting a new decode epoch and IDR keyframe. Window-size caches remain reusable at matching bitrates. Reinitialization incurs setup cost while already-encoded reference frames continue delivering in order.
- Headroom is reserved for parity, headers, alpha channels, and encoder bursts. This policy controls video target bitrate; total wire traffic is still influenced by PNG frames, alpha data, audio, keyframes, and the minimum encoder floor. Resolution, FPS, and media-resource scheduling follow their own configurations.

## Diagnostics

With `--stats`, the server reports every two seconds:

- `udp_delivery_ack`: local-clock delivery-ACK elapsed time and percentiles.
- `udp_pacing_kbps`: current pacing rate and interval maximum.
- `udp_pending_messages` / `udp_pending_bytes`: latest and interval-peak in-flight message count and payload bytes.
- `udp_pacing_backoff_repair` / `udp_pacing_backoff_delay` / `udp_pacing_recovery`: counts of actual rate adjustments.
- `udp_tcp_repair_requests` / `udp_tcp_send_fallback`: counts of valid repair requests and TCP fallbacks after datagram send failure.
- `udp_repair_feedback_grace`: repair requests within 20 ms of the final DATA boundary (repaired immediately, excluded from pacing feedback).
- `video_target_kbps`: latest and interval-peak target bitrate for successfully encoded frames. NVENC startup logs also record window ID, target bitrate, UDP pacing rate, and decode epoch.

## Verification

`cargo test --locked udp::` verifies backoff cooldowns, floor/ceiling bounds, recovery, delay shifts, cross-message pacing, invalid/duplicate/early ACKs, immediate repair vs. delayed samples, and in-flight byte accounting. A slow-rate local UDP socket fixture inserts ~500 ms between the final DATA and parity packets to verify that ACKs arriving while parity is still queued already establish a valid sampling boundary, preventing low-latency receivers from permanently missing recovery feedback.

```sh
python3 tests/session_fixture.py python3 tests/udp_adaptive_smoke.py
python3 tests/session_fixture.py python3 tests/udp_adaptive_smoke.py --nvenc
python3 tests/session_fixture.py python3 tests/udp_adaptive_smoke.py --two-windows
python3 tests/session_fixture.py python3 tests/udp_flow_smoke.py
WAYBRIDGE_UDP_TEST_KINDS=1,2 python3 tests/session_fixture.py python3 tests/windows_udp_smoke.py
```

The adaptive regression runs the production PE/CNG receiver under private Wine/Xvfb, the real compositor, and a loopback-only local proxy. It drops all chunks of the first UDP message so it repairs via TCP, injects 200 ms of control feedback delay, then recovers—asserting frame-by-frame PNG pixel and sequence identity plus diagnostics showing backoff followed by recovery within the cap. Because the current receiver requests missing messages every 100 ms, a 200 ms feedback delay can trigger repair first, classifying that run as repair backoff; pure delay-only backoff without repairs is covered separately by deterministic clock tests. None of these tests modify host routing, qdiscs, network services, or display settings.

2026-11-17 test run: all 7 UDP/controller unit tests passed; the production PE/CNG receiver received 180 frames cleanly through the local proxy with 16 chunks of the first message dropped and 10 ACKs delayed by 200 ms, recording backoff to ~8.0 Mbit/s under an 80 Mbit/s cap followed by recovery above 24 Mbit/s with exact per-frame sequence and pixel match. These numbers illustrate controller behavior in the fixture rather than throughput or latency gains.

Existing regressions also pass: 20 windows / 40 frames with 600 ms initial repair delay under the 8-message credit limit, cancellation within 100 ms on disconnect during 1 Mbit/s large-frame sends, and production PNG/H.264 loss, corruption, replay, and UDP-blackhole-to-TCP repair with intact reference chains and alpha.

Artifacts are written to `build/tests/udp-adaptive.log` and `build/tests/udp-adaptive-report.json`. Global ordering with more windows, repairs, and disconnect cancellation remain covered by `udp_flow_smoke.py`, and H.264 reference chains and exact alpha remain covered by production PE regressions.

`--nvenc` uses the same production receiver and proxy, dropping the first UDP message and then restoring normal feedback to verify bitrate drop, stepped recovery, frame ordering, IDR start on each switch, and actual decoding. `--two-windows` checks two active windows splitting the same UDP budget and decoding independently. Reports are written to `build/tests/udp-adaptive-nvenc-report.json` and `build/tests/udp-adaptive-nvenc-two-report.json`.

2026-11-19 local loss-injection run: a single window received 270 frames as the encoder target stepped from 6 to 4.5 Mbit/s and recovered via 5.4 back to 6 Mbit/s. Two windows received 360 frames, splitting 2.25 Mbit/s each after repair and recovering to 3 Mbit/s each. Every decode epoch decoded cleanly from its first frame. In the same pass, the PNG regression received 180 frames with exact sequence and pixel match.

On the same day, physical Windows x64 runs over wired LAN and Tailscale each received 360 frames. Media UDP traversed each actual network path while control and TCP media used an SSH tunnel. With a 5 Mbit/s UDP cap and 6 Mbit/s configured bitrate, both H.264 runs held 3.75 Mbit/s. Seven immediate Tailscale repairs fell within the 20 ms sampling grace window, and neither link triggered pacing backoff. See `build/tests/udp-native-paths-report.json`. Wi-Fi and sustained capacity/fairness measurements remain open.

## Next Steps

Follow-up work includes coupling FPS to transport budgets, per-stream delivery, dedicated file data channel integration, and real LAN/VPN capacity and fairness measurements.
