#!/usr/bin/env bash
# M0/M1 verification on the Linux NVIDIA development host; Windows hardware is separate.
set -euo pipefail
cd "$(dirname "$0")/.."
case "${1:-}" in
  ""|--with-wine) ;;
  *) echo "Usage: $0 [--with-wine]" >&2; exit 2 ;;
esac
if [[ "${WAYBRIDGE_TEST_SESSION_FIXTURE:-}" != 1 ]]; then
  # This test allocates its own delegated scope and private real user manager;
  # it must run before the remaining suite replaces the manager with a fixture.
  cargo build --workspace --locked
  python3 tests/session_lifecycle_smoke.py
  python3 tests/session_error_shutdown_smoke.py
  exec python3 tests/session_fixture.py bash "$0" "$@"
fi
verification_data=$(mktemp -d -t waybridge-validation-data.XXXXXXXX)
trap 'rm -rf -- "$verification_data"' EXIT
export XDG_DATA_HOME="$verification_data"
./scripts/test.sh
cargo build --workspace --locked
python3 tests/configuration_smoke.py
python3 tests/session_admission_smoke.py
python3 tests/transfer_worker_smoke.py
python3 tests/desktop_services_smoke.py
python3 tests/desktop_services_smoke.py --special-runtime
python3 tests/session_launch_policy_smoke.py
python3 tests/desktop_overrides_smoke.py
python3 service/tests/direct_service_smoke.py --compositor target/debug/waybridge
python3 service/tests/app_icons_smoke.py
dotnet run --project gui/Waybridge.Control.Tests -c Release
python3 tests/gui_direct_smoke.py
python3 tests/gui_codec_smoke.py
python3 tests/stream_config_hdr_smoke.py
python3 tests/hdr_video_smoke.py
if [[ "${1:-}" == --with-wine ]]; then
  ./scripts/test-windows.sh
fi
