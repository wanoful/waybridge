#!/usr/bin/env bash
# Cross-publish the native Windows GUI and matching streaming worker from Linux.
set -euo pipefail
cd "$(dirname "$0")/.."
architecture="${1:-arm64}"
case "$architecture" in
  arm64|x64) architectures=("$architecture") ;;
  all) architectures=(arm64 x64) ;;
  *) echo "Usage: $0 [arm64|x64|all]" >&2; exit 2 ;;
esac
for arch in "${architectures[@]}"; do
  if [[ "$arch" == arm64 ]]; then
    ./scripts/build-windows-arm64.sh
    native=build/windows-arm64
    platform=ARM64
  else
    cmake -S client -B build/windows -G Ninja -DCMAKE_TOOLCHAIN_FILE=mingw.cmake -DCMAKE_BUILD_TYPE=Release
    cmake --build build/windows --target waybridge-client
    native=build/windows
    platform=x64
  fi
  output="build/control-center-$arch"
  rm -rf -- "$output"
  # Programmatic WinUI needs no XAML/PRI compiler. The SDK still generates the
  # full native WinRT component manifest; only its final mt.exe merge is replaced.
  dotnet publish gui/Waybridge.Control/Waybridge.Control.csproj -c Release \
    -r "win-$arch" -p:Platform="$platform" --self-contained true \
    -p:WindowsAppSDKSelfContained=true -p:MicrosoftWindowsAppSDKPackageDir= \
    "-p:ManifestTool=$PWD/scripts/merge-windows-manifest.py" -o "$output"
  cp "$native/waybridge-client.exe" "$output/"
  cp README.md "$output/README.md"
  mkdir -p "$output/docs"
  cp docs/*.md "$output/docs/"
  cp scripts/Diagnose-Startup.ps1 scripts/Diagnose-Startup.cmd "$output/"
  cp LICENSE "$output/LICENSE-Waybridge.txt"
  mkdir -p "$output/licenses"
  cp "$native/SDL3-LICENSE.txt" "$output/licenses/"
  cp client/third_party/json.LICENSE.MIT "$output/licenses/nlohmann-json-LICENSE.txt"
  dotnet_version=$(python3 -c 'import json,sys;print(json.load(open(sys.argv[1]))["runtimeOptions"]["includedFrameworks"][0]["version"])' "$output/Waybridge.Control.runtimeconfig.json")
  packages="${NUGET_PACKAGES:-$HOME/.nuget/packages}"
  cp "$packages/microsoft.netcore.app.runtime.win-$arch/$dotnet_version/LICENSE.TXT" "$output/licenses/dotnet-LICENSE.txt"
  cp "$packages/microsoft.netcore.app.runtime.win-$arch/$dotnet_version/THIRD-PARTY-NOTICES.TXT" "$output/licenses/dotnet-NOTICES.txt"
  cp "$packages/microsoft.windowsappsdk/1.8.260921001/license.txt" "$output/licenses/WindowsAppSDK-LICENSE.txt"
  cp "$packages/microsoft.windowsappsdk/1.8.260921001/NOTICE.txt" "$output/licenses/WindowsAppSDK-NOTICE.txt"
  # Official, signed installer, included for machines without the VC++ runtime.
  mkdir -p build/prerequisites
  prerequisite="build/prerequisites/vc_redist.$arch.exe"
  [[ -s "$prerequisite" ]] || curl --fail --location --retry 2 "https://aka.ms/vc14/vc_redist.$arch.exe" -o "$prerequisite"
  cp "$prerequisite" "$output/"
  python3 scripts/package-control-center.py "$arch"
done
