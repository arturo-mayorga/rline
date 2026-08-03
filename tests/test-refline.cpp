// Platform-independent tests for the reference-line math.
// Build on any machine:  g++ -std=c++17 -O2 tests/test-refline.cpp src/refline.cpp -o test-refline

#include "../src/refline.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>

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
    printf("%s  %s (got %.4f, want %.4f +/- %.4f)\n",
           ok ? "  ok  " : "  FAIL", what, got, want, tol);
    if (!ok)
        ++g_fail;
}

int main(int argc, char **argv)
{
    const std::string path = (argc > 1) ? argv[1] : "data/lap.csv";

    RefLine line;
    std::string err;
    if (!loadRefLineCsv(path, line, &err))
    {
        printf("FAILED TO LOAD %s: %s\n", path.c_str(), err.c_str());
        return 1;
    }

    printf("loaded %zu points, %.1f m, %.2f s, origin %.6f/%.6f\n",
           line.pts.size(), line.length, line.lapTime, line.lat0, line.lon0);
    printf("bounds x [%.1f, %.1f]  y [%.1f, %.1f]\n\n",
           line.minX, line.maxX, line.minY, line.maxY);

    printf("-- parse --\n");
    check(line.valid(), "line is valid");
    check(line.pts.size() > 5000, "kept most of the 6004 samples");
    checkNear(line.length, 6411.0f, 20.0f, "lap length matches the analysed path");
    checkNear(line.lapTime, 100.0f, 2.0f, "lap time reconstructs to ~100 s");

    printf("\n-- ordering --\n");
    bool ascending = true;
    for (size_t i = 1; i < line.pts.size(); ++i)
    {
        if (line.pts[i].pct <= line.pts[i - 1].pct)
            ascending = false;
    }
    check(ascending, "points are strictly ascending in LapDistPct");
    check(line.pts.front().pct >= 0.0f && line.pts.back().pct <= 1.0f,
          "LapDistPct stays within 0..1");

    printf("\n-- projection round trip --\n");
    {
        // Reproject a known point through lat/lon and back.
        const size_t i = line.pts.size() / 3;
        const double lat = line.lat0 + line.pts[i].y / line.mPerLat;
        const double lon = line.lon0 + line.pts[i].x / line.mPerLon;
        float x = 0, y = 0;
        projectLatLon(line, lat, lon, x, y);
        checkNear(x, line.pts[i].x, 0.01f, "x round trips to within 1 cm");
        checkNear(y, line.pts[i].y, 0.01f, "y round trips to within 1 cm");
    }

    printf("\n-- cross-track error --\n");
    {
        // A car sitting exactly on the reference line has zero error.
        float worst = 0;
        for (size_t i = 0; i < line.pts.size(); i += 37)
        {
            const RefPoint &p = line.pts[i];
            RefCompare c = compareToRefLine(line, p.pct, p.x, p.y, p.speed);
            worst = std::max(worst, fabsf(c.crossTrack));
        }
        checkNear(worst, 0.0f, 0.01f, "on-line samples report < 1 cm error");
    }

    {
        // Displace known points perpendicular to travel and confirm we recover
        // both the magnitude and the sign. + is right of the direction of travel.
        float worstRight = 0, worstLeft = 0;
        int tested = 0;

        // Index 0/1 straddle the lap seam: sorted by pct they are the log's
        // last and first samples, i.e. two different passes through start /
        // finish, 0.63 m apart. That kink is in the data, not the maths, and
        // it is bounded separately below.
        for (size_t i = 2; i + 1 < line.pts.size(); i += 53)
        {
            const RefPoint &a = line.pts[i];
            // Centred difference: the direction of travel through a, rather
            // than the forward chord, which is tilted by half the turn angle.
            const RefPoint &prev = line.pts[i - 1];
            const RefPoint &next = line.pts[i + 1];
            const float dx = next.x - prev.x, dy = next.y - prev.y;
            const float len = sqrtf(dx * dx + dy * dy);
            if (len < 0.1f)
                continue;

            // Unit vector pointing right of travel.
            const float rx = dy / len, ry = -dx / len;

            RefCompare cr = compareToRefLine(line, a.pct, a.x + 3.0f * rx, a.y + 3.0f * ry, a.speed);
            RefCompare cl = compareToRefLine(line, a.pct, a.x - 2.0f * rx, a.y - 2.0f * ry, a.speed);

            worstRight = std::max(worstRight, fabsf(cr.crossTrack - 3.0f));
            worstLeft = std::max(worstLeft, fabsf(cl.crossTrack + 2.0f));
            ++tested;
        }

        printf("       (%d displaced samples tested)\n", tested);
        check(tested > 50, "enough displaced samples were tested");
        // 2 cm allows for the polyline chords sitting just inside the true arc
        // through a corner. Measured worst case is a few mm.
        checkNear(worstRight, 0.0f, 0.02f, "+3 m right recovers as +3 m");
        checkNear(worstLeft, 0.0f, 0.02f, "-2 m left recovers as -2 m");
    }

    {
        // The seam is inherent to a single-lap log. Pin its size so a future
        // change cannot let it grow unnoticed.
        float worst = 0;
        for (size_t i = 0; i < 2 && i + 1 < line.pts.size(); ++i)
        {
            const RefPoint &a = line.pts[i];
            const RefPoint &b = line.pts[i + 1];
            const float dx = b.x - a.x, dy = b.y - a.y;
            const float len = sqrtf(dx * dx + dy * dy);
            if (len < 0.1f)
                continue;
            RefCompare c = compareToRefLine(line, a.pct,
                                            a.x + 3.0f * (dy / len),
                                            a.y - 3.0f * (dx / len), a.speed);
            worst = std::max(worst, fabsf(c.crossTrack - 3.0f));
        }
        printf("       (seam error %.3f m, over %.2f%% of the lap)\n",
               worst, 100.0f * 2.0f / (float)line.pts.size());
        check(worst < 1.0f, "lap-seam artefact stays under 1 m");
    }

    printf("\n-- reference lookup --\n");
    {
        const size_t i = line.pts.size() / 2;
        const RefPoint &p = line.pts[i];
        RefCompare c = compareToRefLine(line, p.pct, p.x, p.y, p.speed + 5.0f);
        checkNear(c.refSpeed, p.speed, 0.5f, "reference speed matches at the same pct");
        checkNear(c.speedDelta, 5.0f, 0.5f, "speed delta is live minus reference");
        checkNear(c.refTime, p.t, 0.1f, "reference time matches at the same pct");
        check(c.idx >= 0, "a nearest index was returned");
    }

    {
        // Monotonic time is what makes a delta readout meaningful.
        bool monotonic = true;
        for (size_t i = 1; i < line.pts.size(); ++i)
        {
            if (line.pts[i].t < line.pts[i - 1].t)
                monotonic = false;
        }
        check(monotonic, "reference elapsed time increases along the lap");
    }

    printf("\n-- map fit --\n");
    {
        const int w = 320, h = 320, pad = 12;
        MapFit fit = fitMap(line, w, h, pad);
        check(fit.scale > 0, "scale is positive");

        int outside = 0;
        for (const RefPoint &p : line.pts)
        {
            int px = 0, py = 0;
            fit.toScreen(p.x, p.y, px, py);
            if (px < 0 || px > w || py < 0 || py > h)
                ++outside;
        }
        check(outside == 0, "every point fits inside the map box");

        // North must be up: a point further north maps to a smaller y.
        int px1, py1, px2, py2;
        fit.toScreen(0, 0, px1, py1);
        fit.toScreen(0, 100, px2, py2);
        check(py2 < py1, "north points up on screen");
    }

    printf("\n%s (%d failures)\n", g_fail == 0 ? "ALL PASSED" : "FAILURES", g_fail);
    return g_fail == 0 ? 0 : 1;
}
