# rline

A bare-bones iRacing overlay that shows a reference driving line against where
you actually are, so you can learn the line while driving.

Derived from `ir-replay-rc`, stripped to just the parts this job needs.

## What it shows

A car-centred, track-up view of the reference lap's line, with:

- **The line ahead** — roughly 120 m forward and 40 m back, drawn with lateral
  distance exaggerated 3x so a couple of metres off line is legible at a glance
  instead of about four pixels.
- **A cross-track bar** — how far left or right of the line you are, full-scale
  at ±5 m, coloured green under 0.5 m, amber under 1.5 m, red beyond.
- **Brake, turn-in and apex markers** for each corner, as gates labelled `B`,
  `T` and `A` on the line, plus a `BRAKE IN 45 m` countdown. These are derived
  from the reference lap's `Brake` and `SteeringWheelAngle` traces and indexed
  by track position, so they involve no dead reckoning and are exact.
- **Metres off line** and **speed delta** against the reference at the same
  point on track.

Everything reads *fly-to*, like a localizer needle: the bar's centre is you and
the marker is where the line is, so the correction is always to steer toward the
marker. `◀ 1.8 m` means go left 1.8 m - not "you are 1.8 m right".

A whole-lap map was deliberately left out: 6.4 km across a 360 px box is about
18 m per pixel, so a 2 m line error would be a tenth of a pixel — it would show
you where you are, never how far off you are.

## Build

Needs CMake and Visual Studio (any edition with the C++ workload). No network
access at configure time, no package manager, no third-party libraries.

```bash
./build.sh          # -> build/Release/rline.exe
```

The build links the **static CRT**, so the exe runs on a clean Windows install
with no Visual C++ redistributable and no dev tools. `lap.csv` is copied next to
the exe automatically; ship the two files together.

Verified with VS Build Tools 2022 (MSVC 19.30) and CMake 3.22: clean build, no
warnings, and `dumpbin /dependents` on the result lists only `KERNEL32`,
`USER32` and `GDI32` — a 350 KB self-contained exe.

### Building from WSL

MSBuild cannot run with a `\\wsl.localhost\...` working directory, so a build
launched against this tree from the Windows side will fail on UNC paths. Either
keep the project on the Windows filesystem (reachable from WSL under `/mnt/c`),
or copy it to a Windows-side directory before configuring.

## Run

```
rline.exe [--csv <path>] [--x N] [--y N] [--w N] [--h N] [--mph]
          [--exaggeration N] [--demo] [--unlocked] [--verbose] [--exit-after MS]
```

By default the overlay opens at the lower centre of the primary monitor, roughly
where the wheel sits in a cockpit view. **Ctrl+Shift+M** unlocks it so it can be
dragged with the mouse and locks it again; while unlocked it shows a border and
swallows clicks, while locked it is click-through. The position you drag it to
is remembered in `rline-pos.txt` beside the exe. `--unlocked` starts it
unlocked, for when another app already owns the hotkey.

`--demo` replays the reference lap instead of reading iRacing, weaving ±2.5 m
across the line so the bar and readout move. It exercises the entire render path
with no session running — use it to check placement and sizing on your monitor
before going near a car. `--exit-after MS` quits on a timer, for scripted checks.

The CSV needs `Lat`, `Lon`, `LapDistPct` and `Speed` columns. They are located
by header name, so column order and extra channels do not matter — any iRacing
telemetry export of a lap you like will work.

> **iRacing must run in windowed or borderless mode.** This is a layered Win32
> window using colour-key transparency. An exclusive-fullscreen swap chain
> draws straight over it and you will see nothing.

## Tests

The reference-line maths and the overlay geometry are free of `windows.h`, so
they build and run on any platform, with no Windows SDK and no running copy of
iRacing:

```bash
./test.sh
```

Covers CSV parsing, the local-tangent-plane projection, signed cross-track
error, the reference lookup, and the overlay's draw output swept across the
whole lap.

## Layout

```
src/
  main.cpp                      wiring and the tick loop
  refline.{h,cpp}               reference lap: parse, project, compare  (portable)
  ecs.h                         the ECS from ir-replay-rc, unchanged
  components/
    overlay-comp.h              ego state, reference lap, tuning
    rendering-comp.{h,cpp}      backend-agnostic draw list
  systems/
    irtelemetry-sys.{h,cpp}     iRacing SDK -> ego state             (Win32)
    demo-telemetry-sys.{h,cpp}  reference lap -> ego state, for --demo (portable)
    refline-overlay-sys.{h,cpp} ego state -> draw list               (portable)
    window-rendering-sys.{h,cpp} draw list -> layered window         (Win32)
  irsdk/                        official iRacing SDK client, unchanged
tests/
  test-refline.cpp              reference-line maths
  test-overlay.cpp              overlay geometry, headless
  test-lateral.cpp              lateral reconstruction, against a real drive
tools/
  irdump.cpp                    lists a live session's channels; --log captures them
data/
  lap.csv                       reference lap (Road America)
  drive-roadamerica.csv         a real captured drive, used by test-lateral
```

