#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
mkdir -p build/tests
wayland-scanner client-header /usr/share/wayland-protocols/stable/xdg-shell/xdg-shell.xml build/tests/xdg-shell-client-protocol.h
wayland-scanner private-code /usr/share/wayland-protocols/stable/xdg-shell/xdg-shell.xml build/tests/xdg-shell-protocol.c
wayland-scanner client-header /usr/share/wayland-protocols/unstable/xdg-decoration/xdg-decoration-unstable-v1.xml build/tests/xdg-decoration-client-protocol.h
wayland-scanner private-code /usr/share/wayland-protocols/unstable/xdg-decoration/xdg-decoration-unstable-v1.xml build/tests/xdg-decoration-protocol.c
wayland-scanner client-header /usr/share/wayland-protocols/staging/cursor-shape/cursor-shape-v1.xml build/tests/cursor-shape-client-protocol.h
wayland-scanner private-code /usr/share/wayland-protocols/staging/cursor-shape/cursor-shape-v1.xml build/tests/cursor-shape-protocol.c
wayland-scanner private-code /usr/share/wayland-protocols/stable/tablet/tablet-v2.xml build/tests/tablet-protocol.c
wayland-scanner client-header /usr/share/wayland-protocols/staging/xdg-dialog/xdg-dialog-v1.xml build/tests/xdg-dialog-client-protocol.h
wayland-scanner private-code /usr/share/wayland-protocols/staging/xdg-dialog/xdg-dialog-v1.xml build/tests/xdg-dialog-protocol.c
wayland-scanner client-header /usr/share/wayland-protocols/staging/xdg-activation/xdg-activation-v1.xml build/tests/xdg-activation-client-protocol.h
wayland-scanner private-code /usr/share/wayland-protocols/staging/xdg-activation/xdg-activation-v1.xml build/tests/xdg-activation-protocol.c
cc -std=c11 -Wall -Wextra -Werror -Ibuild/tests tests/test_app.c build/tests/xdg-activation-protocol.c build/tests/xdg-dialog-protocol.c build/tests/tablet-protocol.c build/tests/cursor-shape-protocol.c build/tests/xdg-shell-protocol.c build/tests/xdg-decoration-protocol.c -o build/tests/test-app $(pkg-config --cflags --libs wayland-client)
