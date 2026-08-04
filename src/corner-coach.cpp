#include "corner-coach.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace
{
    const float kBrakeOn = 0.12f;

    // Corner numbers are spoken, so they read as words rather than digits.
    const char *ordinal(int n)
    {
        static const char *names[] = {"one", "two", "three", "four", "five", "six",
                                      "seven", "eight", "nine", "ten", "eleven",
                                      "twelve", "thirteen", "fourteen"};
        if (n >= 1 && n <= 14)
            return names[n - 1];
        return "";
    }

    bool inSpan(float pct, float a, float b)
    {
        if (a <= b)
            return pct >= a && pct <= b;
        return pct >= a || pct <= b; // wraps past start/finish
    }
}

void CornerCoach::reset()
{
    _corner = -1;
    _vmin = 0;
    _pastTurnIn = false;
    _peakBrake = 0;
    _peakSteer = 0;
    _releasePct = -1;
    _lastPct = -1;
}

CornerVerdict CornerCoach::judge(const RefLine &line, int idx) const
{
    const RefCorner &rc = line.corners[idx];
    CornerVerdict v;
    v.corner = rc.n;

    char buf[160];

    // Too much brake pressure comes first: telling a driver who is already at
    // maximum pressure to brake for longer just makes them slower.
    if (rc.peakBrake > 0.05f && _peakBrake > rc.peakBrake + brakeOverPressure)
    {
        snprintf(buf, sizeof(buf), "Turn %s, easier on the brake", ordinal(rc.n));
        v.note = buf;
        return v;
    }

    if (_releasePct >= 0 && rc.releasePct >= 0)
    {
        float d = (_releasePct - rc.releasePct);
        if (d < -0.5f)
            d += 1.0f;
        const float metres = d * line.length;
        if (metres < -releaseEarlyM)
        {
            snprintf(buf, sizeof(buf), "Turn %s, trail the brake in further",
                     ordinal(rc.n));
            v.note = buf;
            return v;
        }
    }

    // Slow through a corner is a symptom, not an instruction. If it comes with
    // more lock than the reference needed, the car is understeering, and
    // telling a driver already at the limit of front grip to carry more speed
    // just asks for more of what is not working.
    if (rc.vmin > 1.0f && _pastTurnIn && _vmin < 1e8f && _vmin < rc.vmin - vminDeficitMs)
    {
        // Past the driver's own measured grip peak the wheel has stopped
        // working, so the instruction is to unwind - not to find more speed
        // and not to compare against someone else's steering trace.
        if (_learnedPeak > 0.05f && _peakSteer > _learnedPeak)
            snprintf(buf, sizeof(buf), "Turn %s, too much lock, unwind to make it turn",
                     ordinal(rc.n));
        else if (rc.peakSteer > 0.05f && _peakSteer > rc.peakSteer * steerExcess)
            snprintf(buf, sizeof(buf), "Turn %s, understeer, get it rotated on entry",
                     ordinal(rc.n));
        else
            snprintf(buf, sizeof(buf), "Turn %s, room for more speed, %.0f down",
                     ordinal(rc.n), (rc.vmin - _vmin) * 3.6f);
        v.note = buf;
        return v;
    }

    v.good = true;
    return v;
}

CornerVerdict CornerCoach::update(const RefLine &line, float pct, float speed,
                                  float brake, float throttle, float steer,
                                  float learnedPeakSteer)
{
    CornerVerdict none;

    if (line.corners.empty())
        return none;

    // A jump in track position means a tow, a reset or a new lap; whatever was
    // being measured is no longer a corner the driver drove.
    if (_lastPct >= 0)
    {
        float d = pct - _lastPct;
        if (d < -0.5f)
            d += 1.0f;
        if (d < 0 || d > 0.02f)
        {
            const int was = _corner;
            reset();
            _lastPct = pct;
            if (was >= 0)
                return none;
        }
    }
    _lastPct = pct;
    _learnedPeak = learnedPeakSteer;

    if (_corner < 0)
    {
        for (size_t i = 0; i < line.corners.size(); ++i)
        {
            if (inSpan(pct, line.corners[i].pctEntry, line.corners[i].pctExit))
            {
                _corner = (int)i;
                _pastTurnIn = inSpan(pct, line.corners[i].pctTurnIn,
                                     line.corners[i].pctExit);
                _vmin = _pastTurnIn ? speed : 1e9f;
                _peakBrake = brake;
                _peakSteer = fabsf(steer);
                _releasePct = (brake > kBrakeOn) ? pct : -1.0f;
                return none;
            }
        }
        return none;
    }

    const RefCorner &rc = line.corners[_corner];

    if (inSpan(pct, rc.pctEntry, rc.pctExit))
    {
        // Brake pressure is judged over the whole approach, but speed only
        // from turn-in - otherwise the previous corner's minimum leaks in.
        if (!_pastTurnIn && inSpan(pct, rc.pctTurnIn, rc.pctExit))
            _pastTurnIn = true;
        if (_pastTurnIn)
            _vmin = std::min(_vmin, speed);
        _peakBrake = std::max(_peakBrake, brake);
        _peakSteer = std::max(_peakSteer, fabsf(steer));
        if (brake > kBrakeOn)
            _releasePct = pct;
        return none;
    }

    // Left the corner: judge it now, while it still means something.
    const CornerVerdict v = judge(line, _corner);
    const int done = _corner;
    reset();
    _lastPct = pct;
    (void)done;
    return v;
}
