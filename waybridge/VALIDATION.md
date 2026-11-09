# Validation — 2026-11-09

The source integrates all 14 feature implementations. Results below distinguish the feature/media checkpoints, the desktop+media combined session, the final files-inclusive rerun, and earlier physical-device work. Building an ARM64 executable or running a PE under Wine is not physical WoA validation.

## Current integration status

| Checkpoint | Result |
|---|---|
| Media integration (`2ca36dd`) | Passed Rust fmt, workspace strict Clippy, 7 tests, Linux build, x64/ARM64 clients and UDP probes; media/UDP/audio checks below |
| Desktop + media integration | Passed strict Clippy, 12 Rust tests (11 host + 1 codec), x64/ARM64 builds and combined-session regression |
| Final integration including all file/tree/virtual-source changes | Pending final root rerun; do not infer it from the earlier checkpoints |
| Physical Windows/WoA execution of the complete integrated client | Pending |

The combined `python tests/session_integration_smoke.py` passed at the desktop+media checkpoint with **X11 + audio + UDP + CUDA + HDR policy enabled together**. It authenticates the independent PCM socket, keeps control ping responsive, displays native Wayland and GLX/X11 windows to a PNG-only peer, and verifies safe ordinary-TCP fallback when UDP was offered but never authenticated. SIGTERM exits successfully, removes the owned sink and private X socket, and leaves the host default sink unchanged. It does not claim actual HDR output, UDP pixel delivery, file transfers, all IME/desktop interactions, or every feature in one physical Windows session. Final execution after the file merge is pending.

Cross-system integration preserves a single shutdown request path for Xwayland/audio cleanup, separate listener options/capabilities, per-window presentation credits, global UDP credits, independent audio credits, and PNG-to-video fresh epochs. CUDA source registration must be released before the backing GL target on unmap/destruction; the final lifecycle audit/regression is pending alongside final validation.

## Media integration: executed results

- `cargo fmt --all -- --check`, `cargo clippy --workspace --all-targets --locked -- -D warnings`, `cargo test --workspace --locked` and Linux debug build passed. The vendored Smithay dependency emits three existing warnings; the project strict checks pass. Both Windows client architectures and the standalone UDP probes build; PE inspection confirms x86-64 and ARM64 respectively.
- Extended `tests/windows_udp_smoke.py` uses the production Windows CNG/UDP receiver under a private Wine/Xvfb desktop. PNG kind1, H.264 kind2, HEVC kind4, AV1 kind5, HEVC Main10 kind7 and AV1 Main10 kind8 each delivered 24 strictly ordered frames under the applicable loss/corruption/replay/reordering scenarios. A blackholed H.264 stream recovered through TCP repair. Actual FFmpeg decoding verified SDR color and exact independent alpha; Main10 verified ten-bit PQ/BT.2020 and raw-half alpha16. The HDR fixture's inherited-stdin EOF was corrected before the HDR rerun; it was a harness issue, not a production media fix.
- `tests/udp_flow_smoke.py` passed actual CNG head repair with all 16 future slots occupied; 20 real Wayland windows/40 ordered PNG frames with a 600 ms first-NACK delay enforced the global eight-message gate and resumed after ACK. A noisy 1 Mbit/s transfer stopped on TCP disconnect, with zero datagrams after a 100 ms grace over two seconds. The 64 MiB credit accounting, repair priority and per-window presentation-credit separation remain in the implementation.
- `tests/gpu_smoke.py --binary target/debug/waybridge` passed real GL renderbuffer/CUDA registration, every-pixel unpremultiply/flip/alpha oracle, opaque alpha-readback omission, actual native H.264/HEVC/AV1 GPU inputs, and an injected fourth-frame GPU encode rejection. The current PNG remains exact, a fresh CPU capture occurs without application repaint, and CPU NVENC resumes with a new independent epoch.
- `tests/hdr_video_smoke.py` passed the real FP16/CUDA P010 luma/chroma/alpha oracle, both Main10 codecs with 15-frame IDR/P chains, two-credit pause/resume, odd-size/cache epochs, wrong-codec rejection, SDR monitor fallback and PNG-to-HDR independent-key recovery. The integration rerun used 256×256/257×257 fixtures, producing 149/150 decoded distinct luma codes; those widths cannot prove more than 256 levels. The earlier HDR checkpoint's separate 960×640 runs retained 277 levels. Non-negotiated HDR and injected HDR encode failure/accurate tone-map recovery passed.
- `tests/audio_smoke.py` passed private-session capture, unrelated-sink isolation, authentication/duplicate/legacy gating, four outstanding block credits, future-ACK rejection, independent controls, audio-only disconnection and cleanup. `tests/windows_audio_smoke.py` ran the actual client/WASAPI into an explicitly selected private null endpoint and recovered stereo peaks L=12000/R=6000. No default output device was changed and no generated sound was routed to the user's normal endpoint.
- `tests/windows_codecs_smoke.py` passed actual production-client rejection of unadvertised SDR/HDR kinds 4/5/7/8, codec-bound fallback and subsequent correct PNG painting. Wine advertises no HEVC/AV1 hardware path; the test deliberately does not claim one.
- `WAYBRIDGE_TEST_CODEC=av1-nvenc python tests/windows_smoke.py --udp` passed the actual native viewer's encrypted-UDP receive/paint path, native resize, Unicode clipboard, bidirectional file clipboard, OLE incoming drop and single decoration ownership. Wine capability-gated video to PNG, so this is not real Windows AV1 decode coverage.

