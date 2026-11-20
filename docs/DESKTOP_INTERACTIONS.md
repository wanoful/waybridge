# Desktop Interactions: Windows, Trays, and Drag and Drop

Updated: 2026-11-17. This document distinguishes current implementation, automated regressions, and real-application verification; success with one application does not imply full toolkit compatibility.

## Issues Addressed and Mechanisms

- **Tray clicks**: After negotiating `NOTIFYICON_VERSION_4` with Windows Shell, a single left click can produce both legacy `WM_LBUTTONUP` and `NIN_SELECT`, and a right click can produce both legacy button-up and `WM_CONTEXTMENU`. Forwarding both would toggle a window open and immediately hide it again, or invalidate the first menu token. The client now processes only one callback set matching the negotiated version; SNI/DBusMenu actions return to the originally registered object after validating owner, generation, menu token, and node ID. Menu timestamps do not mix another OS's uptime clock.
- **Window icons**: Tray icons and window icons are distinct protocol objects. Windows prioritize runtime icons (Wayland `xdg-toplevel-icon-v1`, X11 `_NET_WM_ICON`), then fall back to generic lookups by desktop ID, `StartupWMClass`, and X11 class/instance. Runtime updates, removals, stale async results, and reconnects follow explicit invalidation rules. Source badges are composited uniformly on the client without altering application images.
- **Activation and focus**: User actions initiated from the Control Center or tray create a short-lived activation context linked to a window via launched PID or standard `xdg-activation-v1` token before requesting Windows foreground activation. Expired, duplicate, or background requests cannot inherit that user input indefinitely. Before a dialog hides or is destroyed, its parent window's enabled state and focus are restored first. Application windows never set permanent `TOPMOST`; background requests can alert the user instead.
- **Drag and drop**: The original bridge mainly handled file bytes and missed text OLE formats, cross-HWND mouse capture, returning into the original session, X11 XDND targets, and DND icons. Generic `Source` / `DndFocus` / `OfferData` now routes native Wayland/X11 drags and coordinates external MIME delivery with native OLE reads, bounding lifecycles by connection, source/offer, drag generation, and OLE attempt rather than application-name special cases.

Real-device Chrome retesting also exposed that an immediate release can precede remote acceptance—a timing race not caught by paused Qt probes. Waybridge now separates `Pending`/`Accepted`/`Rejected` states, adds target revisions, runs reentrant bounded OLE waits, and waits for target completion receipts. See [Drag-and-Drop Design](DRAG_AND_DROP.md) for responsibilities, 750 ms / 2 s bounds, and test entry points. Text drag-and-drop has been decoupled from file sharing with its own Control Center toggle.

## Current Support Matrix

| Direction / Scenario | Current Behavior | Boundaries |
|---|---|---|
| Same Linux session, within or across Wayland/X11 streamed windows | Preserves native data source, original MIME types, and target-negotiated COPY/MOVE; routes mouse across HWNDs | Depends on source and target supporting the MIME/action; not every widget accepts drops |
| Windows → Wayland/X11 text | Converts `CF_UNICODETEXT` to UTF-8 MIME; hovers/negotiates before COPY | 1 MiB cap; does not emit temporary text files or touch the clipboard |
| Wayland/X11 → Windows text | Converts UTF-8 MIME to OLE `CF_UNICODETEXT` | Plain text only; HTML/RTF/image drag-and-drop not yet bridged |
| Windows → Wayland/X11 files/directories | Transfers payload first, then delivers read-only URIs | Transfer and delivery happen after the Windows drop; no pre-transfer file hover, auto-expand, or scroll negotiation |
| Wayland/X11 → Windows files/directories | OLE virtual-file descriptors + lazy read-only `IStream` with async reads | Target must accept virtual files; `CF_HDROP`-only Windows consumers are not yet covered |
| Leaving a remote window into Windows OLE, then returning | Ends the current OLE attempt and resumes the original Wayland/X11 source; late `IStream`/async results cannot affect a new attempt | Preserves native session drag semantics without re-uploading files back to itself |
| Windows → Xwayland target | Plain text and URI file COPY; XDND Status/Finished, TARGETS, and INCR | Keeps external read-only COPY policy; never upgrades to MOVE |
| Wayland ↔ X11; X11 ↔ X11 | Native selection/MIME, COPY/MOVE, independent coordinates, and source/target completion confirmation; Windows ingress/egress reuses OLE paths | Validated with Qt protocol matrix and physical Windows fixtures; real workflows like QQ/Dolphin are tracked separately—see [XDND.md](XDND.md) |
| Drag visual (ghost icon) | Uses the source's real Wayland DND icon with animation, alpha, and hotspot/attach offset | Requires source-provided icon; temporary Windows overlay is click-through and non-activating with bounded size, pixels, FPS, and queue; X11 source icons not yet wired |

Cross-machine files always remain COPY with read-only delivery: applications can read or import them, and users copy them into a chosen directory via Dolphin or Explorer when they want persistent editable copies. Waybridge does not add per-app Save As hooks, temporary writable project clones, or automatic write-back. This restriction does not alter normal MOVE or editing semantics for files staying within the same Linux session.

## State Ownership

- `src/data_offer.rs`: immutable MIME data snapshots. `src/data_drop.rs`: session coordinator, Wayland/Xwayland target selection, acceptance state, drop deadlines and cancellation, and source export leases.
- `src/drag.rs`: Wayland source reading, constrained file export, and private protocol sink; source completion and cancellation remain with the native protocol.
- `src/input_focus.rs`, `src/dnd_lifecycle.rs`: unified focus routing, source identity, and feedback; Smithay `input/dnd`, data-device, and XWM handle protocol mechanics. XWM queries still run on the calloop thread, so X11 round-trips do not yet have dedicated threads/deadlines; see [XDND.md](XDND.md) for backport provenance and boundaries.
- `client/file_drag.h`, `client/text_drag.h`: native OLE data representations, virtual file streams, external text, and return-to-session handling; window destruction, COM reentrancy, and late async results cannot revive cancelled gestures.
- `src/drag_visual.rs`, `client/drag_visual.h`: visual feedback only; does not own files, focus, or transfer completion state.
- `src/activation.rs`, `client/window_activation.h`: activation grants, connection generations, and foreground transitions; icons and trays never set permanent window z-order.

## Verification and Next Steps

Real Qt widget regressions cover text inputs, cross-window drags, file-manager model MOVE, and external text; real production Windows worker regressions cover OLE ingress/egress and leave-and-return; private Xwayland regressions cover Unicode, INCR, and read-only URI file copies. Tray regressions use real SNI/QAction, and icon tests use real Wayland/X11 protocol objects.

Dedicated test pages on real Windows Chrome and native Wayland Chrome additionally verify bidirectional text, immediate release, added latency, cancellation, rejection, cross-window drags, and switching targets—checking both source completion status and target text. That result applies strictly to plain text and that application pair.

Next steps expand the real-application matrix and add file hover/progress/cancellation, `CF_HDROP`-only consumers, and richer drag formats. Separate protocols such as XEmbed trays and global menus continue under [ROADMAP.md](ROADMAP.md). Real-world reports for HMCL, QQ, Dolphin, and Blender are recorded with exact versions, backends, and actions rather than patched with app-name special cases.
