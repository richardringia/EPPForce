#!/bin/bash
# Build EPPForce.kext from source using the command-line kernel toolchain.
set -euo pipefail

cd "$(dirname "$0")"
SDK=$(xcrun --show-sdk-path)
KHDR="$SDK/System/Library/Frameworks/Kernel.framework/Headers"
OUT="build"
KEXT="$OUT/EPPForce.kext"
BIN="$KEXT/Contents/MacOS/EPPForce"

rm -rf "$OUT"
mkdir -p "$KEXT/Contents/MacOS"

CXXFLAGS=(
  -arch x86_64 -std=c++17
  -fno-builtin -fno-common -fno-exceptions -fno-rtti
  -fapple-kext -mkernel
  -nostdinc -I"$KHDR"
  -isysroot "$SDK"
  -DKERNEL -DKERNEL_PRIVATE -DDRIVER_PRIVATE -DAPPLE -DNeXT
  -mmacosx-version-min=11.0
  -O2 -Wall
)

echo "==> compiling"
xcrun clang++ "${CXXFLAGS[@]}" -c src/EPPForce.cpp -o "$OUT/EPPForce.o"
# kmod_info stub is plain C
CFLAGS=(
  -arch x86_64 -std=c11
  -fno-builtin -fno-common -fapple-kext -mkernel
  -nostdinc -I"$KHDR" -isysroot "$SDK"
  -DKERNEL -DKERNEL_PRIVATE -DDRIVER_PRIVATE -DAPPLE -DNeXT
  -mmacosx-version-min=11.0 -O2 -Wall
)
xcrun clang "${CFLAGS[@]}" -c src/kmod_info.c -o "$OUT/kmod_info.o"

echo "==> linking kext binary"
xcrun clang++ -arch x86_64 -nostdlib -fapple-kext -mkernel \
  -isysroot "$SDK" \
  -Xlinker -kext \
  "$OUT/EPPForce.o" "$OUT/kmod_info.o" \
  -lkmodc++ -lkmod -lcc_kext \
  -o "$BIN"

echo "==> assembling bundle"
cp src/Info.plist "$KEXT/Contents/Info.plist"

echo "==> ad-hoc codesign (OpenCore injection ignores signature, but keep it clean)"
codesign --force --sign - "$KEXT" 2>/dev/null || true

echo "==> kmutil inspect (validation)"
kmutil inspect --bundle-path "$KEXT" 2>&1 | head -20 || true

echo
echo "built: $KEXT"
file "$BIN"