Media integration logs were written to `/tmp/waybridge-merge-media-*.log`; detailed artifacts are under that integration worktree's `waybridge/build/tests/` and `build/diagnostics/`. Artifacts/logs are not guaranteed to be versioned or copied to the root build directory. Tests used private ports/sockets/prefixes/Xvfb/null sinks and did not restart the existing 49090 service, change display settings or terminate unrelated GPU processes.

## Desktop and files: established feature checks

These are results established by the feature/integration checkpoints. The final aggregate rerun is tracked above; the table is not a claim that each test was just rerun against the final root tree.

| Scope | Tested behavior and entry points |
|---|---|
| Base windows/input/DPI/popups | `smoke.py`, `decorations_smoke.py`, `qt_smoke.py`, `dolphin_smoke.py`, `ghostty_smoke.py`, `windows_smoke.py`: real Wayland/Qt/GTK, 175%→125% scaling, exact alpha, surface-local hit testing, independent overflowing/nested popups, native ownership, clipboard/files, disconnect/reconnect |
| Windows IME | `ime_smoke.py`, `windows_ime_keys_smoke.py`, Ghostty `--ime`: text-input-v3 preedit/commit/cancel, Chinese/emoji, focus generation, native Unicode injection, shortcuts/Ctrl+Space and modifier recovery. This is not execution of Microsoft Pinyin's real candidate engine |
| Cursor | `cursor_smoke.py`, `windows_cursor_smoke.py`: named/hidden/custom/animated cursors, DPI/hotspots, malformed/oversize/stale rejection, animation without main-window damage, non-client sizing cursors and replacement resource bounds |
| Application icons | `app_icon_smoke.py`, `windows_app_icon_smoke.py`: XDG PNG/SVG resolution, identity/revision safety, fallback, per-DPI native HICONs and cleanup |
| Window semantics | `window_semantics_smoke.py`, `windows_semantics_smoke.py`: genuine modal hint versus parent, owner lifetime, min/max, fullscreen epoch/restore, unmap/remap, suspended hidden windows and input-region operations. HTTRANSPARENT covers only the same UI thread |
| Relative pointer | `relative_smoke.py`, `windows_relative_smoke.py`: real locked-pointer/Raw Input handshake, generation guards, clipping, focus/menu/native failure/emergency cleanup and no immediate retrapping |
| X11 | `x11_smoke.py`, `x11_clipboard_smoke.py`: GLX animation, 175% Qt/XCB, alpha, transient/override-redirect/nested menus and Tooltip, parent move/reconnect, native Wayland coexistence, UTF-8/PNG/URI/INCR clipboard and owned socket/lock cleanup |
| Linux native drag-out | `drag_out_smoke.py`, `windows_drag_out_smoke.py`: real Wayland grab→native DoDragDrop, real-button handoff, COPY, lazy FILECONTENTS/IStream reads/Seek/Clone, synchronous/asynchronous completion, cancellation/disconnect; source files and clipboard preserved |
| Directory transfer | `tree_smoke.py`, `windows_drag_out_smoke.py --tree`: four clipboard/drop directions, manifests, nested/empty directories, Unicode/zero-length bytes, source identity/path limits, collision/link/reparse rejection and incomplete staging cleanup |
| Windows virtual-source import | `windows_virtual_smoke.py`: cross-process STA providers without CF_HDROP, FILEDESCRIPTORW/A and FILECONTENTS streams/HGLOBAL, descriptor indices, synthesized parents, sequential/Clone/Seek/Stat, sync/async completion, errors/E_PENDING, cancellation and disconnect. Successful Linux paths are published only after full commit |

