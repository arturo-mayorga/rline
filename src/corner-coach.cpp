#include "corner-coach.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <fstream>

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

    // Signed metres from b to a, taking the short way round the lap.
    float gapM(float a, float b, float length)
    {
        float d = a - b;
        if (d > 0.5f)
            d -= 1.0f;
        if (d < -0.5f)
            d += 1.0f;
        return d * length;
    }
}

void CornerCoach::reset()
{
    _acc.clear();
    _learnedPeak = -1;
    _lastPct = -1;
    // _lastFault deliberately survives: what a corner did to him last lap is
    // still true after a tow or a reset to pits.
}

int CornerCoach::currentCorner() const
{
    for (size_t i = 0; i < _acc.size(); ++i)
        if (_acc[i].active)
            return (int)i;
    return -1;
}

std::string CornerCoach::nameOf(const RefLine &line, int idx) const
{
    if (idx >= 0 && idx < (int)names.size() && !names[idx].empty())
        return names[idx];
    return std::string("Turn ") + ordinal(line.corners[idx].n);
}

bool CornerCoach::loadNames(const std::string &path)
{
    std::ifstream f(path.c_str());
    if (!f)
        return false;

    names.clear();
    std::string ln;
    while (std::getline(f, ln))
    {
        while (!ln.empty() && (ln.back() == '\r' || ln.back() == ' '))
            ln.pop_back();
        if (ln.empty() || ln[0] == '#')
            continue;
        names.push_back(ln);
    }
    return true;
}

int CornerCoach::nextActionable(const RefLine &line, float pct, int from) const
{
    // Spans overlap, so the corner after this one may already be under way -
    // warning about a corner already being driven is worse than saying
    // nothing. Take the first one still entirely ahead.
    const int n = (int)line.corners.size();
    for (int k = 1; k <= n; ++k)
    {
        const int j = (from + k) % n;
        if (!inSpan(pct, line.corners[j].pctEntry, line.corners[j].pctExit))
            return j;
    }
    return -1;
}

float CornerCoach::refCoastM(const RefLine &line, int idx) const
{
    const RefCorner &rc = line.corners[idx];
    if (rc.releasePct < 0)
        return -1.0f; // the reference does not brake here at all

    // First real throttle after the reference let the brake go, within the
    // corner. Searched forward from the release rather than from entry, or the
    // approach - which is flat out - answers immediately.
    for (const RefPoint &p : line.pts)
    {
        if (p.pct < rc.releasePct)
            continue;
        if (p.pct > rc.pctExit)
            break;
        if (p.throttle > coastThrottleOn)
            return gapM(p.pct, rc.releasePct, line.length);
    }
    // Brake off and never back on the throttle before the exit: the reference
    // carries the brake essentially to the apex, so its coast is nil.
    return 0.0f;
}

