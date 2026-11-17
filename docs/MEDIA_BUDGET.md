# Linux media resource admission

The first resource-budget implementation covers the Linux capture/encode path.
It complements the existing 128 MiB queued-raw-frame limit and two presentation
credits per window. Windows decode/presentation resources and transport/file
queues retain their existing separate limits; they are not in this account yet.

## Configuration

The normal Linux configuration file accepts an optional section:

```toml
[media_budget]
cpu_mib = 512
gpu_mib = 1024
window_cpu_mib = 384
window_gpu_mib = 512
encoders = 8
```

These are the initial default admission budgets per compositor session. Window
limits must not exceed session limits. Byte settings accept 4–32768 MiB and
encoder slots accept 1–64; each window may hold at most two codec slots including
its cached resize session. `--check-config` validates them and `--print-config`
shows effective values and provenance. They apply to new sessions, not live
configuration changes. Numbers are policy defaults to calibrate on target
hardware, not advertised memory requirements or performance guarantees.

## What is charged

- Capture workspaces reserve `16 × physical pixels + 64 KiB` of CPU budget before
  allocation. This allows owned RGBA/readback/alpha and the PNG/protocol-copy
  peak. FP16 scenes additionally reserve eight bytes per pixel for a possible
  CPU readback/tone-map scratch; the scratch itself is allocated only when used.
- A GL attachment reserves four bytes per pixel for RGBA8 or eight for RGBA16F.
  The ticket remains with the attachment and is returned after destruction.
- CUDA source pools reserve a padded four-frame allowance, plus the source alpha
  buffer and a small bookkeeping allowance. RGBA and P010 rows are estimated
  independently, including even coded dimensions and stride padding.
- Each active or idle NVENC session holds one codec slot, a GPU estimate of eight
  bytes per coded pixel for SDR/twelve for HDR plus 32 MiB, and the padded software
  RGBA input allocation when using CPU input. Idle sessions count against the
  same account as active ones.

CPU workspace values and CUDA/NVENC values are conservative admission estimates,
not total process RSS or measured driver VRAM. Driver-private allocations,
imported application buffers, shared EGL/CUDA contexts, encoded network packet
lifetimes, socket buffers and application memory remain outside this first
account. The GPU gauges are deliberately named `estimated_bytes`.

## Ownership and recovery

Capture reserves its complete workspace/new-target/new-pool bundle atomically,
then splits the ticket between those owners. Frame tickets survive dequeue,
encoding and fallback, rather than being returned while the worker still holds
pixels. Target tickets follow the GL attachment. A shared native owner is attached
to both CUDA AVHWFramesContexts; their finalizers run after cached pool buffers
are freed. A window/source disappearing therefore cannot prematurely return a
pool still retained by a raw frame, active encoder or cached encoder. All failure
paths and cross-thread final release use the same ownership mechanism.

Before a resize allocates a new target, it releases the old CUDA registration and
GL attachment on the compositor thread. Old native pool/codec references keep
their own tickets until their actual final release. Cached/active codecs are
reclaimed only on the encoder thread; no allocator/destructor executes while
the accounting mutex is held.

A request which exceeds its own configured limit is deferred without evicting
unrelated resources. A video stream whose minimum target/pool/codec working set
cannot fit uses CPU/PNG for that capture. Temporary codec contention also permits
PNG, without marking the codec/GPU permanently unsupported. Capacity recovery
wakes the final static update; resumed video starts a fresh epoch/keyframe.
Oversized PNG itself remains dirty until the user resizes it or chooses a larger
budget for a new session.

Pressure rechecks the requested cost before reclaiming, so a completed worker
can clear stale contention without resetting a healthy foreground codec.
Reclamation first frees idle codec caches; under global contention it may free
an eligible background codec and an old background target. Hidden targets are
preferred. Focused windows are visited first and protected from unrelated
background eviction. Reclaimed background capture waits 500 ms; pressure work
is limited to once per 250 ms. If a hard budget cannot retain all windows,
background updates may wait while the last Windows picture remains visible.
Wayland callbacks, control traffic and input continue.

## Diagnostics and checks

With `--stats`:

- `media_cpu_reserved_bytes`, `media_gpu_estimated_bytes`, `media_encoder_slots`
  report recent values and interval peaks.
- `media_budget_wait` counts transient admission refusals;
  `media_budget_request_exceeds_limit` counts requests that cannot fit alone.
- `media_video_workset_exceeds_limit` identifies temporary CPU/PNG selection.
- `media_target_reclaim` and `media_encoder_reclaim` count pressure handling.

```sh
cargo test --workspace --locked
python3 tests/session_fixture.py python3 tests/media_budget_smoke.py --png-only
python3 tests/session_fixture.py python3 tests/media_budget_smoke.py
python3 tests/session_fixture.py python3 tests/gpu_smoke.py --binary "$PWD/target/debug/waybridge"
```

The native CUDA regression specifically destroys the source and frame while
NVENC still holds its pool, then destroys the encoder on another thread and
requires the owner to release exactly once. Registration failure and independent
HDR primary/fallback pool release are tested too. The regular PNG regression is
included in `scripts/test.sh`; real CUDA/H.264 paths require the NVIDIA host.
