#!/usr/bin/env python3
"""One-line quality verdict for a captured lap. Cheap enough to run on every
lap as it is archived, so telemetry problems surface during the stint instead
of during the post-mortem.

Checks, in the order they have actually bitten us:

  splice   two attempts concatenated into one file - relay truncates and
           restarts lap-NNNN.csv while the archiver is copying it. Shows up as
           a backwards step in SessionTime and an implied rate above 60 Hz.
  partial  the file does not span a full lap (LapDistPct range well under 1.0),
           usually an out-lap, an in-lap or a reset.
  slow     far too long to be a flying lap - an out-lap, an in-lap, or a stint
           spent parked. These pass every other check: they cover the full
           track and sample at 60 Hz, so without this a 598 s stint of sitting
           in the garage reads as a clean lap.
  p2p      push-to-pass boost was engaged. Such a lap has extra power and is
           not comparable to a clean one; it must never be quoted as a best.

Getting the P2P channels right took some care, so do not "simplify" them:

  P2P_Count   is a constant 999 sentinel on this car - it means nothing at all,
              and testing it flagged literally every lap as boosted.
  PushToPass  is the momentary button, high for ~7 samples. It marks the instant
              of activation, not the boost, and it is often pressed on the lap
              BEFORE the one that gets the benefit.
  P2P_Status  is the boost actually being engaged. This is the one that matters.
              It runs for minutes and spans lap boundaries, so a lap can be 100%
              boosted while showing no button press of its own.

Prints one line. Exit status is 0 for a clean flying lap, 1 otherwise, so the
archiver can count problems without parsing.

    python3 tools/lap-qc.py <lap.csv>
"""

import csv
import sys

RATE = 60.0            # telemetry rate; a clean lap hits this almost exactly
RATE_TOL = 2.0
# A flying lap covers essentially the whole track. 0.90 was too loose: a lap
# starting at pct 0.06 covers 94%, passed as clean, and read as a 98.8 s
# personal best against a 100 s reference purely because 6% of the track was
# missing from the timing.
FULL_LAP = 0.98
# The reference lap for the track being driven; anything near double this is
# not a lap. Track-dependent and easy to leave stale: at 100.0 the slow
# threshold sits at 150 s, and a 140 s in-lap at Mugello passes as clean.
REFERENCE_S = 83.63    # Mugello. Road America was 100.0
SLOW_FACTOR = 1.5
P2P_MIN = 0.01         # engaged for >1% of the lap counts as contaminated


def main(path):
    with open(path, newline="") as fh:
        r = csv.reader(fh)
        h = next(r)

        def idx(name):
            return h.index(name) if name in h else None

        ip, ise = idx("LapDistPct"), idx("SessionTime")
        istat, ipush = idx("P2P_Status"), idx("PushToPass")
        if ip is None or ise is None:
            print("%s  UNREADABLE (missing LapDistPct/SessionTime)" % path.split("/")[-1])
            return 1

        pct, t = [], []
        boosted = 0
        presses = 0
        prev_push = 0.0
        reversals = 0
        prev_t = None
        for x in r:
            try:
                p = float(x[ip])
                s = float(x[ise])
            except (ValueError, IndexError):
                continue
            if prev_t is not None and s < prev_t:
                reversals += 1
            prev_t = s
            pct.append(p)
            t.append(s)
            if istat is not None:
                try:
                    if float(x[istat]):
                        boosted += 1
                except (ValueError, IndexError):
                    pass
            if ipush is not None:
                try:
                    v = float(x[ipush])
                except (ValueError, IndexError):
                    v = 0.0
                if v and not prev_push:
                    presses += 1
                prev_push = v

    name = path.split("/")[-1]
    if len(pct) < 100:
        print("%s  UNUSABLE (%d rows)" % (name, len(pct)))
        return 1

    span = t[-1] - t[0]
    rate = len(pct) / span if span > 0 else 0.0
    cover = max(pct) - min(pct)

    boost = boosted / float(len(pct))
    flags = []
    if reversals or abs(rate - RATE) > RATE_TOL:
        flags.append("SPLICE(%d rev, %.1fHz)" % (reversals, rate))
    if cover < FULL_LAP:
        flags.append("PARTIAL(%.0f%% of lap)" % (cover * 100))
    if span > REFERENCE_S * SLOW_FACTOR:
        flags.append("SLOW(%.0fs - not a flying lap)" % span)
    if boost > P2P_MIN:
        flags.append("P2P(boost %.0f%% of lap)" % (boost * 100))
    elif presses:
        flags.append("p2p-press(%d, no boost this lap)" % presses)

    verdict = "ok" if not flags else " ".join(flags)
    print("%-44s %6.1fs %5d rows %5.1fHz  pct %.2f-%.2f  %s"
          % (name, span, len(pct), rate, min(pct), max(pct), verdict))
    return 0 if not flags else 1


if __name__ == "__main__":
    if len(sys.argv) < 2:
        print(__doc__)
        sys.exit(2)
    rc = 0
    for p in sys.argv[1:]:
        try:
            rc |= main(p)
        except Exception as e:                      # never let QC kill the archiver
            print("%s  ERROR %s" % (p.split("/")[-1], e))
            rc = 1
    sys.exit(rc)
