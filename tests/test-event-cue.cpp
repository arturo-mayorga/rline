// Timing tests for the brake countdown. The scheduler is free of any audio
// API, so the thing that actually matters - that the third beep lands on the
// braking point - can be checked without a sound card.
//
// g++ -std=c++17 -O2 -o test-event-cue tests/test-event-cue.cpp
//     src/event-cue.cpp src/refline.cpp

#include "../src/event-cue.h"
#include "../src/refline.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

static int g_fail = 0;

static void check(bool ok, const char *what)
{
    printf("%s  %s\n", ok ? "  ok  " : "  FAIL", what);
    if (!ok)
        ++g_fail;
}

static void checkNear(float got, float want, float tol, const char *what)
{
    const bool ok = fabsf(got - want) <= tol;
    printf("%s  %s (got %.3f, want %.3f +/- %.3f)\n",
           ok ? "  ok  " : "  FAIL", what, got, want, tol);
    if (!ok)
        ++g_fail;
}

namespace
{
    struct Fired
    {
        int beep;
        float t;         // seconds into the run
        float distToPct; // metres still to the braking point
    };

    // Drives at a constant speed from `startPct` and records every beep.
    std::vector<Fired> run(const RefLine &line, EventCue &cue,
                           float startPct, float speed, float seconds,
                           float dt = 1.0f / 60.0f)
    {
        std::vector<Fired> out;
        float pct = startPct;
        float t = 0;

        for (int i = 0; i < (int)(seconds / dt); ++i)
        {
            const float target = cue.targetPct();
            const int b = cue.update(line, pct, speed, dt);
            if (b > 0)
            {
                const float d = (target >= 0) ? distanceAhead(line, pct, target)
                                              : distanceAhead(line, pct, cue.targetPct());
                out.push_back({b, t, d});
            }

            pct += (speed * dt) / line.length;
            if (pct >= 1.0f)
                pct -= 1.0f;
            t += dt;
        }
        return out;
    }

    float firstBrakePctAfter(const RefLine &line, float pct)
    {
        float bestD = -1, bestPct = -1;
        for (const RefEvent &e : line.events)
        {
            if (e.kind != RefEventKind::Brake)
                continue;
            const float d = distanceAhead(line, pct, e.pct);
            if (bestD < 0 || d < bestD)
            {
                bestD = d;
                bestPct = e.pct;
            }
        }
        return bestPct;
    }
}

