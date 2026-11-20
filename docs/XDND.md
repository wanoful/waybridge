# Private Xwayland drag bridge

Updated: 2026-11-17. The production bridge supports native X11 sources and
X11 targets, Wayland ↔ X11 and X11 ↔ X11 inside one session, and X11 ↔ Windows
text/file exports through the existing Windows transport. Internal Linux drags
keep the original MIME source and negotiate COPY/MOVE. Cross-device delivery
remains COPY; incoming file URIs name the existing read-only delivery view.
Applications still decide which formats and actions they accept.

## Ownership and routing

`src/input_focus.rs` forwards keyboard, pointer, touch and DND to a Wayland or
X11 focus target. `src/dnd_lifecycle.rs` wraps either native source with one
identity, de-duplicates Drop/Finish/Cancel and publishes feedback for the next
compositor cycle. Callbacks never re-enter the locked pointer grab. The common
coordinator in `src/data_drop.rs` owns external payload snapshots, target
revisions and source-export leases. The Windows wire protocol is unchanged.

The protocol adapters live in the Smithay fork. The `input/dnd` module defines
Source, DndFocus, and OfferData. The Wayland data-device and XWM implement
these contracts. The fork backports this code onto Smithay v0.7.0.
[Smithay patch series](https://github.com/wanoful/waybridge-smithay/blob/waybridge-0.7.0-1/PATCHES.md)
The old COPY-only production worker was removed. Its implementation is retained
only as the independent `tests/fixtures/xdnd_peer.rs` conformance peer; results
from that peer alone do not establish production compositor coverage.

Windows HWND positions are independent of X root placement. A native X11 drag
therefore sees a shadow target mapped over the private root, while the common
focus routes the actual destination. X11 → X11 also uses this proxy rather than
assuming an ordinary, shared desktop layout. The original XdndSelection owner
is retained. Selection reads use independent child requestor windows under the
shadow target and the original X server Position/Drop timestamp. Concurrent
format reads cannot replace each other's descriptor.

Wayland/external → X11 offers use a new source XID on each target entry; replies
to retired source XIDs cannot update the current target. Native X11 → X11 uses
the gesture's shadow XID consistently, including same-process source/target
clients. Its replies are checked against the active gesture and target; it does
not provide a separate wire revision for each re-entry to the same native target.
XDND has no application-level revision field for that case.

Requested and accepted actions are separate. The source action list and current
request constrain target negotiation; an unsupported or rejected action stays
NONE. Status changes reach an idle source immediately. Position updates are
coalesced while awaiting Status, preserving the latest actual point. Native
Drop is forwarded only once the original source's Drop timestamp is known.
Version-5 Finished requires a valid success bit and supported final action.
Early target completion is retained until the original X11 source can receive
its Finished acknowledgment; a target insertion alone is not source completion.

## Input, cancellation and resources

A native drag owns capture. Newly mapped override-redirect visuals cannot claim
a menu grab; empty X SHAPE regions and empty Wayland input regions are respected. XWM watches
SHAPE changes and exports a wholly empty input region for an explicitly
input-transparent X11 surface; general nonempty X SHAPE geometry is not added
by this change.
On entering an X11 surface, its position is reasserted before a following button
can use the previous window's X sprite coordinates. Explicit cancellation clears
the data target before releasing buttons. An active X11 source receives Escape
through its seat keyboard because XDND has no target-to-source cancel request.
Disconnect, read-only and source/target-window loss follow the same cancellation
path. Attached Wayland toplevel drags are exempt from origin-window loss during
a successful tab merge. The Windows input hit test also governs OLE returns;
transparent OLE tracking windows and source visuals cannot force a false return
or hide a real target. Entering the image after the nonclient area is handled
through DragOver without requiring a second DragEnter.

Native MIME reads and pending X conversions are capped at eight; XDND outgoing
conversions are also capped at eight. Stalled conversions and terminal shadow
transactions expire after ten seconds. Type lists are limited to 64 entries.
Cross-device text and files retain the existing bounded workers, 1 MiB text
limit and read-only policy described in [DRAG_AND_DROP.md](DRAG_AND_DROP.md).
Clipboard and PRIMARY selection ownership are separate from XdndSelection.

XWM dispatch and its property/atom queries run on the compositor's calloop
thread. Pipe transfer is nonblocking, but not every X server roundtrip has a
socket deadline. This is a remaining responsiveness boundary; the old worker's
500 ms per-request limit must not be attributed to this implementation.

## Validation and limits

- `mixed_drag_smoke.py`: actual Qt Wayland/XCB sources and targets; all four text
  combinations, Unicode/large-text SHA-256, actual filesystem MOVE in both mixed
  directions and within an X11 window, Escape/read-only/disconnect and reuse.
- `xdnd_bridge_smoke.py`: production incoming Unicode/INCR-sized text and
  read-only URI delivery copied by a real X11 file model.
- `windows_data_drag_smoke.py --backend xcb`: production Windows PE worker in
  private Wine, native X11 cross-window dragging, OLE-return and Unicode out/in.
- Existing Wayland source-feedback, delayed-offer, visual and Chrome regressions
  remain mandatory.

The protocol basis is [XDND](https://wiki.freedesktop.org/www/Specifications/XDND/).
No application-name branches were added. This does not certify every QQ,
Dolphin, Java or Blender workflow. X11 drag-image extraction into the dedicated
Windows ghost is still missing; ordinary transparent popup rendering may show
such visuals. Cross-device HTML/RTF/images, LINK/ASK/private actions, XEmbed tray,
CF_HDROP-only consumers and pre-transfer file hover remain separate work.
