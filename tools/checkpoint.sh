#!/bin/bash
# Saves progress outside the working tree so a crashed app, a removed worktree or a
# bad reset never loses work (RULES R-S2):
#   1. every local branch (all worktrees share this repo) -> bare mirror ROTKPort/.cache/backup/rotk.git
#   2. uncommitted work in each worktree -> refs/checkpoint/<branch> in the BACKUP only
#      (git stash create: touches neither the worktree nor the index; the snapshot is
#      pushed by hash, so `git log --all` of the project never shows stash-like commits)
#
#   tools/checkpoint.sh          any time; idempotent; seconds
# Restore: git fetch ROTKPort/.cache/backup/rotk.git 'refs/checkpoint/*:refs/checkpoint/*'
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && git rev-parse --path-format=absolute --git-common-dir)"
ROOT="$(dirname "$ROOT")"
BACKUP="${ROTK_CACHE:-$HOME/Desktop/Claude/ROTKPort/.cache}/backup/rotk.git"

[[ -d "$BACKUP" ]] || git init -q --bare "$BACKUP"
git -C "$BACKUP" config core.logAllRefUpdates always

git -C "$ROOT" push -q --force "$BACKUP" 'refs/heads/*:refs/heads/*'

git -C "$ROOT" worktree list --porcelain | awk '/^worktree /{print $2}' | while read -r wt; do
  branch="$(git -C "$wt" rev-parse --abbrev-ref HEAD)"
  [[ "$branch" == HEAD ]] && branch="detached/$(basename "$wt")"
  if [[ -n "$(git -C "$wt" ls-files -u 2>/dev/null)" ]]; then
    echo "skip snapshot: $branch (merge in progress in $wt)"
    continue
  fi
  snap="$(git -C "$wt" stash create "checkpoint $(date '+%F %T')" 2>/dev/null || true)"
  if [[ -n "$snap" ]]; then
    git -C "$ROOT" push -q --force "$BACKUP" "$snap:refs/checkpoint/$branch"
    echo "snapshot: $branch -> ${snap:0:9}"
  fi
done

echo "checkpoint ok: $(git -C "$BACKUP" for-each-ref | wc -l | tr -d ' ') refs in $BACKUP"
