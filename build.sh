#!/usr/bin/env bash
# Cross-compile GhostWin.exe with mingw-w64 (x64), plus ARM64 via zig if available.
# The icon (res/ghostwin.ico) is generated: see tools/make_icon.cpp + tools/make_icon.py.
set -euo pipefail
cd "$(dirname "$0")"
mkdir -p build
FLAGS="-std=c++17 -O2 -Wall -Wextra -Wno-cast-function-type -Wno-missing-field-initializers -municode -mwindows -DUNICODE -D_UNICODE -D_WIN32_WINNT=0x0A00"
LIBS="-ldwmapi -lcomctl32 -lshell32 -luser32 -lgdi32 -ladvapi32"
x86_64-w64-mingw32-windres -I res res/ghostwin.rc -O coff -o build/ghostwin-x64.res
x86_64-w64-mingw32-g++ $FLAGS src/ghostwin.cpp build/ghostwin-x64.res -o build/GhostWin.exe -static -static-libgcc -static-libstdc++ -s $LIBS
x86_64-w64-mingw32-g++ -std=c++17 -O1 -municode -DUNICODE -D_UNICODE -D_WIN32_WINNT=0x0A00 tests/gw_test.cpp -o build/gw_test.exe -static -s -luser32 -lgdi32 -ladvapi32 -ldwmapi
if python3 -m ziglang version >/dev/null 2>&1; then
  python3 -m ziglang rc /fo build/ghostwin-arm64.res /i res res/ghostwin.rc
  python3 -m ziglang c++ -target aarch64-windows-gnu -std=c++17 -O2 -DUNICODE -D_UNICODE \
    -municode -Wl,--subsystem,windows src/ghostwin.cpp build/ghostwin-arm64.res -o build/GhostWin-arm64.exe -s $LIBS
fi
ls -l build/*.exe
