#!/usr/bin/env python3
"""Compare brake bias settings from laps already captured.

No bookkeeping: dcBrakeBias is in the telemetry, so laps are grouped by the
setting they were actually driven at. Change it in the car, drive a few laps,
run this.

    tools/bias.py [lapdir] [--track mugello]

Three things this does that the first version did not, all learned on
2026-08-11/12:

  per corner   Minimum speed per corner is far more sensitive to bias than lap
               time and much less polluted by traffic - and on a race weekend
               most laps have someone in front. Lap time is reported too, but
               the corner table is the one to read.

  partial laps A bias sweep is driven as a run of pit-exit laps, which never
               cover a full lap and so cannot be timed as one. Every corner
               lies inside pct 0.061-1.000, so the common segment is compared
               instead - that is how 55.15 was shown to beat 57.25 by 0.64 s.

  cheaply      Lap files carry 574 columns and ~13 MB since the per-car arrays
               were added. Only the handful of columns needed are parsed, or a
               sweep takes minutes instead of seconds.
"""

import csv
import glob
import os
import sys
from collections import defaultdict

TRACKS = {
    # name: (length_m, [(corner label, entry pct, exit pct)])
    "mugello": (5189.0, [
        ("turn one", 0.063, 0.227),
        ("turn four", 0.212, 0.326),
        ("turn six", 0.298, 0.531),
        ("turn ten", 0.510, 0.621),
        ("turn twelve", 0.611, 0.771),
        ("turn fifteen", 0.775, 0.895),
    ]),
}

NEED = ["dcBrakeBias", "SteeringWheelAngle", "Speed", "LapDistPct", "SessionTime"]
# Every corner starts after this, so a pit-exit lap still covers all of them.
SEGMENT_FROM = 0.061


def read(path):
    """Only the columns needed, by index, so a 13 MB file is cheap."""
    with open(path, newline="") as fh:
        r = csv.reader(fh)
        head = next(r)
        if "dcBrakeBias" not in head:
            return None
        idx = {}
        for n in NEED:
            if n not in head:
                return None
            idx[n] = head.index(n)

        out = {n: [] for n in NEED}
        prev_t = None
        for row in r:
            try:
                vals = {n: float(row[idx[n]]) for n in NEED}
            except (ValueError, IndexError):
                continue
            if prev_t is not None and vals["SessionTime"] < prev_t:
                return None                    # spans a session boundary
            prev_t = vals["SessionTime"]
            if not (0.0 <= vals["LapDistPct"] <= 1.0):
                continue
            for n in NEED:
                out[n].append(vals[n])
    return out if len(out["Speed"]) > 2000 else None


def main(argv):
    track = "mugello"
    paths = []
    i = 0
    while i < len(argv):
        if argv[i] == "--track" and i + 1 < len(argv):
            track = argv[i + 1]
            i += 2
            continue
        paths.append(argv[i])
        i += 1

    # The LIVE lap directory by default: it holds only the current session's
    # files. The archive holds every partial scan copy ever taken - over 1500
    # by 2026-08-12 - and walking it takes minutes for no benefit, because a
    # bias sweep is always about laps just driven.
    lapdir = paths[0] if paths else "/mnt/c/rline-coach/laps"
    length, corners = TRACKS[track]

    groups = defaultdict(list)
    for fn in sorted(glob.glob(os.path.join(lapdir, "*.csv"))):
        # Cheap reject before opening: a lap is several MB at 574 columns.
        try:
            if os.path.getsize(fn) < 3_000_000:
                continue
        except OSError:
            continue
        try:
            rows = read(fn)
        except OSError:
            continue
        if not rows:
            continue
        bias = sum(rows["dcBrakeBias"]) / len(rows["dcBrakeBias"])
        # Bias changed mid-lap: it belongs to neither setting.
        if max(rows["dcBrakeBias"]) - min(rows["dcBrakeBias"]) > 0.05:
            continue
        groups[round(bias, 2)].append((os.path.basename(fn), rows))

    if not groups:
        print("no laps with a bias channel in %s" % lapdir)
        return 0

    print("brake bias, %s, from %s\n" % (track, lapdir))
    print("%-8s %5s %11s %11s %10s" %
          ("bias", "laps", "segment", "full lap", "steer>1.5"))
    print("-" * 50)

    per_corner = {}
    for bias in sorted(groups, reverse=True):
        laps = groups[bias]
        segs, fulls, over = [], [], []
        mins = {c[0]: [] for c in corners}
        for _, r in laps:
            pct, t, sp = r["LapDistPct"], r["SessionTime"], r["Speed"]

            # Common segment: from SEGMENT_FROM to the end of the lap.
            seg = [(p, tt) for p, tt in zip(pct, t) if p >= SEGMENT_FROM]
            if len(seg) > 1000:
                segs.append(seg[-1][1] - seg[0][1])
            if max(pct) - min(pct) > 0.98:
                fulls.append(t[-1] - t[0])

            fast = [i for i in range(len(sp)) if sp[i] > 15]
            if fast:
                over.append(100.0 * sum(
                    1 for i in fast if abs(r["SteeringWheelAngle"][i]) > 1.5) / len(fast))

            kmh = 3.6 if max(sp) < 120 else 1.0
            for label, lo, hi in corners:
                seg_sp = [sp[i] for i in range(len(pct)) if lo <= pct[i] <= hi]
                if seg_sp:
                    mins[label].append(min(seg_sp) * kmh)

        per_corner[bias] = mins
        print("%-8.2f %5d %11s %11s %9.1f%%" % (
            bias, len(laps),
            "%.2f s" % min(segs) if segs else "-",
            "%.2f s" % min(fulls) if fulls else "-",
            sum(over) / len(over) if over else 0.0))

    print("\nbest minimum speed per corner, km/h")
    print("%-8s %s" % ("bias", "".join("%13s" % c[0] for c in corners)))
    print("-" * (8 + 13 * len(corners)))
    for bias in sorted(per_corner, reverse=True):
        cells = ""
        for label, _, _ in corners:
            v = per_corner[bias][label]
            cells += "%13s" % ("%.0f" % max(v) if v else "-")
        print("%-8.2f%s" % (bias, cells))

    print("\nRead the corner table first: minimum speed moves with bias and is far")
    print("less polluted by traffic than lap time. 'steer>1.5' falling means the")
    print("car is rotating with less lock, which is the point of moving it rearward.")
    print("Compare the SEGMENT column across settings - full laps are often absent")
    print("from a sweep because every run starts from the pits.")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
