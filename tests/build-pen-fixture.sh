#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
pen_build=build/tests/pen-fixture
mkdir -p "$pen_build"
while read -r name protocol; do
  wayland-scanner client-header "$protocol" "$pen_build/$name-client-protocol.h"
  wayland-scanner private-code "$protocol" "$pen_build/$name-protocol.c"
done <<'PROTOCOLS'
xdg-shell /usr/share/wayland-protocols/stable/xdg-shell/xdg-shell.xml
xdg-decoration /usr/share/wayland-protocols/unstable/xdg-decoration/xdg-decoration-unstable-v1.xml
cursor-shape /usr/share/wayland-protocols/staging/cursor-shape/cursor-shape-v1.xml
tablet-v2 /usr/share/wayland-protocols/stable/tablet/tablet-v2.xml
xdg-dialog /usr/share/wayland-protocols/staging/xdg-dialog/xdg-dialog-v1.xml
xdg-activation /usr/share/wayland-protocols/staging/xdg-activation/xdg-activation-v1.xml
PROTOCOLS
cc -std=c11 -Wall -Wextra -Werror -I"$pen_build" tests/pen_app.c "$pen_build"/*-protocol.c -o "$pen_build/pen-app" $(pkg-config --cflags --libs wayland-client)
