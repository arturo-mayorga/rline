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
`/mnt/c/rline-coach/archive/qc.log`; **poll that log during the stint**, not
after. It catches spliced files, partial laps, and push-to-pass. Watching the
relay log alone is not enough — it says a lap was *written*, never whether the
copy that survived is intact.

Use `quick.py` while he is driving — `analyze.py` takes long enough that he is a
lap and a half further on by the time it answers.

**`analyze.py`'s "ref vmin" column is wrong for overlapping corners.** It reports
the minimum over the whole span, so a corner picks up its neighbour's minimum;
that is why adjacent corners print identical values. The trustworthy per-corner
figures are `RefCorner::vmin`, measured from turn-in.

`data/lap.csv` is the reference lap (100.0 s, 6413 m, 11 detected corners).
Captured laps land in `/mnt/c/rline-coach/laps/lap-NNNN.csv`.

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
is written. Starting it mid-stint does not recover what is already gone.

**The trigger is relay's own log, not a settle timer.** relay prints
`lap N written` *after* it closes the file, so that line is a guarantee the lap
is complete and it is copied that instant. A settle-based scan runs alongside as
a safety net. This matters because the survival window can be shorter than any
settle: an 8 s poll / 10 s settle saved **1 of 8** flying laps on 2026-08-05
while appearing to run perfectly, and even 1 s poll / 3 s settle lost one lap
that evening to a reset within 3 s of the line. Verify it works rather than that
it is running — drop a >4 MB file into the laps directory, append a matching
`relay: lap N written` line, and confirm it is archived in the same second.

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

## Corner numbering — read this before speaking a single corner number

The detector numbers what it finds 1..N from start/finish. **Those are not the
track's turn numbers.** It finds 11 corners across 16 numbered turns here and
does not see flat-out kinks at all. An entire session was coached using detector
numbers, sending the driver to the wrong piece of road every time.

`data/corner-names.txt` holds the mapping, is copied next to the exe by CMake,
and is read at startup by `CornerCoach::loadNames`. The rig speaks those names.
When the reference lap changes, redo the mapping.

The anchor is **the Carousel: the only corner the reference takes with no brake
at all** (`RefCorner::releasePct < 0`). Everything else follows from its place in
the lap. Cross-check by asking him which corners are grip-limited: the ones he
names must be the detected corners with low vmin and high peak steer, and the
leftovers must be the fastest with the least lock.

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

Tell the driver to copy **the whole `C:\Users\amayorga\rline-dist` folder** and
restart — it now carries `corner-names.txt` as well as `lap.csv`, and without it
the rig speaks detector numbering again.

`./test.sh` runs six portable suites on Linux — no Windows SDK, no iRacing.
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

Next levers, in order, as of the end of 2026-08-05:

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

1. **Sustained-load grip is invisible to the current model, and it caused a bad
   call.** `GripCurve` indexes on steering *angle*, so the Carousel — long, fast,
   heavily loaded at only 0.62 rad — reads as "0% past peak" and looks like an
   uncommitted lift. It was nearly coached as "carry more speed" before he said
   the Carousel is grip-limited. The `room for more speed` cue in `CornerCoach`
   has the same blind spot. Needs a lateral-G-over-time measure, not an angle
   threshold.
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
