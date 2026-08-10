#!/usr/bin/env python3
"""Where does the time actually go?

Compares a captured lap against the reference lap, indexed by track position.
The headline is the cumulative time delta by corner: three seconds is never
spread evenly, and this localises it before anything else is worth looking at.

    analyze.py <lap.csv> [--ref data/lap.csv] [--corners]

Runs on the analysis machine, never on the rig.
"""

import csv
import math
import sys
from bisect import bisect_left

# The track being driven, not the repository's oldest reference. Override with
# --ref. Pointing this at the wrong circuit does not fail loudly - it silently
# compares every corner against a different piece of road.
REF_DEFAULT = "/home/amayorga/dev/rline/rline/data/muguello-ref.csv"


def load_reference(path):
    """Reference lap ordered by track position, with elapsed time and corners."""
    lat, lon, pct, spd, brk, str_ = [], [], [], [], [], []
    with open(path) as f:
        for row in csv.DictReader(f):
            try:
                la, lo = float(row["Lat"]), float(row["Lon"])
            except (KeyError, ValueError):
                continue
            if la == 0 and lo == 0:
                continue
            lat.append(la); lon.append(lo)
            pct.append(float(row["LapDistPct"]))
            spd.append(float(row["Speed"]))
            brk.append(float(row.get("Brake", 0) or 0))
            str_.append(float(row.get("SteeringWheelAngle", 0) or 0))

    lat0 = sum(lat) / len(lat)
    lon0 = sum(lon) / len(lon)
    r = math.radians(lat0)
    mlat = 111132.92 - 559.82 * math.cos(2 * r) + 1.175 * math.cos(4 * r)
    mlon = 111412.84 * math.cos(r) - 93.5 * math.cos(3 * r)

    pts = sorted(zip(pct, [(lo - lon0) * mlon for lo in lon],
                     [(la - lat0) * mlat for la in lat], spd, brk, str_))

    # Elapsed time by integrating distance over speed: independent of log rate.
    t, length = [0.0], 0.0
    for i in range(1, len(pts)):
        d = math.hypot(pts[i][1] - pts[i - 1][1], pts[i][2] - pts[i - 1][2])
        length += d
        t.append(t[-1] + d / max(pts[i - 1][3], 1.0))

    return {
        "pct": [p[0] for p in pts],
        "x": [p[1] for p in pts], "y": [p[2] for p in pts],
        "speed": [p[3] for p in pts], "brake": [p[4] for p in pts],
        "steer": [p[5] for p in pts],
        "t": t, "length": length, "laptime": t[-1],
    }


def detect_corners(ref):
    """Corner spans from the steering trace, mirroring the overlay's logic."""
    steer, pct = ref["steer"], ref["pct"]
    n = len(steer)
    smax = max(abs(s) for s in steer)
    enter, exit_ = 0.22 * smax, 0.12 * smax

    s = [0.0] * n
    for i in range(1, n):
        s[i] = s[i - 1] + math.hypot(ref["x"][i] - ref["x"][i - 1],
                                     ref["y"][i] - ref["y"][i - 1])

    spans, inc, start = [], False, 0
    for i in range(n):
        a = abs(steer[i])
        if not inc and a > enter:
            inc, start = True, i
        elif inc and a < exit_:
            inc = False
            spans.append((start, i))
    if inc:
        spans.append((start, n - 1))

    merged = []
    for sp in spans:
        if merged and s[sp[0]] - s[merged[-1][1]] < 40.0:
            merged[-1] = (merged[-1][0], sp[1])
        else:
            merged.append(sp)

    corners = []
    for a, b in merged:
        if s[b] - s[a] < 20.0:
            continue
        apex = max(range(a, b + 1), key=lambda i: abs(steer[i]))
        # Entry: back up to where the driver was last at full speed.
        entry = a
        while entry > 0 and s[a] - s[entry] < 250.0:
            entry -= 1
        corners.append({"n": len(corners) + 1, "entry": entry, "turnin": a,
                        "apex": apex, "exit": b,
                        "pct_entry": pct[entry], "pct_apex": pct[apex],
                        "pct_exit": pct[b]})
    return corners


