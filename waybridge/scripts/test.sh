#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
cargo fmt --all --check
cargo clippy --workspace --all-targets --locked -- -D warnings
cargo test --workspace --locked
cargo build --locked
python tests/smoke.py
python tests/drag_out_smoke.py
python tests/tree_smoke.py
python tests/decorations_smoke.py

python tests/qt_smoke.py
if command -v dolphin >/dev/null; then
  python tests/dolphin_smoke.py
fi

if command -v ghostty >/dev/null; then
  python tests/ghostty_smoke.py
fi
