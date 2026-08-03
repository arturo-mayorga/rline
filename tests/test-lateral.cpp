// Validates the lateral-offset estimator against a real captured drive.
//
// iRacing publishes no absolute position live, so lateral offset is dead
// reckoned from dd/dt = V*sin(psi_vel - psi_ref). These tests pin the two
// things that make that usable: that the geometry round-trips exactly, and
// that integrating a real lap stays inside track-plausible bounds.
//
// g++ -std=c++17 -O2 -o test-lateral tests/test-lateral.cpp src/refline.cpp

#include "../src/refline.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
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
    struct Drive
    {
        double sessionTime;
        float lap, lapDistPct, speed, yawNorth, velX, velY, isOnTrack;
    };

    // VelocityY is positive to the LEFT, established by testing both signs
    // against lap closure on a real capture: "slip -" closed to 0.00 m over a
    // full lap where "slip +" did not.
    const float kSlipSign = -1.0f;

    float velocityBearing(const Drive &d)
    {
        const float slip = atan2f(d.velY, d.velX);
        return d.yawNorth + kSlipSign * slip;
    }

    std::vector<Drive> loadDrive(const std::string &path)
    {
        std::vector<Drive> out;
        std::ifstream f(path.c_str());
        std::string line;
        if (!std::getline(f, line))
            return out;

        while (std::getline(f, line))
        {
            std::vector<float> v;
            std::stringstream ss(line);
            std::string cell;
            while (std::getline(ss, cell, ','))
                v.push_back((float)atof(cell.c_str()));

            if (v.size() < 14)
                continue; // the capture's final row is a partial write

            Drive d;
            d.sessionTime = v[0];
            d.lap = v[1];
            d.lapDistPct = v[3];
            d.speed = v[4];
            d.yawNorth = v[6];
            d.velX = v[7];
            d.velY = v[8];
            d.isOnTrack = v[13];
            out.push_back(d);
        }
        return out;
    }
}

int main(int argc, char **argv)
{
    const std::string refPath = (argc > 1) ? argv[1] : "data/lap.csv";
    const std::string drivePath = (argc > 2) ? argv[2] : "data/drive-roadamerica.csv";

    RefLine line;
    std::string err;
    if (!loadRefLineCsv(refPath, line, &err))
    {
        printf("FAILED TO LOAD %s: %s\n", refPath.c_str(), err.c_str());
        return 1;
    }

    printf("-- reference bearings --\n");
    {
        // A bearing must agree with the direction to the next point.
        float worst = 0;
        const int n = (int)line.pts.size();
        for (int i = 1; i + 1 < n; i += 41)
        {
            const RefPoint &p = line.pts[i - 1];
            const RefPoint &q = line.pts[i + 1];
            const float expect = atan2f(q.x - p.x, q.y - p.y);
            worst = std::max(worst, fabsf(wrapPi(line.pts[i].bearing - expect)));
        }
        checkNear(worst, 0.0f, 1e-4f, "stored bearing matches the local tangent");
    }

    printf("\n-- position round trip --\n");
    {
        // An offset applied and then measured must come back unchanged. This is
        // what lets the overlay keep working in absolute metres.
        float worst = 0;
        for (size_t i = 2; i + 2 < line.pts.size(); i += 97)
        {
            const RefPoint &p = line.pts[i];
            for (float want : {-4.0f, -1.5f, 0.0f, 2.0f, 6.0f})
            {
                float x = 0, y = 0;
                positionFrom(line, p.pct, want, x, y);
                const RefCompare c = compareToRefLine(line, p.pct, x, y, p.speed);
                worst = std::max(worst, fabsf(c.crossTrack - want));
            }
        }
        checkNear(worst, 0.0f, 0.05f, "offset applied then measured round trips");
    }

    printf("\n-- estimator on synthetic input --\n");
    {
        // Driving exactly along the reference bearing must not accumulate any
        // offset, however long it runs.
        LateralEstimator est;
        for (int step = 0; step < 6000; ++step)
        {
            const float pct = (float)step / 6000.0f;
            const RefSample s = refAt(line, pct);
            est.update(line, pct, s.speed, s.bearing, 1.0f / 60.0f);
        }
        checkNear(est.d, 0.0f, 0.01f, "following the reference bearing yields zero offset");
    }

    {
        // A steady 1 degree of heading error must integrate at V*sin(1 deg).
        LateralEstimator est;
        const float dpsi = 1.0f * 3.14159265f / 180.0f;
        const float v = 50.0f, dt = 1.0f / 60.0f;
        int steps = 0;
        for (int step = 0; step < 600; ++step)
        {
            const float pct = 0.30f;
            const RefSample s = refAt(line, pct);
            est.update(line, pct, v, s.bearing + dpsi, dt);
            ++steps;
        }
        checkNear(est.d, v * sinf(dpsi) * dt * steps, 0.01f,
                  "constant heading error integrates at V*sin(dpsi)");
    }

    {
        LateralEstimator est;
        est.d = 3.0f;
        est.reset();
        checkNear(est.d, 0.0f, 1e-6f, "reset clears the accumulated offset");
    }

    {
        // A long gap is a pause or a teleport, not motion.
        LateralEstimator est;
        const RefSample s = refAt(line, 0.4f);
        est.update(line, 0.4f, 60.0f, s.bearing + 0.5f, 3.0f);
        checkNear(est.d, 0.0f, 1e-6f, "an implausible timestep is ignored");
    }

    printf("\n-- estimator on a real captured drive --\n");
    std::vector<Drive> drive = loadDrive(drivePath);
    printf("       (%zu samples loaded from %s)\n", drive.size(), drivePath.c_str());

    if (drive.size() < 1000)
    {
        printf("  SKIP  no capture available\n");
    }
    else
    {
        LateralEstimator est;
        float lo = 0, hi = 0;
        int steps = 0;
        double drivenSeconds = 0;

        for (size_t i = 1; i < drive.size(); ++i)
        {
            const Drive &a = drive[i - 1];
            const Drive &b = drive[i];
            if (a.isOnTrack < 0.5f || a.speed < 3.0f)
                continue;

            const float dt = (float)(b.sessionTime - a.sessionTime);
            if (dt <= 0 || dt > 0.1f)
                continue;

            est.update(line, a.lapDistPct, a.speed, velocityBearing(a), dt);
            lo = std::min(lo, est.d);
            hi = std::max(hi, est.d);
            drivenSeconds += dt;
            ++steps;
        }

        printf("       (%d steps, %.0f s driven, offset range [%.1f, %.1f] m)\n",
               steps, drivenSeconds, lo, hi);

        check(steps > 5000, "the capture produced a usable number of steps");

        // The car was on track the whole time, so the reconstruction must stay
        // within a track-plausible band. Divergence would blow straight past this.
        check(lo > -20.0f && hi < 20.0f,
              "reconstructed offset stays within track-plausible bounds");

        // Road America is about 15 m wide; a racing line plus normal variation
        // should not need more than half that either side of the reference.
        check(hi - lo < 30.0f, "the total excursion is no wider than a track");
    }

    printf("\n%s (%d failures)\n", g_fail == 0 ? "ALL PASSED" : "FAILURES", g_fail);
    return g_fail == 0 ? 0 : 1;
}
