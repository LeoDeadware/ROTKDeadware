#!/bin/bash
# Triage of the newest crash of a test run: reason, lifted host frames, the guest function at
# the fault (Ghidra name, title/symbols.tsv name, callers) and the head of its decompilation.
#   tools/crash_triage.sh [RUN_DIR | NAME_PREFIX] [LINES=40]
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
ARG="${1:-probe_free}"
LINES="${2:-40}"
if [[ -d "$ARG" ]]; then RUN="$ARG"; else RUN="$(ls -td "${ROTK_CACHE:-$HOME/Desktop/Claude/ROTKPort/.cache}/runs/$ARG"-* 2>/dev/null | head -1)"; fi
[[ -d "$RUN" ]] || { echo "no run for $ARG"; exit 1; }
CR="$(find "$RUN" -name 'rotk-crash-*.txt' | sort | tail -1)"
echo "run: $RUN"
grep -v "donor gate\|mixer running" "$RUN/stderr.log" | awk '!s[$0]++' | grep -E "FATAL|dispatch-miss|first call|warning" | tail -6
[[ -n "$CR" ]] || { echo "no crash report"; exit 0; }
echo "--- host frames (lifted)"
grep -oE "gen::f_00[0-9A-F]{6}" "$CR" | awk '!s[$0]++' | head -8 | tr '\n' ' '; echo
VA="$(grep -oE "raised from 00[0-9A-F]{6}|guest memory fault .* near guest 00[0-9A-F]{6}|unlifted address 00[0-9A-F]{6}" "$CR" | grep -oE "00[0-9A-F]{6}" | head -1)"
[[ -z "$VA" ]] && VA="$(grep -oE "gen::f_00[0-9A-F]{6}" "$CR" | head -1 | grep -oE "00[0-9A-F]{6}")"
python3 -I - "${ROTK_DATA:-$ROOT}" "$ROOT" "$VA" "$LINES" <<'PY'
import sys, csv, os
data, root, va, lines = sys.argv[1], sys.argv[2], int(sys.argv[3], 16), int(sys.argv[4])
fns = []
for l in open(os.path.join(data, "recon/ghidra/export/functions.tsv")):
    f = l.rstrip("\n").split("\t")
    try: fns.append((int(f[0], 16), f[1], f[2], f[4] if len(f) > 4 else ""))
    except ValueError: pass
fns.sort()
own = max((f for f in fns if f[0] <= va), default=None)
names = {}
for r in csv.DictReader((l for l in open(os.path.join(root, "title/symbols.tsv")) if not l.startswith("#")), delimiter="\t"):
    for v in (r["target_va"] or "").split():
        names.setdefault(int(v, 16), set()).add(f"{r['name']}({r['confidence']})")
print(f"--- fault 0x{va:06X} in {own[1]} [{own[2]}] = {sorted(names.get(own[0], ['?']))}")
print(f"    callers: {own[3][:120]}")
dec = open(os.path.join(data, "recon/ghidra/export/decomp.c")).read()
tag = f"// ==== {own[1]} @"
i = dec.find(tag)
if i >= 0:
    j = dec.find("// ==== FUN_", i + 10)
    body = [l for l in dec[i:j].split("\n") if l.strip()]
    print("\n".join(body[:lines]))
PY
