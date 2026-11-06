#!/usr/bin/env python3
"""soak_monitor.py — memory / CPU sampler for soak runs (R-T9).

  soak_monitor.py sample <run dir> <pid>     (lib.sh RUN_MONITOR) every 10 s until <pid> exits:
      appends "t  frame  rss_kb  cpu_s" to <run dir>/soak.tsv, where rss/cpu are those of the
      game process (the `rotk` descendant of <pid>) and frame is the last d3d8 periodic line.
  soak_monitor.py report <run dir> [from_frame]
      Prints frames, CPU ms per frame (all threads of the process: an upper bound of the
      game's own work; the d3d8 "busy" value includes the game's BlockUntilVerticalBlank
      wait, so it reads 33.3 ms at the game's 30 fps), RSS start/end/max and the RSS slope
      over the second half. Exit 1 if memory grows linearly: slope > 2 KiB/frame (120 MiB per
      60k frames) and end RSS > first-half max + 128 MiB. Exit 2: no samples.
"""
import os
import re
import subprocess
import sys
import time

PER = re.compile(rb"d3d8: frame (\d+):")


def last_frame(run):
    try:
        with open(os.path.join(run, "runtime.log"), "rb") as f:
            f.seek(0, 2)
            n = f.tell()
            # kernel trace lines (K> ...) can flood the log at ~1 MB/min: widen the tail window
            for win in (16384, 262144, 4194304):
                f.seek(max(0, n - win))
                hits = PER.findall(f.read())
                if hits:
                    return int(hits[-1])
            return 0
    except OSError:
        return 0


def cpu_seconds(s):
    days = 0
    if "-" in s:
        d, s = s.split("-", 1)
        days = int(d)
    total = 0.0
    for part in s.split(":"):
        total = total * 60 + float(part)
    return total + days * 86400


def game_proc(root):
    out = subprocess.run(["ps", "-A", "-o", "pid=,ppid=,rss=,time=,comm="],
                         capture_output=True, text=True).stdout
    procs = {}
    for line in out.splitlines():
        p = line.split(None, 4)
        if len(p) == 5:
            procs[int(p[0])] = (int(p[1]), int(p[2]), p[3], p[4])
    kids = {root}
    changed = True
    while changed:
        changed = False
        for pid, (ppid, *_r) in procs.items():
            if ppid in kids and pid not in kids:
                kids.add(pid)
                changed = True
    for pid in kids:
        if pid in procs and os.path.basename(procs[pid][3]) == "rotk":
            return procs[pid][1], cpu_seconds(procs[pid][2])
    return None


def sample(run, root):
    out = os.path.join(run, "soak.tsv")
    t0 = time.time()
    with open(out, "a") as f:
        f.write("t\tframe\trss_kb\tcpu_s\n")
        while True:
            try:
                os.kill(root, 0)
            except OSError:
                return 0
            g = game_proc(root)
            if g:
                f.write(f"{time.time() - t0:.0f}\t{last_frame(run)}\t{g[0]}\t{g[1]:.2f}\n")
                f.flush()
            time.sleep(10)


def slope(xs, ys):
    n = len(xs)
    mx, my = sum(xs) / n, sum(ys) / n
    den = sum((x - mx) ** 2 for x in xs)
    return sum((x - mx) * (y - my) for x, y in zip(xs, ys)) / den if den else 0.0


def report(run, start):
    rows = []
    for line in open(os.path.join(run, "soak.tsv")):
        p = line.split()
        if p and p[0] != "t":
            rows.append((float(p[0]), int(p[1]), int(p[2]), float(p[3])))
    # frame 0 = the sampler missed the periodic line (log flooded): rebuild it from time with
    # a linear fit (the game is paced, frame ~ t) over the rows that have a frame
    known = [(r[0], r[1]) for r in rows if r[1] > 0]
    if len(known) >= 2 and len(known) < len(rows):
        k = slope([a for a, _ in known], [b for _, b in known])
        mt = sum(a for a, _ in known) / len(known)
        mf = sum(b for _, b in known) / len(known)
        print(f"soak: {len(rows) - len(known)} samples without a frame, rebuilt from time "
              f"({k:.1f} frames/s)")
        rows = [r if r[1] > 0 else (r[0], int(mf + k * (r[0] - mt)), r[2], r[3]) for r in rows]
    rows = [r[1:] for r in rows]
    rows = [r for r in rows if r[0] >= start]
    if len(rows) < 4:
        print(f"soak: {len(rows)} samples after frame {start}: too few")
        return 2
    f0, f1 = rows[0][0], rows[-1][0]
    frames = f1 - f0
    cpu_ms = (rows[-1][2] - rows[0][2]) * 1000 / frames if frames else 0
    half = rows[len(rows) // 2:]
    kb_per_frame = slope([r[0] for r in half], [r[1] for r in half])
    first_max = max(r[1] for r in rows[:len(rows) // 2])
    rss = [r[1] for r in rows]
    print(f"soak: frames {f0}..{f1} ({frames}), CPU {cpu_ms:.1f} ms/frame (all threads), "
          f"RSS start {rss[0] // 1024} MiB end {rss[-1] // 1024} MiB max {max(rss) // 1024} MiB, "
          f"2nd-half slope {kb_per_frame * 1024:.0f} B/frame")
    if kb_per_frame > 2 and rss[-1] > first_max + 128 * 1024:
        print("soak: RSS grows linearly -> leak")
        return 1
    return 0


if __name__ == "__main__":
    if len(sys.argv) >= 4 and sys.argv[1] == "sample":
        sys.exit(sample(sys.argv[2], int(sys.argv[3])))
    if len(sys.argv) >= 3 and sys.argv[1] == "report":
        sys.exit(report(sys.argv[2], int(sys.argv[3]) if len(sys.argv) > 3 else 0))
    print(__doc__)
    sys.exit(2)
