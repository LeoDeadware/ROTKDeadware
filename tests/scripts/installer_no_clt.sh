#!/bin/bash
# Coordinator 2026-11-06: "on a machine without CLT the installer shows the install dialog and
# doesn't crash". Simulated with a PATH whose xcode-select fails `-p` and records `--install`
# (ROTK_NO_UI=1, ROTK_CLT_WAIT=6). Three entry points:
#   1. tools/dmg/clt.sh: records `--install`, writes the progress message, exits 1 (no hang);
#   2. tools/dmg/build_game.sh: stops at step 1 with "100 ERROR: Command Line Tools…", exit 1;
#   3. the launcher (DMG layout, no imported disc, fresh HOME): calls --install, logs the alert,
#      exits 74 (not a signal), before touching the disc.
# Then the same with a working xcode-select (the real one): clt.sh exits 0 without --install.
source "$(dirname "$0")/../lib.sh"
T="${ROTK_CACHE:-$ROOT/.cache}/runs/installer_no_clt-$$"; rm -rf "$T"; mkdir -p "$T/fake" "$T/home"
cat > "$T/fake/xcode-select" <<SH
#!/bin/bash
echo "\$*" >> "$T/calls.txt"
[[ "\$1" == "--install" ]] && exit 0
echo "xcode-select: error: unable to get active developer directory" >&2; exit 2
SH
chmod +x "$T/fake/xcode-select"
export ROTK_NO_UI=1 ROTK_CLT_WAIT=6
FP="$T/fake:/usr/bin:/bin:/usr/sbin:/sbin"
SEMANTIC=$((SEMANTIC + 1))

# 1. clt.sh
PATH="$FP" bash "$ROOT/tools/dmg/clt.sh" "$T/prog1" > "$T/clt.log" 2>&1; rc=$?
[[ $rc == 1 ]] || fail "clt.sh exit $rc, want 1"
grep -q -- "--install" "$T/calls.txt" 2>/dev/null || fail "clt.sh did not run xcode-select --install"
grep -q "Command Line Tools" "$T/prog1" 2>/dev/null || fail "clt.sh wrote no message for the player"
grep -q "^100 ERROR" "$T/prog1" 2>/dev/null || fail "clt.sh: no final ERROR line (the progress window would wait)"

# 2. build_game.sh
: > "$T/calls.txt"
mkdir -p "$T/kit"; cp "$ROOT/tools/dmg/clt.sh" "$T/kit/"
PATH="$FP" bash "$ROOT/tools/dmg/build_game.sh" "$T/kit" "$T/nodisc" "$T/out/rotk-bin" "$T/prog2" > "$T/build.log" 2>&1; rc=$?
[[ $rc == 1 ]] || fail "build_game.sh exit $rc, want 1"
grep -q -- "--install" "$T/calls.txt" || fail "build_game.sh did not ask for CLT"
tail -1 "$T/prog2" | grep -q "^100 ERROR.*Command Line Tools" || fail "build_game.sh: last progress line '$(tail -1 "$T/prog2")'"
[[ -e "$T/out/rotk-bin" ]] && fail "build_game.sh produced a binary without a compiler"

# 3. the launcher in the DMG layout
: > "$T/calls.txt"
A="$T/ROTK.app/Contents"; mkdir -p "$A/MacOS" "$A/Resources/build"
cp "$ROOT/tools/dmg/clt.sh" "$A/Resources/build/"
clang -O1 -o "$A/MacOS/ROTK" "$ROOT/tools/launcher/launcher.c" -framework CoreGraphics -framework CoreFoundation 2> "$T/cc.log" || { fail "launcher does not compile"; finish; }
env -i HOME="$T/home" PATH="$FP" ROTK_NO_UI=1 ROTK_CLT_WAIT=6 ROTK_DATA= "$A/MacOS/ROTK" > "$T/launcher.out" 2>&1; rc=$?
[[ $rc == 74 ]] || fail "launcher exit $rc, want 74 (>128 = crash)"
grep -q -- "--install" "$T/calls.txt" || fail "launcher did not ask for CLT"
L=$(find "$T/home" -name rotk.log | head -1)
grep -q "launcher: alert: Command Line Tools" "${L:-/dev/null}" "$T/launcher.out" 2>/dev/null || fail "launcher logged no CLT alert"
grep -q "import" "${L:-/dev/null}" 2>/dev/null && grep -q "import: start" "$L" && fail "launcher imported the disc without CLT"

# with CLT (the real xcode-select): no --install, exit 0
if xcode-select -p >/dev/null 2>&1; then
  : > "$T/calls.txt"
  bash "$ROOT/tools/dmg/clt.sh" "$T/prog4" > "$T/clt4.log" 2>&1 || fail "clt.sh fails with CLT installed"
fi
echo "installer_no_clt: launcher rc=$rc, calls: $(sort -u "$T/calls.txt" 2>/dev/null | tr '\n' ' ')"
finish
