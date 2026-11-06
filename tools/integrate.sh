#!/bin/bash
# One call for the orchestrator's integration loop (RULES R-I1; HP2 did it by hand ~15 times):
#   guard -> integrate lock -> detached worktree ROTKPort/.cache/integ at main ->
#   merge branches one by one (a conflicting branch is skipped and reported with the owners
#   of the conflicting paths) -> build -> tests/run.sh -> fast-forward main -> install ->
#   smoke of the INSTALLED .app -> checkpoint -> line in docs/status/orchestrator.md.
#
#   tools/integrate.sh --branches a,b,c [--quick|--full] [--install] [--no-checkpoint] [--dry]
#   tools/integrate.sh --all-ready ...    branches whose docs/status/<role>.md says "ready: yes"
# --dry: merge + build + test in the integ tree, never touch main (a role checking its branch
#        against another one uses this instead of merging main into itself, R-I2).
set -uo pipefail

ROOT="$(cd "$(dirname "$0")/.." && git rev-parse --path-format=absolute --git-common-dir)"
ROOT="$(dirname "$ROOT")"
INTEG="${ROTK_CACHE:-$HOME/Desktop/Claude/ROTKPort/.cache}/integ"
MODE=quick INSTALL=0 CHECKPOINT=1 DRY=0 BRANCHES=""
while [[ $# -gt 0 ]]; do
  case "$1" in
    --branches) BRANCHES="$2"; shift ;;
    --all-ready) BRANCHES="$(grep -l '^ready: *yes' "$ROOT"/docs/status/*.md 2>/dev/null |
                  xargs -n1 grep -h '^branch:' | awk '{print $2}' | paste -sd, -)" ;;
    --quick) MODE=quick ;; --full) MODE=full ;;
    --install) INSTALL=1 ;; --no-checkpoint) CHECKPOINT=0 ;; --dry) DRY=1 ;;
    *) sed -n 2,12p "$0"; exit 2 ;;
  esac
  shift
done
[[ -n "$BRANCHES" ]] || { echo "no branches"; exit 2; }

if [[ -z "${PORTKIT_INTEG_LOCKED:-}" ]]; then
  python3 -I "$ROOT/tools/guard.py" >/dev/null; g=$?
  (( g == 2 )) && { echo "guard STOP — free resources first"; exit 2; }
  exec env PORTKIT_INTEG_LOCKED=1 python3 -I "$ROOT/tools/lock.py" integrate -- "$0" \
    --branches "$BRANCHES" "--$MODE" $([[ $INSTALL == 1 ]] && echo --install) \
    $([[ $CHECKPOINT == 0 ]] && echo --no-checkpoint) $([[ $DRY == 1 ]] && echo --dry)
fi

mkdir -p "$INTEG" && REPORT="$INTEG/report-$(date +%Y%m%d-%H%M%S).md"
TREE="$INTEG/tree"
if [[ ! -d "$TREE/.git" && ! -f "$TREE/.git" ]]; then
  git -C "$ROOT" worktree add -q --detach "$TREE" main
fi
git -C "$TREE" checkout -q --detach main && git -C "$TREE" reset -q --hard main
# Untracked toolchain (x86recomp, deps) is gitignored: clone it from the main checkout (APFS cp -c)
for d in third_party/x86recomp third_party/deps; do
  [[ -d "$ROOT/$d" && ! -d "$TREE/$d" ]] && mkdir -p "$TREE/third_party" && cp -cR "$ROOT/$d" "$TREE/$d"
done

owner_of() {  # path -> owner from CLAUDE.md ownership table
  awk -F'|' -v p="$1" '/^\| [A-Za-z].*\| `/{o=$2; gsub(/^ +| +$/,"",o); n=split($3,a,","); for(i=1;i<=n;i++){x=a[i]; gsub(/[ `]/,"",x); if(x!="" && index(p,x)==1){print o; exit}}}' "$ROOT/CLAUDE.md"
}

merged=() skipped=()
IFS=, read -ra list <<< "$BRANCHES"
for b in "${list[@]}"; do
  if git -C "$TREE" merge -q --no-ff --no-edit "$b" >/dev/null 2>&1; then
    merged+=("$b")
  else
    files="$(git -C "$TREE" diff --name-only --diff-filter=U)"
    git -C "$TREE" merge --abort
    who="$(for f in $files; do echo "$f ($(owner_of "$f"))"; done | paste -sd' ' -)"
    skipped+=("$b: conflict in $who")
  fi
done

# Lift pins must agree (R-B2)
pins="$(git -C "$TREE" show HEAD:lift/LIFT_ID 2>/dev/null)"
build_ok=1
cmake -S "$TREE" -B "$INTEG/out" -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo >"$INTEG/cmake.log" 2>&1 \
  && cmake --build "$INTEG/out" -j"$(python3 -I "$ROOT/tools/guard.py" --jobs)" >"$INTEG/build.log" 2>&1 || build_ok=0

test_ok=0
if (( build_ok )); then
  BUILD="$INTEG/out" bash "$TREE/tests/run.sh" "$MODE" "$INTEG/tests.md" && test_ok=1
fi

{
  echo "# Integration $(date '+%F %T') ($MODE)"
  echo; echo "merged: ${merged[*]:-none}"
  for s in "${skipped[@]+"${skipped[@]}"}"; do echo "- skipped $s"; done
  echo "lift: $pins"; echo "build: $([[ $build_ok == 1 ]] && echo ok || echo FAILED, $INTEG/build.log)"
  [[ -f "$INTEG/tests.md" ]] && cat "$INTEG/tests.md"
} > "$REPORT"

if (( build_ok && test_ok && ! DRY )) && (( ${#merged[@]} )); then
  new="$(git -C "$TREE" rev-parse HEAD)"
  if git -C "$ROOT" rev-parse --abbrev-ref HEAD | grep -qx main; then
    git -C "$ROOT" merge -q --ff-only "$new" || { echo "main checkout not fast-forwardable (local edits?)" >> "$REPORT"; }
  else
    git -C "$ROOT" update-ref refs/heads/main "$new" "$(git -C "$ROOT" rev-parse main)"
  fi
  echo "main -> $(git -C "$ROOT" rev-parse --short main)" >> "$REPORT"
  if (( INSTALL )); then
    python3 -I "$ROOT/tools/lock.py" install -- bash "$TREE/tools/install_app.sh" "$INTEG/out" >> "$REPORT" 2>&1 \
      && bash "$TREE/tools/app_smoke.sh" >> "$REPORT" 2>&1 || echo "INSTALL/SMOKE FAILED" >> "$REPORT"
  fi
  (( CHECKPOINT )) && bash "$ROOT/tools/checkpoint.sh" >> "$REPORT" 2>&1
  verdict="main updated"
else
  verdict=$( (( DRY )) && echo "dry run" || echo "main NOT updated")
fi
mkdir -p "$ROOT/docs/status"
echo "- $(date '+%F %H:%M') integrate: merged ${merged[*]:-none}; skipped ${#skipped[@]}; build $build_ok; tests $test_ok; $verdict ($REPORT)" \
  >> "$ROOT/docs/status/orchestrator.md"
cat "$REPORT"
(( build_ok && test_ok ))