std::string CornerCoach::fault(const RefLine &line, int idx) const
{
    const RefCorner &rc = line.corners[idx];
    const Acc &a = _acc[idx];

    // 1. Dragging the brake past where the reference released it. Checked
    //    first because it is the cause and the rest are symptoms: pressure
    //    still on at turn-in is what makes the front push and the rear light.
    if (a.releasePct >= 0 && rc.releasePct >= 0)
    {
        if (gapM(a.releasePct, rc.releasePct, line.length) > releaseLateM)
            return "off the brake sooner";
    }

    // 2. Too much pressure, which produces overslowing. Telling a driver
    //    already at maximum pressure to brake longer just makes them slower.
    if (rc.peakBrake > 0.05f && a.peakBrake > rc.peakBrake + brakeOverPressure)
        return "easier on the brake";

    // 3. Freewheeling between the pedals. The corner is at the right speed and
    //    still losing time, because a stretch of it is spent neither braking
    //    nor accelerating.
    //
    //    Fenced hard, because this is the closest thing in the vocabulary to
    //    "trail the brake in further", which was removed for telling this
    //    driver to do more of his dominant fault:
    //
    //      - a LATE release is caught by check 1 above and told the opposite,
    //        so a brake-dragger can never reach this branch;
    //      - it additionally requires the release to be measurably EARLY, so
    //        being merely near the reference is not enough;
    //      - the corner must be one the reference genuinely brakes for;
    //      - and the excess coast must be large, not marginal.
    //
    //    The phrasing names the RATE of release, never its duration or its
    //    pressure, so it cannot be heard as "brake harder" or "brake longer" -
    //    which is how "still deep on the brake" went wrong in 2026-08-04.
    if (rc.peakBrake >= aeroPeakBrake && a.releasePct >= 0 && rc.releasePct >= 0 &&
        a.throttleOnPct >= 0)
    {
        const float refCoast = refCoastM(line, idx);
        // gapM(a, b) is metres from b to A - the order reads backwards and has
        // now produced three sign bugs in one evening. Throttle-on is AFTER the
        // release, so it goes first.
        const float hisCoast = gapM(a.throttleOnPct, a.releasePct, line.length);
        const float earlyM = gapM(rc.releasePct, a.releasePct, line.length);

        if (refCoast >= 0.0f && earlyM > releaseLateM &&
            hisCoast > refCoast + coastExcessM)
            return "ease the brake off gradually";
    }

    // 4. Past the driver's own measured grip peak the wheel has stopped
    //    working, so the instruction is to unwind - not to find more speed and
    //    not to compare against someone else's steering trace.
    if (_learnedPeak > 0.05f && a.peakSteer > _learnedPeak)
        return "too much lock, unwind";

    // 5. A plain speed deficit, and only once the wheel is known to be inside
    //    the grip peak. Asking for more speed while the front is already
    //    saturated is the advice that has made this driver slower before.
    //
    //    Never on a corner the reference barely brakes for. GripCurve indexes
    //    on steering *angle*, and in this car load rises with speed, not lock:
    //    Arrabbiata is 4.2 g at 251 km/h on 0.88 rad, so it reads as nowhere
    //    near the peak and this branch would fire on a turn taken flat. The
    //    reference's own brake trace is the honest test - a corner it takes on
    //    a trace of brake is aero-limited, and what is missing there is
    //    commitment and line, never entry speed. It catches Mugello's detected
    //    corner 3 (peak brake 0.13, a quarter of the lap, five fast turns) and
    //    Road America's Carousel, which is the corner that first exposed this.
    if (rc.peakBrake < aeroPeakBrake)
        return std::string();

    if (rc.vmin > 1.0f && a.pastTurnIn && a.vmin < 1e8f &&
        a.vmin < rc.vmin - vminDeficitMs)
    {
        if (rc.peakSteer > 0.05f && a.peakSteer > rc.peakSteer * steerExcess)
            return "turn in earlier";
        return "room for more speed";
    }

    return std::string();
}

