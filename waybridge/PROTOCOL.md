# Waybridge protocol v3

All TCP packets begin with a big-endian u32 length counting the kind byte and body, in `1..=75497472`. Unsupported or malformed framing closes the connection. Binary channels are distinct:

| Kind | Body |
|---|---|
| 0 | UTF-8 JSON object |
| 1 | PNG RGBA frame, 36-byte geometry header |
| 2 | H.264 SDR + optional lossless alpha, 60-byte header |
| 3 | PCM audio, independent audio socket and credits |
| 4 / 5 | HEVC / AV1 SDR + optional lossless alpha, 60-byte header |
| 6 | Ordered UDP TCP-repair container, wrapping original kinds 1/2/4/5/7/8 |
| 7 / 8 | Experimental HEVC / AV1 Main10 PQ + optional raw-half alpha16 PNG |

First client message: `{"type":"hello","version":3,"token":"...","dpi":168,"work_width":3840,"work_height":2080}`. Authentication and exclusive viewer ownership precede window/media announcements. The server replies with `codec:"mixed"`, `codecs`, `features` and a random per-session `media_key`. Both ends must use v3. DPI defaults to 96; work dimensions are physical monitor work-area pixels. This TCP protocol is not intrinsically encrypted; use the local listener through SSH or another trusted protected transport. UDP media has its own AEAD, whose keys travel on the authenticated control connection.

Optional client hello fields are `split_media`, `frame_ack`, `cursor_v1`, `window_icon_v1`, `window_semantics_v1`, `pointer_lock`, `file_drop_out`, `file_tree`, `audio_pcm`, `udp_fec_v1`, `codecs`, `hdr` and `color_spaces`. Enable extensions only as described in their sections; the server advertises `ime_v3` for local text input. Missing fields retain compatible v3 behavior. `features` lists implemented extensions, not proof that a GPU/decoder/device is usable.

A viewer advertises `png_rgba` unconditionally. It adds `h264_alpha`, `hevc_alpha` or `av1_alpha` only after an actual synchronous D3D11-aware system MFT decodes a small access unit to a timestamp-matching NV12 GPU texture. HEVC/AV1 use dynamically loaded `MFTEnumEx`; missing enumeration/decoder/GPU support keeps PNG available. Main10 has additional checks below. The server advertises implemented formats; driver/size/resource failures may still select PNG. `auto` continues to choose H.264, not whichever codec appears newest.

## Media connection

A viewer requesting `split_media:true` opens a second TCP connection to the same address and port. Authenticate it with `{"type":"hello","version":3,"token":"...","channel":"media","media_key":"..."}`. Only the current viewer's random key is accepted, and only one media socket attaches. The server replies `media_ready`, then sends frame kinds 1/2/4/5/7/8 over this socket. Input, window control, clipboard and file messages stay on the control socket. Socket failure closes the session; reconnect restores live windows. Existing SSH forwarding carries both connections. Test/simple viewers may omit `split_media` to multiplex on one socket.

## Optional UDP/FEC v1

Server `--udp-listen` plus client hello `udp_fec_v1:true`, `split_media:true`, `frame_ack:true` are all required. The server hello contains `udp_fec_v1:{version:1,session:"<u64 decimal>",port,tx_key:"<base64 32 bytes>",rx_key:"<base64 32 bytes>",chunk:1024,group:8}` or null. Keys are named from the server's perspective and **must travel over SSH** with the control connection; the default TCP protocol is not intrinsically encrypted. Client uses an explicitly configured directly reachable UDP endpoint, independently of its tunneled TCP address. Fresh keys/session are generated on reconnect. No audio negotiation is changed.

Datagrams are at most 1080 bytes: clear header `"WBU1" + u64 session + u64 counter` (big-endian), AES-256-GCM ciphertext, 16-byte GCM tag. The whole 20-byte clear header is AAD. The 12-byte nonce is four zero bytes followed by that direction's nonzero u64 counter; every sealed datagram consumes a new counter, including parity and keepalives. Keys are distinct per direction; counter overflow fails rather than reuses a nonce. Client rejects authenticated duplicates and counters older than a 4096-packet replay window. Client keepalives are a single encrypted zero byte; server accepts increasing counters from the first authenticated endpoint only. No unauthenticated amplification or automatic NAT rebinding.

Server plaintext shard header is `u8 type` (1=data,2=XOR parity), `u8 original_kind` (1=PNG,2=H264,4=HEVC,5=AV1,7=HEVC Main10,8=AV1 Main10), `u16 index`, `u16 data_count`, `u16 reserved=0`, `u32 total_body_bytes`, `u64 message` (all multibyte fields big-endian). It is followed by 1024 data bytes except a shorter final data shard. Parity has exactly 1024 bytes and index is the group's first data index (multiple of 8); it XORs up to 8 zero-padded data shards. Whole bodies include the original frame header. Geometry/epoch/sequence/alpha formats are unchanged. UDP frame bodies are capped at 8 MiB; count and payload lengths are validated before allocation.

The media TCP writer first emits `{"type":"udp_start"}` after all previous ordinary TCP frames. The receiver buffers UDP messages until this barrier, then delivers increasing global message IDs from 1. It responds over authenticated control with `udp_ack {message}` after queueing a complete frame. These transport ACKs release the bounded repair cache, **not** video presentation credits. `udp_retry {message}` requests the missing next message (100 ms client interval, server repair rate limited to 50 ms/message). Binary TCP kind **6** contains `u64 message + u8 original_kind + original_frame_body`. This also carries new frames when UDP is stale (>3 seconds), a frame is oversized or the cache is full; it follows the same ordered delivery and duplicate suppression. Repeated repairs reuse the same message ID, never a new encoded reference frame.

