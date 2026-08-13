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
tail -12 /mnt/c/Users/amayorga/rline-build/relay-out.txt  # "connected, 281 channels"
```

**The rig now says what it is running, and that answers the question that cost
an evening on 2026-08-09.** Immediately after the handshake it sends one line
reporting what it actually loaded, and relay prints it and writes it to
`C:\rline-coach\rig-build.txt`:

```
relay: rig build 2790a5ce, compiled Aug  9 2026 22:24:34, protocol 2
relay:   track mugello - reference 455bbfae, 5021 points, 5189 m, 83.63 s, 6 corners
relay:   6 corner names, first is "turn one"
```

Every runtime field is measured from the structures in use, not from build
settings, so a stale `lap.csv` sitting beside the exe reports *itself*. The two
fields that settle it at a glance are `first` - "turn one" means the current
Mugello mapping, a corner *name* like "San Donato" means a pre-2026-08-11 names
file, and "Turn one" capitalised means the Road America folder - and `ref`, a
hash of the reference file's bytes. Known-good Mugello hashes: reference
`455bbfae`, corner names `c6e15132`, exe `2790a5ce`.

`built` is advisory and goes stale: it is `__DATE__`/`__TIME__` from
`build-id.cpp`, which only recompiles when that file changes, so a fresh build
can report an old timestamp. The exe hash is exact - trust that instead.

Three things get shouted about, all of which have actually happened: a track
that disagrees with what this machine built (`*** WRONG TRACK ***`), no corner
names loaded, and no reference lap loaded. **And silence is itself the
diagnostic** - an exe built before this feature says nothing, so relay prints
`*** the rig sent no build id ***` when the first telemetry frame arrives with
no report before it. Never conclude the rig is current because the log looks
normal; look for the build block.

Watch for completed laps by **polling** — `tail -f` on `/mnt/c` never fires,
because WSL gets no inotify events for files written by Windows processes. This
silently reported nothing through an entire 8-lap stint:

```
Monitor: f=/mnt/c/Users/amayorga/rline-build/relay-out.txt; n=$(wc -l < "$f")
         while true; do sleep 15; c=$(wc -l < "$f")
           [ "$c" -lt "$n" ] && n=0                 # relay restarted, file truncated
           [ "$c" -gt "$n" ] && { tail -n +$((n+1)) "$f" | grep -E \
             "lap [0-9]+ written|disconnect|connected|error"; n=$c; }
         done
```

## Hearing the driver

He holds a spare wheel button and talks; the rig recognises it locally and
sends one line up the same socket the telemetry uses:

```
HEAR|conf=0.91|text=that felt like understeer at the exit of turn eight
```

Relay appends every utterance to `/mnt/c/rline-coach/inbox.txt` with a
timestamp and prints `relay: <- heard (91%) ...` to its log. **Unlike
`outbox.txt`, it is never truncated** - a note to him is consumed once, but a
thing he said is a record worth keeping. Poll it the same way as everything
else on `/mnt/c`.

The button is read **straight off the wheel**, not through iRacing. Binding it
to iRacing's push-to-talk needs no button code at all and was the first
attempt - but **that button transmits to everyone in the session**, which is
not acceptable for talking to a coach. He caught this before it shipped.

**Binding is done from the wheel, in the overlay** - press **Ctrl+Shift+M** for
move-window mode and press the button you want. The panel shows
`press a wheel button to set talk`, then `talk button 0:7 - press to change`.
Never assume or hard-code which button he uses; move mode is the rig's only
configuration screen, and it is the one reachable without a command line on a
box that deliberately has no dev tools.

Discovery waits to see every button released first, so a paddle resting against
a stop cannot bind itself, and pressing again while still in move mode rebinds -
a mistake is corrected on the spot. Speech is never recognised while binding.
The binding lives in `rline-talk.txt` beside the exe as `<device>:<button>`
(both zero-based). `--talk-button 0:7` sets it directly and `--bind-talk` does
the same discovery headlessly, but neither is the normal route.
It uses `joyGetPosEx` from winmm, already linked for the countdown beeps, so no
new dependency and no DirectInput; that covers the first 32 buttons of the
first 16 devices, which is where a wheel's buttons are. **The wheel is readable
with iRacing closed**, so the button can be tested from the desktop.

`--talk-channel <name>` still allows the telemetry-channel route for anyone who
wants it. It is off by default and prints a warning about broadcasting.

Dictation is only active while the button is held. `conf` is the engine's own
average per-word confidence - **treat anything under about 0.5 as "ask him
again" rather than acting on it**, because a misheard corner number sends him
to the wrong piece of road just as surely as detector numbering does.

Recognition is the one thing that deliberately runs on the rig rather than
here: audio is orders of magnitude larger than telemetry, and SAPI was already
linked in for the coaching voice. Everything that decides what may reach the
wire is in `src/voice-line.cpp` and is tested on Linux - a misrecognition
containing `|` cannot forge a field, and a stuck recogniser cannot flood the
link.

## Talking to the driver

```bash
printf 'SAY|secs=14|text=Turn six, easier on the brake\n' >> /mnt/c/rline-coach/outbox.txt
```

**Nothing interrupts a sentence in progress.** Notes go into a priority queue
(`src/speech-queue.h`) rather than straight to the voice. Until 2026-08-05 the
corner coach and the relay shared one slot and the speaker used
`SPF_PURGEBEFORESPEAK`, so whichever arrived second cut the first off mid-word -
and if two arrived between ticks the first was silently overwritten and never
spoken. He reported the symptom precisely: "your own words are cut mid sentence
to say the next thing."

The rules: a started sentence always finishes; corner cues go ahead of
conversational notes because they are about to be needed; and a corner cue that
cannot be delivered within **4 s** is dropped rather than spoken late, because
naming a corner after he has passed it points him at the wrong piece of road.
Relay notes never go stale. Do not reintroduce interruption to "fix" staleness -
staleness is handled by the shelf life.

Spoken via SAPI and shown in a panel. The file is truncated once sent, so an
empty `outbox.txt` means delivered. Keep notes short — they are read aloud
while driving, so no numbers that need reading twice.

## Analysing laps

Never read raw lap CSVs into context: ~8 MB, 281 columns. Use the tools.

```bash
python3 tools/lap-qc.py <lap.csv>    # FIRST: is this lap trustworthy at all?
python3 tools/quick.py <lap.csv>     # 0.3 s: lap time, per-corner vmin and lock
python3 tools/analyze.py <lap.csv>   # slower: time lost per corner vs the reference
python3 tools/coach.py <lap.csv> --send  # one spoken note for a lap
python3 tools/bias.py                # groups laps by dcBrakeBias automatically
```

**Run `lap-qc.py` before quoting any number from a lap.** The archiver runs it
automatically on everything it takes and appends the verdict to
`/mnt/e/games/qc.log`; **poll that log during the stint**, not
after. It catches spliced files, partial laps, and push-to-pass. Watching the
relay log alone is not enough — it says a lap was *written*, never whether the
copy that survived is intact.

Use `quick.py` while he is driving — `analyze.py` takes long enough that he is a
lap and a half further on by the time it answers.

**`analyze.py`'s "ref vmin" column is wrong for overlapping corners.** It reports
the minimum over the whole span, so a corner picks up its neighbour's minimum;
that is why adjacent corners print identical values. The trustworthy per-corner
figures are `RefCorner::vmin`, measured from turn-in.

Reference laps live in `data/`. Captured laps land in
`/mnt/c/rline-coach/laps/lap-NNNN.csv`.

| reference | track | lap | length | detected corners |
|---|---|---|---|---|
| `data/lap.csv` | Road America | 100.0 s | 6413 m | 11 of 16 turns |
| `data/muguello-ref.csv` | **Mugello — this week** | 83.63 s | 5189 m | 6 of 15 turns |

(The Mugello filename is misspelled as committed. Left alone rather than
renamed mid-week; nothing references it by name yet except these notes.)

**Check whether the archiver is already running before starting one** — it is
left running deliberately between sessions, so it protects laps even when
nobody is watching:

```bash
ps -eo pid,args | grep "[a]rchive-laps.sh" ||
  nohup bash tools/archive-laps.sh > /tmp/archiver.log 2>&1 & disown