int main(int argc, char **argv)
{
    const std::string refPath = (argc > 1) ? argv[1] : "data/lap.csv";

    RefLine line;
    std::string err;
    if (!loadRefLineCsv(refPath, line, &err))
    {
        printf("FAILED TO LOAD %s: %s\n", refPath.c_str(), err.c_str());
        return 1;
    }

    int brakePoints = 0;
    for (const RefEvent &e : line.events)
        if (e.kind == RefEventKind::Brake)
            ++brakePoints;
    printf("reference lap has %d braking points\n\n", brakePoints);
    check(brakePoints >= 5, "the reference lap has braking points to cue");

    printf("-- a single approach --\n");
    {
        // Start well before the first braking point and drive up to it.
        const float target = firstBrakePctAfter(line, 0.02f);
        const float speed = 70.0f; // m/s
        EventCue cue;

        // Begin far enough out that the countdown starts cleanly.
        const float startPct = target - (380.0f / line.length);
        std::vector<Fired> beeps = run(line, cue, startPct, speed, 6.0f);

        printf("       (%zu beeps fired)\n", beeps.size());
        for (const Fired &f : beeps)
            printf("       beep %d at t=%.3f s, %.1f m to go\n", f.beep, f.t, f.distToPct);

        check(beeps.size() == 3, "exactly three beeps fire");

        if (beeps.size() == 3)
        {
            check(beeps[0].beep == 1 && beeps[1].beep == 2 && beeps[2].beep == 3,
                  "they fire in order");

            // The whole point: the last beep lands on the braking point.
            checkNear(beeps[2].distToPct, 0.0f, speed * (1.0f / 60.0f) + 0.5f,
                      "the third beep lands on the braking point");

            // An even rhythm is what makes it anticipatory.
            checkNear(beeps[1].t - beeps[0].t, cue.intervalS, 0.03f,
                      "beep 1 to 2 is one interval");
            checkNear(beeps[2].t - beeps[1].t, cue.intervalS, 0.03f,
                      "beep 2 to 3 is one interval");
        }
    }

    printf("\n-- speed independence --\n");
    {
        // The final beep must land on the point at any speed, since the cue is
        // scheduled in time-to-go rather than distance.
        float worst = 0;
        int runs = 0;
        for (float speed : {30.0f, 50.0f, 70.0f, 85.0f})
        {
            const float target = firstBrakePctAfter(line, 0.02f);
            EventCue cue;
            const float startPct = target - (390.0f / line.length);
            // Stop just after the approach: run any longer at high speed and
            // the next braking point starts a second countdown.
            std::vector<Fired> beeps = run(line, cue, startPct, speed,
                                           390.0f / speed + 0.2f);
            if (beeps.size() == 3)
            {
                worst = std::max(worst, fabsf(beeps[2].distToPct));
                ++runs;
            }
        }
        printf("       (%d speeds produced a full countdown, worst miss %.2f m)\n",
               runs, worst);
        check(runs == 4, "every speed produced a full countdown");
        check(worst < 1.5f, "the third beep lands within a metre or so at any speed");
    }

    printf("\n-- no burst when arriving late --\n");
    {
        // Appearing close to a braking point must not machine-gun the beeps.
        const float target = firstBrakePctAfter(line, 0.02f);
        EventCue cue;
        const float startPct = target - (20.0f / line.length);
        std::vector<Fired> beeps = run(line, cue, startPct, 70.0f, 2.0f);
        printf("       (%zu beeps fired)\n", beeps.size());
        check(beeps.empty(), "no countdown when there is no time for one");
    }

    printf("\n-- quiet when it should be --\n");
    {
        EventCue cue;
        std::vector<Fired> beeps = run(line, cue, 0.02f, 1.0f, 2.0f);
        check(beeps.empty(), "no beeps while crawling below the speed floor");
    }

    {
        RefLine empty;
        EventCue cue;
        check(cue.update(empty, 0.5f, 60.0f, 1.0f / 60.0f) == 0,
              "an unloaded reference lap is silent");
    }

    {
        // A reference lap with no brake channel has no braking points, so the
        // cue must simply stay quiet rather than fire at nothing.
        RefLine noEvents = line;
        noEvents.events.clear();
        EventCue cue;
        check(cue.update(noEvents, 0.05f, 70.0f, 1.0f / 60.0f) == 0,
              "a lap without braking points is silent");
    }

    printf("\n-- a whole lap --\n");
    {
        // Every braking point should get exactly one full countdown.
        EventCue cue;
        std::vector<Fired> beeps = run(line, cue, 0.0f, 55.0f,
                                       line.length / 55.0f + 1.0f);
        int thirds = 0, ones = 0;
        for (const Fired &f : beeps)
        {
            if (f.beep == 1)
                ++ones;
            if (f.beep == 3)
                ++thirds;
        }
        printf("       (%zu beeps, %d countdowns started, %d completed, "
               "%d braking points)\n",
               beeps.size(), ones, thirds, brakePoints);
        check(thirds == ones, "every countdown that starts also completes");
        check(thirds >= brakePoints - 2 && thirds <= brakePoints,
              "each braking point gets one countdown");
    }

    printf("\n-- turn-in countdown --\n");
    {
        int turnPoints = 0;
        for (const RefEvent &e : line.events)
            if (e.kind == RefEventKind::TurnIn)
                ++turnPoints;

        EventCue cue;
        cue.kind = RefEventKind::TurnIn;
        cue.beeps = 2;
        cue.maxLookM = 250.0f;

        // Approach the first turn-in point.
        float target = -1, bestD = -1;
        for (const RefEvent &e : line.events)
        {
            if (e.kind != RefEventKind::TurnIn)
                continue;
            const float d = distanceAhead(line, 0.02f, e.pct);
            if (bestD < 0 || d < bestD)
            {
                bestD = d;
                target = e.pct;
            }
        }

        const float speed = 60.0f;
        const float startPct = target - (240.0f / line.length);
        std::vector<Fired> beeps = run(line, cue, startPct, speed,
                                       240.0f / speed + 0.2f);

        printf("       (%d turn-in points; %zu beeps on the first approach)\n",
               turnPoints, beeps.size());
        for (const Fired &f : beeps)
            printf("       beep %d at t=%.3f s, %.1f m to go\n", f.beep, f.t, f.distToPct);

        check(turnPoints >= 5, "the reference lap has turn-in points to cue");
        check(beeps.size() == 2, "exactly two beeps fire");
        if (beeps.size() == 2)
        {
            check(beeps[0].beep == 1 && beeps[1].beep == 2, "they fire in order");
            checkNear(beeps[1].distToPct, 0.0f, speed * (1.0f / 60.0f) + 0.5f,
                      "the second beep lands on the turn-in point");
            checkNear(beeps[1].t - beeps[0].t, cue.intervalS, 0.03f,
                      "the two beeps are one interval apart");
        }
    }

    printf("\n-- the two countdowns do not collide --\n");
    {
        // The braking countdown ends on the braking point and the turn-in one
        // starts an interval before turn-in. If turn-in followed braking by
        // less than one interval the two would overlap.
        float worstGapS = 1e9f;
        int pairs = 0;

        for (const RefEvent &b : line.events)
        {
            if (b.kind != RefEventKind::Brake)
                continue;
            // The turn-in belonging to the same corner.
            for (const RefEvent &t : line.events)
            {
                if (t.kind != RefEventKind::TurnIn || t.corner != b.corner)
                    continue;
                const float d = distanceAhead(line, b.pct, t.pct);
                const RefSample s = refAt(line, b.pct);
                const float gapS = d / std::max(s.speed, 1.0f);
                if (gapS < worstGapS)
                {
                    worstGapS = gapS;
                    ++pairs;
                }
            }
        }

        printf("       (tightest braking-to-turn-in gap: %.2f s)\n", worstGapS);
        check(pairs > 0, "corners pair a braking point with a turn-in");
        // One interval of headroom means the turn-in countdown starts no
        // earlier than the braking beep it follows.
        check(worstGapS > 0.5f,
              "the gap leaves room for the turn-in countdown");
    }

    printf("\n-- apex countdown --\n");
    {
        int apexPoints = 0;
        for (const RefEvent &e : line.events)
            if (e.kind == RefEventKind::Apex)
                ++apexPoints;

        EventCue cue;
        cue.kind = RefEventKind::Apex;
        cue.beeps = 2;
        cue.maxLookM = 250.0f;

        float target = -1, bestD = -1;
        for (const RefEvent &e : line.events)
        {
            if (e.kind != RefEventKind::Apex)
                continue;
            const float d = distanceAhead(line, 0.02f, e.pct);
            if (bestD < 0 || d < bestD)
            {
                bestD = d;
                target = e.pct;
            }
        }

        const float speed = 55.0f;
        const float startPct = target - (240.0f / line.length);
        std::vector<Fired> beeps = run(line, cue, startPct, speed,
                                       240.0f / speed + 0.2f);

        printf("       (%d apexes; %zu beeps on the first approach)\n",
               apexPoints, beeps.size());
        check(apexPoints >= 5, "the reference lap has apexes to cue");
        check(beeps.size() == 2, "exactly two beeps fire");
        if (beeps.size() == 2)
        {
            checkNear(beeps[1].distToPct, 0.0f, speed * (1.0f / 60.0f) + 0.5f,
                      "the second beep lands on the apex");
            checkNear(beeps[1].t - beeps[0].t, cue.intervalS, 0.03f,
                      "the two beeps are one interval apart");
        }
    }

    printf("\n-- no two tones overlap, over a whole lap --\n");
    {
        // Driven at the reference lap's own speed profile rather than a
        // constant speed, so the spacing is the one that will actually be
        // heard. Every tone from all three countdowns goes into one timeline.
        const float kFinalToneS = 0.090f; // longest tone the system plays

        EventCue brake, turn, apex;
        brake.kind = RefEventKind::Brake;
        brake.beeps = 3;
        turn.kind = RefEventKind::TurnIn;
        turn.beeps = 2;
        turn.maxLookM = 250.0f;
        apex.kind = RefEventKind::Apex;
        apex.beeps = 2;
        apex.maxLookM = 250.0f;

        struct Tone { float t; const char *cue; int beep; };
        std::vector<Tone> tones;

        const float dt = 1.0f / 60.0f;
        float pct = 0.0f, t = 0;
        for (int i = 0; i < 60 * 240; ++i)
        {
            const RefSample s = refAt(line, pct);
            const float v = std::max(s.speed, 1.0f);

            int b;
            if ((b = brake.update(line, pct, v, dt)) > 0) tones.push_back({t, "brake", b});
            if ((b = turn.update(line, pct, v, dt)) > 0)  tones.push_back({t, "turn", b});
            if ((b = apex.update(line, pct, v, dt)) > 0)  tones.push_back({t, "apex", b});

            pct += (v * dt) / line.length;
            if (pct >= 1.0f)
                break;
            t += dt;
        }

        std::sort(tones.begin(), tones.end(),
                  [](const Tone &a, const Tone &b) { return a.t < b.t; });

        float worst = 1e9f;
        int worstAt = -1;
        for (size_t i = 1; i < tones.size(); ++i)
        {
            const float gap = tones[i].t - tones[i - 1].t;
            if (gap < worst)
            {
                worst = gap;
                worstAt = (int)i;
            }
        }

        printf("       (%zu tones over the lap; tightest gap %.0f ms", tones.size(), worst * 1000.0f);
        if (worstAt > 0)
            printf(", between %s beep %d and %s beep %d",
                   tones[worstAt - 1].cue, tones[worstAt - 1].beep,
                   tones[worstAt].cue, tones[worstAt].beep);
        printf(")\n");

        check(tones.size() > 40, "the lap produced a full set of cues");
        check(worst > kFinalToneS,
              "no tone starts while the one before it is still playing");
    }

    printf("\n%s (%d failures)\n", g_fail == 0 ? "ALL PASSED" : "FAILURES", g_fail);
    return g_fail == 0 ? 0 : 1;
}
