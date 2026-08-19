#!/usr/bin/env python3
"""What the rest of the field actually does through each corner.

The problem this solves. Every "time lost per corner" figure so far is measured
against data/muguello-ref.csv, an 83.63 s lap that is **2.4 s faster than the
quickest lap anyone drove in his session**. Chasing per-corner numbers from a
lap nobody there could match is how a driver ends up being told to carry more
speed into a corner he is already at the limit of - the cue that has made this
one slower every single time.

So this builds the target from the session itself. The rig streams
CarIdxLapDistPct for all 72 slots (see data/car-channels.txt), so every car's
position is known at 60 Hz and their speed follows from differentiating it.

    tools/field.py <lap.csv>...

Two things to keep in mind about the numbers:

  smoothing  Other cars' positions arrive over the network well below 60 Hz, so
             the raw derivative is a staircase. It is smoothed over ~0.3 s,
             which is fine for a corner minimum and useless for a brake trace.
             Do not try to read anyone else's technique out of this - only
             their speed.

  resolution A car whose position updates only a handful of times through a
             corner has no resolvable minimum - the smoothing runs straight
             through it and reports a speed far higher than anything real. The
             first version of this reported 175 km/h through turn twelve, where
             the reference does 133 and the fastest human managed about 140.
             Sending a driver at a corner on that number is exactly the failure
             this tool exists to avoid, so a corner is now discarded unless the
             car produced enough DISTINCT positions through it to resolve one.

  who        A car's minimum through a corner is only meaningful if it was
             actually racing: pit road, off-track and not-in-world slots are
             excluded. It still cannot tell a car on a slow lap from a fast one,
             so the useful statistic is the BEST any car managed, not the mean.
"""

import csv
import sys

TRACK_M = 5189.0
TRK_ON_TRACK = 3
SMOOTH_S = 0.30

CORNERS = [
    ("turn one", 0.063, 0.227),
    ("turn four", 0.212, 0.326),
    ("turn six", 0.298, 0.531),
    ("turn ten", 0.510, 0.621),
    ("turn twelve", 0.611, 0.771),
    ("turn fifteen", 0.775, 0.895),
]

# Distinct position updates a car must produce inside a corner before its
# minimum is believed. Below this the smoothing window is wider than the corner
# and the "minimum" is really an average of the approach.
MIN_UPDATES = 12

# A derived minimum above this multiple of the reference's is not a fast driver,
# it is an unresolved corner. The reference is a verified-clean lap quicker than
# anyone in the session, so nobody is beating it through a slow corner by tens of
# km/h; when the first version reported 175 through turn twelve against a 133
# reference, that was undersampling, not somebody's line.
#
# The method itself is sound: run against the ego car's own 60 Hz position it
# reproduces true minimum speed to within 3 km/h at every corner. Only sparse
# cars need bounding, and this errs towards discarding them - a missing target
# is recoverable, a target 40 km/h too high is not.
MAX_OVER_REF = 1.10

# The reference lap's own minimums, for context only - see the docstring.
REF = {"turn one": 129, "turn four": 196, "turn six": 229,
       "turn ten": 147, "turn twelve": 133, "turn fifteen": 153}


def load(path):
    with open(path, newline="") as fh:
        r = csv.reader(fh)
        head = next(r)
        want = {n: head.index(n) for n in ("SessionTime", "LapDistPct", "Speed")
                if n in head}
        if len(want) < 3:
            return None, None, 0
        ncars = 0
        while "CarIdxLapDistPct_%02d" % ncars in head:
            ncars += 1
        if ncars == 0:
            return None, None, 0

        pidx = [head.index("CarIdxLapDistPct_%02d" % n) for n in range(ncars)]
        sidx = [head.index("CarIdxTrackSurface_%02d" % n)
                if "CarIdxTrackSurface_%02d" % n in head else None
                for n in range(ncars)]
        oidx = [head.index("CarIdxOnPitRoad_%02d" % n)
                if "CarIdxOnPitRoad_%02d" % n in head else None
                for n in range(ncars)]

        mine, others = [], [[] for _ in range(ncars)]
        for row in r:
            try:
                t = float(row[want["SessionTime"]])
                p = float(row[want["LapDistPct"]])
                v = float(row[want["Speed"]])
            except (ValueError, IndexError):
                continue
            mine.append((t, p, v))
            for n in range(ncars):
                try:
                    cp = float(row[pidx[n]])
                except (ValueError, IndexError):
                    continue
                if cp < 0:
                    continue
                if sidx[n] is not None:
                    try:
                        if int(float(row[sidx[n]])) != TRK_ON_TRACK:
                            continue
                    except (ValueError, IndexError):
                        pass
                if oidx[n] is not None:
                    try:
                        if float(row[oidx[n]]):
                            continue
                    except (ValueError, IndexError):
                        pass
                others[n].append((t, cp))
    return mine, others, ncars


