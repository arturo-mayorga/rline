#include "corner-trace.h"

#include <algorithm>

namespace
{
    // Same threshold the corner coach uses to call the brake "on", so the
    // release marker on the panel and the spoken verdict never disagree.
    const float kBrakeOn = 0.12f;

    // History must reach at least this close to the corner's entry, in bins,
    // for the trace to be worth showing.
    const int kEntrySlack = 3;

    bool inSpan(float pct, float a, float b)
    {
        if (a <= b)
            return pct >= a && pct <= b;
        return pct >= a || pct <= b; // wraps past start/finish
    }

    float spanLength(float a, float b)
    {
        return (b >= a) ? (b - a) : (b + 1.0f - a);
    }

    // Whether x lies in the interval (a, b] travelled forwards, wrap included.
    bool crossed(float a, float b, float x)
    {
        if (a <= b)
            return x > a && x <= b;
        return x > a || x <= b;
    }

    // Both traces are binned the same way - hardest pressure anywhere in the
    // bin - so the panel compares like with like. Point-sampling the reference
    // at bin centres instead would step over its peaks and flatter the driver
    // by however much the pedal moved between two samples.
    void binInto(const RefCorner &rc, float pct, float brake,
                 float *bins, bool *seen)
    {
        if (!inSpan(pct, rc.pctEntry, rc.pctExit))
            return;

        int b = (int)(spanFraction(pct, rc.pctEntry, rc.pctExit) *
                      (float)CornerTrace::kBins);
        if (b < 0)
            b = 0;
        if (b >= CornerTrace::kBins)
            b = CornerTrace::kBins - 1;

        if (!seen[b] || brake > bins[b])
            bins[b] = brake;
        seen[b] = true;
    }

    int firstSeen(const bool *seen)
    {
        for (int i = 0; i < CornerTrace::kBins; ++i)
            if (seen[i])
                return i;
        return CornerTrace::kBins;
    }

    // Hold the last seen value across bins the sampler missed. That only
    // happens where the car was quick, and holding is far closer than zero - a
    // dropout to zero would read as a pedal lift that never happened.
    void fillHolding(const float *bins, const bool *seen, int first, float *out)
    {
        float held = bins[first];
        for (int i = 0; i < CornerTrace::kBins; ++i)
        {
            if (seen[i])
                held = bins[i];
            out[i] = held;
        }
    }
}

float spanFraction(float pct, float a, float b)
{
    const float len = spanLength(a, b);
    if (len <= 1e-6f)
        return 0.0f;
    float d = pct - a;
    if (d < 0)
        d += 1.0f;
    return d / len;
}

void CornerTrace::clear()
{
    corner = 0;
    valid = false;
    youPeak = refPeak = 0;
    youRelease = refRelease = -1;
    for (int i = 0; i < kBins; ++i)
    {
        you[i] = 0;
        ref[i] = 0;
    }
}

void CornerExitWatcher::reset()
{
    _lastPct = -1;
}

int CornerExitWatcher::update(const RefLine &line, float pct)
{
    if (line.corners.empty())
        return -1;

    if (_lastPct < 0)
    {
        _lastPct = pct;
        return -1;
    }

    float d = pct - _lastPct;
    if (d < -0.5f)
        d += 1.0f;

    // A tow, a reset to pits or a jump in a replay moves the car without it
    // being driven there, so nothing either side of the jump is a corner.
    if (d < 0 || d > 0.02f)
    {
        _lastPct = pct;
        return -1;
    }

    const float prev = _lastPct;
    _lastPct = pct;

    for (size_t i = 0; i < line.corners.size(); ++i)
        if (crossed(prev, pct, line.corners[i].pctExit))
            return (int)i;

    return -1;
}

bool buildCornerTrace(const RefLine &line, int idx,
                      const PedalPoint *hist, int n, CornerTrace &out)
{
    if (idx < 0 || idx >= (int)line.corners.size() || !hist || n <= 0)
        return false;

    const RefCorner &rc = line.corners[idx];
    const float span = spanLength(rc.pctEntry, rc.pctExit);
    if (span <= 1e-6f)
        return false;

    float bins[CornerTrace::kBins];
    bool seen[CornerTrace::kBins];
    float rbins[CornerTrace::kBins];
    bool rseen[CornerTrace::kBins];
    for (int i = 0; i < CornerTrace::kBins; ++i)
    {
        bins[i] = 0;
        seen[i] = false;
        rbins[i] = 0;
        rseen[i] = false;
    }

    // A lap takes far longer than the history window, so at most one pass
    // through this corner can be in the buffer; every sample inside the span
    // belongs to the corner just driven.
    for (int i = 0; i < n; ++i)
        binInto(rc, hist[i].pct, hist[i].brake, bins, seen);

    const int first = firstSeen(seen);

    // Either the car was never in this corner, or the buffer does not reach
    // back to its entry. Showing the tail alone would read as a corner taken
    // without any brake at all.
    if (first > kEntrySlack)
        return false;

    for (size_t i = 0; i < line.pts.size(); ++i)
        binInto(rc, line.pts[i].pct, line.pts[i].brake, rbins, rseen);

    CornerTrace t;
    t.corner = rc.n;

    fillHolding(bins, seen, first, t.you);

    const int rfirst = firstSeen(rseen);
    if (rfirst < CornerTrace::kBins)
        fillHolding(rbins, rseen, rfirst, t.ref);

    for (int i = 0; i < CornerTrace::kBins; ++i)
    {
        t.youPeak = std::max(t.youPeak, t.you[i]);
        t.refPeak = std::max(t.refPeak, t.ref[i]);
        if (t.you[i] > kBrakeOn)
            t.youRelease = i;
        if (t.ref[i] > kBrakeOn)
            t.refRelease = i;
    }

    t.valid = true;
    out = t;
    return true;
}
