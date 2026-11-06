#!/usr/bin/env python3
"""level_check.py run_dir Level [min_draws] [min_frames]

Semantic check that a run reached gameplay in the given level (e.g. Pat01):
  1. the level's scene Data\\Game\\<Dir>\\<Level>.scx was opened (kernel log, stderr.log)
     after the last ROTK_CALL_AT of the run (so the route, not the prologue, loaded it);
  2. after that open, the d3d8 periodic lines ("frame N: ... K draws last frame") show
     gameplay: K >= min_draws (default 50; movies and loading screens draw 1..20) for at
     least min_frames frames (default 1800 = 1 min at 30 fps), counted until the run ends.
  3. restarts: every re-open of the scene is a restart (mission failed -> "Try Again" reloads it;
     the failure menu itself draws > 50, so the draw counts miss it). Scripted input may fail a
     mission; a first attempt shorter than 1800 frames (1 min) fails the check (suspect: the
     level fails at once, e.g. a broken timer/trigger, or the bot cannot play it) unless every
     re-open came from the retry menu (a scripted A within 30 frames before it) and the first
     attempt is >= 600 frames: then the bot lost the fight. Seen: Cir00 run
     level_cir00-20261102-173925-85164-20491 frames 7800 (Sam at low health fighting orcs),
     8100 (dead), 8440 ("your mission has failed: Try Again / Exit To Menu").
Prints "level <L>: scene at line .., gameplay from frame F, G frames, draws min/avg/max";
exit 0 ok, 1 not reached, 2 error.
"""
import os

# Levels whose own fail condition ends a scripted (non-playing) run quickly; restarts are
# expected there. Pat03: fails in ~400 frames even idle; the producer hand-tested it on the
# installed app 97bf7c0 with a real pad: plays normally (2026-11-02).
RESTART_OK = {"pat03"}
import re
import sys


def main():
    run, level = sys.argv[1], sys.argv[2]
    min_draws = int(sys.argv[3]) if len(sys.argv) > 3 else 50
    min_frames = int(sys.argv[4]) if len(sys.argv) > 4 else 1800
    err = open(os.path.join(run, "stderr.log"), errors="replace").read().splitlines()
    call = max((i for i, l in enumerate(err) if "ROTK_CALL_AT" in l), default=-1)
    scene = re.compile(r"\[krnl\] open .*\\Data\\Game\\\w+\\%s\.scx .*-> 00000000" % re.escape(level), re.I)
    opened = [i for i, l in enumerate(err) if i > call and scene.search(l)]
    if not opened:
        print(f"level {level}: scene never opened after the route call")
        return 1
    # stderr.log carries the d3d8 lines too, in order with the kernel lines.
    per = re.compile(r"^d3d8: frame (\d+): .*?; (\d+) draws last frame")
    frames = [(int(m.group(1)), int(m.group(2))) for l in err[opened[0]:] if (m := per.match(l))]
    play = [(f, d) for f, d in frames if d >= min_draws]
    if not play:
        print(f"level {level}: scene opened (line {opened[0] + 1}) but no gameplay frames "
              f"(last periodic lines: {frames[-3:]})")
        return 1
    first, last = play[0][0], frames[-1][0]
    span = last - first
    tail = [d for f, d in frames if f >= first]
    low = sum(1 for d in tail if d < min_draws)
    draws = [d for _, d in play]
    print(f"level {level}: scene at line {opened[0] + 1}, gameplay from frame {first} to {last} "
          f"({span} frames, {low} periodic samples below {min_draws} draws), draws "
          f"min {min(draws)} / avg {sum(draws) // len(draws)} / max {max(draws)}")
    if len(opened) > 1:
        before = [int(m.group(1)) for l in err[opened[0]:opened[1]] if (m := per.match(l))]
        attempt = (before[-1] - first) if before and before[-1] > first else 0
        print(f"level {level}: scene re-opened {len(opened) - 1} times (mission failed, restarted); "
              f"first attempt {attempt} gameplay frames")
        # Retry-menu restart: the re-open follows a scripted "press A" within 30 frames, i.e. the
        # player confirmed "Try Again" on the mission-failed menu (sweep-qa2: Cir00/Cir01/Pal01
        # re-open exactly at the pad's A frames 8460, 10860, ...). A spontaneous re-open (no A)
        # or a first attempt < 600 frames stays suspect.
        press = re.compile(r"xinput: frame (\d+): \d+ press A\b")
        retry = 0
        for o in opened[1:]:
            a = [int(m.group(1)) for l in err[max(0, o - 5000):o] if (m := press.search(l))]
            f = [int(m.group(1)) for l in err[max(0, o - 5000):o] if (m := re.search(r"frame (\d+):", l))]
            if a and f and 0 <= f[-1] - a[-1] <= 30:
                retry += 1
        print(f"level {level}: {retry}/{len(opened) - 1} restarts through the retry menu (A pressed)")
        if attempt < 1800 and level.lower() not in RESTART_OK:
            if retry == len(opened) - 1 and attempt >= 600:
                print(f"level {level}: short first attempt accepted: every restart came from the "
                      "retry menu (scripted input loses the fight; not a port fault)")
            else:
                return 1
    if span < min_frames:
        print(f"level {level}: only {span} gameplay frames (< {min_frames})")
        return 1
    if low > len(tail) // 4:
        print(f"level {level}: {low}/{len(tail)} samples after gameplay start look like no level "
              "(loading screen, movie, black)")
        return 1
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except (OSError, IndexError, ValueError) as e:
        print(f"level_check: {e}")
        sys.exit(2)
