#!/bin/bash
# Interface language of the launcher and the installer (owner 2026-11-06: «за замовчуванням інсталлер
# та лончер мають бути англійською»; «мову обирають в лончері»): with no saved choice everything
# starts in English; the launcher's Українська switches the launcher text (and the game).
#   1. the launcher window (tools/launcher/launcher.js, ROTK_LAUNCHER_SNAPSHOT) with an empty
#      settings.toml: language English, labels Language / Frame rate / Gamma / Play / Quit and the
#      ProMotion hint in English; with language = "uk": Мова / Грати and the hint in Ukrainian;
#   2. the installer's progress (tools/dmg/build_game.sh with a kit that has no compiler step) and
#      its window title (tools/dmg/progress.js): no Cyrillic by default; ROTK_UI_LANG=uk: Ukrainian;
#   3. the launcher's own alerts (tools/launcher/launcher.c, a bundle without game data whose disc
#      import fails): English by default.
source "$(dirname "$0")/../lib.sh"
T="${ROTK_CACHE:-$ROOT/.cache}/runs/ui_lang_default-$$"; rm -rf "$T"; mkdir -p "$T"
cyr() { LC_ALL=en_US.UTF-8 grep -q '[А-Яа-яІіЇїЄєҐґ]' "$@"; }
SEMANTIC=$((SEMANTIC + 1))
: > "$T/empty.toml"
ROTK_DISPLAY_HZ=60,120 ROTK_LAUNCHER_SNAPSHOT="$T/en.png" osascript -l JavaScript "$ROOT/tools/launcher/launcher.js" "$T/empty.toml" > "$T/en" 2>&1
grep -qx 'language: en' "$T/en" || fail "launcher: language not English by default ($(grep '^language' "$T/en"))"
grep -qx 'labels: Language|Frame rate|Gamma|Play|Quit' "$T/en" || fail "launcher labels not English by default ($(grep '^labels' "$T/en"))"
grep '^hint text:' "$T/en" | cyr && fail "launcher ProMotion hint not English by default"
printf '[launcher]\nlanguage = "uk"\n' > "$T/uk.toml"
ROTK_DISPLAY_HZ=60,120 ROTK_LAUNCHER_SNAPSHOT="$T/uk.png" osascript -l JavaScript "$ROOT/tools/launcher/launcher.js" "$T/uk.toml" > "$T/uk" 2>&1
grep -q '^labels: Мова|.*|Грати|' "$T/uk" || fail "launcher: Українська does not switch the labels ($(grep '^labels' "$T/uk"))"
grep '^hint text:' "$T/uk" | cyr || fail "launcher: Ukrainian ProMotion hint missing"
SEMANTIC=$((SEMANTIC + 1))
mkdir -p "$T/kit"; printf 'exit 1\n' > "$T/kit/clt.sh"
bash "$ROOT/tools/dmg/build_game.sh" "$T/kit" "$T/disc" "$T/out/rotk-bin" "$T/prog_en" > /dev/null 2>&1
[[ -s "$T/prog_en" ]] || fail "installer wrote no progress"
cyr "$T/prog_en" && fail "installer progress not English by default: $(tail -1 "$T/prog_en")"
ROTK_UI_LANG=uk bash "$ROOT/tools/dmg/build_game.sh" "$T/kit" "$T/disc" "$T/out/rotk-bin" "$T/prog_uk" > /dev/null 2>&1
cyr "$T/prog_uk" || fail "installer progress: ROTK_UI_LANG=uk gives no Ukrainian"
grep -E "^\s*win.title = '[^']*'" "$ROOT/tools/dmg/progress.js" | cyr && fail "installer window title not English"
SEMANTIC=$((SEMANTIC + 1))
C="$T/ROTK.app/Contents"; mkdir -p "$C/MacOS" "$C/Resources/import"
clang -O2 -o "$C/MacOS/ROTK" "$ROOT/tools/launcher/launcher.c" -framework CoreGraphics -framework CoreFoundation || { fail "launcher does not build"; finish; }
printf 'import sys; sys.exit(3)\n' > "$C/Resources/import/import_disc.py"
mkdir -p "$T/player"
ROTK_PLAYER_ROOT="$T/player" ROTK_NO_UI=1 ROTK_IMPORT_ISO=/nonexistent.iso HOME="$T/home" "$C/MacOS/ROTK" > /dev/null 2>&1
grep -h 'launcher: alert:' "$T"/player/logs/rotk.log "$T"/home/Library/Application\ Support/ROTK/player/logs/rotk.log 2>/dev/null > "$T/alerts"
[[ -s "$T/alerts" ]] || fail "no launcher alert on a failed import"
cyr "$T/alerts" && fail "launcher alert not English by default: $(head -1 "$T/alerts")"
rm -rf "$T"
# The message helper must not shadow a system command (tr() broke `wc -l | tr -d` in build_game.sh:
# "d: unbound variable", the DMG build failed at «Translating»): no shell function of the
# installer's scripts is named like a command in /bin or /usr/bin.
SEMANTIC=$((SEMANTIC + 1))
for f in "$ROOT"/tools/dmg/*.sh "$ROOT"/tools/dmg/hd/*.sh; do
  for fn in $(sed -nE 's/^[[:space:]]*([A-Za-z_][A-Za-z0-9_]*)[[:space:]]*\(\)[[:space:]]*\{.*/\1/p' "$f"); do
    [[ -x /bin/$fn || -x /usr/bin/$fn ]] && fail "$(basename "$f"): function $fn() shadows /usr/bin/$fn"
  done
done
finish
