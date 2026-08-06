// Replays a captured lap through the per-corner coach.
//
// The point is not that it compiles - it is that it says the right thing about
// laps whose problems are already known from offline analysis. Lap 7 released
// the brakes early; lap 9, driven after being told to carry the brake, went to
// full pressure and overslowed. The coach must diagnose those differently.
//
// g++ -std=c++17 -O2 -o test-corner-coach tests/test-corner-coach.cpp
//     src/corner-coach.cpp src/refline.cpp

#include "../src/corner-coach.h"
#include "../src/grip-curve.h"
#include "../src/refline.h"

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

static int g_fail = 0;

static void check(bool ok, const char *what)
{
    printf("%s  %s\n", ok ? "  ok  " : "  FAIL", what);
    if (!ok)
        ++g_fail;
}

namespace
{
    struct Sample
    {
        float pct, speed, brake, throttle, onTrack, steer;
    };

    std::vector<Sample> loadDrive(const std::string &path)
    {
        std::vector<Sample> out;
        std::ifstream f(path);
        std::string line;
        if (!std::getline(f, line))
            return out;

        std::map<std::string, int> col;
        {
            std::stringstream ss(line);
            std::string c;
            int i = 0;
            while (std::getline(ss, c, ','))
            {
                while (!c.empty() && (c.back() == '\r' || c.back() == ' '))
                    c.pop_back();
                col[c] = i++;
            }
        }

        const char *need[] = {"LapDistPct", "Speed", "Brake", "Throttle", "IsOnTrack", "SteeringWheelAngle"};
        for (const char *n : need)
            if (!col.count(n))
            {
                printf("  (missing column %s)\n", n);
                return out;
            }

        std::vector<float> v;
        while (std::getline(f, line))
        {
            v.clear();
            std::stringstream ss(line);
            std::string c;
            while (std::getline(ss, c, ','))
                v.push_back((float)atof(c.c_str()));

            if ((int)v.size() <= col["IsOnTrack"] || (int)v.size() <= col["SteeringWheelAngle"])
                continue;

            Sample s;
            s.pct = v[col["LapDistPct"]];
            s.speed = v[col["Speed"]];
            s.brake = v[col["Brake"]];
            s.throttle = v[col["Throttle"]];
            s.onTrack = v[col["IsOnTrack"]];
            s.steer = v[col["SteeringWheelAngle"]];
            out.push_back(s);
        }
        return out;
    }

    struct Tally
    {
        int easier = 0, trail = 0, speed = 0, good = 0, total = 0;
    };

