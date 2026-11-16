#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
if [[ "${WAYBRIDGE_TEST_SESSION_FIXTURE:-}" != 1 ]]; then
  exec python3 tests/session_fixture.py bash "$0" "$@"
fi
cargo fmt --all --check
cargo clippy --workspace --all-targets --locked -- -D warnings
cargo test --workspace --locked
cargo test --locked readonly_delivery -- --ignored
cargo build --locked
python3 tests/notifications_smoke.py
python3 tests/notifications_bridge_smoke.py
python3 tests/tray_smoke.py
python3 tests/session_service_status_smoke.py
python tests/output_bind_race_smoke.py
python tests/smoke.py
python tests/cursor_smoke.py
if command -v rsvg-convert >/dev/null; then
  python tests/app_icon_smoke.py
  if command -v Xwayland >/dev/null && pkg-config --exists x11; then
    python tests/window_icon_smoke.py
  fi
fi
python tests/relative_smoke.py
python tests/drag_out_smoke.py
python tests/data_drag_smoke.py
python tests/drag_visual_smoke.py
if command -v Xwayland >/dev/null; then
  python tests/xdnd_bridge_smoke.py
fi
python tests/tree_smoke.py
python tests/decorations_smoke.py
python tests/window_semantics_smoke.py
python tests/activation_smoke.py
if command -v Xwayland >/dev/null && command -v xrandr >/dev/null && pkg-config --atleast-version=3.4 glfw3 && pkg-config --exists gl; then
  python tests/x11_pointer_smoke.py
  python tests/x11_pointer_smoke.py --bounded-screen
fi
if command -v google-chrome-stable >/dev/null; then
  python tests/chrome_drag_smoke.py
fi

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
