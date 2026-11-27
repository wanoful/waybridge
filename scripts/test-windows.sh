#!/usr/bin/env bash
# Windows protocol/native API regressions under private Wine/Xvfb desktops.
set -euo pipefail
cd "$(dirname "$0")/.."
if [[ "${WAYBRIDGE_TEST_SESSION_FIXTURE:-}" != 1 ]]; then
  exec python3 tests/session_fixture.py bash "$0" "$@"
fi
cargo build --workspace --locked
cmake -S client -B build/windows -G Ninja -DCMAKE_TOOLCHAIN_FILE=mingw.cmake -DCMAKE_BUILD_TYPE=Release
cmake --build build/windows --target waybridge-client
python3 tests/windows_monitor_smoke.py
python3 tests/windows_resize_preview_smoke.py
python3 tests/windows_app_icon_smoke.py
python3 tests/windows_touch_smoke.py
python3 tests/windows_pen_smoke.py
python3 tests/windows_gamepad_smoke.py
python3 tests/windows_smoke.py --rich-clipboard
python3 tests/touch_settings_smoke.py
python3 tests/professional_input_settings_smoke.py
python3 tests/windows_tray_smoke.py
python3 tests/tray_actions_e2e.py
python3 tests/native_notifications_smoke.py
python3 tests/windows_drop_lifetime_smoke.py
python3 tests/windows_text_drag_lifetime_smoke.py
c++ -std=c++17 -Wall -Wextra -Werror -pthread tests/ordered_sender_probe.cpp -o build/tests/ordered-sender-probe
build/tests/ordered-sender-probe
c++ -std=c++17 -Wall -Wextra -Werror -pthread tests/windows_media_budget_probe.cpp -o build/tests/windows-media-budget-probe
build/tests/windows-media-budget-probe
python3 tests/windows_tls_smoke.py
c++ -std=c++17 -Wall -Wextra -Werror tests/audio_playout_probe.cpp -o build/tests/audio-playout-probe
build/tests/audio-playout-probe
c++ -std=c++17 -Wall -Wextra -Werror tests/presentation_cadence_probe.cpp -o build/tests/presentation-cadence-probe
build/tests/presentation-cadence-probe
python3 tests/udp_adaptive_smoke.py
python3 tests/udp_flow_smoke.py
xvfb-run -a python3 tests/udp_fec_smoke.py
python3 tests/windows_direct_smoke.py
python3 tests/gui_native_smoke.py
python3 tests/windows_data_drag_smoke.py
if command -v Xwayland >/dev/null; then
  python3 tests/windows_data_drag_smoke.py --backend xcb
fi
python3 tests/windows_drag_out_smoke.py --tree
python3 tests/windows_drag_out_smoke.py
