#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
cargo fmt --all --check
cargo clippy --workspace --all-targets --locked -- -D warnings
cargo test --workspace --locked
cargo build --locked
python tests/smoke.py
python tests/cursor_smoke.py
if command -v rsvg-convert >/dev/null; then
  python tests/app_icon_smoke.py
fi
python tests/relative_smoke.py
python tests/drag_out_smoke.py
python tests/tree_smoke.py
python tests/decorations_smoke.py
python tests/window_semantics_smoke.py

python tests/qt_smoke.py
python tests/ime_smoke.py
if command -v dolphin >/dev/null; then
  python tests/dolphin_smoke.py
  if command -v Xwayland >/dev/null && command -v parec >/dev/null && pactl info >/dev/null 2>&1; then
    python tests/launch_smoke.py
  fi
fi

if command -v ghostty >/dev/null; then
  python tests/ghostty_smoke.py
  python tests/ghostty_smoke.py --ime
fi
