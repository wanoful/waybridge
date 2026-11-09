#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."

# LLVM-MinGW ships a Linux-hosted native Windows ARM64 compiler and sysroot.
if [[ -n "${LLVM_MINGW_ROOT:-}" ]]; then
  export PATH="$LLVM_MINGW_ROOT/bin:$PATH"
elif ! command -v aarch64-w64-mingw32-clang++ >/dev/null 2>&1 &&
     [[ -x /opt/llvm-mingw/llvm-mingw-ucrt/bin/aarch64-w64-mingw32-clang++ ]]; then
  export PATH="/opt/llvm-mingw/llvm-mingw-ucrt/bin:$PATH"
fi

cmake -S client -B build/windows-arm64 -G Ninja \
  -DCMAKE_TOOLCHAIN_FILE=mingw-arm64.cmake -DCMAKE_BUILD_TYPE=Release
cmake --build build/windows-arm64
