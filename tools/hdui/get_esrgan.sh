#!/bin/bash
# Fetches Real-ESRGAN (ncnn-vulkan) from the official GitHub release, pinned by version + sha256,
# into ${ROTK_CACHE}/tools/realesrgan/x (where tools/hdui/build_pack.py looks). Owner approved 2026-11-06.
set -euo pipefail
VER=v0.2.5.0
URL="https://github.com/xinntao/Real-ESRGAN/releases/download/$VER/realesrgan-ncnn-vulkan-20220424-macos.zip"
SHA=e0ad05580abfeb25f8d8fb55aaf7bedf552c375b5b4d9bd3c8d59764d2cc333a
D="${ROTK_CACHE:-$HOME/Desktop/Claude/ROTKPort/.cache}/tools/realesrgan"
[[ -x "$D/x/realesrgan-ncnn-vulkan" ]] && { echo "realesrgan $VER present"; exit 0; }
mkdir -p "$D/dl"; curl -sSL -o "$D/dl/r.zip" "$URL"
echo "$SHA  $D/dl/r.zip" | shasum -a 256 -c - >/dev/null || { echo "get_esrgan: sha256 mismatch"; exit 1; }
unzip -q -o "$D/dl/r.zip" -d "$D/x"; chmod +x "$D/x/realesrgan-ncnn-vulkan"; echo "realesrgan $VER installed in $D/x"
