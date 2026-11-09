# Waybridge protocol v3

All packets begin with a big-endian u32 length counting the kind byte and body. Length must be in `1..=75497472`. Kind `0` is a UTF-8 JSON object; kind `1` is a PNG RGBA frame; kind `2` is H.264 with a lossless alpha mask. Unsupported or malformed framing closes the connection.

First client message: `{"type":"hello","version":3,"token":"...","dpi":168,"work_width":3840,"work_height":2080}`. The server verifies authentication and exclusive viewer ownership before announcing protocol version, codec, and capabilities. This transport is meant to run through an SSH tunnel. The server advertises `codec:"mixed"`, supported `codecs` and a random per-session `media_key`. Both ends must use v3. Clients advertise `codecs:["png_rgba","h264_alpha"]` when hardware decoding is available, otherwise PNG only. DPI defaults to 96; work dimensions are physical monitor work-area pixels.

## Media connection

A viewer requesting `split_media:true` opens a second TCP connection to the same address and port. Authenticate it with `{"type":"hello","version":3,"token":"...","channel":"media","media_key":"..."}`. Only the current viewer's random key is accepted, and only one media socket attaches. The server replies `media_ready`, then sends frame kinds 1/2 over this socket. Input, window control, clipboard and file messages stay on the control socket. Socket failure closes the session; reconnect restores live windows. Existing SSH forwarding carries both connections. Test/simple viewers may omit `split_media` to multiplex on one socket.

## Optional audio connection

Both hellos negotiate `audio_pcm:true`; the server only offers it with `--audio`. The viewer authenticates a separate TCP connection on the same listener using `{"type":"hello","version":3,"token":"...","channel":"audio","media_key":"..."}`. Only one audio channel attaches to the current viewer; it receives `audio_ready`. Failure of this optional channel leaves control/video running. It can use the existing SSH forward. Legacy v3 peers without the capability receive no audio.

Audio packet kind **3** has a 28-byte big-endian header: `u32 rate`, `u16 channels`, `u16 bits`, `u32 frames`, `u64 sequence`, `u64 source_elapsed_us`, followed by interleaved signed **little-endian** PCM. This version emits 480 frames at 48000 Hz, stereo 16-bit (1920 PCM bytes, 10 ms). Timestamps are source-local and do not imply a synchronized viewer clock. Capture spans only the server's private session sink; no microphone or default monitor capture.

The audio socket is duplex: after accepting a block into its bounded playback queue the viewer sends JSON `{"type":"audio_ack","sequence":N}` on **that socket**, with the normal packet framing. The server allows four outstanding packets, including socket buffers; a cumulative ACK must match an outstanding sequence (future ACKs grant no credit). At most four unsent blocks are retained, dropping the oldest. This does not change video presentation credits. The Windows PCM queue is capped at 100 ms, underruns insert silence, and overflow discards oldest samples. No automatic audio socket reconnection or A/V clock synchronization is implemented.

## Windows and input

Server `window`: `id`, `title`, `app_id`, `width`, `height` (logical units), `scale`, optional `parent` window ID, `decoration`. The viewer uses `parent` as the native owner for transient dialogs. `decoration:"server"` selects a native Windows title bar after xdg-decoration negotiation; `"client"` keeps the app-rendered title bar and removes the Windows non-client frame. Repeated messages update metadata and can change decoration ownership without recreating the native window. `window_closed` removes the native window. IDs are stable for the lifetime of the host process and restored on reconnect.

Frame body: u32 window ID, u32 pixel width, u32 pixel height, u64 frame sequence, i32 logical width, i32 logical height, i32 surface-local origin x, i32 surface-local origin y, then PNG bytes (36-byte header). All integers are big-endian. PNG contains **straight** RGBA; the GLES and Windows composition surfaces use premultiplied alpha. Dimensions are bounded to 8192 per axis and 16,777,216 total pixels before framebuffer allocation/Windows decode.

Top-level geometry excludes invisible CSD shadows and **never includes popups**. Each popup has its own frame/ID, preserving its surface-tree alpha and margins. The origin belongs to the displayed frame and must accompany pointer/drop messages so asynchronous geometry changes cannot shift hit testing. Raw frames and completed display frames use bounded latest-frame queues. H.264 encoded packets are delivered/decoded in order; dependent P frames must never be dropped as if they were independent images. Backpressure retains the latest raw update and limits queued encoded access units to two per window. Viewers advertising `frame_ack:true` additionally limit total outstanding frames to two per window, including frames already written into TCP buffers. Client `frame_ack` carries `id` and cumulative `sequence`; send after presentation submission (or buffered GDI paint). PNG replacement cancels credits for unsent independent frames; H.264 reference access units remain ordered. Fallback and closed windows clear their credits. Control messages have priority. Encoding and decoding run outside the compositor/UI event loops.

H.264 kind 2 uses the same 36-byte geometry header, followed by six u32 fields: stream epoch, even coded width, even coded height, keyframe flag (0/1), color byte length, alpha PNG byte length. Annex-B H.264 access-unit bytes follow, then an optional grayscale PNG alpha mask. The server emits an AUD (NAL type 9) at the start of each access unit and removes filler_data (type 12). Earlier v3 servers may include filler; clients can remove it before decoding. On the tested Windows MFT, trailing filler delayed the final picture until another input arrived, even with low-latency mode enabled. Removing filler preserves picture/reference data and sequence timestamps; it does not change frame-credit semantics. Total fixed header is 60 bytes. No mask means fully opaque. Color uses BT.709 limited-range H.264 4:2:0; PNG alpha is full-range lossless. On resize/reconnect a new epoch starts with an IDR, including SPS/PPS. Bounded server encoder caches may reuse a previous resolution, but reactivation still creates a new epoch and forces this independent IDR. Clients must discard previous-epoch pending metadata and reset reference state (or flush a compatible cached decoder); encoded P frames within an epoch must remain ordered. A mask always accompanies its matching color sequence; video decoding may buffer metadata until that sequence is returned. Client `codec_fallback` with `id` requests PNG for the rest of the viewer session.

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