```

A second copy is harmless (the content hash makes archiving idempotent) but it
doubles the log noise.

**Start the lap archiver before he goes out** — `tools/archive-laps.sh`. `relay`
reuses `lap-NNNN.csv` whenever iRacing's lap counter repeats, which happens on
every reset to pits, so a completed flying lap can be destroyed minutes after it
is written. Starting it mid-stint does not recover what is already gone. It
works: on 2026-08-11 a run of rapid resets rewrote `lap-0031.csv` 86 times and
every distinct version was preserved, the content hash making the duplicates
free.

**The archive lives on E:, not C:.** `DEST` is `/mnt/e/games/live` and `QCLOG`
is `/mnt/e/games/qc.log`, both overridable by environment variable. They moved
on 2026-08-11 because 13 GB of laps had filled the system drive to 237 MB free
and *relay tore its writes* - rows lost mid-file, so the sample rate read
36-57 Hz instead of 60 and `LapDistPct` ran to 4033 where it can only be 0..1.
`lap-qc.py` called those laps `SPLICE`, which is the closest verdict it has but
not the cause. **One lap of that entire session survived.** If splices start
appearing across a whole stint, check free space before believing the data.

**The trigger is relay's own log, not a settle timer.** relay prints
`lap N written` *after* it closes the file, so that line is a guarantee the lap
is complete and it is copied that instant. A settle-based scan runs alongside as
a safety net. This matters because the survival window can be shorter than any
settle: an 8 s poll / 10 s settle saved **1 of 8** flying laps on 2026-08-05
while appearing to run perfectly, and even 1 s poll / 3 s settle lost one lap
that evening to a reset within 3 s of the line. Verify it works rather than that
it is running — drop a >4 MB file into the laps directory, append a matching
`relay: lap N written` line, and confirm it is archived in the same second.
Done on 2026-08-11: archived within 4 s, QC verdict `ok / 60.0Hz`.

**`SPLICE` on a `[scan]` line is normal; on a `[log]` line it is not.** The
settle scan copies files while relay is still writing them, so a mid-write copy
legitimately reads as spliced and partial - there were 75 such lines on
2026-08-11 and not one real splice. The `[log]` copies are taken after relay's
`fclose` and are the authoritative ones. Filter on `[log]` and `pct 0.00-1.00`
before quoting any lap time: a `[scan]` row at `pct 0.00-0.99` is a lap caught
just before the line and its time reads about 0.4 s fast.

**Never edit `relay-out.txt` while a monitor is tailing it.** Removing a line
makes the line count drop, the watcher resets and replays the entire log, and
every historical `lap written` line is counted as new - which fired a false
"LAPS ARE BEING LOST" alarm on 2026-08-05.

**Validate a rescued lap before quoting it.** A file can hold two attempts
spliced together. `rows / SessionTime` span must be 60.0 Hz exactly; a splice
reads high (6369 rows / 84.6 s = 75.2 Hz), and `SessionTime` steps backwards at
the join. Corners wholly inside the clean segment are still usable — check the
corner's whole brake zone is covered against the reference's absolute brake-on
distance. Never quote the lap time of a spliced file.

## Race day

A race day is practice, a sprint, a ten minute warmup, then the feature. The
coaching rules are not the same in each, and since 2026-08-12 the rig enforces
that itself rather than relying on anyone remembering.

| session | mode | what the rig may say |
|---|---|---|
| practice, testing | `full` | everything, including corners never raised before |
| warmup | `confirm` | only corners already established. Nothing new. |
| qualifying, race | `silent` | nothing about technique at all |
| pit lane, any caution | `silent` | overrides the session type |
| out-lap | never above `confirm` | the remembered fault predates the stop |

Anything iRacing names unexpectedly falls to `confirm`, never to `full` - a new
session name is likelier to be a race variant than a practice one, and the cost
of guessing wrong is asymmetric.

**Why silence rather than less coaching.** He loses the corner he is not
thinking about - demonstrated on 2026-08-04, 08-09 and 08-11 - and on 2026-08-09
three of four incidents came within a lap or two of a new cue. In practice a
mistimed cue costs half a second and the worst one measured cost 4.4 s. In a
race it costs contact. A ten minute warmup is one out-lap and three or four
flying laps: enough to check a cue still lands, nowhere near enough to teach one.

**Only speaking is suppressed.** The accumulators keep measuring and each
corner's remembered fault keeps updating through a whole silent race, so the
first practice lap afterwards is already feed-forward rather than a wasted
reconnaissance lap. A test asserts exactly that.

Override from here without a rebuild - the rig has no command line:

```bash
printf 'COACH|mode=silent\n' >> /mnt/c/rline-coach/outbox.txt   # panic button
printf 'COACH|mode=auto\n'   >> /mnt/c/rline-coach/outbox.txt   # hand it back
```

`full`/`on`, `confirm`, `silent`/`off`, `auto`. An unrecognised mode is refused
rather than guessed, and a refused command leaves the current mode alone. The
rig logs every transition with the reason.

### What the first race day actually taught (2026-08-12)

Official race at Mugello: practice, 8-minute 2-lap qualifying, 14-lap race.
**Finished P7 from 10th, zero incidents, fuel never in doubt.**

**The session-type auto-detect did not work, and now does.** The SDK's
`parseYaml` returned nothing for every documented spelling of its array syntax,
so `fromSessionType` got an empty string, every session fell to the safe
`confirm`, and the coach had to be pinned by hand with `COACH|mode=silent`.
Replaced by `sessionpolicy::typeFromYaml`, which scans the session-info string
for the matching `SessionNum:` and takes the `SessionType:` before the next one.
Tested on Linux against the four-session league layout, including the case where
a session has no type and must not borrow the next one's.

**`PlayerCarPosition` reads one lower than the classified result.** Telemetry
said 9 for a P10 grid slot and 6 for a P7 finish - both settled values, both
exactly one out. `tools/engineer.py` adds 1. Confirm against a results screen
before trusting it further; it is inferred from two samples.

**`SAY|secs=` is screen time, not shelf life.** Use about **4**. Sending 20-120
left notes on his panel for up to two minutes and he asked for it to stop.

**Fuel per lap must come from completed laps.** `FuelUsePerHour` reads zero off
throttle, so scaling it by lap time gave 2.28 L/lap one sample and near zero the
next - a margin swinging from -33 to +651 laps in six seconds. `engineer.py` now
takes the median tank drop across consecutive completed laps: **1.99 L/lap** at
Mugello, stable over 52 laps.

**Guard the end of a lap-limited race.** When `SessionLapsRemainEx` hits 0,
`SessionTimeRemain` is the 604800-second sentinel; dividing it by lap time
reported 6811 laps remaining and a false "fuel SHORT" on the last lap.

**P2P at Mugello is worth about 5 km/h of top speed, not lap time.** Boosted
297.9 km/h against 292.0 and 292.6 clean, yet the boosted lap came out 88.3
against 88.2 clean. Use it to attack or defend into turn one. **His fastest race
lap was boosted** - the only clean fast laps of the day were qualifying (87.7,
87.8), which is the true PB. Nearly every race lap carried 7-11% boost, so race
pace is boost-contaminated; qualifying is not.

**Session boundaries make `SPLICE` normal.** `SessionTime` resets between
sessions and relay's `lap-0` file spans the change, so it reads as a negative
duration at 0.0 Hz. On a race day `SPLICE` no longer implies a disk problem -
check the duration sign first.

**Do not filter QC lines on `"0.0Hz"`.** It also matches `60.0Hz`, which hid
every clean lap of the race from the monitor. Filter on a negative duration.

**The archiver was copying files that were still growing.** mtime under drvfs
updates lazily, so the 3-second settle passed on a file relay was actively
appending to: a 50 MB `lap-0000` was archived 119 times, about 3 GB in ten
minutes. It now requires the size to be unchanged between consecutive polls.

### Race-day analysis is different in three ways

**Most laps are not measurements.** `lap-qc.py` now flags `FLAG` (any caution)
and `TRAFFIC` (another car within 2 s ahead for more than 10% of the lap).
Expect these to outnumber clean laps in a race. A corner minimum taken two
lengths behind someone is somebody else's line as much as his.

**Never read `SessionFlags` from a lap CSV.** It is carried as float32, and with
the start-light bits set it sits near 2.7e8 where float32 spacing is 32 - so
`irsdk_yellow` (0x8) and `irsdk_yellowWaving` (0x100) are rounded away before
relay ever sees them. Checking it directly reported **"caution 100% of lap" on a
clean practice lap**. The rig now publishes `CoachYellow`, decided where the
integer is still exact, plus `SessionFlagsLo`/`SessionFlagsHi` for anything that
wants the full bitfield, and `CoachPolicy`/`CoachOutLap` so the analysis can see
what the coach was allowed to say. Laps captured before 2026-08-12 have none of
these and the flag check disables itself rather than guessing.

**Fuel makes a stint look like improvement.** `tools/pace.py` regresses lap time
against `FuelLevel` and prints the fit quality next to the correction. Do not
fit it on a coached stint: over the first stint of 2026-08-11 it reports
0.086 s/L at r2 0.58, which across that stint's 20 litres is 1.7 s - almost
exactly the gain that came from the coaching. Fuel fell and lap time fell and
the regression cannot tell which caused which. Corner minimum speeds barely move
with fuel; prefer them.

### Other cars

`data/car-channels.txt` lists which 64-wide `CarIdx` arrays the rig expands into
per-slot scalars (`CarIdxLapDistPct_00` .. `_63`), so the wire protocol, relay,
the CSV writer and every tool are unchanged - just more columns. **It syncs from
the relay**, so changing what is streamed is an edit here and a rig restart,
never a rebuild. Four channels cost roughly 6 MB per lap file on top of 7 MB;
do not paste the whole `CarIdx` list in, there are about twenty.

**Use field data to build the target, never to cue him.** "The car ahead is
carrying 155 through turn ten and you are at 148" is the purest form of the one
cue that has made him slower every single time - an outcome, about someone else,
with social pressure attached. What it is genuinely for is the problem found on
2026-08-11: the reference lap is 83.63 s and the quickest lap anyone posted in
his session was about 86, so every "time lost" figure was measured against a lap
nobody was driving. Field minimums give a same-day, same-track-state target, and
they mark which laps were clean.

## Corner numbering — read this before speaking a single corner number

The detector numbers what it finds 1..N from start/finish. **Those are not the
track's turn numbers.** It finds 11 corners across 16 numbered turns at Road
America, 6 across 15 at Mugello, and does not see flat-out kinks at all. An
entire session was coached using detector numbers, sending the driver to the
wrong piece of road every time.

`data/corner-names.txt` holds the mapping, is copied next to the exe by CMake,
fetched by the rig from the relay at startup, and read by
`CornerCoach::loadNames`. When the reference lap changes, redo the mapping.

**The rig speaks track turn numbers, not names, since 2026-08-11.** He asked
for it mid-session - "I can't keep track of corner names" - so
`data/corner-names-mugello.txt` now reads `turn one`, `turn four`, `turn six`,
`turn ten`, `turn twelve`, `turn fifteen`. Say the same over the relay; never
say "San Donato" and never say a detector index.

A number is often *more* precise than the name was: a detected corner spans
several turns, so naming the exact turn places a cue the name could not. The
brake stabbing on 2026-08-11 was at **T11 Palagio**, inside the span the file
calls `turn ten` (T10 Scarperia). Where a fault sits at the far end of a span,
say the real turn from the relay rather than the file's label.

`data/corner-names-mugello.txt` is the Mugello mapping. **Deploy the one that
matches the reference lap on the rig**, renamed to `corner-names.txt` — the rig
reads that filename and nothing else.

Rebuild the table for any new reference with `tools/dump-corners.cpp` rather
than working it out by hand; it prints the `RefCorner` fields the mapping turns
on, flags the no-brake anchor, and lists the overlapping spans:

```bash
g++ -std=c++17 -O2 -o /tmp/dump-corners tools/dump-corners.cpp src/refline.cpp
/tmp/dump-corners data/muguello-ref.csv
```

4 of 6 Mugello spans overlap their predecessor (8 of 11 at Road America), so
the same rule applies: anything measuring per corner must keep every corner
live at once.

### Mugello — the mapping for this week

Six detected corners across fifteen numbered turns. Every one stands for a
stretch of road, not a turn, so each name below is also a warning about what it
swallows. Anchored at both ends: detected 1 is the only corner braked at 0.95
(San Donato, the slowest); detected 3 is the only one with essentially no brake.

| detected | turn-in | apex | ref vmin | peak brk | **say** | actually covers |
|---|---|---|---|---|---|---|
| 1 | 647 m | 734 m | 129 | 0.95 | **turn one** | T1 San Donato, T2 Luco, T3 Poggio Secco |
| 2 | 1419 m | 1624 m | 196 | 0.40 | **turn four** | T4 Materassi, T5 Borgo San Lorenzo |
| 3 | 1868 m | 1972 m | 229 | 0.13 | **turn six** | T6 Casanova, T7 Savelli, **T8+T9 Arrabbiata** |
| 4 | 2969 m | 3031 m | 147 | 0.88 | **turn ten** | T10 Scarperia, T11 Palagio |
| 5 | 3494 m | 3617 m | 133 | 0.78 | **turn twelve** | T12 Correntaio, T13+T14 Biondetti |
| 6 | 4344 m | 4479 m | 153 | 0.81 | **turn fifteen** | T15 Bucine |

Detected 2 is named for its braking (Materassi) while its apex and minimum land
at Borgo San Lorenzo — the brake release is what he is cued on, so the entry
wins the name. Detected 3 spans a **quarter of the lap** (pct 0.298–0.531), all
of it flat-out; see the grip section below before letting any cue out of it.

### Not all corners here are grip-limited — this inverts the Road America check

At Road America the cross-check was: the corners he calls grip-limited must be
the detected ones with low vmin and high peak steer. **That check is wrong in
this car.** Lateral load here rises with *speed*, not with lock. Sustained
lateral g (p90, so kerb spikes are excluded) against peak steering angle:

| turn | vmin | lat g | peak steer | taken |
|---|---|---|---|---|
| T1 San Donato | 129 | 3.00 | 1.14 | brake 0.95 |
| T4 Materassi | 196 | **4.09** | 0.98 | brake 0.40 |
| T6 Casanova | 229 | **3.93** | 1.15 | brake 0.13 |
| T7 Savelli | 240 | **3.96** | 0.93 | brake 0.06 |
| T8 Arrabbiata 1 | 251 | **4.21** | 0.88 | lift only |
| T9 Arrabbiata 2 | 254 | **4.02** | 0.99 | lift only |
| T10 Scarperia | 147 | 3.20 | 1.26 | brake 0.88 |
| T12 Correntaio | 133 | 3.17 | 1.32 | brake 0.78 |
| T13/T14 Biondetti | 220 | 3.15 | 0.54 | **flat, no lift** |
| T15 Bucine | 153 | 3.27 | 0.87 | brake 0.81 |

Three classes, and they want different coaching:

1. **Mechanically limited** — T1, T10, T12, T15. Slow, ~3.0–3.3 g, the *most*
   lock on the lap. This is where his known fault lives and where the brake
   release cue works. All four are detected corners 1, 4, 5, 6.
2. **Aero-limited** — T4, T6, T7, T8, T9. Fast, **3.9–4.2 g**, and only
   0.88–1.15 rad of lock because at 250 km/h the wing is doing the work.
   Nothing to fix with the brake. Commitment and line, not entry speed.
3. **Not limited at all** — T13, T14 flat with no lift, T3 and T11 close to it.
   Nothing to coach.

**The live hazard:** `GripCurve` indexes on steering *angle* and his peak is
1.5 rad. Arrabbiata 1 is 0.88 rad — it reads as **0% past peak**, so the
`room for more speed` cue in `CornerCoach` will fire on a corner already pulling
4.2 g at 251 km/h taken flat. That is the "carry more speed" family that has
made him slower every time it has been said. It is open thread 1, and Mugello
turns it from a Carousel-shaped edge case into a quarter of the lap. **Decide
what to do about this before the next stint** — the cheapest safe answer is to
suppress the speed-deficit cue on detected corner 3 entirely.

Cross-check the mapping by asking which corners he has to *brake* for, not
which are grip-limited. The answer must be turns one, ten, twelve and fifteen,
and nothing between turn four and turn ten.

### Road America — the previous mapping

The anchor there is **the Carousel: the only corner the reference takes with no
brake at all** (`RefCorner::releasePct < 0`).

| detected | metres | ref vmin | his turn |
|---|---|---|---|
| 1 | 507 | 211 | T1 |
| 2 | 978 | 148 | T3 |
| 3 | 2131 | 112 | T5 |
| 4 | 2425 | 123 | T6 |
| 5 | 2664 | 212 | T7 |
| 6 | 3082 | 126 | **T8** |
| 7 | 3462 | 201 | T9 |
| 8 | 4074 | 282 | **Carousel** |
| 9 | 4913 | 140 | T12 |
| 10 | 5239 | 233 | T13 |
| 11 | 5556 | 159 | T14 |

## What we know about this driver

- **Root cause, as refined on 2026-08-04: he drags the brake.** His brake
  *point* is right — within 5 m of the reference at every corner, every lap.
  The variable is entirely the *release*, and the fault is long-and-light
  rather than short-and-firm: on slow laps he uses **less** peak pressure and
  carries it 20–60 m deeper than the reference. Pressure still on at turn-in is
  what makes the front push, which is what makes him add lock.
- **The lock is the symptom, not the cause.** Lateral grip still peaks near
  1.5 rad and collapses by 2.5–3.5 rad, and past that peak the instruction is
  always *unwind*. But treating the steering as the root cause coaches the
  symptom. Fix the release and the lock comes down on its own — it did, in a
  single lap, twice.
- The proof: at T8, release at −3 m and −6 m produced his two fastest laps
  (101.9 and a matched-reference 126 km/h minimum); +21 to +66 m produced
  102.4–105.5 and one spin. Peak pressure did **not** track lap time — his
  fastest lap had the second-highest peak brake of the set.
- **Second failure mode, new:** carrying 0.66 brake into turn-in at T8 rotated
  the rear and spun him (yaw kept climbing through −3.00 rad of opposite lock).
  Same root fault, opposite end of the car. He is not only an understeerer.
- He is ~3 s off on **most** tracks, so this is technique, not track knowledge.
- Brake bias above 55.6 degrades every metric; at 50–51 he matches his best
  time with far less lock.
- **Straight-line speed is not his problem.** Corners are 80–95% of the deficit
  every lap. He matches a 300.9 km/h reference on throttle alone; on his best
  lap he was 0.07 s *faster* than the reference on the straights.

**Advice that has made him slower.** He has caught every one of these from the
seat before the data did:

- *"Carry the brake to the apex"* — he held near-full pressure and overslowed.
- *"Carry more speed"* — useless at the grip limit while understeering.
- *"Trail the brake in further"* — told him to do more of his worst habit. It
  was firing 3 times in 4 laps from the rig. **Removed from `CornerCoach`, with
  a test asserting it can never be spoken again.**
- *"Get it rotated on entry"* — heard as rotating the car with the brake, which
  is the input that spun it. **Also removed.**
- *"Still deep on the brake there"* — ambiguous; he heard it as *brake more*,
  and his peak pressure dropped while the release went 50 m later. Say **"off
  the brake sooner, short and firm"**, never anything that can be read as more.
- *"Brake a beat later, you're slowing too early"* — cost 4.4 s the lap it was
  spoken, 2026-08-05 at T14: vmin fell from 142 to 109 and he lost the whole
  back half. He did not brake late and run wide; he **stopped braking and
  coasted**, throttle cycling 1.00/0.02/0.13/0.77 in 4th with no brake at all,
  no off, no spin, no incident. Taking away his braking marker without giving
  him a new one removes the thing he was aiming at, and he goes tentative rather
  than faster. Same family as *"carry more speed"*: it names an **outcome**, not
  an **input**. Only ever cue an input he can execute — a pressure, a release
  point, a shape.

Prefer **diagnosis over description**. Comparing channels says what differs,
not why. Always ask whether an instruction is even achievable at the grip limit
before giving it. Track-independent cues beat reference-lap cues, because his
problem travels between circuits.

**Give him one thing at a time.** Demonstrated repeatedly on 2026-08-04: told
two corners at once, he fixes the new one and loses the old one. Confirm a fix
has held for two laps before moving on.

**Feed-forward beats post-mortem** — his idea, and it works. `CornerCoach` now
remembers each corner's fault and speaks it on the exit of the *preceding*
corner ("Turn eight next, off the brake sooner"), falling back to retrospective
only on the first lap. 6–9 notes a lap. He asked for at least 3.

## Building and deploying

```bash
rsync -a --exclude build /home/amayorga/dev/rline/rline/ /mnt/c/Users/amayorga/rline-build/
cd /mnt/c/Users/amayorga/rline-build && \
  "/mnt/e/Program Files/CMake/bin/cmake.exe" --build build --config Release
