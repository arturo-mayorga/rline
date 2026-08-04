#!/usr/bin/env python3
"""Compare brake bias settings using laps already captured.

No bookkeeping needed: dcBrakeBias is in the telemetry, so laps are grouped by
the setting they were actually driven at. Change the bias in the car, drive a
few laps, and this sorts them out afterwards.

    bias.py [lapdir]
"""
import csv, glob, math, os, sys
from collections import defaultdict

d = sys.argv[1] if len(sys.argv) > 1 else "/mnt/c/rline-coach/laps"
groups = defaultdict(list)

for fn in sorted(glob.glob(os.path.join(d, "lap-*.csv"))):
    want = ["dcBrakeBias","SteeringWheelAngle","LatAccel","Speed","LapDistPct",
            "SessionTime","IsOnTrack","Brake"]
    rows = {k: [] for k in want}
    try:
        with open(fn) as f:
            rd = csv.DictReader(f)
            if not rd.fieldnames or "dcBrakeBias" not in rd.fieldnames:
                continue
            for r in rd:
                try:
                    v = {k: float(r[k]) for k in want}
                except (KeyError, ValueError, TypeError):
                    continue
                if v["IsOnTrack"] < 0.5:
                    continue
                for k in want:
                    rows[k].append(v[k])
    except OSError:
        continue

    if len(rows["Speed"]) < 3000:
        continue
    if rows["LapDistPct"][-1] - rows["LapDistPct"][0] < 0.9:
        continue

    bias = round(sum(rows["dcBrakeBias"]) / len(rows["dcBrakeBias"]), 1)
    groups[bias].append((os.path.basename(fn), rows))

if not groups:
    print("no complete laps with a bias channel yet - drive a few and re-run")
    sys.exit(0)

print("%-8s %5s %8s %10s %10s %10s" %
      ("bias", "laps", "best lap", "peak latG", "p95 steer", "steer>1.5"))
print("-" * 60)
for bias in sorted(groups):
    laps = groups[bias]
    times, peaks, p95s, over = [], [], [], []
    for _, r in laps:
        times.append(r["SessionTime"][-1] - r["SessionTime"][0])
        fast = [i for i in range(len(r["Speed"])) if r["Speed"][i] > 15]
        peaks.append(max(abs(r["LatAccel"][i]) for i in fast))
        st = sorted(abs(r["SteeringWheelAngle"][i]) for i in fast)
        p95s.append(st[int(0.95 * len(st))])
        over.append(100.0 * sum(1 for i in fast
                                if abs(r["SteeringWheelAngle"][i]) > 1.5) / len(fast))
    print("%-8.1f %5d %8.1f %10.1f %10.2f %9.1f%%" %
          (bias, len(laps), min(times), sum(peaks)/len(peaks),
           sum(p95s)/len(p95s), sum(over)/len(over)))

print()
print("Lower 'steer>1.5' and higher 'peak latG' mean the car is rotating with")
print("less lock - which is the whole point of moving bias rearward.")
