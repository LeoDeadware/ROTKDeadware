#!/bin/bash
# Fetches third-party code into third_party/ and applies patches/ (RULES R-B5: offline
# when possible). Order of sources: a local clone at the pinned commit (the kit's donor
# or H4Port), else the network — cloning from the network needs the user's OK once.
#
#   tools/bootstrap.sh
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
X86RECOMP_URL="https://github.com/JTM-rootstorm/x86recomp.git"
X86RECOMP_REV="1c90fe3a5dad37d0cca551fc9c8ab66a106ede1e"
LOCAL_CLONES=("$ROOT/../HP2Port" "$ROOT/../H4Port")   # sibling ports

mkdir -p "$ROOT/third_party/deps"
cd "$ROOT/third_party"
if [[ ! -d x86recomp ]]; then
  src="$X86RECOMP_URL"
  for c in "${LOCAL_CLONES[@]}"; do
    if git -C "$c/third_party/x86recomp" cat-file -e "$X86RECOMP_REV" 2>/dev/null; then src="$c/third_party/x86recomp"; break; fi
  done
  git clone -q --no-checkout "$src" x86recomp
  git -C x86recomp checkout -q "$X86RECOMP_REV"
fi
cd x86recomp
if [[ -n "$(git status --porcelain --untracked-files=no)" ]] || [[ "$(git rev-parse HEAD)" != "$X86RECOMP_REV" ]]; then
  echo "x86recomp: local changes present, left as is (rm -rf third_party/x86recomp to redo)"
else
  for p in "$ROOT"/patches/x86recomp-*.patch; do
    [[ -f "$p" ]] || continue
    if git apply --check "$p" 2>/dev/null; then git apply "$p"; echo "applied $(basename "$p")"
    else echo "skipped $(basename "$p") (already applied or conflicts)"; fi
  done
fi

vendor() {  # name url rev [submodules]
  local name=$1 url=$2 rev=$3 dst="$ROOT/third_party/deps/$1-src"
  [[ -d "$dst" ]] && [[ "$(git -C "$dst" rev-parse HEAD 2>/dev/null)" == "$rev" ]] && return
  rm -rf "$dst"
  for c in "${LOCAL_CLONES[@]}"; do
    if [[ "$(git -C "$c/third_party/deps/$name-src" rev-parse HEAD 2>/dev/null)" == "$rev" ]]; then
      cp -c -R "$c/third_party/deps/$name-src" "$dst"; echo "vendored $name (APFS clone of $c)"; return
    fi
  done
  git clone -q "$url" "$dst"; git -C "$dst" checkout -q "$rev"
  [[ "${4:-}" == submodules ]] && git -C "$dst" submodule update -q --init --recursive
  echo "vendored $name (git clone)"
}
vendor zydis https://github.com/zyantific/zydis.git a2278f1d254e492f6a6b39f6cb5d1f5d515659dc submodules
vendor softfloat https://github.com/ucb-bar/berkeley-softfloat-3.git 5c06db33fc1e2130f67c045327b0ec949032df1d
