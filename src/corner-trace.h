#ifndef corner_trace_h_
#define corner_trace_h_

// A finished corner's brake application, frozen so it can be read on the way
// out. Deliberately free of Windows headers, like refline and corner-coach, so
// the binning can be built and tested on any platform.

#include "refline.h"

// Sampled into fixed bins by track position rather than by time, so the
// driver's trace and the reference's line up on a common axis however
// differently the two were driven. Time binning would slide the two apart
// exactly when they differ most, which is the case worth looking at.
struct CornerTrace
{
    static const int kBins = 96;

    int corner = 0;    // 1-based, matching RefCorner::n; 0 = nothing captured
    bool valid = false;

    float you[kBins];
    float ref[kBins];

    float youPeak = 0, refPeak = 0;

    // Bin where each trace finally let the brake go, -1 when it never came on.
    // The release point is the thing this panel exists to show: understeer here
    // comes from letting the pedal go square rather than tapering it.
    int youRelease = -1, refRelease = -1;

    CornerTrace() { clear(); }
    void clear();
};

// One sample of recent pedal history, oldest first.
struct PedalPoint
{
    float pct;
    float brake;
};

// Notices the tick on which the car crosses a corner's exit.
class CornerExitWatcher
{
public:
    // Returns the index into line.corners of the corner just completed, or -1.
    int update(const RefLine &line, float pct);
    void reset();

private:
    float _lastPct = -1;
};

// Builds the frozen trace for one corner out of recent pedal history.
//
// Reconstructing from history rather than accumulating live is what makes
// overlapping corners work, and on this reference lap several do overlap -
// corner 5 opens 300 m before corner 4 closes. A running capture can only be
// inside one corner at a time, so it would pick up the later corner only once
// the earlier one released it, missing the whole braking zone that the panel
// exists to show.
//
// Returns false when history does not reach back to the corner's entry, so a
// partial corner is never frozen as though it were the whole thing.
bool buildCornerTrace(const RefLine &line, int idx,
                      const PedalPoint *hist, int n, CornerTrace &out);

// Fraction of the way from a to b around the lap, handling the wrap past
// start/finish. Exposed for testing.
float spanFraction(float pct, float a, float b);

#endif