    Tally replay(const RefLine &line, const std::vector<Sample> &drive, bool show)
    {
        CornerCoach coach;
        Tally t;
        for (const Sample &s : drive)
        {
            if (s.onTrack < 0.5f)
                continue;
            CornerVerdict v = coach.update(line, s.pct, s.speed, s.brake, s.throttle, s.steer, -1.0f);
            if (v.corner == 0)
                continue;
            ++t.total;
            if (v.good)
                ++t.good;
            else if (v.note.find("easier") != std::string::npos)
                ++t.easier;
            else if (v.note.find("trail") != std::string::npos)
                ++t.trail;
            else
                ++t.speed;
            if (show)
                printf("       T%-3d %s\n", v.corner,
                       v.good ? "(clean)" : v.note.c_str());
        }
        return t;
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

    printf("reference: %d corners with per-corner data\n\n", (int)line.corners.size());
    check(line.corners.size() >= 8, "the reference lap yielded corner data");

    bool anyBrake = false, anyThrottle = false;
    for (const RefCorner &c : line.corners)
    {
        if (c.peakBrake > 0.1f)
            anyBrake = true;
        if (c.fullThrottlePct >= 0)
            anyThrottle = true;
    }
    check(anyBrake, "reference corners carry brake pressure");
    check(anyThrottle, "reference corners carry a throttle pickup point");

    printf("\n-- synthetic: driving the reference back at itself --\n");
    {
        // Replaying the reference lap through the coach must produce no
        // complaints: it is by definition the target.
        std::vector<Sample> perfect;
        for (const RefPoint &p : line.pts)
            perfect.push_back({p.pct, p.speed, p.brake, p.throttle, 1.0f, p.steer});
        Tally t = replay(line, perfect, false);
        printf("       %d corners judged, %d clean, %d easier, %d trail, %d speed\n",
               t.total, t.good, t.easier, t.trail, t.speed);
        check(t.total >= 8, "every corner was judged");
        check(t.good == t.total, "the reference lap draws no criticism");
    }

    printf("\n-- synthetic: heavy braking, released early --\n");
    {
        // Full pressure everywhere, off the brake well before the apex.
        std::vector<Sample> bad;
        for (const RefPoint &p : line.pts)
        {
            float b = p.brake > 0.05f ? 1.0f : 0.0f;
            // release early: drop brake in the second half of each braking zone
            bad.push_back({p.pct, p.speed * 0.92f, b, p.throttle, 1.0f, p.steer});
        }
        Tally t = replay(line, bad, false);
        printf("       %d corners judged, %d clean, %d easier, %d trail, %d speed\n",
               t.total, t.good, t.easier, t.trail, t.speed);
        check(t.easier > 0, "over-pressure is diagnosed as too much brake");
        check(t.trail == 0, "trail-brake-further is never said to this driver");
    }

    printf("\n-- dragging the brake past the reference's release --\n");
    {
        // The dominant fault: the same brake point, but pressure carried on
        // well past where the reference let it go. This is what makes the
        // front push, and before today it produced no cue at all.
        std::vector<Sample> smeared;
        for (const RefPoint &p : line.pts)
            smeared.push_back({p.pct, p.speed, p.brake, p.throttle, 1.0f, p.steer});

        auto within = [](float pct, float a, float b)
        {
            return (a <= b) ? (pct >= a && pct <= b) : (pct >= a || pct <= b);
        };

        // Everything identical to the reference except that the pedal is held
        // on for another 40 m past where the reference let it go.
        int dragged = 0;
        for (const RefCorner &rc : line.corners)
        {
            if (rc.releasePct < 0)
                continue;
            ++dragged;
            for (Sample &s : smeared)
            {
                float d = s.pct - rc.releasePct;
                if (d < -0.5f)
                    d += 1.0f;
                const float m = d * line.length;
                if (m > 0 && m < 40.0f && within(s.pct, rc.pctEntry, rc.pctExit))
                    s.brake = std::max(s.brake, 0.5f);
            }
        }
        printf("       %d corners given a late release\n", dragged);
        int late = 0, total = 0;
        CornerCoach cc;
        GripCurve g;
        for (int lap = 0; lap < 2; ++lap)
            for (const Sample &s : smeared)
            {
                g.add(s.steer, 0, s.speed);
                CornerVerdict v = cc.update(line, s.pct, s.speed, s.brake,
                                            s.throttle, s.steer, g.peakSteer());
                if (v.note.empty())
                    continue;
                ++total;
                if (v.note.find("off the brake sooner") != std::string::npos)
                    ++late;
            }
        printf("       %d notes, %d of them 'off the brake sooner'\n", total, late);
        check(late > 0, "carrying the brake past the reference is diagnosed");
    }

    printf("\n-- warnings are about the corner ahead --\n");
    {
        // A note about the corner just finished can only be filed away; the
        // point of remembering each corner's fault is to warn before arrival.
        std::vector<Sample> bad;
        for (const RefPoint &p : line.pts)
            bad.push_back({p.pct, p.speed * 0.9f, p.brake > 0.05f ? 1.0f : 0.0f,
                           p.throttle, 1.0f, p.steer});
        CornerCoach cc;
        GripCurve g;
        int ahead = 0, behind = 0, dupes = 0;
        std::string prev;
        for (int lap = 0; lap < 3; ++lap)
            for (const Sample &s : bad)
            {
                g.add(s.steer, 0, s.speed);
                CornerVerdict v = cc.update(line, s.pct, s.speed, s.brake,
                                            s.throttle, s.steer, g.peakSteer());
                if (v.note.empty())
                    continue;
                if (v.ahead) ++ahead; else ++behind;
                if (v.note == prev) ++dupes;
                prev = v.note;
            }
        printf("       %d ahead, %d retrospective, %d repeated back to back\n",
               ahead, behind, dupes);
        check(ahead > 0, "the coach warns about corners still to come");
        check(ahead > behind, "warnings outnumber post-mortems once a lap is known");
        check(dupes == 0, "the same note is never said twice running");
    }

    if (argc > 2)
    {
        for (int i = 2; i < argc; ++i)
        {
            printf("\n-- real lap: %s --\n", argv[i]);
            std::vector<Sample> drive = loadDrive(argv[i]);
            if (drive.size() < 500)
            {
                printf("       (no usable data)\n");
                continue;
            }
            Tally t = replay(line, drive, true);
            printf("       %d judged: %d clean, %d easier, %d trail, %d speed\n",
                   t.total, t.good, t.easier, t.trail, t.speed);
            check(t.total >= 6, "a full lap judged most corners");
        }
    }

    printf("\n%s (%d failures)\n", g_fail == 0 ? "ALL PASSED" : "FAILURES", g_fail);
    return g_fail == 0 ? 0 : 1;
}
