#!/usr/bin/env python3
"""Lap times corrected for fuel load, for comparing laps across a race stint.

Why this exists. Every pace number quoted so far came from practice, where the
car is refuelled between runs and every lap carries roughly the same load. A
race stint is not like that: the car sheds fuel continuously, so the last lap is
faster than the first for reasons that have nothing to do with driving. Compare
lap 2 with lap 18 uncorrected and the driver looks like he improved by a second.

What this does NOT do is pretend to know the car's fuel sensitivity. It is
measured, by regressing lap time against fuel level across the laps given, and
the fit quality is printed so a bad fit can be disbelieved. With few laps, or
laps where the driving varied more than the fuel did, the fit will be poor and
the correction should be ignored - which is why r2 is printed next to it rather
than hidden.

    python3 tools/pace.py <lap.csv>...
    python3 tools/pace.py --coef 0.031 <lap.csv>...   # s per litre, if known

**Do not fit this on a stint where coaching was happening.** Run over the first
stint of 2026-08-11 it reports 0.086 s per litre at r2 0.58, which over that
stint's 20 litres is 1.7 s - almost exactly the improvement that came from the
coaching itself. Fuel fell and lap time fell, and the regression cannot tell
which caused which. It will happily attribute a driver getting better to the
tank getting lighter. Fit it on a stint where nobody was being told anything,
or pass a coefficient measured that way with --coef.

**Prefer corner minimum speeds where you can.** They barely move with fuel -
the car is slower everywhere by a similar small amount - whereas lap time is
the integral of that difference and is dominated by it. This tool exists for
the cases where a lap time is genuinely what you want.
"""

import csv
import sys

# Only laps that actually cover a lap are worth timing. Same threshold as
# lap-qc.py, and for the same reason: a lap starting at pct 0.06 covers 94% and
# reads a second and a half fast.
FULL_LAP = 0.98


def read(path):
    """(lap time, mean fuel litres, pct covered) or None if not a usable lap."""
    with open(path, newline="") as fh:
        r = csv.reader(fh)
        h = next(r)

        def idx(n):
            return h.index(n) if n in h else None

        ip, it = idx("LapDistPct"), idx("SessionTime")
        ifuel = idx("FuelLevel")
        if ip is None or it is None:
            return None

        lo = hi = None
        t0 = t1 = None
        fuel = []
        prev = None
        for x in r:
            try:
                p, t = float(x[ip]), float(x[it])
            except (ValueError, IndexError):
                continue
            if prev is not None and t < prev:
                return None                     # spliced; not a lap
            prev = t
            if t0 is None:
                t0 = t
            t1 = t
            lo = p if lo is None else min(lo, p)
            hi = p if hi is None else max(hi, p)
            if ifuel is not None:
                try:
                    fuel.append(float(x[ifuel]))
                except (ValueError, IndexError):
                    pass

    if t0 is None or hi is None:
        return None
    cover = hi - lo
    if cover < FULL_LAP:
        return None
    mean_fuel = sum(fuel) / len(fuel) if fuel else None
    return (t1 - t0, mean_fuel, cover)


def fit(times, fuels):
    """Least squares of time against fuel. Returns (slope, intercept, r2)."""
    n = len(times)
    if n < 3:
        return None
    mx = sum(fuels) / n
    my = sum(times) / n
    sxx = sum((f - mx) ** 2 for f in fuels)
    sxy = sum((f - mx) * (t - my) for f, t in zip(fuels, times))
    if sxx <= 1e-9:
        return None                              # fuel never varied; nothing to fit
    slope = sxy / sxx
    intercept = my - slope * mx
    ss_tot = sum((t - my) ** 2 for t in times)
    ss_res = sum((t - (slope * f + intercept)) ** 2 for f, t in zip(fuels, times))
    r2 = 1.0 - ss_res / ss_tot if ss_tot > 1e-9 else 0.0
    return (slope, intercept, r2)


def main(argv):
    coef = None
    paths = []
    i = 0
    while i < len(argv):
        if argv[i] == "--coef" and i + 1 < len(argv):
            coef = float(argv[i + 1])
            i += 2
            continue
        paths.append(argv[i])
        i += 1

    laps = []
    for p in paths:
        try:
            got = read(p)
        except Exception as e:
            print("%-42s ERROR %s" % (p.split("/")[-1], e))
            continue
        if got is None:
            continue
        laps.append((p.split("/")[-1], got[0], got[1]))

    if not laps:
        print("no complete laps")
        return 1

    have_fuel = [l for l in laps if l[2] is not None]
    fitted = None
    if coef is None and len(have_fuel) >= 3:
        fitted = fit([l[1] for l in have_fuel], [l[2] for l in have_fuel])
        if fitted:
            coef = fitted[0]

    if coef is None:
        print("no fuel data (or too few laps to fit) - times are uncorrected\n")

    # Correct everything to the lightest load seen, so the corrected numbers are
    # comparable with a qualifying-style lap rather than with nothing.
    ref_fuel = min(l[2] for l in have_fuel) if have_fuel else None

    print("%-42s %8s %7s %10s" % ("lap", "time", "fuel", "corrected"))
    for name, t, f in sorted(laps, key=lambda l: l[1]):
        if coef is not None and f is not None and ref_fuel is not None:
            corr = t - coef * (f - ref_fuel)
            print("%-42s %7.2fs %6.1fL %9.2fs" % (name, t, f, corr))
        else:
            print("%-42s %7.2fs %6s %10s"
                  % (name, t, "-" if f is None else "%.1f" % f, "-"))

    if fitted:
        slope, _, r2 = fitted
        print("\nfuel sensitivity %.3f s per litre, r2 %.2f, over %d laps"
              % (slope, r2, len(have_fuel)))
        # An honest reading of the fit rather than a number presented bare.
        if r2 < 0.5:
            print("  r2 is low: the driving varied more than the fuel did over "
                  "these laps.\n  Treat the correction as noise and compare "
                  "corner minimum speeds instead.")
        if slope < 0:
            print("  slope is negative - a heavier car came out faster, which is "
                  "not physical.\n  Something else changed across this run "
                  "(tyres, track, traffic). Do not correct on it.")
    elif coef is not None:
        print("\nfuel sensitivity %.3f s per litre (given, not measured)" % coef)

    return 0


if __name__ == "__main__":
    if len(sys.argv) < 2:
        print(__doc__)
        sys.exit(2)
    sys.exit(main(sys.argv[1:]))
