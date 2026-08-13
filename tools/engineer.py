#!/usr/bin/env python3
"""Race engineer's status, from the live 1 Hz feed.

Everything the coaching tools do works on completed laps, which is the wrong
timebase for a race: fuel, gaps and flags all have to be called while they still
matter. This reads relay's live.csv - the last ten seconds, rewritten once a
second - and answers the questions an engineer is actually asked.

    python3 tools/engineer.py            # one status block
    python3 tools/engineer.py --brief    # one line, for a monitor

Fuel per lap is measured from completed laps, not from the instantaneous rate.
FuelUsePerHour reads zero whenever the driver is off throttle, so scaling it by
lap time gave 2.28 L/lap at one sample and near zero at the next - a margin that
swung from -33 laps to +651 laps in six seconds. Only the tank level at the
start of each completed lap is stable enough to call fuel on, and the median of
those deltas is used so one splash or one bad lap cannot move it.

Gaps need the CarIdx* columns, which exist only once car-channels.txt is
streaming. Without them the gap lines are omitted rather than estimated - a
made-up gap is worse than no gap.
"""

import csv
import glob
import os
import sys

TRACK_M = 5189.0        # Mugello
LIVE = "/mnt/c/rline-coach/live.csv"

# iRacing reports a sentinel rather than a count when a session is timed.
LAPS_UNLIMITED = 32000

# irsdk_TrkLoc: 3 == OnTrack. Anything else is not racing us right now.
TRK_ON_TRACK = 3