cp build/Release/rline.exe /mnt/c/Users/amayorga/rline-dist/
```

Build `--target rline` rather than everything: `relay.exe` runs on this machine
during a session and relinking it fails with `LNK1104` while it is up.

**Since 2026-08-11 the driver copies `rline.exe` alone.** The rig fetches
`lap.csv` and `corner-names.txt` from the relay itself, so a track change, a
new reference or a renamed corner needs no folder copy - only a rig restart.
Copy the whole `rline-dist` folder only when it is a fresh install with no data
files beside the exe; the sync handles that case too, it is just slower.

### The rig fetches its own track data

On startup, before it loads anything, the rig opens a short-lived connection to
the relay on the same port, says which files it has and what they hash to, and
takes back any that differ. Then it loads from disk exactly as it always did -
sync-then-load, so there is no runtime reload path to get wrong.

```
relay: <- rig asking for track data (protocol 1)
relay:    lap.csv - sending 788134 bytes [455bbfae], rig had [none]
relay:    corner-names.txt - already current [c6e15132]
```

Relay serves from beside `relay.exe`, which is where CMake already puts the pair
chosen by `RLINE_TRACK`, so what it serves and what this machine built cannot
disagree. `--data <dir>` overrides it. Relay prints the pair and their hashes at
startup, and shouts if either is missing.

**It fails soft, deliberately.** A relay that is down, busy with an older
connection, or serving a truncated file all end the same way: cached files
untouched, rig starts anyway, and the log says
`no relay at ... using the cached files`. **That line means the sync did not
happen** - it is the state where the rig coaches from whatever was last copied,
which is the failure the feature exists to remove. Do not read a normal-looking
startup as proof the data is current; look for the `updated`/`already current`
count.

Two properties worth not breaking, both asserted in `test-data-sync`: only
`lap.csv` and `corner-names.txt` can ever be written (an allowlist, so path
traversal is unrepresentable rather than sanitised), and bytes are hashed after
arrival and written to a temp name renamed only on a match - a truncated
download must never replace a good reference lap. The portable half is
`src/data-sync.cpp`, tested on Linux; the socket halves are
`src/data-sync-net.cpp` on the rig and `serveSync` in `tools/relay.cpp`.

### Switching the rig to a different track

The track is not a setting. `main.cpp` defaults to `lap.csv` beside the exe and
`corner-coach-sys.cpp` hard-codes `corner-names.txt`; CMake copies `data/lap.csv`
and `data/corner-names.txt` into the build directory. So a track change is a
**file swap plus a redeploy**, and both files must move together — a Mugello
reference with Road America names speaks the wrong corner at every single one.

**Done 2026-08-09.** All five items below are complete and `rline-dist` ships
Mugello. What was changed, and what to know about each:

1. **The two data files are swapped by CMake.** `RLINE_TRACK` defaults to
   `mugello` and selects the pair; `-DRLINE_TRACK=roadamerica` switches back.
   The tests still name `data/lap.csv` directly, so they are unaffected by
   whatever is deployed.
2. **`tools/quick.py` now carries a per-track table** with `--track`, default
   mugello, and prints the spoken corner name. Its spans are turn-in→exit, not
   entry→exit, so a corner no longer reports its neighbour's minimum speed —
   that leak had put four wrong reference vmins in the Road America table
   (corners 4, 7, 8, 10 read 112/126/259/140 instead of 123/201/283/233).
   Regenerate with `tools/dump-corners.cpp`, never by hand.
3. **`tools/analyze.py`** — `REF_DEFAULT` now points at `data/muguello-ref.csv`.
4. **`tools/lap-qc.py`** — `REFERENCE_S = 83.63`.
5. **The speed-deficit cue is suppressed on aero-limited corners**, and the rule
   is track-independent rather than a hard-coded corner 3: `CornerCoach` refuses
   check 4 entirely when `RefCorner::peakBrake < aeroPeakBrake` (0.25). That
   catches Mugello's detected 3 (0.13) and Road America's Carousel (0.00), which
   is the corner that first exposed the blind spot. `test-corner-coach` asserts
   no aero corner can ever draw a speed cue, on both references.

**A measurement-window bug was found doing this, and it would have fired every
lap.** `CornerCoach`'s accumulator measured peak brake and brake release over
entry→exit, while `RefCorner` measures them over entry→**apex**; peak steer was
entry→exit against the reference's turnIn→exit. At Road America the spans are
short enough that this never showed. At Mugello, detected 1 (San Donato) runs on
to within a few metres of the braking for Materassi, so it picked up the *next*
corner's brake and reported a **460 m late release** — driving the reference lap
back at itself produced "San Donato, off the brake sooner" every lap. That is
his dominant-fault cue, on the most important braking corner on the circuit,
fired on a perfect lap. Fixed in `corner-coach.cpp` by measuring each figure over
exactly the window `RefCorner` uses for its counterpart. The regression test is
"the reference lap draws no criticism", now run against **both** references —
that assertion is what caught it, and it is why the Mugello reference is now a
registered test case (`corner-coach-mugello`) rather than a manual check.

`lap-qc.py` itself works unchanged on captured laps: the rig sends all 281
channels including `SessionTime` and `P2P_Status`. It is only the *reference*
exports that lack them, and those are never run through it.

`./test.sh` runs eleven portable suites on Linux — no Windows SDK, no iRacing —
twelve runs, because `test-corner-coach` is run once against each reference.
`test-corner-coach` covers what the rig says out loud, including assertions that
the two harmful cues can never come back.

**Audit the spoken prompts by replaying real laps through `CornerCoach`** rather
than reasoning about them. A ~40-line harness that loads `data/lap.csv`, feeds
archived lap CSVs through `CornerCoach::update` and tallies the notes is what
revealed it was saying "trail the brake in further" three times in four laps.
Keep one `CornerCoach` across all laps, as the rig does, or feed-forward never
warms up.

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

## Where the driving got to

**T8 and T5 were both solved on 2026-08-04** — T8 to a minimum speed of 126,
exactly the reference, with the lock down to 1.55 rad; T5 to 112, also exact.
Both fell to the release-point cue, each within one lap of hearing it. On
2026-08-05 T8 held (vmin 130/127/129 against a 126 reference) but **T5 came
apart again** — release walked from −37 m to +29 m across three laps while peak
pressure *fell* 0.87 → 0.75, vmin collapsed to 85 against 112, and lock reached
2.75 rad. Meanwhile T1 was fixed in the same stint: peak brake 1.00 → 0.64
against a 0.61 reference, vmin 187 → 205. He fixed the new corner and lost the
old one, exactly as on 2026-08-04.

**T8 was solved on 2026-08-05 evening and held three laps.** The cue was
"squeeze the brake harder then off it sooner, short and firm" and it landed in
one lap: release +15 m late → −24 m early, vmin 116 → 129 against a 126
reference, peak steer following it down 1.87 → 1.23 a lap later. That is now
three corners fixed by the same release-point cue (T5, T8, and T8 again after
it drifted), and the pattern is always the same - the lock follows the release
by about a lap, so do not coach the steering.

**Brake bias 55.15 beat 57.25 by 0.64 s**, measured the same evening. He ran a
run of pit-exit laps while adjusting it, all `PARTIAL(94%)`, which cannot be
timed as laps - but every corner including T1 lies inside pct 0.061→1.000, so
**compare the common segment rather than waiting for a full lap**. On that
segment: 98.02 s at 55.15 against 98.66 s at 57.25, with T1 196→209 and T8
117→125 (both essentially at the reference) and peak steer at T8 down
1.52→1.38. Caveats worth repeating before leaning on it: two clean laps against
four, the T1 gain is confounded by a T1 cue given at the same time, and the
groups are twenty minutes apart. It agrees with the earlier independent finding
that above 55.6 every metric degrades.

**Lap times: use only P2P-clean laps.** The oft-quoted 101.9 s was 97% boosted
and does not count; so were 102.4 and 102.8. His **best clean lap is 103.5 s**
(`20260804-221246-lap0024`), then 103.7 and 104.2. Today's 104.8 and 105.1 are
clean, so he is ~1.3 s off his own best rather than ~3 s. Any lap-time ranking
from 2026-08-04 that predates this check silently mixed boosted and clean laps
and should be redone with `lap-qc.py`. Corner-level findings (brake release,
vmin, steering) survive — boost changes power, not braking.

Whether `data/lap.csv` itself used P2P is **unknown**: it is a different export
format with Lat/Lon and no P2P channels at all, so the 100.0 s target may or may
not be boost-assisted. Worth resolving before treating it as a fair target.

**`data/muguello-ref.csv` is clean, and that was established without a P2P
channel** — the same method settles `lap.csv` if anyone wants it. The file has
no `P2P_Status`, so `lap-qc.py` cannot read it, but the physics answer it:

- 60.08 Hz confirmed by integrating GPS distance against `Speed` (16.645 ms per
  sample, p05 16.51 / p95 16.78). 5021 rows, 83.63 s, 5189 m.
- A genuine flying lap, not a splice or an out-lap: `LapDistPct` is monotonic
  0.00050 → 0.99990 with the only reversal being the final wrap, and speed is
  continuous across start/finish (79.45 → 79.39 m/s).
- **No boost.** The main straight is one continuous 16.4 s wide-open pull from
  177 to 299 km/h across start/finish, and its acceleration decays smoothly and
  monotonically the whole way — 7.6 → 5.0 → 2.1 → 1.1 → 0.36 m/s². A P2P
  activation puts a step in that curve. Comparing acceleration at matched
  gear/RPM bins across all nine wide-open stretches, no straight is
  systematically stronger than any other.
- The two +2 m/s² jumps that scan does find (pct 0.226 and 0.323) are corner
  exits where lateral load unwinds and the traction budget frees up. Look for
  boost on a straight at constant gear, never right after an apex.

So the 83.63 s is a fair target. Unverifiable from the file: whether the car
even has P2P, and track limits — there is no `IsOnTrack` channel in this format.

**2026-08-09: the track changed to Mugello for the week.** Everything below is
Road America and is paused, not discarded. What transfers is the *fault* — the
long-and-light brake release — because it travels between circuits; that is the
whole reason track-independent cues are preferred. What does not transfer is
every corner name, every reference number, and the grip heuristic. Do not open
a Mugello stint by quoting a Road America corner.

### Mugello, 2026-08-09 — the first evening of laps

54 complete clean laps across three stints, all QC-verified, none P2P-affected.

|  | stint 1 | stints 2–3 |
|---|---|---|
| flying laps | 16 | 32 |
| best | 86.8 | **86.6** |
| median | 88.8 | **87.9** |
| sub-88 | 5 (31%) | 18 (56%) |

**The gain is consistency, not peak.** The best lap moved only 0.2 s; the median
moved 0.9 s and the sub-88 rate nearly doubled. Quote it that way — the peak
number alone overstates what happened.

**The four mechanically-limited corners were the right place to look, and all
four fell to release cues**, exactly as at Road America. End state on a good lap
(reference in brackets): San Donato 129 [129], Scarperia 155 [147], Correntaio
137 [133], Bucine 164 [153]. Casanova reached 229 [229] without ever being
coached — the aero suppression left it alone and it came good on its own.

**Materassi is the one corner with no safe cue, and that is a tooling limit.**
It ran 165–186 against a 196 reference with essentially no correlation to any
brake metric. Fixing its pressure (0.71 → 0.31) and then its release (+127 m →
0 m) each landed perfectly and moved the minimum speed by almost nothing. The
segment data explains why: the loss splits between the *run down from San
Donato* (11 km/h down before the braking zone even starts) and *Borgo San
Lorenzo* at the far end of the same detected span, with his lock there at
0.90–0.94 against a 1.01 reference — under-committed, not over-driving. The only
instruction that addresses it is "carry more speed", which is the worst cue in
his history. **Leave it alone until the sustained-lateral-G measure exists**
(open thread 1). Twice it improved to a session best on laps where it was not
cued.

**Next target is San Donato's exit, not its apex.** Its minimum speed is fine —
it hit the reference exactly — but he ran **1.00 brake pressure against a 0.95
reference on every single lap of the evening**, releasing 40–75 m early, and
the exit is where the run to Materassi is lost. That is mechanical, consistent
and cueable, unlike Materassi itself.

**Scarperia never beds in.** Fixed and re-fixed six times; it lands within one
lap every time and drifts back within two or three. Expect to prompt it every
stint. It is also the corner where the lock signature is worth watching: 1.98 rad
with a late release preceded the worst moment of the evening.

**Four incidents, and they cluster.** Two spins (Correntaio lap 33, San Donato
exit lap 43), one save (Bucine lap 36), one low-speed spin that looked like
avoidance rather than a driving error (Borgo San Lorenzo lap 55, braked from 146
to a standstill). **None were at the corner he was working on** — each was
somewhere he had stopped concentrating while executing a cue elsewhere. Three of
the four came within a lap or two of either a personal best or a new cue.

### Mugello, 2026-08-11 — the first evening coached against correct cues

30 complete flying laps, all QC-clean at 60 Hz, none lost. Best **87.9**, median
**89.0**, 12 of 30 under 89. Slightly off 08-09 (86.6 / 87.9) - but the rig ran
a pre-2026-08-09 exe that entire evening *and* the first stint of this one, so
08-09's coaching numbers describe a rig firing a false turn-one cue every lap
with no aero suppression. These are the first laps driven against correct cues.

**Three faults were fixed and held.** Turn ten, turn twelve and turn fifteen all
reached and held reference minimum speed simultaneously, with no active cue by
the end. The brake stabbing went to **zero mid-corner re-applications - better
than the reference lap**, which has one at turn one.

**Baseline the dab count before cueing it.** The reference itself re-applies the
brake once, at turn one, where T1 runs into T2. Two presses there is correct
technique. Four (2026-08-11 lap 16) is the near-spin signature.

**Turn one resists coaching** - it degraded under a pressure cue and under the
coast cue and came right all three times it was left alone. See the memory of
the same name; the short version is that its gap can only be closed by braking
longer, which is the banned direction, so leave it uncued.

**Release point cannot be read without peak pressure.** At turn fifteen he
released 70-79 m *early* and ran 157-159; releasing at the reference point
dropped him to 139-145. The reference's late release works because it is at
0.81; his at 0.58 is just a long light drag - the root fault exactly. Both
halves of "squeeze the brake harder then off it sooner" applied, and it landed
in one lap.

**The coast cue has a range.** It produced the biggest gain of the night
(6.26 s to 4.08 s lost) while there was slack, then stopped paying: across the
second stint, *less* coasting correlated with *slower* laps. Retired rather than
repeated. Lap time tracks corner minimum speeds, not coast distance, once the
corners are near reference.

Next levers at Mugello, in order:

1. **Revisit the trail-brake ban.** It was written for the opposite fault to the
   one he has now, and it is the binding constraint on turn one and on every
   corner's coast - roughly 1.5 s. Replay these 30 laps through `CornerCoach`
   before changing anything.
2. **Split detected corner 4** so turn eleven can be cued apart from turn ten.
   Both of the evening's stabbing incidents were at T11, cued as "turn ten".
3. **The sustained-lateral-G measure** (open thread 1), which is what would make
   turn four and turn six - about 1.5 s more - coachable at all.
4. **Brake bias is 55.58**, right at the top of the band where every metric
   starts degrading. Untested at Mugello; the supporting data is Road America.

Next levers at Road America, in order, as of the end of 2026-08-05:

1. **T14 is the only corner still clearly down** — 145–148 against a 159
   reference across the whole evening. It is also the most delicate: it is where
   "brake a beat later" cost 4.4 s, and where he went off. Cue an input there or
   nothing at all.
2. **T1 is half done.** After the "easier on the brake" cue his vmin went
   196 → 202 → 209 but peak pressure went *up* (0.74 → 0.87), which is the
   opposite of what was asked. Worth one clean look before deciding whether the
   cue landed or he simply started braking later.
3. **Hold T5, T8 and T1 on the same lap.** Each is now solved individually;
   they have not yet coincided.
4. **Two offs in one stint** (T14, the Carousel), incidents 5 → 7. If that
   continues, it is worth asking whether he is being pushed to drive at a limit
   he has not settled into, rather than adding another corner.

## Open threads

1. **Sustained-load grip is invisible to the current model, and Mugello makes
   it urgent.** `GripCurve` indexes on steering *angle*, so the Carousel — long,
   fast, heavily loaded at only 0.62 rad — reads as "0% past peak" and looks
   like an uncommitted lift. It was nearly coached as "carry more speed" before
   he said the Carousel is grip-limited. The `room for more speed` cue in
   `CornerCoach` has the same blind spot. Needs a lateral-G-over-time measure,
   not an angle threshold.

   **At Road America this was one corner. At Mugello it is a quarter of the
   lap.** He flagged on 2026-08-09 that not all corners in this car are
   grip-limited, and the data agrees emphatically: the five fastest turns
   (Materassi through Arrabbiata 2) pull 3.9–4.2 g on 0.88–1.15 rad of lock and
   a trace of brake, while the four slow ones pull only 3.0–3.3 g on 1.14–1.32
   rad. Load tracks speed, not angle. All five aero-limited turns sit inside
   detected corner 3, so a single blind cue covers all of them. Until the
   lateral-G measure exists, suppress the speed-deficit cue on that corner.
2. The frozen brake panel is built and deployed, but **his remaining time is not
   in the brake trace** — T1 is pressure, the Carousel is sustained load. A
   second lane showing steering angle against the learned grip peak has been
   offered twice and not yet taken.
3. ~~Mic input via SAPI recognition~~ — **built 2026-08-05**, free dictation on
   a wheel button. See "Hearing the driver". Still open: nothing yet *acts* on
   what he says. A spoken **"mark"** should timestamp the moment he felt
   something wrong, and low-confidence utterances should prompt a spoken "say
   again" rather than being silently dropped.
4. **Nothing acts on what he says.** Utterances land in `inbox.txt` and are
   read by a human eye only. A spoken "mark" should timestamp the moment, and
   a low-confidence utterance should prompt a spoken "say again" rather than
   being silently dropped.
5. **The fixed-phrase grammar is built and parked.** `data/voice-grammar.txt`
   (34 phrases), `src/voice-grammar.cpp` and a passing `test-voice-grammar`
   exist but are deliberately **not linked into rline** — he asked to try
   dictation first on 2026-08-05. Wiring it in is a second SAPI grammar loaded
   alongside dictation, tagged `via=cmd` on the wire, which `voice-line` already
   supports.
6. **The SAPI-facing code has no test coverage.** `voice-input-sys.cpp` and
   `coach-speech-sys.cpp` only build on Windows, so a signature change on this
   side compiles green here and fails there — it did exactly that on
   2026-08-05. Build the rig target before believing a refactor is done.
7. Programmable HUD channels: push `CH|id=..|type=bar|expr=speed-ref.speed` and
   have the rig evaluate it, so new gauges need no rebuild. Never started.
8. No way to mute or retune the rig's coach from the relay — changing what it
   says still needs a rebuild and a restart. A `COACH|off` wire command was
   scoped and not built. Now that speech is a queue, this would be a matter of
   refusing pushes at one priority.
