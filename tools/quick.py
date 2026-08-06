#!/usr/bin/env python3
"""One fast pass over a captured lap, for live coaching between laps.

analyze.py resamples the whole lap against the reference and takes long enough
that the driver is a lap and a half further on by the time it answers. This
reads only the handful of columns a coaching note actually turns on, in a
single streaming pass, and prints one block.

    python3 tools/quick.py /mnt/c/rline-coach/laps/lap-0018.csv
"""

import csv
import sys

# Corner spans as the rig detects them on data/lap.csv, with the reference's
# own minimum speed through each. Hard-coded so this needs no second file read.
CORNERS = [
    (1, 0.0400, 0.1183, 211),
    (2, 0.1158, 0.1892, 148),
    (3, 0.2995, 0.3651, 112),
    (4, 0.3456, 0.4107, 112),
    (5, 0.3795, 0.4511, 123),
    (6, 0.4470, 0.5142, 126),
    (7, 0.4763, 0.6034, 126),
    (8, 0.5978, 0.6726, 259),
    (9, 0.7336, 0.7984, 140),
    (10, 0.7806, 0.8533, 140),
    (11, 0.8309, 0.9016, 159),
]

# Where lateral grip stops improving for this driver, in radians. Past this,
# more lock produces less grip - so it is the number worth counting.
PEAK_STEER = 1.5


def main(path):
    with open(path, newline="") as fh:
        r = csv.reader(fh)
        hdr = next(r)
        ip = hdr.index("LapDistPct")
        isp = hdr.index("Speed")
        ist = hdr.index("SteeringWheelAngle")
        ib = hdr.index("Brake")
        ise = hdr.index("SessionTime")

        # per-corner: min speed, peak steer, samples, samples past the peak
        agg = {c[0]: [1e9, 0.0, 0, 0] for c in CORNERS}
        t0 = t1 = None
        n = 0

        for x in r:
            try:
                p = float(x[ip])
                v = float(x[isp]) * 3.6
                s = abs(float(x[ist]))
                t = float(x[ise])
            except (ValueError, IndexError):
                continue
            n += 1
            if t0 is None:
                t0 = t
            t1 = t
            for num, a, b, _ in CORNERS:
                if a <= p <= b:
                    g = agg[num]
                    if v < g[0]:
                        g[0] = v
                    if s > g[1]:
                        g[1] = s
                    g[2] += 1
                    if s > PEAK_STEER:
                        g[3] += 1

    if not n:
        print("no usable rows")
        return

    lap = (t1 - t0) if (t0 is not None and t1 is not None) else 0.0
    print("lap %.1f s   (reference 100.0)   %d rows" % (lap, n))
    print("%3s %8s %8s %9s %9s" % ("cnr", "vmin", "ref", "pkSteer", ">peak"))

    over = []
    for num, _, _, refv in CORNERS:
        vmin, pk, tot, past = agg[num]
        if tot == 0:
            continue
        pct = 100.0 * past / tot
        flag = "  <<" if pct > 5.0 else ""
        print("%3d %8.0f %8d %9.2f %8.1f%%%s" % (num, vmin, refv, pk, pct, flag))
        if pct > 5.0:
            over.append((pct, num, pk, vmin, refv))

    if over:
        over.sort(reverse=True)
        pct, num, pk, vmin, refv = over[0]
        print("\nworst lock: turn %d at %.2f rad, %.0f%% past the grip peak, "
              "%.0f km/h under" % (num, pk, pct, refv - vmin))
    else:
        print("\nno corner past the grip peak - clean lap on the wheel")


if __name__ == "__main__":
    main(sys.argv[1] if len(sys.argv) > 1 else "/mnt/c/rline-coach/laps/lap-0001.csv")
