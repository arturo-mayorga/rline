#!/usr/bin/env python3
"""Turn one completed lap into a short spoken coaching note.

Deterministic on purpose: the same lap always produces the same note, so the
advice is reproducible and can be checked against the telemetry afterwards.

    coach.py <lap.csv> [--send]

Prints the SAY line, and with --send appends it to the relay outbox.
"""

import csv
import importlib.util
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
OUTBOX = "/mnt/c/rline-coach/outbox.txt"

spec = importlib.util.spec_from_file_location("az", os.path.join(HERE, "analyze.py"))
az = importlib.util.module_from_spec(spec)
spec.loader.exec_module(az)


def ref_pedals(path):
    p, thr, brk = [], [], []
    for row in csv.DictReader(open(path)):
        try:
            if float(row["Lat"]) == 0:
                continue
            p.append(float(row["LapDistPct"]))
            thr.append(float(row["Throttle"]))
            brk.append(float(row["Brake"]))
        except (KeyError, ValueError, TypeError):
            continue
    o = sorted(range(len(p)), key=lambda i: p[i])
    return [p[i] for i in o], [thr[i] for i in o], [brk[i] for i in o]


def forward(lap):
    """On-track, monotonically forward samples only."""
    out = {k: [] for k in ("pct", "thr", "brk", "spd", "t")}
    last, t0 = -1.0, None
    for i in range(len(lap["LapDistPct"])):
        p = lap["LapDistPct"][i]
        if lap["IsOnTrack"][i] < 0.5 or p <= last:
            continue
        if t0 is None:
            t0 = lap["SessionTime"][i]
        last = p
        out["pct"].append(p)
        out["thr"].append(lap["Throttle"][i])
        out["brk"].append(lap["Brake"][i])
        out["spd"].append(lap["Speed"][i])
        out["t"].append(lap["SessionTime"][i] - t0)
    return out


def last_brake_before(pcts, brk, apex, window=0.05):
    r = None
    for i in range(len(pcts)):
        if apex - window <= pcts[i] <= apex + 0.01 and brk[i] > 0.10:
            r = pcts[i]
    return r


def first_full_throttle(pcts, thr, apex):
    for i in range(len(pcts)):
        if pcts[i] >= apex and thr[i] > 0.95:
            return pcts[i]
    return None


def analyse(lap_path, ref_path=az.REF_DEFAULT):
    ref = az.load_reference(ref_path)
    corners = az.detect_corners(ref)
    rp, rt, rb = ref_pedals(ref_path)
    y = forward(az.load_lap(lap_path))

    if len(y["pct"]) < 500 or y["pct"][-1] - y["pct"][0] < 0.9:
        return None  # not a complete lap

    laptime = y["t"][-1]
    L = ref["length"]

    releases, throttles = [], []
    for c in corners:
        ap = c["pct_apex"]
        yr = last_brake_before(y["pct"], y["brk"], ap)
        rr = last_brake_before(rp, rb, ap)
        if yr is not None and rr is not None:
            releases.append((c["n"], (yr - rr) * L))
        yf = first_full_throttle(y["pct"], y["thr"], ap)
        rf = first_full_throttle(rp, rt, ap)
        if yf is not None and rf is not None:
            throttles.append((c["n"], (yf - rf) * L))

    return {
        "laptime": laptime,
        "delta": laptime - ref["laptime"],
        "releases": releases,
        "throttles": throttles,
        "avg_release": sum(d for _, d in releases) / len(releases) if releases else 0.0,
        "avg_throttle": sum(d for _, d in throttles) / len(throttles) if throttles else 0.0,
    }


def note(a, prev_delta=None):
    """One short sentence. Spoken aloud, so no numbers that need reading twice."""
    d = a["delta"]
    parts = []

    if prev_delta is not None and abs(d - prev_delta) > 0.15:
        parts.append("%.1f, %s" % (a["laptime"],
                                   "better" if d < prev_delta else "slower"))
    else:
        parts.append("%.1f, %.1f off" % (a["laptime"], d))

    rel = a["avg_release"]
    thr = a["avg_throttle"]

    # Worst single corner, so the advice has somewhere to be applied.
    worst = min(a["releases"], key=lambda r: r[1]) if a["releases"] else None

    if rel < -12:
        s = "Still releasing the brakes early"
        if worst and worst[1] < -18:
            s += ", worst in turn %d" % worst[0]
        parts.append(s + ". Carry the brake to the apex.")
    elif rel < -6:
        parts.append("Brake release is closer. Keep trailing it in.")
    else:
        parts.append("Brake release is good.")
        if thr > 12:
            parts.append("Now get to full throttle earlier.")

    return " ".join(parts)


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        return 1

    a = analyse(sys.argv[1])
    if a is None:
        print("(incomplete lap, no note)")
        return 0

    prev = None
    if "--prev" in sys.argv:
        prev = float(sys.argv[sys.argv.index("--prev") + 1])

    text = note(a, prev)
    line = "SAY|secs=14|text=%s" % text

    print("lap %.1f s   delta %+.2f   avg brake release %+.0f m   avg full throttle %+.0f m"
          % (a["laptime"], a["delta"], a["avg_release"], a["avg_throttle"]))
    print(line)

    if "--send" in sys.argv:
        with open(OUTBOX, "a") as f:
            f.write(line + "\n")
        print("(sent)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