def fuel_per_lap(lapdir="/mnt/c/rline-coach/laps"):
    """Litres per lap, from the tank level at the start of each completed lap.

    Only the first data row of each file is read, so this stays cheap even
    though the lap files are now ~13 MB each. Returns (litres, laps_measured).
    """
    levels = []
    for path in sorted(glob.glob(os.path.join(lapdir, "lap-*.csv"))):
        try:
            with open(path, newline="") as fh:
                r = csv.reader(fh)
                head = next(r)
                if "FuelLevel" not in head or "Lap" not in head:
                    continue
                fi, li = head.index("FuelLevel"), head.index("Lap")
                row = next(r)
                levels.append((float(row[li]), float(row[fi])))
        except (OSError, StopIteration, ValueError, IndexError):
            continue

    levels.sort()
    deltas = []
    for (l0, f0), (l1, f1) in zip(levels, levels[1:]):
        if l1 != l0 + 1:
            continue            # not consecutive laps; the gap is not one lap
        d = f0 - f1
        # A refuel shows as a negative delta, and a lap spent in the pits burns
        # almost nothing. Neither is a racing lap's consumption.
        if 0.3 < d < 20.0:
            deltas.append(d)

    if not deltas:
        return (None, 0)
    deltas.sort()
    return (deltas[len(deltas) // 2], len(deltas))


def load(path):
    with open(path, newline="") as fh:
        rows = list(csv.DictReader(fh))
    return rows


def f(row, name, default=0.0):
    try:
        return float(row[name])
    except (KeyError, TypeError, ValueError):
        return default


def gaps(row, ncars, own_pct, own_speed):
    """(ahead_seconds, behind_seconds) to the nearest car actually on track."""
    if own_speed < 5.0:
        return (None, None)
    ahead = behind = None
    self_idx = int(f(row, "PlayerCarIdx", -1))
    for n in range(ncars):
        if n == self_idx:
            continue
        key = "CarIdxLapDistPct_%02d" % n
        if key not in row:
            continue
        p = f(row, key, -1.0)
        if p < 0:
            continue
        if int(f(row, "CarIdxTrackSurface_%02d" % n, -1)) != TRK_ON_TRACK:
            continue
        if f(row, "CarIdxOnPitRoad_%02d" % n, 0):
            continue
        d = p - own_pct
        if d > 0.5:
            d -= 1.0
        elif d < -0.5:
            d += 1.0
        secs = abs(d) * TRACK_M / own_speed
        if d > 0:
            ahead = secs if ahead is None else min(ahead, secs)
        elif d < 0:
            behind = secs if behind is None else min(behind, secs)
    return (ahead, behind)


def status(path=LIVE):
    rows = load(path)
    if not rows:
        return None
    r = rows[-1]

    ncars = 0
    while "CarIdxLapDistPct_%02d" % ncars in r:
        ncars += 1

    lap_time = f(r, "LapLastLapTime")
    if lap_time <= 0:
        lap_time = f(r, "LapBestLapTime")

    fuel = f(r, "FuelLevel")
    # Measured across completed laps. The instantaneous rate is kept only to
    # print alongside it, never to compute the margin.
    per_lap, nlaps = fuel_per_lap()
    est_rate = f(r, "FuelUsePerHour") * lap_time / 3600.0 if lap_time > 0 else 0.0

    time_left = f(r, "SessionTimeRemain")
    laps_left_ex = f(r, "SessionLapsRemainEx")
    if laps_left_ex >= LAPS_UNLIMITED:
        # Timed session: derive laps from the clock.
        laps_left = (time_left / lap_time) if lap_time > 0 else 0.0
        timed = True
    elif laps_left_ex <= 0:
        # A lap-limited race that has finished. SessionTimeRemain is then the
        # 604800-second (seven day) sentinel, and treating this as a timed
        # session divided that by the lap time and reported 6811 laps
        # remaining - which surfaced as "fuel SHORT, margin -6908 laps" on the
        # last lap of a race that had fuel to spare. Zero laps left means zero.
        laps_left = 0.0
        timed = False
    else:
        laps_left = laps_left_ex
        timed = False

    # The same sentinel makes the clock meaningless in a lap-limited race.
    if not timed and time_left >= LAPS_UNLIMITED:
        time_left = laps_left * lap_time

    fuel_laps = (fuel / per_lap) if per_lap else None

    own_pct = f(r, "LapDistPct")
    own_speed = f(r, "Speed")
    ahead, behind = gaps(r, ncars, own_pct, own_speed) if ncars else (None, None)

    # PlayerCarPosition reads one LOWER than the classified result on this car.
    # Two independent checks on 2026-08-12: it read 9 in qualifying cooldown for
    # a P10 grid slot, and 6 for the whole of the final race lap for a P7
    # finish. Both settled values, both exactly one out, so this is a systematic
    # offset rather than a live-versus-final difference. Corrected here rather
    # than at every call site. Worth re-confirming against a results screen next
    # session - it is inferred from two samples, not from documentation.
    return {
        "lap": int(f(r, "Lap")),
        "pos": int(f(r, "PlayerCarPosition")) + 1,
        "class_pos": int(f(r, "PlayerCarClassPosition")) + 1,
        "time_left": time_left,
        "laps_left": laps_left,
        "timed": timed,
        "lap_time": lap_time,
        "best": f(r, "LapBestLapTime"),
        "fuel": fuel,
        "per_lap": per_lap or 0.0,
        "per_lap_laps": nlaps,
        "per_lap_rate": est_rate,
        "fuel_laps": fuel_laps,
        "margin": (fuel_laps - laps_left) if fuel_laps is not None else None,
        "yellow": bool(f(r, "CoachYellow")),
        "pits": bool(f(r, "OnPitRoad")),
        "incidents": int(f(r, "PlayerCarMyIncidentCount")),
        "policy": int(f(r, "CoachPolicy", -1)),
        "track_temp": f(r, "TrackTemp"),
        "ahead": ahead,
        "behind": behind,
        "ncars": ncars,
    }


def mmss(s):
    return "%d:%02d" % (int(s) // 60, int(s) % 60)


def main(argv):
    brief = "--brief" in argv
    paths = [a for a in argv if not a.startswith("--")]
    s = status(paths[0] if paths else LIVE)
    if s is None:
        print("no live data")
        return 1

    fuel_txt = "?" if s["fuel_laps"] is None else "%.1f laps" % s["fuel_laps"]
    marg = s["margin"]
    marg_txt = "?" if marg is None else ("+%.1f" % marg if marg >= 0 else "%.1f" % marg)

    if brief:
        bits = ["P%d L%d" % (s["pos"], s["lap"])]
        bits.append("%s left" % (mmss(s["time_left"]) if s["timed"]
                                 else "%.0f laps" % s["laps_left"]))
        bits.append("fuel %.1fL (%s, margin %s)" % (s["fuel"], fuel_txt, marg_txt))
        if s["ahead"] is not None:
            bits.append("ahead %.1fs" % s["ahead"])
        if s["behind"] is not None:
            bits.append("behind %.1fs" % s["behind"])
        if s["yellow"]:
            bits.append("YELLOW")
        print(" | ".join(bits))
        return 0

    print("position   P%-3d (class P%d)   lap %d   inc %d"
          % (s["pos"], s["class_pos"], s["lap"], s["incidents"]))
    if s["timed"]:
        print("remaining  %s  (~%.1f laps at %.1fs)" % (mmss(s["time_left"]),
                                                        s["laps_left"], s["lap_time"]))
    else:
        print("remaining  %.0f laps  (%s)" % (s["laps_left"], mmss(s["time_left"])))
    print("last lap   %.2fs   best %.2fs   track %.0fC" % (s["lap_time"], s["best"],
                                                           s["track_temp"]))
    if s["per_lap"]:
        print("fuel       %.2f L   %.2f L/lap over %d laps   %s   margin %s laps"
              % (s["fuel"], s["per_lap"], s["per_lap_laps"], fuel_txt, marg_txt))
    else:
        print("fuel       %.2f L   L/lap not measurable yet (needs two "
              "consecutive completed laps)" % s["fuel"])
    if s["ncars"]:
        a = "clear" if s["ahead"] is None else "%.1fs" % s["ahead"]
        b = "clear" if s["behind"] is None else "%.1fs" % s["behind"]
        print("gaps       ahead %s   behind %s   (%d car slots)" % (a, b, s["ncars"]))
    else:
        print("gaps       (no CarIdx columns - car-channels.txt not streaming)")
    flags = []
    if s["yellow"]:
        flags.append("YELLOW")
    if s["pits"]:
        flags.append("PITS")
    pol = {0: "full", 1: "confirm", 2: "silent"}.get(s["policy"], "?")
    print("state      %s   coach %s" % (" ".join(flags) if flags else "green", pol))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