Global transport credits: at most 8 unacknowledged messages and 64 MiB of original frame bodies, including TCP-fallback messages. A single larger body is allowed only with no other outstanding messages. The media writer checks this budget before removing a new frame from its queue; repair packets bypass that gate and retain priority. A valid cumulative transport ACK frees pending-message and byte credits, then wakes the writer under its queue mutex. This is independent of per-window presentation credits.

Cache: at most 16 messages/32 MiB; pending repair queue at most 4. Receiver: at most 16 messages/96 MiB and a 32-ID lookahead. Over-budget UDP fragments wait for TCP repair. A head TCP repair (`message==next`) delivers directly before draining completed future messages, so a full future-message table cannot reject the repair needed to make progress. The sender's tighter global credits keep valid future-message traffic within receiver budgets. Session close and replacement cancel an in-progress paced body between datagrams, without sending that cancelled body as a TCP fallback. Invalid/unnegotiated peers retain ordinary TCP. No congestion adaptation, FEC negotiation, per-window independent delivery, selective retransmission, IDR-on-loss or endpoint roaming is implemented.

Implementation API references: [ring AEAD nonce requirements](https://docs.rs/ring/0.17.14/ring/aead/struct.Nonce.html), [Windows CNG authenticated mode](https://learn.microsoft.com/en-us/windows/win32/api/bcrypt/ns-bcrypt-bcrypt_authenticated_cipher_mode_info).

## Optional audio connection

Both hellos negotiate `audio_pcm:true`; the server only offers it with `--audio`. The viewer authenticates a separate TCP connection on the same listener using `{"type":"hello","version":3,"token":"...","channel":"audio","media_key":"..."}`. Only one audio channel attaches to the current viewer; it receives `audio_ready`. Failure of this optional channel leaves control/video running. It can use the existing SSH forward. Legacy v3 peers without the capability receive no audio.

Audio packet kind **3** has a 28-byte big-endian header: `u32 rate`, `u16 channels`, `u16 bits`, `u32 frames`, `u64 sequence`, `u64 source_elapsed_us`, followed by interleaved signed **little-endian** PCM. This version emits 480 frames at 48000 Hz, stereo 16-bit (1920 PCM bytes, 10 ms). Timestamps are source-local and do not imply a synchronized viewer clock. Capture spans only the server's private session sink; no microphone or default monitor capture.

The audio socket is duplex: after accepting a block into its bounded playback queue the viewer sends JSON `{"type":"audio_ack","sequence":N}` on **that socket**, with the normal packet framing. The server allows four outstanding packets, including socket buffers; a cumulative ACK must match an outstanding sequence (future ACKs grant no credit). At most four unsent blocks are retained, dropping the oldest. This does not change video presentation credits. The Windows PCM queue is capped at 100 ms, underruns insert silence, and overflow discards oldest samples. No automatic audio socket reconnection or A/V clock synchronization is implemented.

## Windows and input

The optional Linux `--xwayland` backend uses the same v3 window/frame/input formats. It requires no client capability or new binary kind. X11 top-level metadata adds `x11:true`; ordinary X11 toplevels use `type:window`, class as `app_id`, UTF-8 title and optional transient parent. Original client decorations are preserved; undecorated clients receive the native server frame.

Rootless override-redirect menus use `type:popup`, `root`, `parent`, root-relative logical `x/y`, and independent IDs/frames; they never expand the root's frame. `input_popup:false` marks Tooltip/Notification surfaces, which do not establish a native capture. The compositor publishes `popup_grab` for actual input menus and routes captured owner coordinates to the correct X11 popup surface. Native outside-click/capture cancellation sends Escape only to that root's currently focused X11 keyboard target. Removing the last input popup releases capture even when a Tooltip remains. Unmap/remap uses a fresh remote ID; parent closure and XWM disconnect remove live X11 remote windows without ending native Wayland streams.

X11 keyboard focus is a compositor enum forwarding to Smithay `X11Surface` keyboard targets, including ICCCM InputFocus/WM_TAKE_FOCUS, while the underlying wl_surface carries the Wayland keyboard events. X stacking is raised on focus changes so overlapping X root coordinates do not send mouse input to a different X window. X11 has a shared Xft/client density; individual remote presentation scales remain per window. Clipboard CLIPBOARD bridges UTF-8 text, PNG and URI lists using bounded worker I/O and existing clipboard generation checks; PRIMARY, XDND and XIM are not implemented.

Server `window`: `id`, `title`, `app_id`, `width`, `height` (logical units), `scale`, optional `parent` window ID, `decoration`. The viewer uses `parent` as the native owner for transient dialogs. `decoration:"server"` selects a native Windows title bar after xdg-decoration negotiation; `"client"` keeps the app-rendered title bar and removes the Windows non-client frame. Repeated messages update metadata and can change decoration ownership without recreating the native window. `window_closed` removes the native window. IDs are stable for the lifetime of the host process and restored on reconnect.

Frame body: u32 window ID, u32 pixel width, u32 pixel height, u64 frame sequence, i32 logical width, i32 logical height, i32 surface-local origin x, i32 surface-local origin y, then PNG bytes (36-byte header). All integers are big-endian. PNG contains **straight** RGBA; the GLES and Windows composition surfaces use premultiplied alpha. Dimensions are bounded to 8192 per axis and 16,777,216 total pixels before framebuffer allocation/Windows decode.

Top-level geometry excludes invisible CSD shadows and **never includes popups**. Each popup has its own frame/ID, preserving its surface-tree alpha and margins. The origin belongs to the displayed frame and must accompany pointer/drop messages so asynchronous geometry changes cannot shift hit testing. Raw frames and completed display frames use bounded latest-frame queues. All encoded video packets are delivered/decoded in order; dependent P frames must never be dropped as if they were independent images. Backpressure retains the latest raw update and limits queued encoded access units to two per window. Viewers advertising `frame_ack:true` additionally limit total outstanding frames to two per window, including frames already written into TCP buffers. Client `frame_ack` carries `id` and cumulative `sequence`; send after presentation submission (or buffered GDI paint). PNG replacement cancels credits for unsent independent frames; Video reference access units remain ordered. Fallback and closed windows clear their credits. Control messages have priority. Encoding and decoding run outside the compositor/UI event loops.

H.264 kind 2 uses the same 36-byte geometry header, followed by six u32 fields: stream epoch, even coded width, even coded height, keyframe flag (0/1), color byte length, alpha PNG byte length. Annex-B H.264 access-unit bytes follow, then an optional grayscale PNG alpha mask. The server emits an AUD (NAL type 9) at the start of each access unit and removes filler_data (type 12). Earlier v3 servers may include filler; clients can remove it before decoding. On the tested Windows MFT, trailing filler delayed the final picture until another input arrived, even with low-latency mode enabled. Removing filler preserves picture/reference data and sequence timestamps; it does not change frame-credit semantics. Total fixed header is 60 bytes. No mask means fully opaque. Color uses BT.709 limited-range H.264 4:2:0; PNG alpha is full-range lossless. On resize/reconnect or resuming video after an independent PNG, a new epoch starts with an IDR, including SPS/PPS. Bounded server encoder caches may reuse a previous resolution, but reactivation still creates a new epoch and forces this independent IDR. Clients must discard previous-epoch pending metadata and reset reference state (or flush a compatible cached decoder); encoded P frames within an epoch must remain ordered. A mask always accompanies its matching color sequence; video decoding may buffer metadata until that sequence is returned. Client `codec_fallback` with `id` requests PNG for the rest of the viewer session.

HEVC kind **4** and AV1 kind **5** are optional v3 extensions. They reuse the exact
60-byte geometry/epoch/coded-size/key/color-length/alpha-length header above. The
kind identifies the codec; it must match an explicitly advertised client capability.
The server's default `--codec auto` continues to select H.264, including when a
viewer supports all codecs. `--codec hevc-nvenc` or `--codec av1-nvenc` explicitly
selects the new codec; a viewer omitting that capability receives PNG. No new codec
is sent speculatively to an old viewer.

HEVC is 8-bit Main, BT.709 limited-range 4:2:0 SDR, one Annex-B access unit per
packet. Each unit starts with AUD (NAL 35); filler NAL 38 is removed with FFmpeg
`filter_units`. A new epoch or cache reuse starts with VPS/SPS/PPS plus IDR.
AV1 is 8-bit Main, BT.709 limited-range 4:2:0 SDR, one low-overhead OBU temporal
unit per packet. FFmpeg `av1_metadata` inserts a temporal delimiter and removes
padding OBUs; H.264/HEVC NAL filters are never applied to AV1. A new epoch or cache
reuse starts with a sequence header and an independently decodable key frame.
The header's `key` field means independent random access for all codecs; AV1 does
not have H.264 IDRs. Independent PNG alpha, stream sequence pairing, ordered
reference frames, two presentation credits and bounded queues are unchanged.

Encoder and decoder caches include codec as well as peer/window/size identity.
New viewers attach `codec:"h264_alpha"|"hevc_alpha"|"av1_alpha"` to
`codec_fallback`. The server rejects a fallback naming a different codec from the
window's active video stream, clears matching queued/reference state and selects
PNG for that window for the rest of the viewer session. An omitted codec retains
the existing v3 fallback behavior. Decode creation/processing, stalled output,
unsupported formats and presentation errors all request this safe PNG fallback;
reconnect creates fresh session state. There is no software codec fallback hidden
behind an advertised GPU capability. These SDR formats remain 8-bit 4:2:0; Main10/P010 HDR is the separate opt-in extension below. 4:4:4 is not implemented.

`--gpu-input cuda` changes only server-local color capture/encoder input. Wire
codec kinds, straight color semantics, independent lossless alpha, epoch/keyframe
rules and two presentation credits do not change. Successful CUDA color input has
no CPU color round trip; transparent alpha still reads back independently. GPU
failure may send an independent PNG, then resume CPU NVENC under a fresh epoch.

Server `popup`: `id`, `root` toplevel ID, `parent` surface ID (possibly another popup), `x`, `y` relative to the root's streamed client origin, logical `width`, `height`, and `scale`. Map to an owned top-level `WS_POPUP` without activation/taskbar entry, not a clipped `WS_CHILD`. Position uses the root client's Windows screen origin and DPI. Parent and popup sizes are independent. `window_closed` applies to either kind. `popup_grab` carries `root` and `active`; the viewer routes captured mouse input through the root while the Wayland popup grab is active. `popup_cancel` with root `id` dismisses the grab after native capture loss.

Server `window_action`: `id`, `action` (`move`, `resize`, `minimize`, `maximize`, `restore`), optional `edge` using xdg_toplevel resize-edge values. These requests originate in the application's client-side title bar. Move/resize requests require a valid same-client pointer grab. The viewer runs the native Windows action. Its resulting geometry/state is sent back through `resize`.

Client messages:

| type | fields |
|---|---|
| `focus` | `id`, `focused` |
| `pointer` | `id`, `x`, `y` in logical image coordinates; `origin_x`, `origin_y` from the displayed frame |
| `leave` | `id` |
| `button` | `id`, `button` (Linux evdev BTN code), `down`; optional pointer coordinates/origin as above |
| `key` | `id`, `key` (Linux evdev code, without XKB +8), `down` |
| `scroll` | `id`, `x`, `y` |
| `resize` | `id`, `width`, `height` (logical viewer client area), `dpi`, `work_width`, `work_height` (physical monitor work area), logical `work_x`, `work_y` relative to the root client origin, optional `maximized` boolean |
| `viewport` | same monitor/work-area fields as resize; update placement without resizing the application |
| `frame_ack` | `id`, cumulative `sequence`; return presentation credit |
| `ping` | opaque `nonce`, echoed in server `pong` |
| `codec_fallback` | `id`; permanently select PNG for this viewer session |
| `cancel_input` | `id`; release held input when native window move/resize takes capture |
| `close` | `id` |
| `refresh` | no fields |

Focus loss and disconnection release held keys/buttons. Stale loss/leave events only affect the window that still owns that focus. Keyboard focus is sent after mapping; popup grabs use the originating input serial. Each toplevel has an output and preferred fractional scale; viewporter and xdg-output are advertised, with integer buffer-scale fallback for older clients. Keyboard repeat is supplied by the receiving Wayland application; repeated Win32 keydown messages do not generate stuck keys.

## Experimental scRGB / Main10 HDR

Default is 8-bit BT.709 SDR. Linux must enable `--hdr experimental-scrgb` with `--codec hevc-nvenc` or `av1-nvenc`; Windows must opt in with `WAYBRIDGE_HDR=1`. The server hello reports `hdr`, `color_spaces` (`bt709_sdr`, optionally `bt2020_pq`) and `color_depths` (8, optionally 10). The viewer must explicitly send `hdr:true`, include `bt2020_pq` and advertise `hevc_main10_pq_alpha16` and/or `av1_main10_pq_alpha16`. A codec string alone never enables HDR. Client runtime probes must actually decode a Main10 sample into P010, create usable P010 plane views and verify FP16/scRGB composition support; an active-HDR monitor is also required for that window.

`color_mode {id,hdr:true|false}` selects the monitor policy for a live toplevel. It is subject to negotiated HDR/codecs, never a bypass. Display/move/settings changes re-evaluate the native monitor. No system display setting is changed. An SDR monitor or unsupported path produces SDR; decoding/presentation failures request codec-bound PNG fallback for the rest of the viewer session.

Only Wayland surfaces explicitly tagged through `wp_color_manager_v1.create_windows_scrgb` enter this HDR subset. Linear Rec.709 scRGB uses 80 nits per unit. A true RGBA16F target composites the surface tree; untagged SDR subsurfaces are converted from sRGB to linear before blending. Same-GPU CUDA unpremultiplies and converts to BT.2020 non-constant-luminance/PQ limited-range P010. HEVC kind7 and AV1 kind8 carry 10-bit 4:2:0 Main10 access units with PQ/BT.2020 metadata. Their fixed 60-byte video header is identical to kinds 2/4/5; stream epoch, independent key frame, coded padding, exact sequence pairing and two presentation credits retain the same meaning.

The optional alpha payload is a 16-bit grayscale PNG storing **raw IEEE FP16 alpha bits**, not normalized UNORM16 opacity; no mask means alpha 1.0. The viewer decodes it without quantization, converts P010/PQ back to linear scRGB, premultiplies and presents an FP16 DirectComposition surface. Successful HDR color stays on the GPU; transparent alpha still reads back two bytes/pixel, with GPU copies and synchronous waits. A GPU-generated SDR backup supports accurate current-frame PNG recovery. Returning from PNG to any video codec starts a fresh epoch and independent key frame, including cached sizes.

Unsupported gamut/non-finite input or unavailable HDR falls back to SDR: the supported linear Rec.709 path uses Reinhard `rgb/(1+luminance)` then sRGB. This subset does not advertise ICC or arbitrary parametric creators, general color-managed desktop support, HDR X11, broad-gamut mapping or 4:4:4. SDR output/preferred descriptions stay SDR. Physical Windows/WoA Main10 MFT and HDR display behavior require device validation; software shader arithmetic alone does not establish them.

## Optional native window semantics

A v3 viewer opts in with `window_semantics_v1:true` in its first hello. The server echoes the negotiated boolean and advertises the feature; all fields/messages in this section require that agreement. Older v3 viewers keep their original geometry/control behavior and are not advertised xdg fullscreen capability. Minimize and maximize capabilities remain available; window-menu capability is not advertised.

Negotiated `window` metadata additionally contains `min_size:[w,h]`, `max_size:[w,h]` (logical units, zero means unspecified; normalized to at most 8192 per axis), `modal` (the genuine `xdg-dialog-v1` hint), `fullscreen`, `fullscreen_epoch`, `mapping_epoch`, and `input_region`. `parent:null` explicitly removes ownership. Parent changes apply to existing HWNDs; missing/unmapped parents are cleared, parent unmap/destruction reparents surviving transient toplevels to the live grandparent or clears their parent. A transient parent alone never means modal. A mapped modal child blocks input to its direct owner, redirects that owner's activation, and releases it when the hint/object/child is removed. Other independent windows stay usable.

`window_action` adds `fullscreen` and `unfullscreen` actions and includes `fullscreen_epoch`. The viewer saves the prior native placement, uses the monitor rectangle and borderless fullscreen style (distinct from maximized work-area bounds), and restores the prior normal/maximized placement. Negotiated `resize` also sends `fullscreen` and `fullscreen_epoch`; only the current epoch can change the fullscreen configure state. Older-epoch geometry may still be used as an intermediate resize, but cannot overwrite newer fullscreen intent. Application min/max constraints apply to normal/maximized client dimensions, including DPI conversion and native tracking/programmatic resizing; fullscreen dimensions bypass those constraints. Restoring from fullscreen preserves hidden/minimized state.

Server `window_visibility`: `id`, `mapped`, `mapping_epoch`, `fullscreen`, `fullscreen_epoch`, on buffer map/unmap. Native windows are hidden on unmap and reused on remap. Wayland unmap clears the role's parent/modal/title/app_id, min/max/geometry, focus/grabs and requested fullscreen, as required for a new mapping. A remapping application must send its metadata again. Client `window_state`: `id`, `visible`, `minimized`, `mapping_epoch` reports native visibility after the window has first been shown; stale epochs are ignored. Hidden/minimized windows receive xdg `suspended` and low-rate callbacks, skip capture, and release input; restore marks the retained surface dirty for a fresh frame. Already queued media is decoded/retained in order and acknowledged after retention while hidden, so frame credits and H.264 references cannot deadlock restoration. It does not report occlusion by other desktop windows.

`input_region` is independent of visual alpha. It is null for conservative default hit testing, or `{origin_x,origin_y,width,height,layers:[{x,y,width,height,rects}]}` in root surface logical coordinates. Each layer is a mapped root/subsurface; `rects:null` accepts its full bounds, `[]` accepts none, otherwise ordered `[add,x,y,width,height]` operations add/subtract within its bounds. Union the layers. Limits are 64 layers and 256 operations total; excessive/invalid regions and regions whose geometry does not match the displayed frame fall back to accepting input. The Windows viewer preserves non-client/CSD resize hit tests and returns `HTTRANSPARENT` for excluded client pixels. Win32 only forwards that result to underlying windows in the same UI thread: this supports overlapping Waybridge windows, **not general click-through to other processes or the desktop**. No visual cropping, alpha-derived input shape, or cross-process pass-through is claimed.

## Local Windows IME (optional `ime_v3` feature)

The compositor exposes `zwp_text_input_manager_v3`. Only the enabled text-input belonging to the actual keyboard-focused surface receives composition. `ime_state` control messages contain `enabled`, an opaque `generation`, and, when enabled, target window/popup `id`, `rectangle:[x,y,width,height]` in surface-local logical coordinates and numeric content `purpose`. Neither surrounding application text nor document contents are forwarded.

The viewer returns `{"type":"ime","id":N,"generation":G,"preedit":"nihao","cursor_begin":5,"cursor_end":5}` and/or `commit:"你好"`. Preedit cursor offsets are UTF-8 byte boundaries (or -1), not UTF-16 units. Strings are bounded to 60 KiB each, below the Wayland event size limit. Updates are sent in one text-input done batch with the application's current commit serial. An empty preedit cancels it. Focus/enable generation and actual focused target must match; stale updates are ignored. Enable state and pending client fields take effect only on the application's text-input `commit` request.

Windows keeps the native candidate UI, sends IMM result/preedit strings, and positions its local caret/candidate window from the remote rectangle, image origin and current client scaling. Normal text uses this channel when enabled; modifiers, shortcuts and navigation retain the physical-key channel. Unsupported applications keep physical-key input. The wire version stays v3; older endpoints ignore the optional state/messages. XIM and surrounding-text reconversion are not implemented.

## Native cursor extension (`cursor_v1`)

This optional extension keeps protocol v3 compatibility. The viewer opts in with `cursor_v1:true` in its control `hello`. A supporting server lists `cursor_v1` in `features` and echoes `cursor_v1:true` only when negotiated. No cursor packets are sent without opt-in. A new viewer connected to an older v3 server keeps its default native arrow.

The server sends JSON `cursor` on the control connection: `id` is the pointer-focused root toplevel ID (also applies to its popups), `generation` is a monotonically increasing seat-local update number, and `kind` is `named`, `hidden`, or `bitmap`. `id:0` resets the cursor to the default. Apply an update only to the matching root's client area; never override native non-client resize/title-bar cursors or another application's cursor. Discard older generations. Reconnection resends current state and subsequent pointer events update ownership.

- `named` carries `name`, a CSS/cursor-icon name such as `default`, `text`, `pointer`, `wait`, `ew-resize`, or `nwse-resize`. The viewer maps these to shared Windows system cursors. Names without an exact system equivalent may use an approximation/default arrow.
- `hidden` hides the local cursor in the matching client area. It does not use process-global `ShowCursor` counters.
- `bitmap` carries `format:"rgba8-premultiplied"`, `width`, `height`, `scale`, `hotspot_x`, `hotspot_y`, and `pixels` (canonical base64 of exactly `width*height*4` bytes). Pixels are top-down, row-major premultiplied RGBA, without a PNG header. Dimensions are `1..=256` per axis; `scale` is finite in `1..=4`. The hotspot is in **bitmap pixels**, within its dimensions. `scale` is source pixels per logical unit; the viewer scales pixels and hotspot by its window DPI / 96 / scale. Negative or outside Wayland hotspots are represented with transparent padding before these bounds are applied.

The compositor supports both `wp_cursor_shape_manager_v1` and ordinary `wl_pointer.set_cursor` surfaces (including surface trees, buffer scale and animated commits). Custom cursor frame callbacks run independently of application-window damage and continue without a viewer. Bitmap capture/callbacks are capped at 30 updates per second and bounded before GPU allocation/import (at most 16 source surfaces, each at most 256×256, with at most 65,536 total source pixels); oversized or failed bitmaps use the default arrow. Each side retains only **one latest cursor message/bitmap**, replacing superseded animation updates. The server and viewer never place cursor animation into the H.264 reference queue. Each bitmap is at most 262,144 decoded bytes / 349,528 base64 characters; the viewer owns at most one custom `HCURSOR` and releases replaced handles.

## Application icon extension (`window_icon_v1`)

The viewer opts in with `window_icon_v1:true` in its control `hello`; a supporting server lists `window_icon_v1` in `features` and echoes the negotiated boolean. This remains optional within v3: no icon messages are emitted for old/simple viewers, and a new viewer keeps generic native icons with an older server.

`window` metadata carries `icon_revision` (monotonically increasing for this window's application identity). The server asynchronously resolves the application's desktop file and icon, then sends control JSON `window_icon`: `id`, `app_id`, `revision`, `width:128`, `height:128`, and `png` (base64 PNG or `null` for generic application icons). The extension uses bounded ASCII desktop-file IDs; missing/unusable IDs normalize to an empty identity. On metadata identity/revision changes, the viewer clears the old icon immediately. It applies a result only when the window is still alive and both normalized application identity and revision match. Pending old results and messages for unknown/closed IDs are ignored. Reconnection reannounces metadata before the current icon.

PNG is straight RGBA, normalized with aspect ratio and transparent padding to 128×128, at most 96 KiB decoded PNG / 128 KiB base64. The viewer validates exact PNG dimensions before copying pixels, constructs per-DPI large/small native `HICON`s and sets both with `WM_SETICON`. Replaced/closed-window custom icons are destroyed; system fallback handles are shared. Windows DPI changes rescale from the bounded master without a round trip.

Resolution runs off the compositor thread: XDG applications lookup, desktop-file `Icon`, selected GTK/KDE theme (or `WAYBRIDGE_ICON_THEME`), theme size ranking/inheritance, hicolor and unthemed/pixmaps fallbacks. Desktop `Exec` is never executed. Source files are bounded to 2 MiB; PNG dimensions to 1024×1024 / decoder allocation to 16 MiB. SVG support uses optional `rsvg-convert`, limited to 2 seconds wall/CPU, 512 MiB virtual memory and 96 KiB output, and killed if its parent exits. Failure returns generic icons. At most the first 128 live toplevels load/retain icon data, matching the viewer window limit. Worker and transport queues replace pending work per window (maximum 128); the worker has a 64-entry positive/negative LRU cache with 60-second lookup TTL. Themes/configuration are sampled at server startup; existing windows do not hot-reload icon files. XPM and client-supplied xdg-toplevel-icon hints are not supported by this first extension.

## Locked relative pointer (optional v3 extension)

The server advertises `features:[...,"relative_pointer_locked","locked_pointer"]`.
The viewer opts in with `hello.pointer_lock:true`. Viewers omitting it keep ordinary
absolute input and never receive lock requests. No media or frame-credit fields change.

The compositor exposes `zwp_relative_pointer_manager_v1` and
`zwp_pointer_constraints_v1`. **Only locked-pointer constraints are activated in this
version**; confined-pointer requests remain inactive. Relative motion is currently
provided while locked; an application requesting only relative-pointer without a lock
continues to receive ordinary absolute input. Popup locks and cursor-position hints
are not implemented.

When a lock's target is mapped, has keyboard focus in its root toplevel, contains the
pointer in its input/constraint region, and no popup grab is active, the server sends
`{"type":"pointer_lock","id":1,"generation":7,"active":true}`. `id` is the root
window; generation is a nonzero server-process-lifetime u64 and changes for every
request, including reconnect/recreation. The viewer must verify native focus and
establish Raw Input, a hidden cursor and native clipping before replying
`{"type":"pointer_lock_ack","id":1,"generation":7,"active":true}`. Only then does
the compositor send Wayland `locked`. A native failure replies `active:false`.
A request expires after two seconds without a valid acknowledgement. Absolute
positions are held at the anchor during pending/active lock.

While active the viewer sends
`{"type":"relative_pointer","id":1,"generation":7,"dx":12,"dy":-3}`.
Both deltas must be finite JSON numbers, with absolute value at most 1,000,000 per
packet. Wrong targets/generations and malformed deltas are ignored; invalid JSON
closes the transport as before. Windows passes raw relative mouse counts without
Windows pointer acceleration as both Wayland accelerated and unaccelerated deltas;
the server supplies a monotonic microsecond timestamp. Absolute-device Raw Input is
not converted into relative motion.

Server `pointer_lock` with `active:false` releases that exact target/generation.
The viewer may independently send `pointer_lock_ack` with `active:false` after first
restoring native cursor state. Focus loss, close/destruction, move/resize, popup grab,
constraint removal, unmapping, region exclusion, disconnect and
**Ctrl+Alt+Shift+F12** end a lock. Native cleanup precedes notification/network waits.
The viewer restores its prior clip, visibility count and cursor, and restores the old
cursor position only while its window remains foreground. Held mouse buttons are
released before removing an implicit Wayland grab. An emergency/native rejection
blocks the same persistent constraint until a real pointer leave/reentry, a focus
change, or a newly created constraint; it cannot immediately trap the cursor again.
Oneshot constraints require a newly created object after deactivation.

## Clipboard

Both directions send `{"type":"clipboard","items":{"<MIME>":"<base64>"}}`. Supported formats are `text/plain;charset=utf-8`, `text/plain`, and `image/png`. Maximum decoded size is 12 MiB per format. Linux clipboard reads are asynchronous and tagged with an ownership generation to discard stale responses. Windows tracks the clipboard sequence number to avoid echo loops.

## Files

Upload from Windows:

1. `upload_begin`: `transfer` u64, basename `name`, `size` u64.
2. `upload_chunk`: `transfer`, base64 `data`; at most 65536 decoded bytes.
3. `upload_end`: `transfer`; received size must match metadata.
4. `upload_commit`: array `transfers`. Optional `drop:true`, target `id`, logical `x`, `y`, displayed `origin_x`, `origin_y` requests a Wayland Copy drop instead of setting the file clipboard.

Only finalized files may be offered to applications. New destination-local URIs are generated after materialization. Names cannot contain separators, drive syntax, control characters, or traversal components. Server emits `upload_ready` or `error`. Partial uploads are cancelled on disconnect. Completed files remain available to applications.

Download from Linux file selection:

1. `files_begin`: `batch` generation, `files` array with `id`, basename `name`, `size`.
2. `file_chunk`: `batch`, `file` ID, base64 `data` (64 KiB chunks).
3. `files_done`: `batch`, after exact-length transfer; alternatively `files_error`.

Windows creates local staged files, then publishes CF_HDROP. A new local clipboard selection supersedes an older download. Bulk transmission is bounded and happens on a worker thread; it does not wait for network capacity on the compositor thread.

v3 supports 128 files per batch and 1 GiB per regular file. It supports Copy semantics only. File paths from one system are never passed directly to applications on the other system.

### Native Linux → Windows file drag

Opt in with `file_drop_out:true` in the viewer hello; the server advertises
`file_drop_out` in `features`. This extension uses JSON on the control socket,
independent of clipboard ownership and `files_*` batches. It supports one active
outbound operation and COPY only. Without `file_tree`, at most 128 regular files
of 1 GiB each are offered; negotiated directory trees use the limits below. Sources
must offer `text/uri-list` and COPY through wl_data_device v3 or newer. Without `file_tree`, directories are rejected.
Final-component symlinks, nonlocal/non-UTF-8 URIs, unsupported Windows names and
case-insensitive name collisions are rejected. Source files are never deleted.

1. Server `drag_offer`: unique `drag` u64, source window `id`, `action:"copy"`, and
   `files:[{id,name,size}]`; file IDs are consecutive from zero. This transfers only
   metadata. A worker reads the URI list with a 10-second deadline and 1 MiB limit,
   opens/pins source files, and validates their type and size.
2. The viewer continues normal Wayland input while inside streamed windows. On
   leaving them with the real left button still held it sends `drag_claim {drag}`
   and suspends ordinary pointer/button forwarding for that gesture. The compositor
   routes the grab to a private, unmapped Wayland data-device target. Once COPY is
   negotiated it sends `drag_ready {drag}`. Native focus/capture changes during
   this handoff must not release the Wayland grab. Held keys are released and
   focus changes are deferred until the gesture ends, preventing stuck modifiers.
3. Only while that same real left-button gesture is still held, Windows calls
   `DoDragDrop` with `DROPEFFECT_COPY`, exposing `FileGroupDescriptorW` and
   `FileContents`/`IStream` virtual files. It never starts a drag after an early
   release and never publishes Linux paths or nonexistent CF_HDROP paths. Targets
   requiring CF_HDROP alone are not supported by this outbound extension.
4. `drag_read {drag,request,file,offset,length}` requests an independent range;
   `request` is unique within the operation, `length` is 1..65536, and at most
   eight requests may be outstanding. `drag_data {drag,request,data}` returns
   base64 bytes (possibly fewer at EOF), or `{drag,request,error}`. No file bytes
   are sent until requested. The server has a bounded worker/queue, checks file
   identity metadata for in-place changes, and rejects oversized/invalid reads.
   Windows delivers responses directly from its socket receiver to waiting
   streams, independently of the UI inbox. Read failures/timeouts fail the copy.
5. On real button release the viewer sends `drag_drop {drag}` before completing
   the native drop. This releases the Wayland grab over the private target and
   produces `dnd_drop_performed`; it does not finish the source. Then
   `drag_result {drag,success,effect}` reports native completion (`effect` is
   `"copy"` or `"none"`). With `IDataObjectAsyncCapability`, success waits for
   `EndOperation`; returning from `DoDragDrop` alone does not release source files.
6. Success finishes the private offer; failure destroys/cancels it. Smithay emits
   the corresponding source terminal event. `drag_end {drag,success}` retires the
   operation on the viewer. Source destruction, disconnect, read failure, rejection,
   Esc and a 120-second idle timeout cancel the operation. Repeated/stale terminal
   messages are ignored. No cancelled source is later marked finished.

Opened source descriptors live until completion/cancellation. Streams support
Read/Seek/Stat/Clone and bounded memory, with no full-file staging or destination
temporary files. The target owns files it creates. No move/delete acknowledgement
is accepted, and clipboard contents are unaffected.

### Directory trees (`file_tree`)

Both sides opt in through viewer hello `file_tree:true` and server feature
`file_tree`. Flat `upload_*`/`files_*` messages remain supported; a peer without
this capability receives regular files only, with the original 128-file limit.
No additional binary packet kinds are used.

The Windows frontend also imports true virtual sources with no CF_HDROP using
FILEGROUPDESCRIPTORW/A plus per-descriptor FILECONTENTS. This uses the same
`tree_upload_*` protocol; the wire manifest never contains source-local paths.
Original descriptor indices are preserved when requesting contents, while missing
parent directories are synthesized before strict manifest validation. Directory
descriptors require no content medium.

An apartment-marshaled IDataObject feeds the single MTA upload worker. File
contents are acquired one file at a time and read in 64 KiB chunks; a returned
IStream is cloned and rewound when supported, with sequential non-seekable streams
accepted from their initial position. Missing FD_FILESIZE requires IStream::Stat;
known-size HGLOBAL contents are supported up to 12 MiB. IStorage and unknown-size
HGLOBAL contents are rejected. E_PENDING is retried within a 30-second progress
deadline. Streams, storage media and source references are released on failure,
source loss, clipboard epoch change, target loss or disconnect. Windows additionally
requests COM call cancellation; Wine lacks this API and cancels after a finite
provider call returns. A provider that never returns or ignores OS cancellation
still requires source-process recovery; untrusted providers are not isolated in
a broker process.

For async-enabled IDataObjectAsyncCapability sources, StartOperation precedes
worker access and EndOperation runs on the source-facing STA only after Linux
commit acknowledgement or failure. Other OLE drops keep Drop open with COM-aware
message dispatch until the full tree succeeds/fails, returning COPY/NONE
accordingly; Escape cancels this synchronous wait. Async COPY acceptance means
the import was scheduled, with final success reported by EndOperation. Linux
publishes only complete destination-local paths; there is no lazy FUSE filesystem
or remote IStream backing Linux application reads. Successful imports retain the
same private inbox lifetime as filesystem tree imports, and never delete source
items. Windows/Outlook/archive-provider compatibility requires real-Windows tests.

A tree manifest is a parent-before-child array of
`{id,name,kind,size}`. IDs are consecutive from zero, `kind` is `"file"` or
`"directory"`, and directory size is zero. `name` is a **relative** `/`-separated
path including the selected top-level basename. Explicit directory entries
preserve empty directories; repeated basenames under different parents are valid.
Only the top-level entries become clipboard/drop URIs or CF_HDROP paths.

Limits apply to the entire batch: 128 roots, 512 entries, 16 path components,
1 GiB per file, 8 GiB total file bytes, 240 UTF-16 code units per relative path,
and 240 UTF-8 bytes/UTF-16 code units per component. Empty components, `.`, `..`,
absolute/drive/UNC paths, backslashes, controls, ADS/colon syntax, Windows reserved
names, trailing dots/spaces, duplicate IDs/paths and case-insensitive collisions
are rejected before destination creation. Parents must already be declared
directories with exactly matching spelling. There is no implicit rename or
overwrite. Win32 staging additionally requires the complete temporary path to fit
MAX_PATH; longer paths fail with an error rather than truncation.

Symlinks, NTFS reparse points (including junctions/cloud placeholders), and special
files fail the entire batch. Linux inspects through O_PATH and walks relative to
pinned directory fds, without following links or opening a device/FIFO for I/O.
Windows opens/inspects reparse points and denies source replacement while handles
are held. File contents/names are copied; permissions, executable bits, timestamps
and hard-link identity are not preserved. Detected source changes fail the copy.

Linux clipboard → Windows:

1. `tree_begin {batch,epoch,entries}` after validating/opening the complete source
   manifest. `batch` is the Linux clipboard generation.
2. `tree_chunk {batch,file,data}` for file entries, at most 65536 decoded bytes.
   Directories and zero-byte files require no chunks.
3. `tree_done {batch}`, or `tree_error {batch,message}`. Only exact-length,
   complete trees are published from a fresh private staging directory. A newer
   batch, local selection, failure or disconnect removes unfinished staging.

Windows → Linux (clipboard and incoming OLE drop):

1. `tree_upload_begin {batch,epoch,drop,entries}`; `drop` defaults to false.
   The server validates the manifest and creates a fresh private tree, then
   acknowledges `tree_upload_ready {batch,stage:"begin"}`.
2. `tree_upload_chunk {batch,file,data}` (64 KiB decoded maximum), followed by
   `tree_upload_end {batch,file}` for **every file**, including zero-byte files.
3. `tree_upload_commit {batch,drop,...}` must match the begin purpose. With
   `drop:true`, target `id`, `x`, `y`, `origin_x`, `origin_y` have the existing
   upload-drop meanings. The server offers top-level destination-local URIs only
   after every file is finalized, then acknowledges stage `"commit"`.
4. `tree_upload_cancel {batch}` or `tree_upload_error {batch,message}` retires
   unfinished uploads. Already committed files are retained. Queued chunks for
   retired batches are ignored, avoiding repeated error/control-queue floods.

There are at most two active server upload trees. The Windows client uses one
upload worker and at most two pending requests, reports a full queue, cancels
superseded clipboard requests, and preserves explicit queued drop requests. Begin
and commit acknowledgements have 10-second deadlines. Disconnect releases jobs,
source handles and unfinished destination trees.

`file_tree_epoch {epoch}` advances a viewer-local counter when its local clipboard
changes (excluding its own remote publications). It cancels pending Linux export
reads/workers. File download begins carry the captured epoch; clipboard upload
begin/commit must match the current epoch. Drop uploads are independent of
clipboard epochs. A stale epoch cannot overwrite a newer local selection.
Linux clipboard messages additionally carry `generation`; receivers ignore older
formats and give a completed file batch precedence over text formats from that
same generation. Duplicate/stale file begins are ignored.

For native Linux → Windows drag, `drag_offer` adds `tree:true` and its `files`
array uses this manifest. FILEDESCRIPTORW names use Windows relative separators;
directory descriptors set FILE_ATTRIBUTE_DIRECTORY. FILECONTENTS/lindex and
`drag_read` refer to the original manifest indices, including directory slots,
but only file slots provide streams. The existing real-button handoff, COPY-only
and asynchronous completion rules apply. Native targets own and clean up any
destination files they create; Waybridge only removes its own private staging.

Completed clipboard/drop trees remain at
`$XDG_RUNTIME_DIR/waybridge-inbox/tree-<batch>/` or `%TEMP%\Waybridge-tree-*` so
applications can read them later. They may be manually removed once consumers no
longer need them. Copy/cancellation never deletes source files. Virtual directory
descriptors and all four directions are tested with real Wayland and native OLE
probes under Wine/Xvfb; Windows Explorer/third-party compatibility still needs
Windows 10/11 device validation.
