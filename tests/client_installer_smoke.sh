#!/usr/bin/env bash
# Run under xvfb-run on Linux with Wine and both built installers.
set -euo pipefail
cd "$(dirname "$0")/.."
root="$PWD"
test_area=$(mktemp -d)
export WINEPREFIX="$test_area/prefix" WINEDEBUG=-all WINEARCH=win64 WINEDLLOVERRIDES="mscoree,mshtml="
cleanup() {
  wineserver -k >/dev/null 2>&1 || true
  wineserver -w
  rm -rf -- "$test_area"
}
trap cleanup EXIT

timeout 60 wineboot -u
wine reg add 'HKCU\Software\Wine' /v Version /d win10 /f
windows_folder() {
  winepath -u "$(wine cmd /d /c "echo %$1%" | tr -d '\r')"
}
localapp=$(windows_folder LOCALAPPDATA)
shortcut="$(windows_folder APPDATA)/Microsoft/Windows/Start Menu/Programs/Waybridge/Waybridge.lnk"
install_path="$localapp/Programs/Waybridge/app"
protocol='HKCU\Software\Classes\waybridge'
uninstall='HKCU\Software\Microsoft\Windows\CurrentVersion\Uninstall\Waybridge'

wrong_arch=0
timeout 30 wine "$root/build/Waybridge-Setup-win-arm64.exe" /S || wrong_arch=$?
test "$wrong_arch" -eq 2
timeout 90 wine "$root/build/Waybridge-Setup-win-x64.exe" /S
test -f "$shortcut"
wine reg query "$protocol\shell\open\command" /ve | grep -F -- '--launch-link "%1"'
wine reg query "$uninstall" /v DisplayVersion
(cd "$install_path" && sha256sum --check --quiet SHA256SUMS.txt)

mkdir -p "$localapp/Waybridge"
echo 'keep-me' > "$localapp/Waybridge/setup-test-profile"
touch "$install_path/obsolete-runtime.dll"
timeout 90 wine "$root/build/Waybridge-Setup-win-x64.exe" /S
test ! -f "$install_path/obsolete-runtime.dll"
grep -qx 'keep-me' "$localapp/Waybridge/setup-test-profile"

timeout 45 wine "$install_path/Uninstall.exe" /S
wineserver -w
test ! -d "$install_path"
test ! -f "$shortcut"
grep -qx 'keep-me' "$localapp/Waybridge/setup-test-profile"
if wine reg query "$protocol" >/dev/null 2>&1 || wine reg query "$uninstall" >/dev/null 2>&1; then
  echo 'Waybridge registry entries survived uninstall' >&2
  exit 1
fi
echo 'PASS: x64 install, upgrade, uninstall, payload and URI checks, profile preservation, and ARM64 guard'
