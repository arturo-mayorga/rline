#!/usr/bin/env python3
"""One fast pass over a captured lap, for live coaching between laps.

analyze.py resamples the whole lap against the reference and takes long enough
that the driver is a lap and a half further on by the time it answers. This
reads only the handful of columns a coaching note actually turns on, in a
single streaming pass, and prints one block.

    python3 tools/quick.py /mnt/c/rline-coach/laps/lap-0018.csv
    python3 tools/quick.py lap.csv --track roadamerica

Defaults to Mugello, which is what the rig is currently shipping.
"""

import csv
import sys

# Corner spans exactly as the rig's detector finds them, with the reference's
# own minimum speed through each. Hard-coded so this needs no second file read
# and stays fast enough to answer between laps.
#
# Each row is (detected number, spoken name, pctTurnIn, pctExit, ref vmin km/h).
# The span is turn-in to exit, NOT entry to exit: corner spans overlap - 4 of 6
# here and 8 of 11 at Road America - so a window that opens at the braking
# point picks up the previous corner's apex and reports its minimum speed
# instead of this corner's. That leak is what makes analyze.py print the same
# "ref vmin" for adjacent corners, and it had put four wrong reference numbers
# in this table.
#
# Regenerate with the rig's own detector rather than by hand - anything worked
# out another way will disagree with what the car is actually being coached on:
#
#   g++ -std=c++17 -O2 -o /tmp/dump-corners tools/dump-corners.cpp src/refline.cpp
#   /tmp/dump-corners data/muguello-ref.csv
TRACKS = {
    # Six detected corners across fifteen numbered turns. Every name stands for
    # a stretch of road, not a turn - see data/corner-names-mugello.txt.
    "mugello": {
        "ref_lap_s": 83.63,
        "corners": [
            (1, "San Donato",  0.1246, 0.2272, 129),
            (2, "Materassi",   0.2734, 0.3257, 196),
            (3, "Casanova",    0.3600, 0.5309, 229),
            (4, "Scarperia",   0.5720, 0.6213, 147),
            (5, "Correntaio",  0.6733, 0.7708, 133),
            (6, "Bucine",      0.8371, 0.8949, 153),
        ],
        # Corners the reference barely brakes for. Load here rises with speed,
        # not lock, so these pull the most lateral g on the lap while looking
        # unthreatened on the wheel. A speed deficit through one of them is not
        # a corner to ask for more entry speed in - the rig suppresses that cue
        # on them, and so should anything read off this table.
        "aero": {3},
    },
    "roadamerica": {
        "ref_lap_s": 100.0,
        "corners": [
            (1,  "T1",        0.0901, 0.1183, 211),
            (2,  "T3",        0.1658, 0.1892, 148),
            (3,  "T5",        0.3496, 0.3651, 112),
            (4,  "T6",        0.3953, 0.4107, 123),
            (5,  "T7",        0.4294, 0.4511, 212),
            (6,  "T8",        0.4970, 0.5142, 126),
            (7,  "T9",        0.5264, 0.6034, 201),
            (8,  "Carousel",  0.6478, 0.6726, 283),
            (9,  "T12",       0.7834, 0.7984, 140),
            (10, "T13",       0.8306, 0.8533, 233),
            (11, "T14",       0.8808, 0.9016, 159),
        ],
        "aero": {8},
    },
}

# Where lateral grip stops improving for this driver, in radians. Past this,
# more lock produces less grip - so it is the number worth counting.
PEAK_STEER = 1.5


def main(path, track):
    if track not in TRACKS:
        print("unknown track %r - known: %s" % (track, ", ".join(sorted(TRACKS))))
        return 1
    cfg = TRACKS[track]
    corners = cfg["corners"]
    aero = cfg["aero"]

    with open(path, newline="") as fh:
        r = csv.reader(fh)
        hdr = next(r)
        ip = hdr.index("LapDistPct")
        isp = hdr.index("Speed")
        ist = hdr.index("SteeringWheelAngle")
        ib = hdr.index("Brake")
        ise = hdr.index("SessionTime")

        # per-corner: min speed, peak steer, samples, samples past the peak
        agg = {c[0]: [1e9, 0.0, 0, 0] for c in corners}
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
            for num, _, a, b, _ in corners:
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
        return 1

    lap = (t1 - t0) if (t0 is not None and t1 is not None) else 0.0
    print("%s: lap %.1f s   (reference %.2f)   %d rows"
          % (track, lap, cfg["ref_lap_s"], n))
    print("%3s %-11s %7s %7s %8s %8s" % ("cnr", "name", "vmin", "ref", "pkSteer", ">peak"))

    over = []
    for num, name, _, _, refv in corners:
        vmin, pk, tot, past = agg[num]
        if tot == 0:
            continue
        pct = 100.0 * past / tot
        tag = "  aero" if num in aero else ""
        flag = "  <<" if pct > 5.0 else ""
        print("%3d %-11s %7.0f %7d %8.2f %7.1f%%%s%s"
              % (num, name, vmin, refv, pk, pct, tag, flag))
        # Aero corners are excluded from the "worst" pick for the same reason
        # the rig will not cue speed on them: the answer there is commitment
        # and line, and there is no brake release to move.
        if pct > 5.0 and num not in aero:
            over.append((pct, num, name, pk, vmin, refv))

    if over:
        over.sort(reverse=True)
        pct, num, name, pk, vmin, refv = over[0]
        print("\nworst lock: %s at %.2f rad, %.0f%% past the grip peak, "
              "%.0f km/h under" % (name, pk, pct, refv - vmin))
    else:
        print("\nno corner past the grip peak - clean lap on the wheel")
    return 0


if __name__ == "__main__":
    args = [a for a in sys.argv[1:] if not a.startswith("--")]
    track = "mugello"
    for i, a in enumerate(sys.argv):
        if a == "--track" and i + 1 < len(sys.argv):
            track = sys.argv[i + 1]
            if track in args:
                args.remove(track)
    lap = args[0] if args else "/mnt/c/rline-coach/laps/lap-0001.csv"
    sys.exit(main(lap, track))
