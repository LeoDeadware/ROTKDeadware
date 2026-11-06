#!/usr/bin/env python3
"""Runs a command under a project-wide lock (macOS has no flock(1)).
Blocks until the lock is free; prints who holds it while waiting.

  tools/lock.py heavy -- cmake --build out --target rotk_game -j8   lift / full game-lib build
  tools/lock.py install -- tools/install_app.sh                       orchestrator only
  tools/lock.py integrate -- tools/integrate.sh ...                   one integration at a time
  tools/lock.py runs -- tests/scripts/x.sh                            game test runs: 2 slots
  tools/lock.py status

`heavy` first waits on tools/guard.py (RAM/swap/disk). `runs` is a semaphore with
2 slots (runs-0.lock, runs-1.lock, ...): the third run waits (RULES R-T3).
"""
import fcntl
import os
import subprocess
import sys
import time

LOCK_DIR = os.path.expanduser(os.environ.get("ROTK_CACHE", "~/Desktop/Claude/ROTKPort/.cache") + "/locks")
LOCKS = ("heavy", "install", "integrate", "runs")
RUN_SLOTS = int(os.environ.get("ROTK_RUNS_MAX", "2"))


def holder(path):
    try:
        return open(path).read().strip() or "?"
    except OSError:
        return "?"


def paths(name):
    if name == "runs":
        return [os.path.join(LOCK_DIR, f"runs-{i}.lock") for i in range(RUN_SLOTS)]
    return [os.path.join(LOCK_DIR, name + ".lock")]


def try_lock(path):
    f = open(path, "a+")
    try:
        fcntl.flock(f, fcntl.LOCK_EX | fcntl.LOCK_NB)
        return f
    except BlockingIOError:
        f.close()
        return None


def status():
    for name in LOCKS:
        for p in paths(name):
            if not os.path.exists(p):
                print(f"{os.path.basename(p)[:-5]}: free")
                continue
            f = try_lock(p)
            if f:
                f.close()
                print(f"{os.path.basename(p)[:-5]}: free")
            else:
                print(f"{os.path.basename(p)[:-5]}: held by {holder(p)}")


def main():
    if len(sys.argv) >= 2 and sys.argv[1] == "status":
        return status()
    if len(sys.argv) < 4 or sys.argv[1] not in LOCKS or sys.argv[2] != "--":
        sys.exit(__doc__)
    name, cmd = sys.argv[1], sys.argv[3:]
    guard = os.path.join(os.path.dirname(os.path.abspath(__file__)), "guard.py")
    if name == "heavy" and subprocess.call([sys.executable, "-I", guard, "--wait"]) != 0:
        sys.exit("[lock] resources too low; not starting")
    os.makedirs(LOCK_DIR, exist_ok=True)
    waited, f, path = False, None, None
    while not f:
        for p in paths(name):
            f = try_lock(p)
            if f:
                path = p
                break
        if f:
            break
        if not waited:
            print(f"[lock] {name} held by " + "; ".join(holder(p) for p in paths(name)) + "; waiting",
                  file=sys.stderr)
            waited = True
        time.sleep(2)
    f.seek(0)
    f.truncate()
    f.write(f"{os.getcwd()} pid {os.getpid()} since {time.strftime('%H:%M:%S')}: {' '.join(cmd)}\n")
    f.flush()
    try:
        sys.exit(subprocess.call(cmd))
    finally:
        f.seek(0)
        f.truncate()


if __name__ == "__main__":
    main()
