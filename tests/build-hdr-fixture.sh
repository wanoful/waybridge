#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
bash tests/build-fixture.sh
xml=/usr/share/wayland-protocols/staging/color-management/color-management-v1.xml
wayland-scanner client-header "$xml" build/tests/color-management-client-protocol.h
wayland-scanner private-code "$xml" build/tests/color-management-protocol.c
cc -std=c11 -Wall -Wextra -Werror -Ibuild/tests tests/hdr_app.c build/tests/{xdg-shell,xdg-decoration,cursor-shape,tablet,xdg-dialog,xdg-activation,color-management}-protocol.c -o build/tests/hdr-app $(pkg-config --cflags --libs wayland-client)