def speeds(series):
    """[(t, pct, km/h)] from a position series, smoothed over SMOOTH_S."""
    out = []
    j = 0
    for i in range(len(series)):
        t, p = series[i]
        while j < len(series) and series[j][0] < t - SMOOTH_S:
            j += 1
        if j >= i:
            continue
        t0, p0 = series[j]
        dt = t - t0
        if dt <= 0:
            continue
        d = p - p0
        if d < -0.5:
            d += 1.0          # lap wrap
        elif d < 0:
            continue          # went backwards; not a racing sample
        out.append((t, p, d * TRACK_M / dt * 3.6))
    return out


def main(paths):
    best = {c[0]: None for c in CORNERS}
    best_car = {c[0]: None for c in CORNERS}
    mine_best = {c[0]: None for c in CORNERS}
    seen_cars = set()

    for path in paths:
        try:
            mine, others, ncars = load(path)
        except OSError:
            continue
        if mine is None:
            print("%s: no CarIdx columns - not captured with car-channels.txt"
                  % path.split("/")[-1])
            continue

        kmh = 3.6 if max(v for _, _, v in mine) < 120 else 1.0
        for label, lo, hi in CORNERS:
            seg = [v * kmh for _, p, v in mine if lo <= p <= hi]
            if seg:
                m = min(seg)
                if mine_best[label] is None or m > mine_best[label]:
                    mine_best[label] = m

        for n in range(ncars):
            if len(others[n]) < 200:
                continue
            sp = speeds(others[n])
            if not sp:
                continue
            seen_cars.add(n)
            for label, lo, hi in CORNERS:
                inseg = [(p, v) for _, p, v in sp if lo <= p <= hi]
                if len(inseg) < 20:
                    continue
                # Distinct positions, not samples: the stream repeats the last
                # known position at 60 Hz between network updates, so sample
                # count says nothing about whether the minimum is resolvable.
                if len({round(p, 5) for p, _ in inseg}) < MIN_UPDATES:
                    continue
                m = min(v for _, v in inseg)
                # Implausible values mean the derivative caught a teleport or a
                # tow, not a car going slowly.
                if m < 40 or m > 320:
                    continue
                if m > REF[label] * MAX_OVER_REF:
                    continue
                if best[label] is None or m > best[label]:
                    best[label] = m
                    best_car[label] = n

    print("%-14s %8s %8s %8s   %s" % ("corner", "you", "field", "ref", "gap to field"))
    print("-" * 62)
    for label, _, _ in CORNERS:
        y = mine_best[label]
        f = best[label]
        gap = ("%+.0f" % (y - f)) if (y is not None and f is not None) else "-"
        print("%-14s %8s %8s %8d   %s%s" % (
            label,
            "%.0f" % y if y is not None else "-",
            "%.0f" % f if f is not None else "-",
            REF[label], gap,
            "" if best_car[label] is None else "  (car %d)" % best_car[label]))
    print("\n%d cars contributed. 'field' is the best any car managed through that"
          % len(seen_cars))
    print("corner, so it is a real target from the same session, same track state.")
    print("The 'ref' column is the reference lap, which was 2.4 s faster than")
    print("anyone in the 2026-08-12 race - context, not a target.")
    return 0


if __name__ == "__main__":
    if len(sys.argv) < 2:
        print(__doc__)
        sys.exit(2)
    sys.exit(main(sys.argv[1:]))
