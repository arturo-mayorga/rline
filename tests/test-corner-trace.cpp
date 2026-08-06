// The frozen corner trace has to line the driver's brake application up
// against the reference's on a common axis. The check that catches every way
// that can go wrong - binning, the lap wrap, the resampling of the reference,
// and corners whose spans overlap - is to replay the reference lap through it
// and require that what comes out is the reference itself.

#include "../src/corner-trace.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
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
    const char *path = (argc > 1) ? argv[1] : "data/lap.csv";

    RefLine line;
    std::string err;
    if (!loadRefLineCsv(path, line, &err))
    {
        printf("cannot load %s: %s\n", path, err.c_str());
        return 1;
    }
    printf("-- reference: %d corners over %.0f m, brake=%d --\n",
           (int)line.corners.size(), line.length, (int)line.hasBrake);

    // The overlap this code exists to handle is a property of the reference
    // lap, so say out loud whether it is actually present.
    int overlaps = 0;
    for (size_t i = 1; i < line.corners.size(); ++i)
        if (line.corners[i].pctEntry < line.corners[i - 1].pctExit)
            ++overlaps;
    printf("       %d corners begin before their predecessor ends\n", overlaps);

    printf("-- span fraction --\n");
    check(fabsf(spanFraction(0.55f, 0.5f, 0.7f) - 0.25f) < 1e-4f,
          "quarter of the way through a plain span");
    check(fabsf(spanFraction(0.99f, 0.98f, 0.02f) - 0.25f) < 1e-4f,
          "before start/finish on a span that wraps");
    check(fabsf(spanFraction(0.01f, 0.98f, 0.02f) - 0.75f) < 1e-4f,
          "after start/finish on a span that wraps");

    printf("-- replaying the reference lap --\n");
    // 40 s of history at 60 Hz, exactly as the overlay keeps it.
    const int kHist = 2400;
    std::vector<PedalPoint> ring;

    CornerExitWatcher watch;
    std::vector<CornerTrace> got;

    // Two passes: a corner whose entry wraps past start/finish is already under
    // way when the replay begins, so only the second lap sees them all.
    for (int pass = 0; pass < 2; ++pass)
    {
        for (size_t i = 0; i < line.pts.size(); ++i)
        {
            ring.push_back(PedalPoint{line.pts[i].pct, line.pts[i].brake});
            if ((int)ring.size() > kHist)
                ring.erase(ring.begin());

            const int done = watch.update(line, line.pts[i].pct);
            if (done < 0)
                continue;

            CornerTrace t;
            const bool built = buildCornerTrace(line, done, ring.data(),
                                                (int)ring.size(), t);
            if (built && pass == 1)
                got.push_back(t);
        }
    }

    printf("       captured %d of %d corners\n",
           (int)got.size(), (int)line.corners.size());
    check(got.size() == line.corners.size(), "every corner is captured once");

    bool ordered = got.size() == line.corners.size();
    for (size_t i = 0; i < got.size() && i < line.corners.size(); ++i)
        if (got[i].corner != line.corners[i].n)
            ordered = false;
    check(ordered, "captured in lap order, numbered like the reference");

    float worst = 0;
    int worstCorner = 0;
    for (size_t c = 0; c < got.size(); ++c)
        for (int i = 0; i < CornerTrace::kBins; ++i)
        {
            const float d = fabsf(got[c].you[i] - got[c].ref[i]);
            if (d > worst)
            {
                worst = d;
                worstCorner = got[c].corner;
            }
        }
    printf("       worst bin disagreement %.3f (turn %d)\n", worst, worstCorner);
    // Not zero: the driver's trace keeps the hardest pressure in each bin while
    // the reference is sampled at the bin's centre, so a steep ramp differs by
    // whatever the pedal moved in one bin's width.
    check(worst < 0.2f, "driving the reference reproduces the reference");

    bool peaksAgree = true, releasesAgree = true;
    for (size_t c = 0; c < got.size(); ++c)
    {
        if (fabsf(got[c].youPeak - got[c].refPeak) > 0.05f)
            peaksAgree = false;
        if (abs(got[c].youRelease - got[c].refRelease) > 2)
            releasesAgree = false;
    }
    check(peaksAgree, "peak pressures agree");
    check(releasesAgree, "release points land in the same bin");

    for (size_t c = 0; c < got.size() && c < 3; ++c)
        printf("       turn %d: peak %.0f%% vs %.0f%%, release bin %d vs %d\n",
               got[c].corner, got[c].youPeak * 100.0f, got[c].refPeak * 100.0f,
               got[c].youRelease, got[c].refRelease);

    printf("-- a partial corner is refused, not shown --\n");
    {
        // History that only reaches the middle of the corner would draw as a
        // corner taken with no brake at all, which is worse than drawing
        // nothing.
        const RefCorner &rc = line.corners[0];
        std::vector<PedalPoint> partial;
        for (size_t i = 0; i < line.pts.size(); ++i)
        {
            const float f = spanFraction(line.pts[i].pct, rc.pctEntry, rc.pctExit);
            if (line.pts[i].pct >= rc.pctEntry && line.pts[i].pct <= rc.pctExit &&
                f > 0.5f)
                partial.push_back(PedalPoint{line.pts[i].pct, line.pts[i].brake});
        }
        CornerTrace t;
        const bool built = buildCornerTrace(line, 0, partial.data(),
                                            (int)partial.size(), t);
        check(!partial.empty(), "the partial history has samples in it");
        check(!built && !t.valid, "history short of the entry is refused");
    }

    printf("-- a jump in track position is not a corner exit --\n");
    {
        // A tow or a reset to pits moves the car without it being driven there.
        const RefCorner &rc = line.corners[0];
        CornerExitWatcher w;
        float p = rc.pctEntry;
        for (int k = 0; k < 6; ++k, p += 0.001f)
            w.update(line, p);

        // Land beyond the corner's exit in a single step.
        const int r = w.update(line, rc.pctExit + 0.01f);
        check(r < 0, "teleporting past the exit reports no corner");
    }

    printf("\n%s\n", g_fail ? "FAILED" : "all good");
    return g_fail ? 1 : 0;
}
