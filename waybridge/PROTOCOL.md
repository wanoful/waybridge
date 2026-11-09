# Waybridge protocol v3

All packets begin with a big-endian u32 length counting the kind byte and body. Length must be in `1..=75497472`. Kind `0` is a UTF-8 JSON object; kind `1` is a PNG RGBA frame; kind `2` is H.264 with a lossless alpha mask. Unsupported or malformed framing closes the connection.

First client message: `{"type":"hello","version":3,"token":"...","dpi":168,"work_width":3840,"work_height":2080}`. The server verifies authentication and exclusive viewer ownership before announcing protocol version, codec, and capabilities. This transport is meant to run through an SSH tunnel. The server advertises `codec:"mixed"`, supported `codecs` and a random per-session `media_key`. Both ends must use v3. Clients advertise `codecs:["png_rgba","h264_alpha"]` when hardware decoding is available, otherwise PNG only. DPI defaults to 96; work dimensions are physical monitor work-area pixels.

## Media connection

A viewer requesting `split_media:true` opens a second TCP connection to the same address and port. Authenticate it with `{"type":"hello","version":3,"token":"...","channel":"media","media_key":"..."}`. Only the current viewer's random key is accepted, and only one media socket attaches. The server replies `media_ready`, then sends frame kinds 1/2 over this socket. Input, window control, clipboard and file messages stay on the control socket. Socket failure closes the session; reconnect restores live windows. Existing SSH forwarding carries both connections. Test/simple viewers may omit `split_media` to multiplex on one socket.

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

### Native Linux → Windows file drag

Opt in with `file_drop_out:true` in the viewer hello; the server advertises
`file_drop_out` in `features`. This extension uses JSON on the control socket,
independent of clipboard ownership and `files_*` batches. It supports one active
outbound operation, at most 128 regular files of 1 GiB each, and COPY only. Sources
must offer `text/uri-list` and COPY through wl_data_device v3 or newer. Directories,
final-component symlinks, nonlocal/non-UTF-8 URIs, unsupported Windows names and
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