Smithay patches remain local under `vendor/smithay`, with sources/licenses in `vendor/smithay/PATCHES.md`; no system Cargo cache or host compositor is modified. New window semantics/cursors/relative input/IME/OLE combinations need continued physical-device testing even when the relevant Linux/PE API fixture passes.

## Earlier physical Windows ARM64 baseline

An earlier native MSVC ARM64 build ran on Snapdragon 8cx Gen 3 / Adreno 690 (driver 30.0.4035.6900). Removing H.264 filler NAL12 eliminated the final-picture hold: all 120 native 1280×720 access units produced sequenced GPU textures, including the last frame. The decoder probe's mean transform time was 1.139 ms, with a 22.910 ms cold maximum. WIC tests verified exact alpha/premultiplied pixels and faster image/alpha decoding than the earlier GDI+ path.

That baseline full client retained hardware H.264 on static translucent and animated fixtures. Bounded decoder/encoder caches were checked over repeated resize epochs; the native GPU hash probe matched fresh decoders on all 33 frames. Earlier warm maximize/restore `resize_to_present` samples averaged about 98.72 ms with the cached server versus 284.35 ms previously, while a first cold large size still took about 305 ms. These are fixture/stage/presentation-submission timings, not input-to-photon or Moonlight comparisons. Historical artifacts include `build/diagnostics/decode-native-120-fixed.log`, `image-bench.log`, `resize-ui-results.json` and adjacent client logs when retained.

This physical evidence applies to that earlier H.264/PNG/resize baseline. It does not establish physical execution of the final integrated binary, the Linux cross-built ARM64 executable, HEVC/AV1/Main10 decoders, HDR displays or Microsoft Pinyin. Linux LLVM-MinGW cross-builds confirm architecture and Windows system imports, not runtime hardware compatibility.

## Remaining device and compatibility validation

- Real Windows/WoA HEVC/AV1 and Main10 MFT availability, correct GPU planes/textures, DirectComposition FP16 HDR presentation, active HDR↔SDR transitions, per-pixel alpha and multi-monitor DPI. The production HDR shader/WIC numerical checks under private software D3D11 verify arithmetic only; WARP is not advertised as HDR hardware.
- Microsoft Pinyin candidate positioning, selection, composition/cancel and Win+Space/shortcut switching in Qt/GTK on the physical device. XIM and surrounding-text reconversion are not implemented.
- Windows Explorer/desktop and third-party OLE targets; Outlook/archive/cloud providers, real COM cancellation and provider hangs. No FUSE, IStorage, unknown-size HGLOBAL, move semantics or general third-party compatibility claim. An unresponsive provider that ignores OS cancellation may require source-process recovery.
- Cross-process transparent click-through and KDE blur are not implemented. Relative-only/unlocked motion, confined-pointer and popup locks remain unsupported. X11 lacks PRIMARY, XDND, XIM, full EWMH and automatic crash restart.
- General multi-GPU/driver/application coverage, congestion adaptation, endpoint roaming, Opus, microphone, strict A/V synchronization and device hot-switching remain outside this implementation. CUDA retains GPU copies/synchronous waits and independent alpha readback; it is not an entirely zero-copy pipeline.
- Actual input-to-display latency, high-refresh-rate/bandwidth behavior and comparative latency against Moonlight remain unmeasured. Sunshine/Moonlight source trees are unchanged references; their wire protocol is not used.
