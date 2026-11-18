# Waybridge protocol v3

## Authenticated TLS entry

`waybridge-service` accepts TLS 1.2/1.3 on port 49092 by default. Management uses bounded LF-delimited JSON. Each native control/media/audio socket independently checks the leaf DER SHA-256 pin **before** sending `{id:1,op:"stream",client_id,client_token,session}\n`. On `{id:1,ok:true,result:{stream_ready:true,version:3}}\n`, that same TLS socket carries the unchanged binary v3 framing below, including the existing token/media-key handshake. The entry validates the target session and forwards only to its owned loopback listener. Client revocation closes its active streams. Pairing, trust persistence, authorization scope and limits are specified in [service/README.md](SERVICE.md).

Raw v3 listeners remain local compatibility endpoints. The TLS gateway and SSH tunnel are protected transports for the same business protocol.

## Packet framing

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

First client message: `{"type":"hello","version":3,"token":"...","dpi":168,"work_width":3840,"work_height":2080}`. Authentication and exclusive viewer ownership precede window/media announcements. The server replies with `codec:"mixed"`, `codecs`, `features` and a random per-session `media_key`. Both ends must use v3. DPI defaults to 96; work dimensions are physical monitor work-area pixels. This TCP protocol is not intrinsically encrypted; use the local listener through the TLS entry, SSH or another trusted protected transport. UDP media has its own AEAD, whose keys travel on the authenticated control connection.

`monitor_width` and `monitor_height` optionally supply the full physical monitor dimensions, including the taskbar area, in `hello`, `resize` and `viewport`. The shared Xwayland output uses the hello dimensions; per-window Wayland outputs also use the dimensions in subsequent updates. Older clients fall back to the work dimensions. Work-area fields retain their popup-placement meaning. Managed X11 top-level geometry is constrained inside this shared screen on mapping, configure, resize and reconnect, since Xwayland clips absolute pointer coordinates at the root-screen boundary even when the native viewer displays the entire window. Physical-to-logical bounds use the same nearest-integer rounding as XWM and the viewer, avoiding 2560→2559 truncation at 175% DPI.

Optional client hello fields are `split_media`, `frame_ack`, `cursor_v1`, `window_icon_v1`, `window_semantics_v1`, `pointer_lock`, `toplevel_drag_v1`, `file_drop_out`, `file_tree`, `audio_pcm`, `udp_fec_v1`, `codecs`, `hdr` and `color_spaces`. Enable extensions only as described in their sections; the server advertises `ime_v3` for local text input. Missing fields retain compatible v3 behavior. `features` lists implemented extensions, not proof that a GPU/decoder/device is usable.

A viewer advertises `png_rgba` unconditionally. It adds `h264_alpha`, `hevc_alpha` or `av1_alpha` only after an actual synchronous D3D11-aware system MFT decodes a small access unit to a timestamp-matching NV12 GPU texture. HEVC/AV1 use dynamically loaded `MFTEnumEx`; missing enumeration/decoder/GPU support keeps PNG available. Main10 has additional checks below. The server advertises implemented formats; driver/size/resource failures may still select PNG. `auto` chooses H.264 for SDR and HEVC when HDR is requested; actual output still requires the relevant negotiated capability.

## Media connection

A viewer requesting `split_media:true` opens a second TCP connection to the same address and port. Authenticate it with `{"type":"hello","version":3,"token":"...","channel":"media","media_key":"..."}`. Only the current viewer's random key is accepted, and only one media socket attaches. The server replies `media_ready`, then sends frame kinds 1/2/4/5/7/8 over this socket. Input, window control, clipboard and file messages stay on the control socket. Socket failure closes the session; reconnect restores live windows. The TLS entry and existing SSH forwarding carry both connections. Test/simple viewers may omit `split_media` to multiplex on one socket.

## Optional UDP/FEC v1

Server `--udp-listen` plus client hello `udp_fec_v1:true`, `split_media:true`, `frame_ack:true` are all required. The server hello contains `udp_fec_v1:{version:1,session:"<u64 decimal>",port,tx_key:"<base64 32 bytes>",rx_key:"<base64 32 bytes>",chunk:1024,group:8}` or null. Keys are named from the server's perspective and **must travel over an authenticated encrypted control connection**, such as the TLS entry or SSH; raw v3 TCP is not intrinsically encrypted. Client uses an explicitly configured directly reachable UDP endpoint, independently of its tunneled TCP address. Fresh keys/session are generated on reconnect. No audio negotiation is changed.

Datagrams are at most 1080 bytes: clear header `"WBU1" + u64 session + u64 counter` (big-endian), AES-256-GCM ciphertext, 16-byte GCM tag. The whole 20-byte clear header is AAD. The 12-byte nonce is four zero bytes followed by that direction's nonzero u64 counter; every sealed datagram consumes a new counter, including parity and keepalives. Keys are distinct per direction; counter overflow fails rather than reuses a nonce. Client rejects authenticated duplicates and counters older than a 4096-packet replay window. Client keepalives are a single encrypted zero byte; server accepts increasing counters from the first authenticated endpoint only. No unauthenticated amplification or automatic NAT rebinding.

Server plaintext shard header is `u8 type` (1=data,2=XOR parity), `u8 original_kind` (1=PNG,2=H264,4=HEVC,5=AV1,7=HEVC Main10,8=AV1 Main10), `u16 index`, `u16 data_count`, `u16 reserved=0`, `u32 total_body_bytes`, `u64 message` (all multibyte fields big-endian). It is followed by 1024 data bytes except a shorter final data shard. Parity has exactly 1024 bytes and index is the group's first data index (multiple of 8); it XORs up to 8 zero-padded data shards. Whole bodies include the original frame header. Geometry/epoch/sequence/alpha formats are unchanged. UDP frame bodies are capped at 8 MiB; count and payload lengths are validated before allocation.

The media TCP writer first emits `{"type":"udp_start"}` after all previous ordinary TCP frames. The receiver buffers UDP messages until this barrier, then delivers increasing global message IDs from 1. It responds over authenticated control with `udp_ack {message}` after queueing a complete frame. These transport ACKs release the bounded repair cache, **not** video presentation credits. `udp_retry {message}` requests the missing next message (100 ms client interval, server repair rate limited to 50 ms/message). Binary TCP kind **6** contains `u64 message + u8 original_kind + original_frame_body`. This also carries new frames when UDP is stale (>3 seconds), a frame is oversized or the cache is full; it follows the same ordered delivery and duplicate suppression. Repeated repairs reuse the same message ID, never a new encoded reference frame.

Global transport credits: at most 8 unacknowledged messages and 64 MiB of original frame bodies, including TCP-fallback messages. A single larger body is allowed only with no other outstanding messages. The media writer checks this budget before removing a new frame from its queue; repair packets bypass that gate and retain priority. A valid cumulative transport ACK frees pending-message and byte credits, then wakes the writer under its queue mutex. This is independent of per-window presentation credits.

