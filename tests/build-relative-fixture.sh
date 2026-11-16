#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
bash tests/build-fixture.sh
for spec in relative-pointer pointer-constraints; do
  xml="/usr/share/wayland-protocols/unstable/$spec/$spec-unstable-v1.xml"
  wayland-scanner client-header "$xml" "build/tests/$spec-client-protocol.h"
  wayland-scanner private-code "$xml" "build/tests/$spec-protocol.c"
done
cc -std=c11 -Wall -Wextra -Werror -Ibuild/tests tests/relative_app.c build/tests/{xdg-shell,xdg-decoration,cursor-shape,tablet,xdg-dialog,xdg-activation,relative-pointer,pointer-constraints}-protocol.c -o build/tests/relative-app $(pkg-config --cflags --libs wayland-client)
