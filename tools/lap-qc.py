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
  flag     the lap was run under a caution. Nothing measured on it means
           anything, and on race day these will outnumber clean laps.

           NOT read from SessionFlags, which cannot survive the wire: it is
           carried as float32, and with the start-light bits set it sits near
           2.7e8 where float32 spacing is 32 - so irsdk_yellow (0x8) and
           irsdk_yellowWaving (0x100) are rounded away before the relay sees
           them. Checking it directly reported "caution 100% of lap" on a clean
           practice lap. The rig now publishes CoachYellow, decided where the
           integer is still exact, plus SessionFlagsLo/Hi for anything else that
           wants the bitfield. Old laps have none of these and skip the check.
  traffic  another car was close enough ahead to affect the lap. A corner
           minimum taken two lengths behind someone is not a data point about
           this driver, and in a race most laps are like this. Needs the
           CarIdx* columns, which only exist once car-channels.txt is streaming
           - without them the check is silently skipped rather than guessed at.

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

TRACK_M = 5189.0       # Mugello. Road America was 6413
# Following distance under which a lap stops being a measurement of this
# driver. Two seconds is the usual dirty-air threshold; well inside that the
# corner speeds are somebody else's line as much as his.
TRAFFIC_S = 2.0
# How much of the lap has to be spent that close before the lap is written off.
# A single corner spent catching someone is not a ruined lap.
TRAFFIC_FRACTION = 0.10

# irsdk_TrkLoc: -1 NotInWorld, 0 OffTrack, 1 InPitStall, 2 ApproachingPits,
# 3 OnTrack. Only 3 is a car that can be traffic.
TRK_ON_TRACK = 3

# Caution bits from src/irsdk/irsdk_defines.h - yellow, yellowWaving, caution,
# cautionWaving. Same set as sessionpolicy::kYellowFlagMask, deliberately.
YELLOW_MASK = 0x00000008 | 0x00000100 | 0x00004000 | 0x00008000


def main(path):
    with open(path, newline="") as fh:
        r = csv.reader(fh)
        h = next(r)

        def idx(name):
            return h.index(name) if name in h else None

        ip, ise = idx("LapDistPct"), idx("SessionTime")
        istat, ipush = idx("P2P_Status"), idx("PushToPass")
        ispeed = idx("Speed")
        # CoachYellow first; SessionFlagsLo is the exact low half if an older
        # rig sent it. Never bare SessionFlags - see the note above.
        iyellow = idx("CoachYellow")
        iflagslo = idx("SessionFlagsLo")
        iself = idx("PlayerCarIdx")

        # Per-car columns, present only once car-channels.txt is being
        # streamed. Absent is normal on every lap captured before that, so the
        # traffic check disables itself rather than reporting "no traffic".
        # Discovered from the header, not assumed: iRacing's CarIdx arrays are
        # 72 wide, not the 64 everyone remembers, and a hard-coded 64 would
        # silently ignore any car in the last eight slots.
        cars = []
        for n in range(128):
            cp = idx("CarIdxLapDistPct_%02d" % n)
            if cp is None:
                continue
            cars.append((n, cp, idx("CarIdxOnPitRoad_%02d" % n),
                         idx("CarIdxTrackSurface_%02d" % n)))
        if ip is None or ise is None:
            print("%s  UNREADABLE (missing LapDistPct/SessionTime)" % path.split("/")[-1])
            return 1

        pct, t = [], []
        boosted = 0
        yellow = 0
        close = 0
        traffic_checked = 0
        self_idx = None
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

            if iyellow is not None:
                try:
                    if float(x[iyellow]):
                        yellow += 1
                except (ValueError, IndexError):
                    pass
            elif iflagslo is not None:
                try:
                    if int(float(x[iflagslo])) & YELLOW_MASK:
                        yellow += 1
                except (ValueError, IndexError):
                    pass

            if cars:
                if self_idx is None and iself is not None:
                    try:
                        self_idx = int(float(x[iself]))
                    except (ValueError, IndexError):
                        self_idx = -1
                try:
                    v = float(x[ispeed]) if ispeed is not None else 0.0
                except (ValueError, IndexError):
                    v = 0.0
                # Below walking pace the time-gap blows up and means nothing.
                if v > 5.0:
                    traffic_checked += 1
                    nearest = None
                    for n, cp, ipit, isurf in cars:
                        if n == self_idx:
                            continue
                        try:
                            theirs = float(x[cp])
                        except (ValueError, IndexError):
                            continue
                        if isurf is not None:
                            try:
                                if int(float(x[isurf])) != TRK_ON_TRACK:
                                    continue
                            except (ValueError, IndexError):
                                pass
                        if ipit is not None:
                            try:
                                if float(x[ipit]):
                                    continue
                            except (ValueError, IndexError):
                                pass
                        # Forward gap only. A car alongside or behind is not
                        # what dirties the air in front of this one.
                        d = theirs - p
                        if d < 0:
                            d += 1.0
                        if d <= 0 or d > 0.5:
                            continue
                        gap = d * TRACK_M / v
                        if nearest is None or gap < nearest:
                            nearest = gap
                    if nearest is not None and nearest < TRAFFIC_S:
                        close += 1
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
    if yellow:
        flags.append("FLAG(caution %.0f%% of lap)" % (100.0 * yellow / len(pct)))
    if traffic_checked and close / float(traffic_checked) > TRAFFIC_FRACTION:
        flags.append("TRAFFIC(within %.0fs for %.0f%% of lap)"
                     % (TRAFFIC_S, 100.0 * close / traffic_checked))
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
