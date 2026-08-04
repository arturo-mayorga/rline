---
name: rline-coach
description: Run a live iRacing driving-coaching session with rline - stream telemetry from the sim rig, analyse laps, and push spoken coaching back to the driver. Use when working on the rline overlay, analysing captured laps in C:\rline-coach\laps, diagnosing why the driver is off the pace, sending notes to the rig, or building new coaching cues, gauges and corner callouts.
---

# rline coaching sessions

An iRacing driving coach split across two machines. Read this before touching
anything; several of the constraints below were learned the hard way.

## The split, and why

**The sim rig** (separate Windows box, no dev tools) runs `rline.exe`: the
overlay, corner beeps, speech, per-corner coaching and the learned grip curve —
all deterministic at 60 Hz. It streams every scalar iRacing channel here.

**This machine** runs `relay.exe`, which spools per-lap CSVs to
`C:\rline-coach\laps\` and forwards coaching lines from
`C:\rline-coach\outbox.txt` back to the rig.

The rig executable must stay frozen — reinstalling on it is friction — so all
intelligence lives on this side where code changes freely. Anything
time-critical runs on the rig, because **the network must never be inside a
60 Hz loop**. The rig sends *everything* rather than a curated subset, so a new
analysis never requires touching the rig.

## Starting a session

```bash
# 1. relay, here
nohup powershell.exe -Command "Start-Process -FilePath \
  'C:\Users\amayorga\rline-build\build\Release\relay.exe' \
  -WorkingDirectory 'C:\Users\amayorga\rline-build\build\Release' \
  -RedirectStandardOutput 'C:\Users\amayorga\rline-build\relay-out.txt' \
  -WindowStyle Minimized" >/dev/null 2>&1 & disown

# 2. driver launches rline.exe on the rig - no arguments needed
# 3. confirm
tail -5 /mnt/c/Users/amayorga/rline-build/relay-out.txt   # "connected, 281 channels"
```

Watch for completed laps event-driven rather than polling:

```
Monitor: tail -f -n 0 /mnt/c/Users/amayorga/rline-build/relay-out.txt \
         | grep -E --line-buffered "lap [0-9]+ written|rig disconnected"
```

## Talking to the driver

```bash
printf 'SAY|secs=14|text=Turn six, easier on the brake\n' >> /mnt/c/rline-coach/outbox.txt
```

Spoken via SAPI and shown in a panel. The file is truncated once sent, so an
empty `outbox.txt` means delivered. Keep notes short — they are read aloud
while driving, so no numbers that need reading twice.

## Analysing laps

Never read raw lap CSVs into context: ~8 MB, 281 columns. Use the tools.

```bash
python3 tools/analyze.py <lap.csv>   # time lost per corner vs the reference
python3 tools/coach.py <lap.csv> --send  # one spoken note for a lap
python3 tools/bias.py                # groups laps by dcBrakeBias automatically
```

`data/lap.csv` is the reference lap (Road America, 100.0 s). Captured laps land
in `/mnt/c/rline-coach/laps/lap-NNNN.csv`, split on the lap counter.

## What we know about this driver

- **Root cause: he drives past the front tyres' peak slip angle.** Lateral grip
  peaks near 1.5 rad of steering and collapses to 17.5 m/s² by 2.5–3.5 rad. The
  reference never exceeds 2.0 rad. When the car pushes he adds lock, destroying
  the grip he needs. Peak *combined* G is higher than the reference's — tyres
  are not the limit, steering input is.
- He is ~3 s off on **most** tracks, so this is technique, not track knowledge.
- Brake bias above 55.6 degrades every metric; at 50–51 he matches his best
  time with far less lock.
- First stint acting on this cut time past the limit from 2.41% to 0.74% but
  lap time stayed flat — grip freed and not yet spent. Entry speed is the
  next lever.

**Two pieces of advice that made him slower**, both caught by him from the seat
before the data showed it:

- *"Carry the brake to the apex"* — he held near-full pressure and overslowed.
  Trail braking means pressure **tapering**; he was already braking harder than
  the reference.
- *"Carry more speed"* — useless at the grip limit while understeering.

Prefer **diagnosis over description**. Comparing channels says what differs,
not why. Always ask whether an instruction is even achievable at the grip limit
before giving it. Track-independent cues beat reference-lap cues, because his
problem travels between circuits.

## Building and deploying

```bash
rsync -a --exclude build /home/amayorga/dev/rline/rline/ /mnt/c/Users/amayorga/rline-build/
cd /mnt/c/Users/amayorga/rline-build && \
  "/mnt/e/Program Files/CMake/bin/cmake.exe" --build build --config Release
cp build/Release/rline.exe /mnt/c/Users/amayorga/rline-dist/
```

Then tell the driver to copy from `C:\Users\amayorga\rline-dist` and restart.
`./test.sh` runs five portable suites on Linux — no Windows SDK, no iRacing.

## Gotchas that have cost real time

- **Always use absolute paths in bash.** The shell's cwd resets between calls;
  a relative `rsync` silently copied the wrong tree and built unchanged sources
  three separate times. Verify a fix reached the binary (`strings`, a changed
  log line) before concluding it did not work.
- **Kill `rline.exe` / `relay.exe` before rebuilding** — a running exe holds its
  own file and the link fails with `LNK1104`.
- **Never build under `%TEMP%`** — MSBuild warns `MSB8029` and incremental
  builds genuinely go stale, producing binaries with mismatched struct layouts.
- **iRacing must run windowed or borderless.** Exclusive fullscreen draws over
  the layered overlay and nothing appears.
- The firewall rule for port 7642 must match the network profile; a Private
  rule does nothing on a network classified Public.
- iRacing publishes **no absolute position** live — no Lat/Lon, no world X/Y/Z.
  Lateral offset is dead reckoned and resets each lap. The corner cues do not
  depend on it and are exact.

## Open threads

1. Entry speed at bias 50–51 — the lever that should finally convert to time.
2. Peak-versus-sustained lateral G: he makes higher peak G than the reference
   yet is slower, which may mean spikes where the reference is smooth.
3. Mic input via SAPI recognition — a command grammar, especially **"mark"** to
   timestamp a moment he felt something wrong.
4. Programmable HUD channels: push `CH|id=..|type=bar|expr=speed-ref.speed` and
   have the rig evaluate it, so new gauges need no rebuild. Never started.