Cache: at most 16 messages/32 MiB; pending repair queue at most 4. Receiver: at most 16 messages/96 MiB and a 32-ID lookahead. Over-budget UDP fragments wait for TCP repair. A head TCP repair (`message==next`) delivers directly before draining completed future messages, so a full future-message table cannot reject the repair needed to make progress. The sender's tighter global credits keep valid future-message traffic within receiver budgets. Session close and replacement cancel an in-progress paced body between datagrams, without sending that cancelled body as a TCP fallback. Invalid/unnegotiated peers retain ordinary TCP. No congestion adaptation, FEC negotiation, per-window independent delivery, selective retransmission, IDR-on-loss or endpoint roaming is implemented.

Implementation API references: [ring AEAD nonce requirements](https://docs.rs/ring/0.17.14/ring/aead/struct.Nonce.html), [Windows CNG authenticated mode](https://learn.microsoft.com/en-us/windows/win32/api/bcrypt/ns-bcrypt-bcrypt_authenticated_cipher_mode_info).

## Optional audio connection

Both hellos negotiate `audio_pcm:true`; the server only offers it with `--audio`. The viewer authenticates a separate TCP connection on the same listener using `{"type":"hello","version":3,"token":"...","channel":"audio","media_key":"..."}`. Only one audio channel attaches to the current viewer; it receives `audio_ready`. Failure of this optional channel leaves control/video running. It can use the TLS entry or existing SSH forward. Legacy v3 peers without the capability receive no audio.

Audio packet kind **3** has a 28-byte big-endian header: `u32 rate`, `u16 channels`, `u16 bits`, `u32 frames`, `u64 sequence`, `u64 source_elapsed_us`, followed by interleaved signed **little-endian** PCM. This version emits 480 frames at 48000 Hz, stereo 16-bit (1920 PCM bytes, 10 ms). Timestamps are source-local and do not imply a synchronized viewer clock. Capture spans only the server's private session sink; no microphone or default monitor capture.

The audio socket is duplex: after accepting a block into its bounded playback queue the viewer sends JSON `{"type":"audio_ack","sequence":N}` on **that socket**, with the normal packet framing. The server allows four outstanding packets, including socket buffers; a cumulative ACK must match an outstanding sequence (future ACKs grant no credit). At most four unsent blocks are retained, dropping the oldest. This does not change video presentation credits. The Windows PCM queue is capped at 100 ms, underruns insert silence, and overflow discards oldest samples. No automatic audio socket reconnection or A/V clock synchronization is implemented.

## Windows and input

The optional Linux `--xwayland` backend uses the same v3 window/frame/input formats. It requires no client capability or new binary kind. X11 top-level metadata adds `x11:true`; ordinary X11 toplevels use `type:window`, class as `app_id`, UTF-8 title and optional transient parent. Original client decorations are preserved; undecorated clients receive the native server frame.

Rootless override-redirect menus use `type:popup`, `root`, `parent`, root-relative logical `x/y`, and independent IDs/frames; they never expand the root's frame. `input_popup:false` marks Tooltip/Notification and empty-input visual surfaces, which do not establish a native capture. The compositor publishes `popup_grab` for actual input menus and routes captured owner coordinates to the correct X11 popup surface. Native outside-click/capture cancellation sends Escape only to that root's currently focused X11 keyboard target. Removing the last input popup releases capture even when a Tooltip remains. Unmap/remap uses a fresh remote ID; parent closure and XWM disconnect remove live X11 remote windows without ending native Wayland streams.

X11 keyboard focus is a compositor enum forwarding to Smithay `X11Surface` keyboard targets, including ICCCM InputFocus/WM_TAKE_FOCUS, while the underlying wl_surface carries the Wayland keyboard events. X stacking is raised on focus changes so overlapping X root coordinates do not send mouse input to a different X window. X11 has a shared Xft/client density; individual remote presentation scales remain per window. Clipboard CLIPBOARD bridges UTF-8 text, PNG and URI lists with existing generation checks; PRIMARY and XIM are not implemented. XDND uses the common Source/DndFocus/OfferData lifecycle for native Wayland/X11 sources and targets; XWM pipe I/O is nonblocking but property queries still run on calloop. See [XDND.md](XDND.md) for actions, coordinates, completion and resource limits.

Server `window`: `id`, `title`, `app_id`, `width`, `height` (logical units), `scale`, optional `parent` window ID, `decoration`. The viewer uses `parent` as the native owner for transient dialogs. `decoration:"server"` selects a native Windows title bar after xdg-decoration negotiation; `"client"` keeps the app-rendered title bar and removes the Windows non-client frame. Repeated messages update metadata and can change decoration ownership without recreating the native window. `window_closed` removes the native window. IDs are stable for the lifetime of the host process and restored on reconnect.

Native Wayland decoration requests are honored: ServerSide remains the default, an explicit ClientSide request keeps Chrome's own frame. X11 windows all enter the same persistent output. Xwayland cannot bind native per-window output globals; individual resize/presentation DPI does not alter its shared RandR desktop or client density.

Opt-in `toplevel_drag_v1:true` enables cross-HWND client DnD and `xdg-toplevel-drag-v1`. Server `toplevel_drag` carries `active:true`, `source` (origin window ID), `id` (attached toplevel ID or zero), and surface-geometry-local `offset_x/y`. While active, capture survives origin closure and focus changes are deferred; the attached HWND moves with the cursor and is excluded from drag targeting. Route ordinary pointer/button packets to the native window actually under the cursor, mapping its displayed image to logical coordinates. Use `id:0` for no target, including release outside all windows. Deliver the last motion before button-up, then keep the state until server `active:false`. No-target normal release completes the window move; `toplevel_drag_cancel` (Escape/capture loss/disconnect) explicitly cancels. Detached surface unmap clears the attachment, allowing reattachment. Internal drag state excludes relative pointer locks. The same capture messages are reused for native X11 drags. XDND and native OLE data transfers use the independent source/offer lifecycle described below; the toplevel attachment protocol itself remains Wayland-only.

Frame body: u32 window ID, u32 pixel width, u32 pixel height, u64 frame sequence, i32 logical width, i32 logical height, i32 surface-local origin x, i32 surface-local origin y, then PNG bytes (36-byte header). All integers are big-endian. PNG contains **straight** RGBA; the GLES and Windows composition surfaces use premultiplied alpha. Dimensions are bounded to 8192 per axis and 16,777,216 total pixels before framebuffer allocation/Windows decode.

Top-level geometry excludes invisible CSD shadows and **never includes popups**. Each popup has its own frame/ID, preserving its surface-tree alpha and margins. The origin belongs to the displayed frame and must accompany pointer/drop messages so asynchronous geometry changes cannot shift hit testing. Raw frames and completed display frames use bounded latest-frame queues. All encoded video packets are delivered/decoded in order; dependent P frames must never be dropped as if they were independent images. Backpressure retains the latest raw update and limits queued encoded access units to two per window. Viewers advertising `frame_ack:true` additionally limit total outstanding frames to two per window, including frames already written into TCP buffers. Client `frame_ack` carries `id` and cumulative `sequence`; send after presentation submission (or buffered GDI paint). PNG replacement cancels credits for unsent independent frames; Video reference access units remain ordered. Fallback and closed windows clear their credits. Control messages have priority. Encoding and decoding run outside the compositor/UI event loops.

H.264 kind 2 uses the same 36-byte geometry header, followed by six u32 fields: stream epoch, even coded width, even coded height, keyframe flag (0/1), color byte length, alpha PNG byte length. Annex-B H.264 access-unit bytes follow, then an optional grayscale PNG alpha mask. The server emits an AUD (NAL type 9) at the start of each access unit and removes filler_data (type 12). Earlier v3 servers may include filler; clients can remove it before decoding. On the tested Windows MFT, trailing filler delayed the final picture until another input arrived, even with low-latency mode enabled. Removing filler preserves picture/reference data and sequence timestamps; it does not change frame-credit semantics. Total fixed header is 60 bytes. No mask means fully opaque. Color uses BT.709 limited-range H.264 4:2:0; PNG alpha is full-range lossless. On resize/reconnect or resuming video after an independent PNG, a new epoch starts with an IDR, including SPS/PPS. Bounded server encoder caches may reuse a previous resolution, but reactivation still creates a new epoch and forces this independent IDR. Clients must discard previous-epoch pending metadata and reset reference state (or flush a compatible cached decoder); encoded P frames within an epoch must remain ordered. A mask always accompanies its matching color sequence; video decoding may buffer metadata until that sequence is returned. Client `codec_fallback` with `id` requests PNG for the rest of the viewer session.

HEVC kind **4** and AV1 kind **5** are optional v3 extensions. They reuse the exact
60-byte geometry/epoch/coded-size/key/color-length/alpha-length header above. The
kind identifies the codec; it must match an explicitly advertised client capability.
The server's default `--codec auto` selects H.264 for SDR and HEVC when
HDR is requested, subject to viewer capabilities. `--codec hevc-nvenc` or `--codec av1-nvenc` explicitly
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

With `window_semantics_v1`, a changed X11 CSD ConfigureRequest position during
the application's own held left-button gesture can also emit `move`.
Private X11 root coordinates never become Windows desktop coordinates;
initial placement and requests without that gesture stay local to Xwayland.
The compositor releases the application's input grab and the viewer continues
in the native move loop, preventing feedback at the hidden X11 root's edges.
The viewer retains the original native mouse-down anchor and accounts for
motion that arrived while the request was in transit before entering that loop.
Fullscreen, pointer-lock and active data-drag windows do not use this path.
`unminimize` restores an X11 window while retaining fullscreen, separately from
`restore` for an unmaximize request. EWMH `move`/`resize` start the Windows modal
operation at the current screen cursor position.

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

Default is 8-bit BT.709 SDR. Linux must enable `--hdr experimental-scrgb` with `--codec auto`, `hevc-nvenc` or `av1-nvenc`; Windows must opt in with `WAYBRIDGE_HDR=1`. The server hello reports `hdr`, `color_spaces` (`bt709_sdr`, optionally `bt2020_pq`) and `color_depths` (8, optionally 10). The viewer must explicitly send `hdr:true`, include `bt2020_pq` and advertise `hevc_main10_pq_alpha16` and/or `av1_main10_pq_alpha16`. A codec string alone never enables HDR. Client runtime probes must actually decode a Main10 sample into P010, create usable P010 plane views and verify FP16/scRGB composition support; an active-HDR monitor is also required for that window.

`color_mode {id,hdr:true|false}` selects the monitor policy for a live toplevel. It is subject to the current global HDR preference and negotiated HDR/codecs, never a bypass. Display/move/settings changes re-evaluate the native monitor. No system display setting is changed. An SDR monitor or unsupported path produces SDR; decoding/presentation failures request codec-bound PNG fallback for the rest of the viewer session.

Only Wayland surfaces explicitly tagged through `wp_color_manager_v1.create_windows_scrgb` enter this HDR subset. Linear Rec.709 scRGB uses 80 nits per unit. A true RGBA16F target composites the surface tree; untagged SDR subsurfaces are converted from sRGB to linear before blending. Same-GPU CUDA unpremultiplies and converts to BT.2020 non-constant-luminance/PQ limited-range P010. HEVC kind7 and AV1 kind8 carry 10-bit 4:2:0 Main10 access units with PQ/BT.2020 metadata. Their fixed 60-byte video header is identical to kinds 2/4/5; stream epoch, independent key frame, coded padding, exact sequence pairing and two presentation credits retain the same meaning.

The optional alpha payload is a 16-bit grayscale PNG storing **raw IEEE FP16 alpha bits**, not normalized UNORM16 opacity; no mask means alpha 1.0. The viewer decodes it without quantization, converts P010/PQ back to linear scRGB, premultiplies and presents an FP16 DirectComposition surface. Successful HDR color stays on the GPU; transparent alpha still reads back two bytes/pixel, with GPU copies and synchronous waits. A GPU-generated SDR backup supports accurate current-frame PNG recovery. Returning from PNG to any video codec starts a fresh epoch and independent key frame, including cached sizes.

Unsupported gamut/non-finite input or unavailable HDR falls back to SDR: the supported linear Rec.709 path uses Reinhard `rgb/(1+luminance)` then sRGB. This subset does not advertise ICC or arbitrary parametric creators, general color-managed desktop support, HDR X11, broad-gamut mapping or 4:4:4. SDR output/preferred descriptions stay SDR. Physical Windows/WoA Main10 MFT and HDR display behavior require device validation; software shader arithmetic alone does not establish them.

## Optional native window semantics

A v3 viewer opts in with `window_semantics_v1:true` in its first hello. The server echoes the negotiated boolean and advertises the feature; all fields/messages in this section require that agreement. Older v3 viewers keep their original geometry/control behavior and are not advertised xdg fullscreen capability. Minimize and maximize capabilities remain available; window-menu capability is not advertised.

Negotiated `window` metadata additionally contains `min_size:[w,h]`, `max_size:[w,h]` (logical units, zero means unspecified; normalized to at most 8192 per axis), `modal` (the genuine `xdg-dialog-v1` hint), `fullscreen`, `fullscreen_epoch`, `mapping_epoch`, and `input_region`. `parent:null` explicitly removes ownership. Parent changes apply to existing HWNDs; missing/unmapped parents are cleared, parent unmap/destruction reparents surviving transient toplevels to the live grandparent or clears their parent. A transient parent alone never means modal. A mapped modal child blocks input to its direct owner, redirects that owner's activation, and releases it when the hint/object/child is removed. Other independent windows stay usable.

`window_action` adds `fullscreen` and `unfullscreen` actions and includes `fullscreen_epoch`. The viewer saves the prior native placement, uses the monitor rectangle and borderless fullscreen style (distinct from maximized work-area bounds), and restores the prior normal/maximized placement. Negotiated `resize` also sends `fullscreen` and `fullscreen_epoch`; only the current epoch can change the fullscreen configure state. Resize replies with a superseded epoch are discarded, including their geometry, so an in-flight normal-size reply cannot make a fullscreen game change display mode again. Application min/max constraints apply to normal/maximized client dimensions, including DPI conversion and native tracking/programmatic resizing; fullscreen dimensions bypass those constraints. Restoring from fullscreen preserves hidden/minimized state.

Server `window_visibility`: `id`, `mapped`, `mapping_epoch`, `fullscreen`, `fullscreen_epoch`, on buffer map/unmap. Native windows are hidden on unmap and reused on remap. Wayland unmap clears the role's parent/modal/title/app_id, min/max/geometry, focus/grabs and requested fullscreen, as required for a new mapping. A remapping application must send its metadata again. Client `window_state`: `id`, `visible`, `minimized`, `mapping_epoch` reports native visibility after the window has first been shown; stale epochs are ignored. Hidden/minimized windows receive xdg `suspended` and low-rate callbacks, skip capture, and release input; restore marks the retained surface dirty for a fresh frame. Already queued media is decoded/retained in order and acknowledged after retention while hidden, so frame credits and H.264 references cannot deadlock restoration. It does not report occlusion by other desktop windows.

An X11 buffer detach preserves `_NET_WM_STATE_FULLSCREEN`; it is not an
xdg-shell role reset. Native fullscreen survives minimize/taskbar restore and
rejects system move/size commands and external position/size changes. Display
and DPI changes refit the monitor rectangle. Image layout uses physical frame
dimensions with rounded extents. A fullscreen frame within one logical pixel
of the monitor in each axis fills the native client rectangle to absorb X11
fractional-DPI quantization; other aspect ratios retain letterboxing. Input and
IME mapping use that same image rectangle.

Legacy X11 borderless placement also uses native fullscreen when the application
requests a normal, non-transient, non-maximized window without WM decorations
at the shared output origin and with exactly the output's logical dimensions.
This covers borderless games which do not set `_NET_WM_STATE_FULLSCREEN`.
The native resize acknowledgement does not add that EWMH flag. Application
geometry/decoration requests update this inferred placement; compositor-generated
ConfigureNotify events do not re-infer fullscreen after an explicit exit.

`input_region` is independent of visual alpha. It is null for conservative default hit testing, or `{origin_x,origin_y,width,height,layers:[{x,y,width,height,rects}]}` in root surface logical coordinates. Each layer is a mapped root/subsurface; `rects:null` accepts its full bounds, `[]` accepts none, otherwise ordered `[add,x,y,width,height]` operations add/subtract within its bounds. Union the layers. Limits are 64 layers and 256 operations total; excessive/invalid regions and regions whose geometry does not match the displayed frame fall back to accepting input. The Windows viewer preserves non-client/CSD resize hit tests and returns `HTTRANSPARENT` for excluded client pixels. Win32 only forwards that result to underlying windows in the same UI thread: this supports overlapping Waybridge windows, **not general click-through to other processes or the desktop**. No visual cropping, alpha-derived input shape, or cross-process pass-through is claimed.

## Local Windows IME (optional `ime_v3` feature)

The compositor exposes `zwp_text_input_manager_v3`. Only the enabled text-input belonging to the actual keyboard-focused surface receives composition. `ime_state` control messages contain `enabled`, an opaque `generation`, and, when enabled, target window/popup `id`, `rectangle:[x,y,width,height]` in surface-local logical coordinates and numeric content `purpose`. Neither surrounding application text nor document contents are forwarded.

The viewer returns `{"type":"ime","id":N,"generation":G,"preedit":"nihao","cursor_begin":5,"cursor_end":5}` and/or `commit:"你好"`. Preedit cursor offsets are UTF-8 byte boundaries (or -1), not UTF-16 units. Strings are bounded to 60 KiB each, below the Wayland event size limit. Updates are sent in one text-input done batch with the application's current commit serial. An empty preedit cancels it. The server ends an active preedit using null text/cursor -1 and suppresses redundant clears after it has ended; plain commits do not start an empty GTK composition. A commit without a preedit field also ends the previous composition. Focus/enable generation and actual focused target must match; stale updates are ignored. Enable state and pending client fields take effect only on the application's text-input `commit` request.

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

`window` metadata carries `icon_revision` (monotonically increasing for this window's identity or icon source). The server asynchronously resolves its current runtime/desktop icon, then sends control JSON `window_icon`: `id`, `app_id`, `revision`, `width:128`, `height:128`, and `png` (base64 PNG or `null` for generic application icons). The extension uses bounded ASCII desktop-file IDs; missing/unusable IDs normalize to an empty identity. On metadata identity/revision changes, the viewer clears the old icon immediately. It applies a result only when the window is still alive and both normalized application identity and revision match. Pending old results and messages for unknown/closed IDs are ignored. Reconnection reannounces metadata before the current icon.

PNG is straight RGBA, normalized with aspect ratio and transparent padding to 128×128, at most 96 KiB decoded PNG / 128 KiB base64. The viewer validates exact PNG dimensions before copying pixels, constructs per-DPI large/small native `HICON`s and sets both with `WM_SETICON`. Replaced/closed-window custom icons are destroyed; system fallback handles are shared. Windows DPI changes rescale from the bounded master without a round trip.

Runtime pixels have priority: committed Wayland `xdg-toplevel-icon-v1` buffers,
then its theme icon name, or X11 `_NET_WM_ICON`; otherwise use the desktop icon.
Wayland `set_icon` copies one bounded SHM buffer into pending state and applies it
only at the toplevel's surface commit. The icon object and its buffers may be
destroyed immediately after set_icon without losing this snapshot. Clearing or
assigning an empty icon restores the desktop/default source. SHM ARGB8888 is
unpremultiplied on the worker; XRGB8888 is opaque. Runtime dimensions are at most
512×512; one snapshot is at most 1 MiB, with at most 64 supplied Wayland size
candidates considered. X11 property reads are bounded to 2 MiB and run on a private
worker connection with bounded connect/setup/reply waits; malformed, oversized or
missing data falls back to desktop/default. `_NET_WM_ICON` and WM_CLASS changes
invalidate the old revision without synchronous property reads on dispatch.

Wayland desktop fallback first uses its exact XDG desktop-file ID, then the
standard StartupWMClass/desktop-ID compatibility lookup for noncanonical app_ids.
X11 matches WM_CLASS class then instance against `StartupWMClass`, before desktop-ID
case matching and a final case-insensitive StartupWMClass match. Equal-score
matches honor XDG data-directory priority and stable desktop-ID ordering;
Hidden/user entries mask the same system desktop ID. No process names, window
titles or application-specific rules select an icon.

Resolution runs off the compositor thread: XDG applications lookup, desktop-file `Icon`, selected GTK/KDE theme (or `WAYBRIDGE_ICON_THEME`), theme size ranking/inheritance, hicolor and unthemed/pixmaps fallbacks. Desktop `Exec` is never executed. Source files are bounded to 2 MiB; PNG dimensions to 1024×1024 / decoder allocation to 16 MiB. SVG support uses optional `rsvg-convert`, limited to 2 seconds wall/CPU, 512 MiB virtual memory and 96 KiB output, and killed if its parent exits. Failure returns generic icons. Desktop lookup and transmission follow the first 128 live toplevels, matching the viewer window limit. Worker and transport queues replace pending work per window (maximum 128); queued runtime pixel payloads are additionally limited to 16 MiB; the worker has a 64-entry positive/negative LRU cache with 60-second lookup TTL. Themes/configuration are sampled at server startup; existing windows do not hot-reload icon files. XPM and legacy ICCCM WM_HINTS icon pixmaps are not decoded; standard runtime/desktop fallback still applies.

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
pointer in its input/constraint region, and no popup grab or outbound file drag is active, the server sends
`{"type":"pointer_lock","id":1,"generation":7,"active":true}`. `id` is the root
window; generation is a nonzero server-process-lifetime u64 and changes for every
request, including reconnect/recreation. The viewer must verify native focus and
establish Raw Input, a hidden cursor and native clipping before replying
`{"type":"pointer_lock_ack","id":1,"generation":7,"active":true}`. Only then does
the compositor send Wayland `locked`. A native failure replies `active:false`.
Native OLE input ownership also rejects new locks. Before entering DoDragDrop the
viewer releases any previous lock; delayed requests cannot re-clip the OLE cursor.
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
publishes only complete destination-local paths through a read-only FUSE view;
reads use the already-downloaded private cache, not network-lazy FUSE or remote
IStream backing. File and directory modes remain 0600/0700 so saved copies are
ordinarily writable; the RO mount rejects overwrite/create/rename/remove. Successful imports retain the
same private inbox lifetime as filesystem tree imports, and never delete source
items. Windows/Outlook/archive-provider compatibility requires real-Windows tests.

Completed Windows CF_HDROP caches are protected by explicit user DACLs denying
write/delete and directory child deletion, not by FILE_ATTRIBUTE_READONLY. Copies
through Explorer/Shell inherit ordinary destination permissions. A filesystem that
maps those ACLs into READONLY (currently Wine) is rejected with a transfer
`download_error`; no CF_HDROP is published and unpublished staging is removed.
OLE FileContents streams expose STGM_READ and reject Write/SetSize. Cache lifetime
permits delayed reads and viewer disconnect; only the transfer's incomplete or
unpublished objects are cleaned. This is a delivery contract for normal desktop
applications, not a same-user/admin security boundary.

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

Completed Linux clipboard/drop trees are published under the read-only view
`$XDG_RUNTIME_DIR/waybridge-inbox/readonly/<unique-batch>/`; Windows materialized
inputs remain in protected `%TEMP%\Waybridge-tree-*` caches. Applications can
read them later. Managed writable staging is not a public editing destination;
caches are retained across viewer disconnect and need lifecycle-aware cleanup. Copy/cancellation never deletes source files. Virtual directory
descriptors and all four directions are tested with real Wayland and native OLE
probes under Wine/Xvfb; Windows Explorer/third-party compatibility still needs
Windows 10/11 device validation.

## Live stream settings and GUI worker control

The v3 hello advertises `stream_config_v1`. A negotiated viewer may send
`stream_config {request_id,fps,bitrate,codec,hdr,read_only}`; FPS is 1–120,
bitrate is 1000–200000 kbps **per video window**, and codec is
`auto|h264|hevc|av1|png`. The reply has the same type/request ID and `ok`;
success includes accepted settings, `requested_codec`, `hdr_requested`, and the resolved `codec`; failure includes `message` and changes nothing. Optional `hdr` preserves the prior preference when absent.
The initial server `--codec png` remains a hard PNG policy: a later video request returns PNG with `fallback_reason:"session_video_disabled"`. Auto selects H.264 for SDR and HEVC when HDR is requested; viewer capability/fallback and per-window HDR selection still govern actual frame encoding. HDR-only preference changes also invalidate capture and reconfigure encoding.
Changing FPS/bitrate/codec clears cached encoder sessions on the encoder worker;
new video starts with a fresh epoch and independent keyframe. Already in-flight
frames remain valid and must still be acknowledged. Entering read-only releases
held input and rejects pointer, relative motion, buttons, scroll, keys and IME.
`file_drop_out_enabled {enabled}` changes outbound drag availability and cancels
an active outbound operation when disabled.

The optional Windows launch form `waybridge-client.exe --control-pipe NAME`
connects to a randomly named, current-user-only pipe owned by the GUI. It accepts
UTF-8 newline-delimited JSON, at most 64 KiB per request. The first request is
`{type:"start",version:1,host,port,token,settings,transport?,source_id?}`. Optional `transport` is `{type:"tls",fingerprint,client_id,client_token,session}`; `host`/`port` then identify the TLS entry and `token` remains the backend v3 credential. Without it the worker uses the legacy socket transport. Authentication stays inside
the pipe; it is absent from the worker command line. Subsequent commands are
`settings {settings}`, `status`, `send_files {paths}`, `focus_window {id}` and
`disconnect`. Pipe EOF stops the local worker without terminating remote apps.

The optional local `source_id` is the GUI's persistent connection-profile ID;
without it the worker namespaces application taskbar identities by host/port.
It is never an authentication credential or sent to Linux. Local settings include
`tray` and `notifications` (default true), plus `badge_png` (empty for the brand
badge, otherwise a normalized 64×64 PNG, at most 24 KiB / 32768 base64 characters).
Badge bytes stay in the Windows profile/worker; per-application masters are kept
separate from composed window and tray icons. Notification logos are composed
when created/replaced, without re-alerting old notifications on appearance edits.

Worker events include sanitized `connected` capabilities (never media keys),
`stream_config` acknowledgements, `local_settings_applied`, `settings_warning`,
`error`, `status` (actual per-window codec/GPU/presentation, TCP bytes and presented
updates), `timings`, and `transfer` metadata. Local application and remote
configuration acknowledgements are separate, asynchronous events. Telemetry has
a bounded writer queue so a blocked GUI cannot stall video. The pipe contains no
video, audio, clipboard payloads or file chunks. Settings for HDR, UDP and audio
output devices require reconnect; GPU input/HDR/UDP server capabilities additionally
depend on how the Linux session was created.

The SSH helper uses its own bounded NDJSON RPC with request IDs. Supported
operations are `hello`, `list`, `ensure`, `info`, `applications`, `launch` and
`end`. Only `ensure` returns the token over the authenticated SSH channel;
list/info/catalog never include it. Session descriptors remain version 1 and
add optional `connection {listen,token_file}` for new GUI attachment, retaining
old CLI launch compatibility. Descriptors and tokens must be same-user private
regular files. A pidfd plus `/proc` start identity pins an explicit shutdown to
the expected server. Detaching the SSH helper leaves daemonized sessions alive.

Managed application launch in both TLS and SSH modes uses the same helper and shared user HOME/data/user bus. Session admission rejects a second graphical desktop for the same user before creating Waybridge. Browser/wallet backend choices are explicit user configuration; no Chrome profile or password-store arguments are inserted automatically and no application-name conflict scanner is used. GIO desktop launch applies the explicit Waybridge desktop append/override configuration while preserving field expansion; low-level compositor `--launch` retains literal argv behavior. See SESSION_POLICY.md and CONFIGURATION.md for the current contract.

## Remote application trays (`tray_v1`)

Both hellos explicitly set `tray_v1:true`; this negotiates the adapter protocol,
not desktop availability. `tray_status` reports `available`, a diagnostic
`message`, `mode:watcher_host|existing_watcher_host`, and `xembed:false`.
An older peer receives no tray snapshots/icons and its actions are ignored.

The server sends reliable `tray_snapshot` messages with `{epoch,items}`. Each
item has `{id,generation,app_id,title,status,menu,item_is_menu,icon_revision}`;
status follows SNI's Active/Passive/NeedsAttention. A snapshot replaces the list,
including background-only and Passive items. `id` is an opaque SNI identity, not
a process ID. `epoch` changes on viewer replacement/refresh; `generation`
changes when the source bus owner changes. Both must be echoed by every action.
Snapshots contain no image bytes and have a 256 KiB serialized ceiling.

`tray_icon {epoch,id,generation,revision,width:128,height:128,png}` carries a
normalized PNG (or null for the placeholder), only when its revision changes or
a viewer reconnects. `revision` must match the current item's `icon_revision`.
These messages use a separate latest-per-item queue: at most 64 entries, 132 KiB
per JSON message and 512 KiB total pending bytes. Control/input acknowledgements
remain ahead of the icon queue. Overflow reports `tray_warning`, keeps the
placeholder, and retries on the next adapter poll; it does not resend every icon
when a title or status changes.

Client actions are `tray_action {epoch,id,generation,action,...}`:

| Action | Extra fields | Linux operation |
|---|---|---|
| `activate` | none | SNI Activate(0,0) |
| `secondary` | none | SecondaryActivate(0,0) |
| `scroll` | `delta` (-1200..1200), `orientation` (`vertical`/`horizontal`) | SNI Scroll |
| `menu` | none | AboutToShow + DBusMenu GetLayout, or constrained ContextMenu fallback |
| `menu_event` | `revision,menu_token,node,timestamp` | DBusMenu Event(node,"clicked",0,0) |

`tray_menu` echoes `{epoch,id,generation,revision,menu_token,root}`. A node has
`{id,label,enabled,visible,separator,toggle,checked,children}`; toggle is the
application's checkmark/radio value. At most 256 nodes and 8 nested levels are
accepted. A menu token belongs to one exported menu. The server verifies the
current bus owner, exported token/revision, current application layout revision,
and enabled/visible state of the node **and every ancestor** before dispatch.
A successfully dispatched token is consumed. The legacy `timestamp` field is accepted for compatibility but not forwarded: Windows uptime belongs to a different clock domain, so the Linux adapter uses DBusMenu's unknown-time value `0`. Read-only input policy also applies
to tray activation, scrolling, menus and menu actions, including queued work.

For an SNI without DBusMenu, ContextMenu is called only when its DesktopEntry/Id
matches an already mapped remote window's app ID. It returns
`tray_menu {epoch,id,generation,fallback:"remote_window"}`; the application draws
that menu through its own remote window. Otherwise a visible `tray_error`
explains the missing path. The bridge does not manufacture a Quit operation;
normal exit uses a menu action supplied by the application.

With `activation_v1`, user tray actions may include a 32-hex
`activation_request`. The native client records the corresponding local user
intent; the worker resolves the SNI unique bus owner's kernel PID and asks the
compositor to bind a one-shot owner grant before calling the application.
Registration alone does not raise a window: a subsequent application activation,
restore or new map must consume the grant. Menu selection creates a fresh grant
after the native menu closes. Disconnect/read-only checks also guard this step.

`tray_refresh` requests a fresh snapshot/epoch. This is a read-only operation;
normal snapshots and icon deltas are automatic. Action payload fields and sizes
are bounded before entering the 64-slot worker queue. Session lifecycle state
uses a separate latest state/revision, so reconnect/disconnect cannot be dropped
behind a full action queue.

## Window activation (`activation_v1`)

Visibility, Linux keyboard focus and Windows foreground activation are separate.
Both hellos negotiate `activation_v1`; the native worker also reports the actual
negotiated boolean to the GUI. A supported new window is initially shown without
activation. The server sends `window_activate {id,origin,request_id}` only for a
specific intent, after window metadata exists. The worker can defer it until the
first displayed frame. Successful Windows focus returns through the existing
`focus` message; Linux does not preemptively move keyboard focus on a request.

For a GUI launch, the foreground GUI grants foreground permission to its worker
with AllowSetForegroundWindow, then requests `activation_prepare
{request_id,app}` through the worker pipe. The server returns
`activation_prepared {request_id,token}`. Management launch carries this token
in the normal application launch context, and `activation_launch
{request_id,pid?}` binds the resulting process to that same connection.
The helper reports raw-spawn PID or the real application PID from GIO's standard
`GAppLaunchContext::launched` signal, never its dispatcher PID.

Wayland applications can use standard `xdg_activation_v1` tokens. Client-created
tokens require a recent input/focus serial on the correct seat and focused
client; tokens are consumed once. Explicit remote launch tokens are also usable
as XDG_ACTIVATION_TOKEN/DESKTOP_STARTUP_ID. GIO launch uses a standard derived
LaunchContext so D-Bus Activate/Open/ActivateAction receive startup platform data.
For this one token-bearing launch, its private derived Desktop Entry enables
StartupNotify; installed entries and persistent overrides remain unchanged.
Process PID/descendants provide an exact fallback for exec applications without
activation support; D-Bus launches without a reported PID can match their desktop
application ID. A new transient of the focused parent and a child process started
after recent input inherit only that source window's local foreground context.

The SNI integration can register an owner grant with a verified D-Bus owner PID.
Unlike GUI launch, registering that grant does not activate an existing mapped
window: a subsequent new map, xdg activation or X11 activation/restore request
must consume it. A mute/quit menu action therefore does not itself switch apps.
An unrelated X11 request has only attention semantics; its EWMH source value or
timestamp cannot grant foreground authority.

Tokens/grants expire after 15 seconds, are bounded, and are invalidated by a
connection change. The native worker additionally rejects a launch if newer
local input changed the user's foreground context. It uses normal foreground
APIs and taskbar flashing on denial, with no input-queue attachment, synthetic
input or permanent topmost window. Read-only input policy still applies.

A modal's mapped=false transition usually precedes its destruction. Before
hiding/destroying a foreground modal, the viewer reconciles enabled owners and
restores its valid owner while it still has foreground permission. Closing a
background dialog does not disturb the foreground app. Linux also retains the
mapped parent as its logical focus target and ignores focus events for stale IDs.

## Desktop notifications (`notifications_v1`)

Both v3 hellos negotiate the boolean `notifications_v1`. The hosted Windows
worker initially sends `notifications_enabled:false`; forwarding opens only
after its GUI has registered native Windows AppNotifications, processed the
current connection and returned `notification_available {available:true}` over
the private host pipe. Standalone workers do not negotiate this capability.
`notifications_enabled {enabled}` changes the live connection's sharing policy;
it never starts or replaces a Linux notification service. The profile setting
`notifications` defaults to true and is independent of the Linux provider choice.

The Linux session emits:

- `notification_status {available,reason,message}`: provider selected, not
  selected, conflicting existing server, diagnostic failure or lost provider.
  A bus owner alone is never treated as a forwardable source.
- `notification {instance,binding,id,revision,app_name,summary,body,actions,
  timeout_ms,png}`: actions are `{key,label}` pairs; `png` is a base64 PNG or
  null. A nonzero `replaces_id` uses the same ID with a new revision. Windows
  uses the same native Tag/Group to replace it without first removing it.
- `notification_close {instance,binding,id,revision}` and `notification_reset`:
  remove the matching native item or all items from that worker connection.

The GUI returns `notification_feedback {instance,binding,id,revision,action}`
or the same identity with `reason:2|4`. The native worker and Linux bridge both
validate current identity. The provider additionally checks the original unique
D-Bus owner, allowed action and unexpired revision before emitting the standard
`ActionInvoked`. Read-only mode permits display/dismissal but converts an action
to dismissal at both the native and Linux boundaries. A valid action is followed
by `NotificationClosed(id,2)`; application close is reason 3, expiry reason 1,
and lost/disabled connection or failed platform presentation reason 4. Signals
are addressed to the originating unique bus owner. Unknown CloseNotification
IDs return a D-Bus error. Previous revisions, owners, provider instances and
connection bindings cannot invoke current applications.

The explicit Linux provider implements `org.freedesktop.Notifications` at its
standard object path, including Notify, CloseNotification, GetCapabilities,
GetServerInformation and the two signals. Advertised capabilities are `body`,
`actions`, and `icon-static`; body markup, inline replies, persistence, sound,
action icons and animation are not advertised. At most four buttons plus the
default click action are accepted. Default expiry is 10 seconds; explicit zero
has no provider expiry, while positive expiry is capped at 24 hours. Windows
notification policy can still remove such an item.

There are at most 64 active items, 16 Notify updates per second, 32 hints and
60 KiB per private provider packet. Text limits are 256 UTF-8 bytes for app name
and each action key/label, 1024 for summary and 8192 for body. Invalid/oversized
requests fail explicitly. Icons use standard image-data/image-path, app icon or
desktop-entry lookup; only local files/theme entries are read, never remote
image URLs. Raw images are bounded to 256×256; normalized PNGs to 128×128 and
32 KiB. The native worker scales the original icon to 64×64 before passing it
to the GUI and supplies a plain placeholder if no icon is available.

The provider's 0700 session directory holds 0600 Unix datagram sockets. A random
provider instance and per-viewer binding prevent stale IPC actions. Heartbeats
expire after three seconds, clearing old items. No disconnected notification
history is replayed. GUI notification work enters a bounded queue before UI
dispatch; an overflowing GUI/worker pipe fails closed instead of silently losing
interactive lifecycle edges. Notification text and activation arguments are not
written to diagnostic logs.

Windows uses AppNotificationManager and the native notification center, retaining
OS notification settings and Do Not Disturb behavior. AppNotifications has no
Dismissed event; disappearance is checked with GetAllAsync every two seconds,
with a further two-second activation grace period before sending dismissal.
Expiry/application close and disconnect cleanup do not depend on that polling.

## Generic drag visual (`data_drag_v1`)

A client can provide a standard `wl_surface` icon to `wl_data_device.start_drag`.
With data_drag_v1 negotiated, this separate surface is captured as visual state,
not as another application window and not by application-specific adapters.
`drag_visual {generation,revision,active,visible?,format?,width?,height?,scale?,
offset_x?,offset_y?,pixels?}` is latest-only/coalesced on both network endpoints.
The bitmap format is base64 `rgba8-premultiplied`; offsets are surface-local
logical coordinates relative to the pointer, including attach/offset deltas and
the subsurface bounding-box origin. `scale` describes the captured pixels per
logical unit. `active:false` ends that generation permanently; visible:false
hides an empty/unsupported icon while retaining its normal source DND lifecycle.

Capture source buffers and the logical bbox are limited to 512 per edge,
65536 pixels and 16 surfaces; DPI-scaled capture and native display are also
bounded to 65536 pixels. A private packet is at most 352 KiB. Oversized/malformed
icons are hidden, without changing the offer or making a drag invalid. Bitmap
capture happens only after surface commits and at most 30 times per second;
source frame callbacks advance independently of the application stream FPS, so
an animated drag icon continues even with a static/slow application window.

The native viewer uses one temporary layered, mouse-transparent, NOACTIVATE
tool window, positioned by a controller-owned timer. Only this ephemeral ghost
is topmost. It never changes application foreground/input focus or creates a
taskbar entry. Native hit testing ignores the ghost itself. End/cancel, source
or icon destruction, connection failure and feature reset remove it and stop
the timer; older generations/revisions cannot resurrect a closed image.
Drag-end controls also replace a pending bitmap with a coalesced reset marker,
so a stale image waiting in the UI inbox cannot reappear after that end.

### Generic MIME drag negotiation (`data_drag_v1`)

A viewer opts in in its initial authenticated hello; the server echoes the
capability. This enables pure UTF-8 text export, capture/routing of all ordinary
Wayland data drags between independent native windows, return from OLE to the
original Wayland source, and bounded `drag_visual` feedback. Legacy peers retain
their previous file and toplevel-drag paths.

- A text `drag_offer` uses the existing `drag`, source `id`, `action:"copy"`
  fields, `files:[]`, `mime` and base64 `text` (at most 1 MiB UTF-8, no NUL).
  It becomes native `CF_UNICODETEXT`, without modifying the clipboard.
- `text_drag_enter {drag,id,x,y,origin_x,origin_y,data}` offers bounded base64
  UTF-8 to a remote target. `text_drag_motion`, `text_drag_drop` and
  `text_drag_cancel` reference the viewer's positive gesture ID. The target
  answers `text_drag_status {drag,accepted}` only for current negotiation;
  acceptance requires a supported MIME and COPY action. Coordinates follow
  ordinary pointer geometry/origin rules. Read-only, disconnect and target
  closure invalidate the gesture. X11 targets are routed through private XDND.
- Server hello `data_drag_result_v1:true` extends this exchange. Enter, motion,
  drop and cancel carry `target_epoch`; status echoes it and adds
  `status:"pending"|"accepted"|"rejected"`. Pending is distinct from a completed
  rejection; acceptance requires the target's supported MIME and COPY action.
  `text_drag_retarget {drag,target_epoch,id,x,y,origin_x,origin_y}` uses a strictly
  newer revision to invalidate the previous native target offer and renegotiate
  the current point without retransmitting text. It is valid only during hover.
  Other messages must match the current gesture and revision. Native HWND
  callbacks have a separate local lease, so a reentrant callback cannot cancel
  the same source gesture after it has moved to another target.
- With that server capability, `text_drag_finished {drag,target_epoch,success}`
  reports the target protocol Finish/cancellation, rather than merely receipt
  of a drop command. The Windows adapter pumps its STA while negotiating for
  at most 750 ms, and waits at most 2 seconds after drop for completion before
  returning COPY. It reports NONE on timeout and does not retry an uncertain
  delivery. These deadlines do not guarantee arbitrary-latency completion or
  rollback of text already accepted by an application. Old servers retain the
  original exchange; clients send retarget only with the advertised capability.
- Client hello `drag_text` and live `text_drag_enabled {enabled}` independently
  enable cross-device plain-text drags (default true). File sharing and clipboard
  direction do not gate text dragging. Read-only policy overrides the setting;
  disabling it cancels active external text drags. Native drags wholly inside
  the Linux session retain their original source, MIME types and actions.
- `drag_return {drag,return_token,id,x,y,origin_x,origin_y}` returns an active
  native OLE drag to the original Wayland source. The monotonic token scopes
  `drag_return_ready {drag,return_token,accepted}`; stale replies cannot release
  a later handoff. The client cancels only that OLE attempt, invalidates its old
  IDataObject/IStream leases and resumes pointer capture. It defers release
  briefly for target negotiation, rather than reporting a cancelled source as
  a newly uploaded successful drop.

Compositor-owned offers carry internal source cookies: a dropped offer may read
its own immutable payload after a newer gesture begins, and its late Finish
cannot clear that newer gesture. Selection pipe writers have bounded concurrency
and write deadlines. These internal identities are not filenames or application
compatibility rules. See [DESKTOP_INTERACTIONS.md](DESKTOP_INTERACTIONS.md) for the
precise direction, MIME, action and hover coverage.

## Native touch (`touch_v1`)

Client hello explicitly requests `touch_v1:true`; server hello echoes negotiated
availability. Capability is immutable for the connection; the separate boolean
`touch_enabled` in the initial client hello selects the saved preference. Missing
preference defaults to enabled for legacy clients that explicitly negotiate
`touch_v1`; a missing capability never enables touch. The hosted Windows worker
always sends its saved `settings.touch` preference (default false). Native
Wayland window metadata includes `touch_input:true`; X11 reports false and keeps
pointer compatibility. The experimental Windows implementation is configured
per connection in the control center; the raw viewer also accepts `--touch`.

The server echoes `touch_settings_v1:true` when touch is negotiated. A negotiated
client can then send `touch_enabled {enabled:bool}` to change its preference
without reconnecting. Disable cancels contacts and invalidates the lease; enable
issues a fresh generation under the usual focus/visibility/read-only rules. The
capability cannot be acquired by a later hello or preference message. Against
older servers the worker preserves its current live preference and emits a
`settings_warning` requiring reconnect for the newly saved choice.

Server `touch_state {id,generation,enabled,requested,max_contacts:10}` reports the
connection preference as `requested`; `enabled` additionally requires an eligible
focused target. It binds input to the
current mapped/visible/input-allowed native Wayland root and mapping epoch.
Client `touch {id,generation,origin_x,origin_y,points:[...]}` contains up to ten
unique slots 0–9 with `phase:"down"|"motion"|"up"`; down/motion carry finite
logical x/y. Coordinates use the ordinary image/crop-origin transform. A batch
is validated in full before dispatch and ends in one `wl_touch.frame`. Motion
and up retain each contact's initial surface. `touch {id,generation,action:"cancel"}`
cancels the entire gesture and invalidates its lease. Policy changes, input
ownership, unmap/death, focus changes and disconnect release contacts; old
leases cannot affect a newer mapping/focus.

An initial Windows down may precede the server focus lease. The native adapter
buffers at most 64 events for 500 ms. An otherwise-disabled state may include
`pending_target:<id>` only for a single explicit focus handoff; this permits an
unsent first gesture for exactly that current HWND to await the matching grant.
Unmarked disable, another-target grant, wrong-target handoff or local focus loss
purges pending input. Read-only, explicit cancel and disconnect are unmarked;
regranting the same HWND cannot replay its revoked old tap. See
[INPUT_DEVICES.md](INPUT_DEVICES.md) for device/application limits and tests.

Touch interaction support is separately negotiated with `touch_interaction_v1`.
Normal touch packets carry `gesture` and concrete `target` (root/popup); `id` and
`generation` remain the root lease. Server `touch_interaction` starts a move,
resize or DND with `operation`, `kind`, root `id`, original `generation`,
`gesture`, owning `slot` and optional `edge`. `touch_interaction_ack` accepts only
the still-held owner. Client `touch_interaction_input` carries that operation,
gesture, slot, `phase:"motion"|"up"|"cancel"` and DND target coordinates; only
matching ownership can change the operation. Server `kind:"end"` retires it.
Native DND and popup routing preserve ownership through focus changes; policy
revocation and invalid release targets cancel. DND export/toplevel metadata tags
`input:"touch"` and `touch_operation`, `touch_generation`, `touch_gesture`,
`touch_slot`, echoed on external claim/return/drop/result when available. See
[INPUT_DEVICES.md](INPUT_DEVICES.md) for routing and physical verification limits.

UDP framing and authenticated ACK/repair messages are unchanged. Adaptive pacing
uses those existing feedback messages, with local send-data boundaries and
separate bounded transport diagnostics; see [NETWORK.md](NETWORK.md). Media
resource budgets are local policy and introduce no wire fields.

## Optional professional input v1

Initial hello negotiates `pen_v1` / `pen_enabled` and `gamepad_v1` /
`gamepad_enabled`. The server echoes supported capabilities and
`pen_settings_v1` / `gamepad_settings_v1` for live boolean preference messages
`pen_enabled` / `gamepad_enabled`. Missing capability or preference means off.
Read-only and focus policy remain authoritative.

`pen_state` grants a root-window generation; `window.pen_input` reports a live
same-client tablet-seat subscription. `pen` packets carry root `id`, concrete
`target`, `generation`, `gesture`, monotonically increasing `sequence`, logical
`tool`, `eraser`, `phase` (`in`, `motion`, `out`, `cancel`), `tip`, `barrel`,
logical `x/y`, crop `origin_x/y`, optional valid normalized `pressure`,
`tilt_x/y` and `rotation`, plus observed `capabilities`. The server validates
axes and exact owner before publishing tablet-v2 events. `window_action` with
`input:"pen"` binds CSD move/resize to `pen_operation`, `pen_generation`,
`pen_gesture`, `pen_tool` and `pen_eraser`; `pen_state.window_operation` marks
authorized lease retirement. `pen_window_end` must match those identities.
An unmarked policy revoke cancels manipulation. See [PEN.md](PEN.md).

`gamepad_focus` supplies the actual local foreground root `id`; it must match
server focus. `gamepad_state` grants `id` and `generation`, plus `supported`,
`requested`, `enabled`, `devices`, `rumble_supported` and `error`. `gamepad`
snapshots bind `id`, `generation`, `slot` (0–3), `device_generation`,
`sequence`, `connected`, `buttons`, signed `lx/ly/rx/ry` and byte `lt/rt`.
`gamepad_rumble` echoes generation, slot and device generation, with 16-bit
`strong`/`weak` and a bounded `duration_ms` heartbeat. Revoke/disconnect
neutralizes and destroys owned devices; stale feedback cannot vibrate a new
incarnation. See [GAMEPADS.md](GAMEPADS.md).
