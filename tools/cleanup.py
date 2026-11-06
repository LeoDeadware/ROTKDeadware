#!/usr/bin/env python3
"""Frees disk where this project grows. Safe any time; prints what it removes (RULES R-R4).

  tools/cleanup.py            prune
  tools/cleanup.py --dry      show only

- ROTKPort/.cache/lift/<id>: keeps lift/CURRENT, every id pinned by a worktree, the 2 newest.
- Test runs (ROTKPort/.cache/runs/*): a run dir with result.json "PASS" and no "keep" file goes
  after 1 h; FAIL / golden candidates ("keep" file) after 24 h; and the whole runs dir is held
  under RUNS_BUDGET_GB by deleting oldest-first (PASS before FAIL). HP2 lesson: 249 run dirs,
  22 GB, because "older than 24 h" alone is not enough at dozens of runs per hour.
- Build dirs of worktrees that no longer exist (ROTKPort/.cache/out/<name>): only if unused
  for 6 h AND no live pid / lock file inside (HP2 lesson: cleanup deleted a build dir mid-run).
- ccache: trimmed to 10 GB.
"""
import json
import os
import shutil
import subprocess
import sys
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
CACHE = os.path.expanduser(os.environ.get("ROTK_CACHE", "~/Desktop/Claude/ROTKPort/.cache") + "")
DRY = "--dry" in sys.argv
RUNS_BUDGET_GB = float(os.environ.get("ROTK_RUNS_BUDGET_GB", "10"))


def du(path):
    total = 0
    for d, _, files in os.walk(path):
        for f in files:
            try:
                total += os.lstat(os.path.join(d, f)).st_size
            except OSError:
                pass
    return total


def rm(path, why, size=None):
    size = du(path) if size is None else size
    print(f"{'would remove' if DRY else 'remove'} {path} ({size / 2**30:.2f} GB) — {why}")
    if not DRY:
        shutil.rmtree(path, ignore_errors=True)
    return size


def recently_used(path, now, window):
    for d, _, files in os.walk(path):
        for f in [None] + files:
            try:
                st = os.stat(os.path.join(d, f) if f else d)
            except OSError:
                continue
            if now - max(st.st_mtime, st.st_atime) < window:
                return True
    return False


def live_pid_inside(path):
    for d, _, files in os.walk(path):
        for f in files:
            if f.endswith((".pid", ".lock")):
                try:
                    pid = int(open(os.path.join(d, f)).read().split()[0])
                    os.kill(pid, 0)
                    return True
                except (OSError, ValueError, IndexError):
                    continue
    return False


def worktrees():
    out = subprocess.run(["git", "-C", ROOT, "worktree", "list", "--porcelain"],
                         capture_output=True, text=True).stdout
    return [l.split(" ", 1)[1] for l in out.splitlines() if l.startswith("worktree ")]


def run_state(p):
    try:
        return json.load(open(os.path.join(p, "result.json"))).get("verdict", "?")
    except (OSError, ValueError):
        return "?"


def main():
    now = time.time()
    trees = worktrees()
    keep = set()
    for t in trees:
        for name in ("lift/LIFT_ID", "lift/CURRENT"):
            try:
                keep.add(open(os.path.join(t, name)).read().strip().split("=")[-1])
            except OSError:
                pass
    lifts = os.path.join(CACHE, "lift")
    if os.path.isdir(lifts):
        ids = sorted(os.listdir(lifts), key=lambda i: os.path.getmtime(os.path.join(lifts, i)))
        keep.update(ids[-2:])
        for i in ids:
            if i not in keep:
                rm(os.path.join(lifts, i), "lift not pinned and not among 2 newest")

    runs = os.path.join(CACHE, "runs")
    if os.path.isdir(runs):
        entries = []
        for r in os.listdir(runs):
            p = os.path.join(runs, r)
            if live_pid_inside(p):
                continue
            age = now - os.path.getmtime(p)
            st = run_state(p)
            kept = os.path.exists(os.path.join(p, "keep"))
            if st.startswith("PASS") and not kept and age > 3600:
                rm(p, "passed run, older than 1 h")
            elif age > 86400:
                rm(p, f"{st} run older than 24 h")
            else:
                entries.append((0 if st.startswith("PASS") else 1, os.path.getmtime(p), p, du(p)))
        total = sum(e[3] for e in entries)
        for _, _, p, size in sorted(entries):
            if total <= RUNS_BUDGET_GB * 2**30:
                break
            total -= rm(p, f"runs over {RUNS_BUDGET_GB:.0f} GB budget", size)

    outs = os.path.join(CACHE, "out")
    live = {os.path.basename(t) for t in trees}
    if os.path.isdir(outs):
        for o in os.listdir(outs):
            p = os.path.join(outs, o)
            if o not in live and not recently_used(p, now, 6 * 3600) and not live_pid_inside(p):
                rm(p, "worktree gone, unused for 6 h, no live process")

    cc = shutil.which("ccache") or ("/opt/homebrew/bin/ccache" if os.path.exists("/opt/homebrew/bin/ccache") else None)
    if cc and not DRY:
        env = dict(os.environ, CCACHE_DIR=os.path.join(CACHE, "ccache"))
        subprocess.run([cc, "--max-size=10G"], env=env, capture_output=True)
        subprocess.run([cc, "--cleanup"], env=env, capture_output=True)
        print("ccache trimmed to 10 GB")


if __name__ == "__main__":
    main()