def load_lap(path):
    """A captured lap, keeping only the channels the analysis uses."""
    want = ["SessionTime", "Lap", "LapDistPct", "Speed", "Throttle", "Brake",
            "Gear", "RPM", "SteeringWheelAngle", "IsOnTrack", "LapDist"]
    out = {k: [] for k in want}
    with open(path) as f:
        rd = csv.DictReader(f)
        for row in rd:
            try:
                vals = {k: float(row[k]) for k in want}
            except (KeyError, ValueError, TypeError):
                continue
            for k in want:
                out[k].append(vals[k])
    return out


def resample_by_pct(lap):
    """Elapsed time and speed as a function of track position, monotonic."""
    pct, t, spd = [], [], []
    t0 = None
    last = -1.0
    for i in range(len(lap["LapDistPct"])):
        p = lap["LapDistPct"][i]
        if lap["IsOnTrack"][i] < 0.5:
            continue
        if p <= last:          # only forward progress
            continue
        if t0 is None:
            t0 = lap["SessionTime"][i]
        last = p
        pct.append(p)
        t.append(lap["SessionTime"][i] - t0)
        spd.append(lap["Speed"][i])
    return pct, t, spd


def interp(xs, ys, x):
    i = bisect_left(xs, x)
    if i <= 0:
        return ys[0]
    if i >= len(xs):
        return ys[-1]
    x0, x1 = xs[i - 1], xs[i]
    if x1 == x0:
        return ys[i]
    f = (x - x0) / (x1 - x0)
    return ys[i - 1] + f * (ys[i] - ys[i - 1])


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        return 1

    lap_path = sys.argv[1]
    ref_path = REF_DEFAULT
    if "--ref" in sys.argv:
        ref_path = sys.argv[sys.argv.index("--ref") + 1]

    ref = load_reference(ref_path)
    corners = detect_corners(ref)
    lap = load_lap(lap_path)
    pct, t, spd = resample_by_pct(lap)

    if len(pct) < 100:
        print("not enough on-track forward motion in this lap to analyse")
        return 1

    covered = pct[-1] - pct[0]
    print("reference : %.2f s, %.0f m, %d corners" % (ref["laptime"], ref["length"], len(corners)))
    print("this lap  : %.1f s over %.1f%% of the track (pct %.3f -> %.3f)"
          % (t[-1], covered * 100, pct[0], pct[-1]))
    stopped = sum(1 for v in lap["Speed"] if v < 1.0)
    if stopped > 60:
        print("            %.0f s stationary - this is not a clean flying lap"
              % (stopped / 60.0))
    print()

    # Cumulative time delta against the reference, by track position.
    def delta_at(p):
        return (interp(pct, t, p) - interp(pct, t, pct[0])) - \
               (interp(ref["pct"], ref["t"], p) - interp(ref["pct"], ref["t"], pct[0]))

    print("%-4s %-8s %10s %10s   %8s %8s" %
          ("cnr", "pct", "cum delta", "lost here", "your vmin", "ref vmin"))
    print("-" * 60)

    rows = []
    prev = delta_at(pct[0])
    for c in corners:
        if not (pct[0] <= c["pct_exit"] <= pct[-1]):
            continue
        d = delta_at(c["pct_exit"])
        lost = d - prev
        prev = d

        def vmin(xs, ys, a, b):
            vals = [ys[i] for i in range(len(xs)) if a <= xs[i] <= b]
            return min(vals) if vals else float("nan")

        yv = vmin(pct, spd, c["pct_entry"], c["pct_exit"]) * 3.6
        rv = vmin(ref["pct"], ref["speed"], c["pct_entry"], c["pct_exit"]) * 3.6
        rows.append((c["n"], lost, yv, rv))
        print("%-4d %-8.3f %+10.2f %+10.2f   %8.0f %8.0f"
              % (c["n"], c["pct_apex"], d, lost, yv, rv))

    print()
    worst = sorted(rows, key=lambda r: -r[1])[:3]
    print("biggest losses:")
    for n, lost, yv, rv in worst:
        note = ""
        if not math.isnan(yv) and not math.isnan(rv) and yv < rv - 5:
            note = "  (min speed %.0f km/h under reference)" % (rv - yv)
        print("  corner %-2d  %+.2f s%s" % (n, lost, note))
    return 0


if __name__ == "__main__":
    sys.exit(main())
