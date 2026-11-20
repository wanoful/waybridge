# File Transfers

The Control Center's file drawer displays uploads, downloads, and files dragged out from Linux. Once both endpoints negotiate the dedicated file channel, you can set a bandwidth limit under **Sharing & Permissions** and click **Apply to current connection** for immediate effect. The default value `0` means unlimited, and the rate limit applies independently to each direction. Progress is measured in file bytes, while rate limiting accounts for on-the-wire chunk sizes.

Sending files or directories from the file drawer, copying files via clipboard, and dropping files onto streamed windows all share the same upload queue. Synced Linux file clipboards can be pasted directly, or you can open the receiving directory from the file drawer. Cross-machine delivery preserves the existing read-only mechanism so you can copy files to a destination directory before editing.

- Once reading starts, each transfer row shows byte progress and a cancel action. Already-completed transfers remain usable.
- Before commit, cancellation aborts remaining file reads and writes. Staged data is cleaned up, and stale clipboard and window-target protections remain in effect.
- Lazy reads for Linux file drag-out transmit only the ranges requested by the target. Progress reflects delivered bytes, while final completion is determined by the native drag-and-drop outcome.
- If the dedicated file data channel drops, in-flight transfers stop. Control, video, audio, and clipboard text continue on their own channels, and transfers can be restarted after reconnecting.

## File Data Channel

A client with the `file_tree` capability can advertise `file_channel_v1` in its `hello`. Once both sides confirm support, the client opens a bidirectional channel through the same TLS entry point and pairing credentials as the control connection, sending `channel: "files"`, the session token, and the active connection's `media_key`. Linux verifies active connection identity and single attachment, then replies with `files_ready`. Negotiated connections always transfer file bytes over this channel.

Upload manifests, admission results, commits, and cancellations remain on the ordered control channel. Data chunks, per-file EOF markers, and file-data fences travel on the file channel. Once the disk worker processes a fence, it sends a data-processing acknowledgement on the control channel. Windows commits only after receiving that acknowledgement, ensuring clipboard generation and window-state checks precede publication. Outbound Linux file clipboard streams and OLE lazy file-read responses also use the file channel, while drag-and-drop offers, targets, read requests, and finalization remain on the control channel. Older endpoints fall back to the legacy file message path.

Send and receive file queues enforce independent byte caps, and the server upload queue reserves capacity for control-side file operations. File transfer threads may block on disk or network I/O without stalling input events or video transmission. File contents still enforce existing tree-manifest validation, path constraints, and read-only delivery on the receiving side. Interrupted batches must be resent.

## Verification

```sh
python3 tests/session_fixture.py python3 tests/tree_smoke.py --file-channel
python3 tests/session_fixture.py python3 tests/tree_smoke.py
python3 tests/session_fixture.py python3 tests/gui_native_smoke.py
```

The tree protocol regressions cover file transfers and drag-and-drop, file-channel authentication checks, control-message responsiveness under rate limiting, and cleanup after an independent channel disconnect. The Windows worker probe verifies real transmission, transfer progress, cancellation, and Linux session preservation.
