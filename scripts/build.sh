#!/bin/sh
# Build loop-island.exe on Linux. Uses llvm-mingw when LLVM_MINGW is set or
# found in ~/.local/opt, otherwise the distribution's MinGW-w64 GCC.
set -e
cd "$(dirname "$0")/.."
: "${LLVM_MINGW:=$(ls -d "$HOME"/.local/opt/llvm-mingw-* 2>/dev/null | head -1)}"
if [ -d "$LLVM_MINGW" ]; then
    set -- -DCMAKE_TOOLCHAIN_FILE=cmake/llvm-mingw.cmake -DLLVM_MINGW="$LLVM_MINGW"
elif command -v x86_64-w64-mingw32-g++ >/dev/null; then
    set -- -DCMAKE_TOOLCHAIN_FILE=cmake/mingw-w64.cmake
else
    echo "Install MinGW-w64 (g++-mingw-w64-x86-64-posix) or set LLVM_MINGW" >&2
    exit 1
fi
cmake -S . -B build "$@" >/dev/null
cmake --build build -j"$(nproc)"
ls -la build/loop-island.exe
