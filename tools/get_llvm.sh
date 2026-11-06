#!/bin/bash
# FALLBACK, UNUSED (coordinator 2026-11-06: the installer compiles with Apple Command Line Tools'
# clang; no LLVM in the DMG). Kept pinned in case CLT ever cannot build the game code:
#   tools/get_llvm.sh [dest]     (default .cache/tools/llvm; downloads ~1.5 GB, extracts ~7.8 GB)
# Official LLVM 19.1.7 release (tag llvmorg-19.1.7), macOS arm64 tarball, checked by sha256.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
DEST="${1:-$ROOT/.cache/tools/llvm}"
VER=19.1.7
TAR="LLVM-$VER-macOS-ARM64.tar.xz"
SHA=d93bf12952d89fe4ec7501c40475718b722407da6a8d651f05c995863468e570
URL="https://github.com/llvm/llvm-project/releases/download/llvmorg-$VER/$TAR"
mkdir -p "$DEST"
[[ -f "$DEST/$TAR" ]] || curl -fL --retry 3 -o "$DEST/$TAR" "$URL"
echo "$SHA  $DEST/$TAR" | shasum -a 256 -c - || { echo "get_llvm: sha256 mismatch"; rm -f "$DEST/$TAR"; exit 1; }
rm -rf "$DEST/x"; mkdir -p "$DEST/x"
tar -xJf "$DEST/$TAR" -C "$DEST/x" --strip-components 1
rm -f "$DEST/$TAR"
"$DEST/x/bin/clang" --version | head -1
