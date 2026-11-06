#!/usr/bin/env python3
"""frame_stats.py frames.tsv first last [budget_ms] [max_over_pct]

Per-frame timing from a ROTK_FRAME_LOG file (d3d8: "frame ms busy_ms gpu_wait_ms draws",
one line per Swap; busy = the frame minus every vblank wait in it, i.e. the host's own cost).
Over frames first..last: busy p50/p95/p99/max, the share of frames over budget_ms (default
16.7 = one 60 Hz vblank), frame-time p50, GPU wait p50/p99, draws avg.
Prints one line "fps: ..." and exits 1 when more than max_over_pct (default 1.0) % of the
frames are over budget, 2 on bad input (no frames in the range).
"""
import sys


def pct(v, p):
    if not v:
        return 0.0
    i = min(len(v) - 1, max(0, int(round(p / 100.0 * (len(v) - 1)))))
    return v[i]


def main():
    path, first, last = sys.argv[1], int(sys.argv[2]), int(sys.argv[3])
    budget = float(sys.argv[4]) if len(sys.argv) > 4 else 16.7
    max_over = float(sys.argv[5]) if len(sys.argv) > 5 else 1.0
    min_fps = float(sys.argv[6]) if len(sys.argv) > 6 else 0.0  # average presented fps
    rows = []
    try:
        with open(path) as f:
            for line in f:
                if line.startswith("#"):
                    continue
                p = line.split()
                if len(p) < 5:
                    continue
                fr = int(p[0])
                if first <= fr <= last:
                    rows.append((fr, float(p[1]), float(p[2]), float(p[3]), int(p[4])))
    except OSError as e:
        print(f"fps: cannot read {path}: {e}")
        return 2
    if not rows:
        print(f"fps: no frames {first}..{last} in {path}")
        return 2
    busy = sorted(r[2] for r in rows)
    ms = sorted(r[1] for r in rows)
    gpu = sorted(r[3] for r in rows)
    over = sum(1 for b in busy if b > budget)
    share = 100.0 * over / len(rows)
    draws = sum(r[4] for r in rows) / len(rows)
    fps = 1000.0 * len(rows) / sum(ms)
    print(f"fps: frames {rows[0][0]}..{rows[-1][0]} n={len(rows)} busy p50 {pct(busy, 50):.2f} "
          f"p95 {pct(busy, 95):.2f} p99 {pct(busy, 99):.2f} max {busy[-1]:.2f} ms; "
          f"over {budget} ms: {over} ({share:.2f} %); frame p50 {pct(ms, 50):.2f} ms, avg {fps:.1f} fps; "
          f"gpu wait p50 {pct(gpu, 50):.2f} p99 {pct(gpu, 99):.2f} ms; draws avg {draws:.0f}")
    if fps < min_fps:
        print(f"fps: average {fps:.1f} fps < {min_fps}")
        return 1
    return 1 if share > max_over else 0


if __name__ == "__main__":
    sys.exit(main())