## What was dropped from ir-replay-rc

Everything driven by the broadcast/replay use case, none of which this needs:

- FTXUI and GoogleTest (both `FetchContent`, both needed network at configure
  time), plus the `XInput`/`DSound` links and `gpad.cpp`
- Replay direction, TV point, head-of-direction and closest-battle director
  systems; incident and overtake detectors; the scraper
- Timing tower, session leaderboard, broadcast car info/summary, current-driver
  overlay, replay-skip graphic
- The TUI (`tui-sys`, `txt-view/`) and the three old `main-*.cpp` entry points
- Camera control and the whole multi-car path: `CarIdx*` arrays, session-results
  YAML parsing. This reads only the player car's `LapDistPct`, `Speed`,
  `YawNorth`, `VelocityX`/`VelocityY`, `Lap` and `IsOnTrack`
- `irsdk_diskclient` — only needed for replaying `.ibt` files

## Fixes carried in

`irsdk_utils.cpp` did not compile. `irsdk_broadcastMsg(msg, var1, MAKELONG(...))`
is ambiguous: `MAKELONG` yields a `LONG`, and `long`→`int` and `long`→`float` are
the same conversion rank, so the two-argument overloads tie. Pinned to the `int`
overload, which is the packing that form intends. **This affects `ir-replay-rc`
too** — it carries the same file.

Three things in the reference window code would have broken a transparent
overlay, and are fixed here:

- **The back buffer was never cleared.** `CreateCompatibleBitmap` returns an
  uninitialised bitmap, so nothing was ever painted in the colour-key colour and
  the window rendered as a black box rather than a transparent one.
- **Partial repaints were offset.** The back buffer's origin is `(0,0)` but it
  was blitted to `ps.rcPaint`'s corner. Harmless only because the whole window
  was always invalidated. This now always paints the full client rect.
- **`Myriad Pro`** is not present on a stock Windows install, so text silently
  fell back to whatever GDI chose. Now `Segoe UI`.

Also added: `WS_EX_NOACTIVATE` and `WS_EX_TOOLWINDOW` so the overlay never
steals focus from the game or shows up in alt-tab, and a fixed ~60 Hz repaint
instead of invalidating on roughly every millisecond of the loop.

One more surfaced only once the thing was on screen: with `CLEARTYPE_QUALITY`
every character had a magenta halo. ClearType antialiases against *subpixels*,
blending glyph edges toward the colour key, which then only punches out pixels
that are exactly magenta. Text is now greyscale-antialiased and sits on a dark
backing panel — which also makes the readout legible over a bright game scene.

## Known limitations

### iRacing publishes no live position

This is the big one. Live shared memory carries no absolute position at all -
of its ~326 channels there is no `Lat`, `Lon`, `Alt`, and no world X/Y/Z.
`LapDist` and `LapDistPct` give position *along* the track and nothing across
it. The `Lat`/`Lon` in `data/lap.csv` exist only because `.ibt` disk exports
include them.

So lateral offset is **dead reckoned**, by integrating the Frenet relation
`dd/dt = V*sin(psi_vel - psi_ref)` where `psi_vel` is the car's actual direction
of travel. Two things make that workable:

- `YawNorth` is a true compass bearing. Measured against bearings derived from
  the reference lap's own lat/lon, the offset is **-0.018 degrees** - so no
  calibration constant is needed.
- The velocity vector, not the heading, must be used. `Yaw` alone includes slip
  angle, which is systematic through corners and drifted **22 m over a lap** in
  testing. Adding `atan2(VelocityY, VelocityX)` removes it. `VelocityY` is
  positive to the *left*, established by integrating a real captured lap both
  ways and keeping the sign that closed.

Measured drift over a full lap is **0 to 3 m**, and that figure is conflated
with genuine lap-to-lap line variation. The estimator resets at each lap
crossing to bound the growth, which means it **assumes you cross start/finish
exactly on the reference line** - cross it 2 m off and the whole lap is biased
by 2 m. If the drawn line looks like it never runs out to the track edge, that
bias is the first thing to suspect. Anchoring the reset on a corner apex, where
lines are far more repeatable, would be the fix.

The corner markers are unaffected by any of this.

### The reference lap's seam

The reference lap's start/finish seam is a genuine artifact of any single-lap
log: sorted by track position, the first and last samples are two different
passes through the line, here 0.63 m apart. Cross-track error can be off by up
to about 0.5 m across that one 0.63 m of track — one frame per lap. Everywhere
else the maths is accurate to well under a centimetre. `test-refline` pins this
bound so it cannot grow unnoticed.
