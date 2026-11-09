# Validation — 2026-11-09

Optional encrypted UDP/FEC:

- Independent review reproduced and fixed two lifecycle/flow bugs. A real PE receiver preloaded with future messages 2–17 used to reject head repair 1; it now delivers exactly 1…17. At 1 Mbit/s, a real noisy Wayland frame used to continue emitting 228 old-session UDP datagrams during two seconds after closing both TCP channels; it now emits zero (100 ms grace, followed by a full two-second observation).
- `tests/udp_flow_smoke.py` launches **20 actual Wayland windows** and receives 40 correct PNG frames with 20 unique IDs. It discards the entire first UDP message and delays the first reliable NACK by 600 ms without blocking UDP reception. Before that repair, observed message IDs are exactly 1…8; then repair priority and transport ACK wakes resume ordered progress. This covers the global count gate rather than relying only on per-window credits. The test includes the full head-repair capacity case and disconnect measurement above, using production CNG/Rust paths and private sockets/Xvfb. Requires OpenSSL development files for the independent crypto fixture, without Python crypto packages.

- A real x64 PE receiver uses the production `UdpMedia` implementation and Windows CNG AES-256-GCM against Rust/ring. 24 PNG frames are exact and ordered both without faults and through a private UDP proxy injecting drops, corrupted tags, duplicates and reordering. Both actual XOR reconstruction and TCP frame repair are exercised. No host-network settings or running services are changed.
- Real NVENC H.264 sends 24 IDR/P access units through the same fault injector; complete FFmpeg decoding verifies the reference chain, colors and exact independent alpha 0/128/255. The harness also blackholes UDP mid-stream and expects continued TCP repair delivery. Delivered sequences are strictly increasing without duplicates; all requested access units decode successfully. Artifacts are `build/tests/udp-*.bin`, `windows-udp.log`.
- Full native Windows client under Wine/Xvfb passes real Qt at 175%: input, overflowing/nested popups, native owner movement, unchanged parent geometry and owned dialogs while carrying actual fault-injected UDP shards. Wine exercises the GDI fallback; physical WoA UDP/Wi-Fi latency remains unmeasured.
- Rust authentication tests reject wrong keys, tampered tags, nonce zero, replays and authenticated endpoint rebinding; new sessions and directions have fresh independent keys. Existing core/Qt/Dolphin/Ghostty regressions pass with default TCP. Reassembly/cache/repair queues are bounded. This is fixed pacing + 8+1 XOR + TCP repair, not Moonlight's full congestion/packet recovery stack.

Maximize/restore resize checks:

- The actual native FFmpeg/NVENC adapter measured roughly 101 ms to create a session and 70 ms to destroy one when alternating 960×640 / 2240×1400. Keeping both sizes and forcing independent SPS/PPS+IDR on reuse reduced warm first-frame encoding to about 3.1 ms. These adapter timings exclude capture, alpha and network processing.
- The server now keeps a global inactive cache of at most two encoder sessions and 4 Mi raw pixels, invalidated on window/session end. Reuse is restricted to the same peer/window/resolution, increments the epoch and forces an IDR. Cached sessions are reclaimed before retrying a failed new allocation. Cold initialization refreshes the selected raw image and skips obsolete intermediate dimensions before encoding. Presentation credits remain two per window. Incoming control messages retain FIFO ordering; raw images continue to coalesce per window.
- `tests/resize_smoke.py` exercises ten actual compositor/application size switches, independent IDRs, intact subsequent P references and exact alpha, extra sizes that evict the cache, and a burst ending at a distinct final size. Same-host loopback time to receive the matching first packet (including the test viewer's alpha parsing), excluding the first cold switch: average 208.46 ms before → 33.79 ms after. Warm 960×640 mean: 185.17 → 17.45 ms; warm 2240×1400 mean: 237.57 → 54.21 ms. First new large size still measured 208.15 ms. Artifacts: `build/diagnostics/resize-latency-before.json`, `resize-latency-after.json` and `.log`.
- Added server `encoder_init`, `encoder_reuse`, `resize_first_encoded` and `resize_first_sent` diagnostics. Resize times start in the server resize handler and end when the first matching pixel-size frame is encoded/sent. They do not include final Windows display.
- Reproducible native fixture generator: `tests/generate_resize_stream.c`, compiled with `native/nvenc.c` and libavcodec/libavutil. `build/diagnostics/resize.h264` + `resize.json` contain 33 access units and ten size transitions, three frames per epoch. Each transition begins with SPS/PPS+IDR. `native-resize-cached.h264`/`.json` exercise forced IDRs from reused actual encoders.
- On the physical Windows ARM64 device at 175% DPI, the new client keeps a bounded decoder pool, flushes/restarts on reuse, retains the old picture during resize, and sends maximize/restore sizes immediately while coalescing interactive drag updates. The resize probe verifies all 33 GPU NV12 frame hashes against fresh decoders, including the cached-encoder fixture. Mean first-frame decoder/presentation processing in the alternating fixture: 23.97 → 7.74 ms, including initial cold startup.
- Real Windows maximize/restore, same new client with old versus new isolated servers: three warm old-server `resize_to_present` samples average 284.35 ms; five warm new-server samples average 98.72 ms. First new maximized size still measured 305.33 ms. Transparent red/half-alpha green/clear regions remained visually correct; six transitions retained hardware decoding and empty stderr. These are resize-to-presentation-submission measurements, not physical input-to-photon latency. Artifact: `build/diagnostics/resize-ui-results.json` and adjacent Windows logs.
- Rust formatting, strict Clippy, four unit tests, Linux protocol/input/clipboard/file smoke, real Qt 175%/125% DPI and input checks, NVENC reference/alpha/credit/fallback tests pass. ARM64 LLVM-MinGW and x64 GNU builds of the updated client/resize probe succeed. Wine/Xvfb CSD maximize/restore/minimize/close and Qt overflow/nested-popup regression checks pass.

Windows ARM64 decoding investigation and fixes:

- Physical device: Snapdragon 8cx Gen 3 / Adreno 690, driver 30.0.4035.6900; native MSVC ARM64 build. The old project stream produced five GPU output samples from six inputs, holding the final picture. Removing trailing H.264 filler_data (NAL type 12) produced one correctly sequenced texture per input. AUD alone and the low-latency property fix alone did not resolve the hold.
- Server NVENC now emits AUD and uses FFmpeg `filter_units` to remove only type 12. Explicit `cbr_padding=0` still produced filler with the installed FFmpeg/driver. CBR/VBV configuration and the two-frame cumulative presentation-credit limit remain unchanged. All 120 decoded RGBA frame hashes match the unfiltered stream. Initial IDR decoding requires neither a second input nor EOF/drain.
- The real server hardware test verifies AUD/no-filler access units, IDR/P reference continuity, exact independent alpha, odd-size epoch reset, two-credit pause/resume, responsive control traffic and live PNG fallback. Debug and Release server binaries are built.
- The physical Windows decoder probe consumes all 120 native 1280×720 access units and returns 120 correctly sequenced GPU textures, including the final picture: transform average 1.139 ms, maximum 22.910 ms (first input). Artifact: `build/diagnostics/decode-native-120-fixed.log`.
- `CODECAPI_AVLowLatencyMode` with `VT_BOOL` returned `E_INVALIDARG`; `VT_UI4=1` succeeded and read back as 1. The client checks the result and also strips filler for compatibility with earlier v3 servers.
- WIC directly produces premultiplied BGRA and 8-bit grayscale alpha. On the physical ARM64 device, same-process/same-image 1280×720 comparison: RGBA GDI+ 18.572 ms → WIC 1.805 ms; alpha GDI+ 5.715 ms → WIC 0.262 ms. Exact alpha, premultiplied colors, truncated-image and dimension rejection pass. Artifact: `build/diagnostics/image-bench.log`.
- Full physical Windows client: a static translucent fixture stayed connected over 30 seconds, followed by about 20 seconds of `weston-simple-shm` animation, with no PNG fallback or stderr errors. Steady animation intervals report H.264 processing around 0.67–0.82 ms, decode queue 0.05–0.11 ms, presentation submission 0.93–1.07 ms. Artifacts: `build/diagnostics/client-live.log`, `client-live.err`. These are stage timings, not input-to-photon latency.
- Updated ARM64 LLVM-MinGW and x64 GNU MinGW clients and both diagnostic probes build successfully. Isolated Wine/Xvfb regression checks pass for WIC image fidelity, native pixels/resize, clipboard/files/OLE and actual Qt 175% input, independent nested/overflow menus and owned dialogs. Wine performance is not substituted for physical-device measurements.

Windows on ARM build checks:

- Native ARM64 Release client built with LLVM-MinGW Clang 22.1.8 using `scripts/build-windows-arm64.sh`; output: `build/windows-arm64/waybridge-client.exe`.
- PE headers report `COFF-ARM64`, `aarch64`, `IMAGE_FILE_MACHINE_ARM64 (0xAA64)`. Imports contain only Windows system DLLs; C++ runtime is statically linked.
- Shared CMake configuration explicitly selects Windows 10 APIs and handles GNU versus LLVM runtime flags. The x64 Release client also rebuilds successfully; its `--version` and `--check-shaders` checks pass under isolated Wine/Xvfb.
- Physical ARM64 execution and hardware decode were validated separately with the native MSVC build above; the Linux host verifies LLVM-MinGW cross-compilation and PE imports. Actual input-to-display latency remains unmeasured.

0.3.0 popup/media checks:

- Real Qt popups extend outside the parent while its logical and pixel dimensions remain unchanged. Nested menus have separate IDs, native owner relationships, input routing and lifetime.
- The Windows executable in isolated Wine/Xvfb confirms `WS_POPUP`, no `WS_CHILD`, no activation/taskbar entry, outside-parent bounds, root mouse capture, actual mouse clicks, parent movement, nested-menu action and unchanged parent client dimensions at 175%.
- Actual Ghostty 1.4.0 GTK/OpenGL at 175% creates an independent overflowing context menu, retains terminal geometry, and dismisses with Escape. Artifact: `build/tests/ghostty-menu.png`.
- Actual NVIDIA NVENC H.264 encodes IDR and P frames. FFmpeg decodes the complete reference chain, checking color channels and alpha 0/128/255 from an independent lossless mask. Odd 641×481 input uses 642×482 coded dimensions, resets epoch and starts with an IDR. Two unacknowledged frames stop media encoding; after acknowledgement, the retained final raw update resumes without breaking P-frame references. Live per-window PNG fallback passes. Artifacts: `build/tests/hardware.h264`, `build/tests/hardware-alpha.png`.
- The media socket requires the current session's random capability and normal token. Wrong media keys are rejected; control ping and existing clipboard/file traffic use the independent control connection.
- Windows H.264 vertex/pixel shaders compile through the actual PE executable in Wine. MF is dynamically loaded so PNG remains available without Media Foundation.
- Default frame schedule is 60 FPS with fixed deadlines; opaque frames skip reverse-alpha conversion. Optional timing counters use each machine's own monotonic clock.

**Still unmeasured:** actual input-to-display latency and comparative latency against Moonlight. Physical hardware decode and full-client presentation are covered by the ARM64 checks above; Wine exercises the PNG/GDI fallback and Linux tests exercise the real NVENC encoder. Synchronous readback and CPU upload to NVENC remain. UDP/FEC, GPU-to-encoder interop, AV1/HEVC and HDR are not implemented.

Earlier 0.2.0 DPI/rendering/input checks:

- Actual Qt Widgets negotiates 175% fractional scale and produces 1680×1120 pixels for 960×640 logical content; live change to 125% produces 1000×625 pixels for 800×500 content. Another window retains its own 175% scale.
- Real translucent Qt surfaces retain exact alpha values 0, 128 and 255 through GLES readback and the PNG transport. Screenshot: `build/tests/alpha.png`.
- Real Qt text input, stale focus/leave handling, pointer/keyboard-triggered popup grabs, Escape dismissal/focus restoration and parented modal-dialog input pass. Popup dismissal now destroys the independent native menu instead of repainting the parent canvas.
- The actual Windows executable sends native frame mouse/keyboard events to Qt at 175%, activates menu items and operates an owned dialog in Wine. Wine explicitly uses the buffered GDI fallback.
- Actual Dolphin in an isolated D-Bus session accepts Ctrl+L, clipboard paste and Enter, navigating to the requested directory at 175%. Its config/cache/data directories are temporary.
- PNG encoding/decoding moved off event threads, with bounded latest-frame queues; surface commits mark only the owning toplevel dirty.
- Both version strings are 0.2.0; wire protocol is v2 and requires updating both ends.

**Not yet verified on physical Windows:** D3D11/DirectComposition presentation, local-desktop per-pixel alpha, and real multi-monitor DPI transitions. The GPU path was cross-compiled, while Wine exercised the GDI fallback. Transparent input-region pass-through and blur are not implemented. CPU PNG and synchronous GPU readback remain performance limits.

Earlier decoration regression checks:

- Standard xdg-decoration negotiation selects a single native Windows frame for supported clients.
- A real client-decorated Wayland fixture retains one header, crops 12px invisible margins, and preserves exact mouse coordinates and true resize geometry.
- Wine checks verify a CSD window has no Windows caption/non-client border, keeps resize hit testing, and its own header can move, minimize, maximize, restore and close the native window. Maximized client area matches monitor work area.
- Actual `weston-terminal` was opened in the native viewer in isolated Wine/Xvfb. Its outer Windows caption is absent and its own title bar successfully moves the native window. Screenshot: `build/tests/weston-terminal-window.png`.
- Existing clipboard/file/input/window tests remain part of the regression suite.

Host: Arch Linux, kernel 7.2.11, NVIDIA RTX 5070 Ti and AMD integrated graphics. The headless EGL renderer selected NVIDIA `/dev/dri/renderD129`. Host desktop remained separate; tests used private Wayland sockets.

Passed:

- `cargo fmt --check`, `cargo clippy --all-targets -- -D warnings`, `cargo test` (four tests).
- `tests/smoke.py`: authentication rejection, two independent Wayland windows, exact color/orientation checks, real 640×480 configure, surface-local mouse coordinates, keyboard/button delivery, release on disconnect, persistent-window reconnect, bidirectional Unicode and exact PNG bytes, 180 kB chunked file transfers in both directions, URI/path translation, traversal rejection, negotiated Wayland Copy drop, closing one window without ending the other.
- `tests/windows_smoke.py`: compiled PE executable running in an isolated Wine/Xvfb environment; native Win32 window pixel reads, resize propagated to the real Wayland client, Windows↔Linux Unicode clipboard, native CF_HDROP files in both directions, native OLE drag source to the viewer and then Wayland, with exact destination file bytes.
- A real `weston-simple-egl` client rendered a colored triangle through Linux DMA-BUF feedback/import and the headless renderer; a frame was visually inspected. Captured frame: `build/tests/egl-frame.png`.
- Linux optimized build and MinGW-w64 Windows x86-64 build. Windows executable imports system DLLs, without third-party runtime DLLs.

The earlier baseline checks predate the physical Windows ARM64 checks above. No gaming latency, bandwidth, high-refresh-rate, HDR, audio, browser/CAD suite, X11, full IME, or general multi-GPU compatibility claim is made. Native PNG/bitmap format conversion on Windows still needs broader application coverage; exact PNG transfer at the Wayland/protocol boundary was tested. Reverse native drag-out is not implemented.

Sunshine and Moonlight source trees are unmodified and are not yet used by the running prototype.
