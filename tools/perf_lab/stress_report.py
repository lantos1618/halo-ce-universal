#!/usr/bin/env python3
"""Turn a stress run's log (debug.stress, port/linux/game/perf_stress.c) into
a table of game tick time against the number of characters.

    tools/perf_lab/stress_report.py build/macos/Halo/host.txt [more logs...]

Each "stress:" line is the ticks between two steps: how many characters had
been placed, the objects and actors alive, the ticks' average and slowest
time and the average of each part (units, AI, effects, objects, the rest).
A game tick has 33.3 ms (30 a second); past that the game slows down.
"""

import re
import sys

LINE = re.compile(r"stress: kind=(\w+) spawned=(\d+) (.*)")
FIELD = re.compile(r"(\w+)=([\d.]+)")
BUDGET_MS = 1000.0 / 30.0


def parse(path):
    rows = []
    for line in open(path, encoding="utf-8", errors="replace"):
        match = LINE.search(line)
        if not match:
            continue
        row = {"kind": match.group(1), "spawned": int(match.group(2))}
        for name, value in FIELD.findall(match.group(3)):
            row[name] = float(value)
        rows.append(row)
    return rows


def bar(value, scale, width=30):
    filled = int(round(min(value / scale, 1.0) * width)) if scale > 0 else 0
    return "#" * filled + "." * (width - filled)


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        return 1
    for path in sys.argv[1:]:
        rows = parse(path)
        if not rows:
            print("%s: no stress lines" % path)
            continue
        kind = rows[0]["kind"]
        print("### %s: %s" % (path, kind))
        print()
        print("| placed | objects | actors | units | tick ms (avg) | tick ms (max) | units | ai | effects | objects | other | load |")
        print("|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|:---|")
        scale = max(max(row.get("tick_ms", 0.0) for row in rows), 1e-3)
        over = None
        for row in rows:
            tick = row.get("tick_ms", 0.0)
            if over is None and tick > BUDGET_MS:
                over = row["spawned"]
            print("| %d | %d | %d | %d | %.2f | %.2f | %.2f | %.2f | %.2f | %.2f | %.2f | `%s` |" % (
                row["spawned"], row.get("objects", 0), row.get("actors", 0), row.get("units", 0), tick,
                row.get("max_ms", 0.0), row.get("units_ms", 0.0), row.get("ai_ms", 0.0), row.get("effects_ms", 0.0),
                row.get("objects_ms", 0.0), row.get("other_ms", 0.0), bar(tick, scale, 20)))
        print()
        first, last = rows[0], rows[-1]
        added = last["spawned"] - first["spawned"]
        if added > 0:
            slope = (last.get("tick_ms", 0.0) - first.get("tick_ms", 0.0)) / added
            print("%.3f ms of tick per %s added (%d to %d)." % (slope, kind[:-1], first["spawned"], last["spawned"]))
            if slope > 0:
                room = (BUDGET_MS - last.get("tick_ms", 0.0)) / slope
                if over is not None:
                    print("The tick passed its 33.3 ms (30 ticks a second) at %d %s." % (over, kind))
                else:
                    print("At that rate the tick would reach 33.3 ms at about %d %s." % (
                        last["spawned"] + max(room, 0), kind))
        print()
    return 0


if __name__ == "__main__":
    sys.exit(main())