CornerVerdict CornerCoach::update(const RefLine &line, float pct, float speed,
                                  float brake, float throttle, float steer,
                                  float learnedPeakSteer)
{
    CornerVerdict none;

    if (line.corners.empty())
        return none;

    if (_acc.size() != line.corners.size())
    {
        _acc.assign(line.corners.size(), Acc());
        _lastFault.assign(line.corners.size(), std::string());
        _raised.assign(line.corners.size(), false);
    }

    // A jump in track position means a tow, a reset or a new lap; whatever was
    // being measured is no longer a corner the driver drove.
    if (_lastPct >= 0)
    {
        float d = pct - _lastPct;
        if (d < -0.5f)
            d += 1.0f;
        if (d < 0 || d > 0.02f)
        {
            _acc.assign(line.corners.size(), Acc());
            _lastPct = pct;
            _learnedPeak = learnedPeakSteer;
            return none;
        }
    }
    _lastPct = pct;
    _learnedPeak = learnedPeakSteer;

    // Every corner whose span covers this sample accumulates, so overlapping
    // corners are each measured over the whole of their own span.
    int finished = -1;
    for (size_t i = 0; i < line.corners.size(); ++i)
    {
        const RefCorner &rc = line.corners[i];
        Acc &a = _acc[i];

        if (inSpan(pct, rc.pctEntry, rc.pctExit))
        {
            if (!a.active)
            {
                a = Acc();
                a.active = true;
                a.vmin = 1e9f;
            }
            // Each figure is measured over exactly the window RefCorner uses
            // for its counterpart, or the comparison is between two different
            // stretches of road. Brake and release run entry..apex; speed and
            // lock run turnIn..exit. Getting this wrong is not a rounding
            // error: Mugello's first corner runs on to within a few metres of
            // the braking for Materassi, so measuring its release all the way
            // to the exit picks up the *next* corner's brake and reports a
            // 460 m late release on a lap that is the reference itself.
            if (!a.pastTurnIn && inSpan(pct, rc.pctTurnIn, rc.pctExit))
                a.pastTurnIn = true;
            if (a.pastTurnIn)
            {
                a.vmin = std::min(a.vmin, speed);
                a.peakSteer = std::max(a.peakSteer, fabsf(steer));
            }
            if (!a.pastApex)
            {
                a.peakBrake = std::max(a.peakBrake, brake);
                if (brake > kBrakeOn)
                    a.releasePct = pct;
            }
            // Where the throttle came back after the brake let go. Only counted
            // once the brake is genuinely off, so a brake-and-throttle overlap
            // on the way in is not mistaken for the end of a coast.
            if (a.throttleOnPct < 0 && a.releasePct >= 0 && brake <= coastBrakeOff &&
                throttle > coastThrottleOn && pct > a.releasePct)
                a.throttleOnPct = pct;
            // Set after accumulating, so the apex sample itself is included -
            // the reference's loop is inclusive of the apex.
            if (!a.pastApex && inSpan(pct, rc.pctApex, rc.pctExit))
                a.pastApex = true;
        }
        else if (a.active)
        {
            // Only one verdict can be spoken per tick; the first corner to
            // close is the one just driven out of.
            if (finished < 0)
                finished = (int)i;
            a.active = false;
        }
    }

    if (finished < 0)
        return none;

    const std::string f = fault(line, finished);
    _lastFault[finished] = f;
    _acc[finished] = Acc();

    // The policy gate. Everything above it runs in every session, so the
    // measurement and the per-corner memory stay current even while the coach
    // is saying nothing. Only speaking is suppressed.
    if (policy == sessionpolicy::kSilent)
        return none;

    char buf[192];

    // Warn about what is coming, using what that corner did to him last time.
    const int j = nextActionable(line, pct, finished);
    if (j >= 0 && !_lastFault[j].empty() &&
        (policy == sessionpolicy::kFull || (j < (int)_raised.size() && _raised[j])))
    {
        CornerVerdict v;
        v.corner = line.corners[j].n;
        v.ahead = true;
        snprintf(buf, sizeof(buf), "%s next, %s",
                 nameOf(line, j).c_str(), _lastFault[j].c_str());
        v.note = buf;
        // Two corners closing in quick succession can both point at the same
        // one ahead. Hearing it twice reads as a stutter, not as emphasis.
        if (v.note == _lastSpoken)
            return none;
        _lastSpoken = v.note;
        if (j < (int)_raised.size())
            _raised[j] = true;
        return v;
    }

    // Nothing known about the corner ahead yet - first lap, or it was clean.
    CornerVerdict v;
    v.corner = line.corners[finished].n;
    if (f.empty())
    {
        v.good = true;
        return v;
    }
    // Retrospective, and therefore first-time by definition - the feed-forward
    // path above is the one that fires for a corner already known. So this is
    // exactly the "raising a new corner" case that kConfirm exists to refuse.
    if (policy != sessionpolicy::kFull)
        return none;

    snprintf(buf, sizeof(buf), "%s, %s", nameOf(line, finished).c_str(), f.c_str());
    v.note = buf;
    if (v.note == _lastSpoken)
        return none;
    _lastSpoken = v.note;
    _raised[finished] = true;
    return v;
}
