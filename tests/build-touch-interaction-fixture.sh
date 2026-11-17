#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
bash tests/build-fixture.sh
cc -std=c11 -Wall -Wextra -Werror -Ibuild/tests tests/touch_interaction_app.c build/tests/{xdg-shell,xdg-decoration,cursor-shape,tablet,xdg-dialog,xdg-activation}-protocol.c -o build/tests/touch-interaction-app $(pkg-config --cflags --libs wayland-client)
