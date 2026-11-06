#!/bin/bash
# Where is the game stuck? Runs it (test profile, background), samples the process after
# AFTER seconds and prints, per thread, the innermost lifted functions (guest VAs) and the
# host function under them (a kernel wait, the HLE, a spin in lifted code).
#   tests/hang_report.sh [AFTER=20] [VAR=value ...]
source "$(dirname "$0")/lib.sh"
AFTER="${1:-20}"
[[ "$AFTER" =~ ^[0-9]+$ ]] && shift || AFTER=20
OUT="${ROTK_CACHE:-$HOME/Desktop/Claude/ROTKPort/.cache}/runs/hang-$(date +%Y%m%d-%H%M%S)"
mkdir -p "$OUT"
( RUN_TIMEOUT=$((AFTER + 30)) run_game hang_probe "$@" > "$OUT/run.txt" 2>&1 ) &
for _ in $(seq 1 60); do pgrep -x rotk > /dev/null && break; sleep 0.5; done
sleep "$AFTER"
PID="$(pgrep -x rotk | head -1)"
[[ -z "$PID" ]] && { echo "hang_report: the game is not running after ${AFTER} s (exited?)"; cat "$OUT/run.txt"; exit 1; }
sample "$PID" 3 -file "$OUT/sample.txt" > /dev/null 2>&1
kill "$PID" 2> /dev/null
wait
python3 -I - "$OUT/sample.txt" <<'PY'
# sample's call tree: "<indent><count> <frame>  (in <image>) ...". Per thread, follow the
# heaviest child at every level; print the last lifted frames and the host leaf.
import re, sys
nodes, thread = [], None
for l in open(sys.argv[1]).read().split("\n"):
    m = re.match(r"^\s+(\d+) (Thread_\d+.*)$", l)
    if m:
        thread = m.group(2)[:70]
        nodes.append((thread, -1, int(m.group(1)), "THREAD"))
        continue
    m = re.match(r"^([\s+!:|]*)(\d+) (.+?)  \(in ", l)
    if m and thread:
        g = re.search(r"gen::(f_00[0-9A-F]{6})", m.group(3))
        nodes.append((thread, len(m.group(1)), int(m.group(2)), g.group(1) if g else m.group(3)[:70]))
by = {}
for n in nodes:
    by.setdefault(n[0], []).append(n)
for t, ns in by.items():
    path, i = [], 1
    depth = -1
    while i < len(ns):
        # children of the current node: the following nodes with the next depth until a shallower one
        kids, j = [], i
        d0 = ns[i][1] if i < len(ns) else None
        while j < len(ns) and ns[j][1] >= d0:
            if ns[j][1] == d0:
                kids.append(j)
            j += 1
        if not kids:
            break
        k = max(kids, key=lambda x: ns[x][2])
        path.append(ns[k])
        i = k + 1
        if i >= len(ns) or ns[i][1] <= ns[k][1]:
            break
    lifted = [p[3] for p in path if p[3].startswith("f_00")]
    if lifted:
        print(f"{t}\n   {path[-1][2]} samples: {' > '.join(lifted[-6:])}  [leaf: {path[-1][3]}]")
PY
echo "full sample: $OUT/sample.txt"
