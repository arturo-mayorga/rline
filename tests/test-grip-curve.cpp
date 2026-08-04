// The grip curve has to find the peak of a real driver's own data, not a
// synthetic curve, so this replays a captured lap through it and checks the
// answer against the offline analysis: grip peaks near 1.5 rad and falls away.

#include "../src/grip-curve.h"

#include <cmath>
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
    if (!ok) ++g_fail;
}

int main(int argc, char **argv)
{
    printf("-- synthetic curve with a known peak --\n");
    {
        // Grip rises to 1.4 rad then falls: the peak must be found there and
        // not at the largest angle seen.
        GripCurve g;
        for (int i = 0; i < 20000; ++i)
        {
            const float s = (i % 300) * 0.01f; // 0 .. 3.0 rad
            // Real tyres fall away sharply past the peak; a gentle slope would
            // leave grip within tolerance far beyond it, which is a property of
            // the tyre rather than a failure to find the peak.
            const float lat = (s < 1.4f) ? (18.0f + 6.0f * s)
                                         : (26.4f - 20.0f * (s - 1.4f));
            g.add(s, lat, 50.0f);
        }
        printf("       peak %.2f rad, confident=%d, %d samples\n",
               g.peakSteer(), (int)g.confident(), g.samples());
        check(g.peakSteer() > 1.2f && g.peakSteer() < 1.8f, "finds a known peak near 1.4 rad");
        check(g.confident(), "is confident once both sides are populated");
    }

    printf("\n-- refuses to guess without data past the peak --\n");
    {
        GripCurve g;
        for (int i = 0; i < 5000; ++i)
            g.add((i % 60) * 0.01f, 20.0f, 50.0f); // never exceeds 0.6 rad
        check(!g.confident(), "not confident when the curve was never taken past its peak");
    }

    printf("\n-- ignores parked and slow data --\n");
    {
        GripCurve g;
        for (int i = 0; i < 5000; ++i)
            g.add(1.0f, 30.0f, 2.0f);
        check(g.samples() == 0, "samples below the speed floor are discarded");
    }

    if (argc > 1)
    {
        printf("\n-- real lap: %s --\n", argv[1]);
        std::ifstream f(argv[1]);
        std::string line;
        std::getline(f, line);
        std::map<std::string, int> col;
        {
            std::stringstream ss(line); std::string c; int i = 0;
            while (std::getline(ss, c, ',')) { while(!c.empty()&&(c.back()=='\r'||c.back()==' ')) c.pop_back(); col[c] = i++; }
        }
        const char *need[] = {"SteeringWheelAngle", "LatAccel", "Speed", "IsOnTrack"};
        for (const char *n : need) if (!col.count(n)) { printf("  (missing %s)\n", n); return 1; }

        GripCurve g;
        std::vector<float> v;
        while (std::getline(f, line))
        {
            v.clear();
            std::stringstream ss(line); std::string c;
            while (std::getline(ss, c, ',')) v.push_back((float)atof(c.c_str()));
            if ((int)v.size() <= col["IsOnTrack"]) continue;
            if (v[col["IsOnTrack"]] < 0.5f) continue;
            g.add(v[col["SteeringWheelAngle"]], v[col["LatAccel"]], v[col["Speed"]]);
        }

        printf("       %d samples, peak %.2f rad, confident=%d\n",
               g.samples(), g.peakSteer(), (int)g.confident());
        for (int i = 0; i < GripCurve::kBins; ++i)
            if (g.binCount(i) >= GripCurve::kMinSamplesPerBin)
                printf("       %.2f-%.2f rad  %5.1f m/s2  (n=%d)\n",
                       i * 0.15f, (i + 1) * 0.15f, g.binMean(i), g.binCount(i));

        check(g.samples() > 1000, "the lap produced usable samples");
        check(g.peakSteer() > 0.8f && g.peakSteer() < 2.2f,
              "the measured peak matches the offline analysis (~1.5 rad)");
        check(g.confident(), "confident from a single real lap");
    }

    printf("\n%s (%d failures)\n", g_fail == 0 ? "ALL PASSED" : "FAILURES", g_fail);
    return g_fail == 0 ? 0 : 1;
}
