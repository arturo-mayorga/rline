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
        check(t.easier >= t.trail,
              "over-pressure is called before trail braking, not after");
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
