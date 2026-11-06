#!/bin/bash
# Golden workflow (RULES R-T5): candidates from UPDATE_GOLDEN=1 runs wait in
# tests/golden/_pending/. Look at them first:
#   tests/golden_accept.sh --index      writes _pending/index.html (candidate | current | diff numbers)
#   tests/golden_accept.sh a.png b.png  moves the named candidates into tests/golden/
# The commit that adds them must say what each image shows ("menu, no overlay, Harry at the door").
set -euo pipefail
G="$(cd "$(dirname "$0")" && pwd)/golden"
P="$G/_pending"
if [[ "${1:-}" == --index ]]; then
  {
    echo "<!doctype html><meta charset=utf-8><title>golden candidates</title><style>body{font:14px sans-serif;background:#222;color:#eee}img{width:480px}td{padding:4px;vertical-align:top}</style><table>"
    for f in "$P"/*.png; do
      [[ -f "$f" ]] || continue
      b="$(basename "$f")"
      d="new"
      [[ -f "$G/$b" ]] && d="$(python3 -I "$(dirname "$0")/scripts/png_diff.py" "$f" "$G/$b" 0 0 2>&1 | tail -1 || true)"
      echo "<tr><td>$b<br>$d</td><td><img src='$b'></td><td><img src='../$b' alt='(no current)'></td></tr>"
    done
    echo "</table>"
  } > "$P/index.html"
  echo "open $P/index.html"
  exit 0
fi
[[ $# -gt 0 ]] || { sed -n 2,7p "$0"; exit 2; }
for b in "$@"; do
  mv "$P/$(basename "$b")" "$G/$(basename "$b")"
  echo "accepted $(basename "$b")"
done
