#include "refline.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>

namespace
{
    const double kPi = 3.14159265358979323846;

    // Index of the nearest reference point either side of which we search for
    // the true nearest segment. At ~1 m sample spacing this is a +/- 45 m
    // window, wide enough to absorb pct jitter and narrow enough that a
    // hairpin's other side can't win.
    const int kSearchWindow = 40;
}

// Shortest signed angle, in (-pi, pi].
float wrapPi(float a)
{
    const float twoPi = 2.0f * 3.14159265358979f;
    a = fmodf(a + 3.14159265358979f, twoPi);
    if (a < 0)
        a += twoPi;
    return a - 3.14159265358979f;
}

namespace
{

    std::vector<std::string> splitLine(const std::string &line)
    {
        std::vector<std::string> out;
        std::string cur;
        for (char c : line)
        {
            if (c == ',')
            {
                out.push_back(cur);
                cur.clear();
            }
            else if (c != '\r')
            {
                cur.push_back(c);
            }
        }
        out.push_back(cur);
        return out;
    }

    std::string trimLower(const std::string &s)
    {
        size_t b = s.find_first_not_of(" \t");
        if (b == std::string::npos)
            return "";
        size_t e = s.find_last_not_of(" \t");
        std::string t = s.substr(b, e - b + 1);
        for (char &c : t)
            c = (char)tolower((unsigned char)c);
        return t;
    }

    int findColumn(const std::vector<std::string> &header, const char *name)
    {
        std::string want = trimLower(name);
        for (size_t i = 0; i < header.size(); ++i)
        {
            if (trimLower(header[i]) == want)
                return (int)i;
        }
        return -1;
    }
}

bool loadRefLineCsv(const std::string &path, RefLine &out, std::string *err)
{
    std::ifstream f(path.c_str(), std::ios::binary);
    if (!f)
    {
        if (err)
            *err = "cannot open " + path;
        return false;
    }
    std::ostringstream ss;
    ss << f.rdbuf();
    return parseRefLineCsv(ss.str(), out, err);
}

bool parseRefLineCsv(const std::string &text, RefLine &out, std::string *err)
{
    out = RefLine();

    std::istringstream in(text);
    std::string line;

    if (!std::getline(in, line))
    {
        if (err)
            *err = "empty file";
        return false;
    }

    std::vector<std::string> header = splitLine(line);
    const int cLat = findColumn(header, "Lat");
    const int cLon = findColumn(header, "Lon");
    const int cPct = findColumn(header, "LapDistPct");
    const int cSpd = findColumn(header, "Speed");

    // Optional: present in an iRacing export, absent from a bare path log.
    const int cBrk = findColumn(header, "Brake");
    const int cStr = findColumn(header, "SteeringWheelAngle");
    const int cThr = findColumn(header, "Throttle");

    if (cLat < 0 || cLon < 0 || cPct < 0 || cSpd < 0)
    {
        if (err)
            *err = "missing a required column (need Lat, Lon, LapDistPct, Speed)";
        return false;
    }

    int needed = std::max(std::max(cLat, cLon), std::max(cPct, cSpd));
    needed = std::max(needed, std::max(cBrk, cStr));
    needed = std::max(needed, cThr);

    out.hasBrake = (cBrk >= 0);
    out.hasSteer = (cStr >= 0);

    struct Raw
    {
        double lat, lon;
        float pct, speed, brake, steer, throttle;
    };
    std::vector<Raw> raw;
    raw.reserve(8192);

    double sumLat = 0, sumLon = 0;

    while (std::getline(in, line))
    {
        if (line.empty() || line == "\r")
            continue;

        std::vector<std::string> f = splitLine(line);
        if ((int)f.size() <= needed)
            continue; // ragged row, skip rather than fail the whole lap

        Raw r;
        r.lat = atof(f[cLat].c_str());
        r.lon = atof(f[cLon].c_str());
        r.pct = (float)atof(f[cPct].c_str());
        r.speed = (float)atof(f[cSpd].c_str());
        r.brake = (cBrk >= 0) ? (float)atof(f[cBrk].c_str()) : 0.0f;
        r.steer = (cStr >= 0) ? (float)atof(f[cStr].c_str()) : 0.0f;
        r.throttle = (cThr >= 0) ? (float)atof(f[cThr].c_str()) : 0.0f;

        // iRacing reports 0,0 before the car is placed on track.
        if (r.lat == 0.0 && r.lon == 0.0)
            continue;

        sumLat += r.lat;
        sumLon += r.lon;
        raw.push_back(r);
    }

    if (raw.size() < 2)
    {
        if (err)
            *err = "fewer than 2 usable samples";
        return false;
    }

    out.lat0 = sumLat / (double)raw.size();
    out.lon0 = sumLon / (double)raw.size();

    // Metres per degree at this latitude (WGS84 approximation).
    const double latRad = out.lat0 * kPi / 180.0;
    out.mPerLat = 111132.92 - 559.82 * cos(2 * latRad) + 1.175 * cos(4 * latRad);
    out.mPerLon = 111412.84 * cos(latRad) - 93.5 * cos(3 * latRad);

    out.pts.reserve(raw.size());
    for (const Raw &r : raw)
    {
        RefPoint p;
        p.pct = r.pct;
        p.speed = r.speed;
        p.brake = r.brake;
        p.steer = r.steer;
        p.throttle = r.throttle;
        p.t = 0;
        p.x = (float)((r.lon - out.lon0) * out.mPerLon);
        p.y = (float)((r.lat - out.lat0) * out.mPerLat);
        out.pts.push_back(p);
    }

    // The log may start anywhere in the lap, so order by track position. This
    // also puts the single wrap in the right place.
    std::sort(out.pts.begin(), out.pts.end(),
              [](const RefPoint &a, const RefPoint &b) { return a.pct < b.pct; });

    // Drop duplicate pct values; they would make interpolation divide by zero.
    out.pts.erase(std::unique(out.pts.begin(), out.pts.end(),
                              [](const RefPoint &a, const RefPoint &b)
                              { return a.pct == b.pct; }),
                  out.pts.end());

    if (out.pts.size() < 2)
    {
        if (err)
            *err = "fewer than 2 distinct track positions";
        return false;
    }

    // Elapsed time by integrating distance/speed. Independent of the log rate,
    // so this works for a 30 Hz export as well as a 60 Hz one.
    out.minX = out.maxX = out.pts[0].x;
    out.minY = out.maxY = out.pts[0].y;

    float t = 0, len = 0;
    for (size_t i = 0; i < out.pts.size(); ++i)
    {
        RefPoint &p = out.pts[i];
        if (i > 0)
        {
            const RefPoint &q = out.pts[i - 1];
            const float d = sqrtf((p.x - q.x) * (p.x - q.x) + (p.y - q.y) * (p.y - q.y));
            len += d;
            const float v = std::max(q.speed, 1.0f);
            t += d / v;
        }
        p.t = t;

        out.minX = std::min(out.minX, p.x);
        out.maxX = std::max(out.maxX, p.x);
        out.minY = std::min(out.minY, p.y);
        out.maxY = std::max(out.maxY, p.y);
    }

    // Direction of travel at each point, from a centred difference so the
    // bearing is the tangent through the point rather than the forward chord.
    {
        const int m = (int)out.pts.size();
        for (int i = 0; i < m; ++i)
        {
            const RefPoint &p = out.pts[((i - 1) % m + m) % m];
            const RefPoint &q = out.pts[(i + 1) % m];
            out.pts[i].bearing = atan2f(q.x - p.x, q.y - p.y);
        }
    }

    // Close the loop back to the start for a whole-lap time and length.
    {
        const RefPoint &a = out.pts.back();
        const RefPoint &b = out.pts.front();
        const float d = sqrtf((b.x - a.x) * (b.x - a.x) + (b.y - a.y) * (b.y - a.y));
        len += d;
        t += d / std::max(a.speed, 1.0f);
    }

    out.lapTime = t;
    out.length = len;

    detectRefEvents(out);
    return true;
}

// Finds each corner from the steering trace, then locates the driver's brake
// point, turn-in and apex within it. Thresholds are relative to the lap's own
// maximum steering angle so this works for any car without tuning.
void detectRefEvents(RefLine &out)
{
    out.events.clear();
    if (!out.hasSteer || out.pts.size() < 32)
        return;

    const int n = (int)out.pts.size();

    float steerMax = 0;
    for (const RefPoint &p : out.pts)
        steerMax = std::max(steerMax, fabsf(p.steer));
    if (steerMax < 1e-3f)
        return;

    const float cornerEnter = 0.22f * steerMax; // "we are turning"
    const float cornerExit = 0.12f * steerMax;  // hysteresis, so wobble on a
                                                // straight cannot open a corner
    const float kMinCornerM = 20.0f;   // shorter runs are noise, not a corner
    const float kMergeGapM = 40.0f;    // a brief unwind mid-corner is one corner
    const float kBrakeLookbackM = 320.0f;
    const float kBrakeOn = 0.12f;

    // Cumulative distance, for thresholds expressed in metres.
    std::vector<float> s(n, 0.0f);
    for (int i = 1; i < n; ++i)
    {
        const RefPoint &a = out.pts[i - 1];
        const RefPoint &b = out.pts[i];
        s[i] = s[i - 1] + sqrtf((b.x - a.x) * (b.x - a.x) + (b.y - a.y) * (b.y - a.y));
    }

    // Corner spans, with hysteresis.
    std::vector<std::pair<int, int>> spans;
    bool inCorner = false;
    int start = 0;
    for (int i = 0; i < n; ++i)
    {
        const float a = fabsf(out.pts[i].steer);
        if (!inCorner && a > cornerEnter)
        {
            inCorner = true;
            start = i;
        }
        else if (inCorner && a < cornerExit)
        {
            inCorner = false;
            spans.push_back({start, i});
        }
    }
    if (inCorner)
        spans.push_back({start, n - 1});

    // Merge spans separated by only a short straight.
    std::vector<std::pair<int, int>> merged;
    for (const auto &sp : spans)
    {
        if (!merged.empty() && s[sp.first] - s[merged.back().second] < kMergeGapM)
            merged.back().second = sp.second;
        else
            merged.push_back(sp);
    }

    int cornerNo = 0;
    int prevApex = -1; // a brake point cannot precede the previous corner's apex
    for (const auto &sp : merged)
    {
        if (s[sp.second] - s[sp.first] < kMinCornerM)
            continue;

        ++cornerNo;

        // Apex: peak steering. The driver starts unwinding after this, which is
        // exactly the cue asked for.
        int apex = sp.first;
        for (int i = sp.first; i <= sp.second; ++i)
        {
            if (fabsf(out.pts[i].steer) > fabsf(out.pts[apex].steer))
                apex = i;
        }

        const int turnIn = sp.first;

        // Brake point: the last time the brake came on before turn-in, within a
        // sensible lookback. Stopping at the previous apex prevents two corners
        // in a sequence from both claiming the same braking event. Corners
        // taken flat simply have none.
        int brake = -1;
        for (int i = turnIn; i > 0 && i > prevApex + 1; --i)
        {
            if (s[turnIn] - s[i] > kBrakeLookbackM)
                break;
            if (out.pts[i].brake > kBrakeOn && out.pts[i - 1].brake <= kBrakeOn)
            {
                brake = i;
                break;
            }
        }

        auto push = [&](int idx, RefEventKind kind)
        {
            RefEvent e;
            e.pct = out.pts[idx].pct;
            e.x = out.pts[idx].x;
            e.y = out.pts[idx].y;
            e.kind = kind;
            e.corner = cornerNo;
            out.events.push_back(e);
        };

        prevApex = apex;

        if (brake >= 0 && out.hasBrake)
            push(brake, RefEventKind::Brake);
        push(turnIn, RefEventKind::TurnIn);
        push(apex, RefEventKind::Apex);
    }

    std::sort(out.events.begin(), out.events.end(),
              [](const RefEvent &a, const RefEvent &b) { return a.pct < b.pct; });

    // Per-corner reference behaviour, so the rig can compare a corner the
    // moment the driver is through it.
    out.corners.clear();
    for (const auto &sp : merged)
    {
        if (s[sp.second] - s[sp.first] < kMinCornerM)
            continue;

        RefCorner rc;
        rc.n = (int)out.corners.size() + 1;

        int apex = sp.first;
        for (int i = sp.first; i <= sp.second; ++i)
            if (fabsf(out.pts[i].steer) > fabsf(out.pts[apex].steer))
                apex = i;

        // Entry reaches back far enough to include the whole braking zone.
        int entry = sp.first;
        while (entry > 0 && s[sp.first] - s[entry] < kBrakeLookbackM)
            --entry;

        rc.pctEntry = out.pts[entry].pct;
        rc.pctTurnIn = out.pts[sp.first].pct;
        rc.pctApex = out.pts[apex].pct;
        rc.pctExit = out.pts[sp.second].pct;

        rc.vmin = out.pts[sp.first].speed;
        for (int i = sp.first; i <= sp.second; ++i)
        {
            rc.vmin = std::min(rc.vmin, out.pts[i].speed);
            rc.peakSteer = std::max(rc.peakSteer, fabsf(out.pts[i].steer));
        }

        for (int i = entry; i <= apex; ++i)
        {
            rc.peakBrake = std::max(rc.peakBrake, out.pts[i].brake);
            if (out.pts[i].brake > kBrakeOn)
                rc.releasePct = out.pts[i].pct;
        }

        for (int i = apex; i < n; ++i)
        {
            if (out.pts[i].throttle > 0.95f)
            {
                rc.fullThrottlePct = out.pts[i].pct;
                break;
            }
        }

        out.corners.push_back(rc);
    }
}

float distanceAhead(const RefLine &line, float fromPct, float toPct)
{
    if (!line.valid())
        return 0;
    float d = toPct - fromPct;
    if (d < 0)
        d += 1.0f; // wrapped past start/finish
    return d * line.length;
}

std::vector<RefEvent> eventsNear(const RefLine &line, float pct,
                                 float aheadM, float behindM)
{
    std::vector<RefEvent> out;
    for (const RefEvent &e : line.events)
    {
        const float ahead = distanceAhead(line, pct, e.pct);
        if (ahead <= aheadM || (line.length - ahead) <= behindM)
            out.push_back(e);
    }

    // Nearest ahead first, so callers can label the next one.
    std::sort(out.begin(), out.end(),
              [&](const RefEvent &a, const RefEvent &b)
              { return distanceAhead(line, pct, a.pct) < distanceAhead(line, pct, b.pct); });
    return out;
}

void projectLatLon(const RefLine &line, double lat, double lon, float &x, float &y)
{
    x = (float)((lon - line.lon0) * line.mPerLon);
    y = (float)((lat - line.lat0) * line.mPerLat);
}

RefCompare compareToRefLine(const RefLine &line, float pct, float x, float y, float speed)
{
    RefCompare out;
    if (!line.valid())
        return out;

    const int n = (int)line.pts.size();

    // Seed from track position: first point at or past pct.
    int seed = (int)(std::lower_bound(line.pts.begin(), line.pts.end(), pct,
                                      [](const RefPoint &p, float v) { return p.pct < v; }) -
                     line.pts.begin());
    if (seed >= n)
        seed = 0;

    // Nearest segment within a window around the seed. Segment i joins point i
    // to point i+1, wrapping at the lap boundary.
    float bestDist2 = -1;
    int bestSeg = -1;
    float bestS = 0; // position along the winning segment, 0..1

    for (int k = -kSearchWindow; k <= kSearchWindow; ++k)
    {
        int i = seed + k;
        i = ((i % n) + n) % n;
        const int j = (i + 1) % n;

        const RefPoint &a = line.pts[i];
        const RefPoint &b = line.pts[j];

        const float dx = b.x - a.x;
        const float dy = b.y - a.y;
        const float len2 = dx * dx + dy * dy;
        if (len2 <= 1e-9f)
            continue;

        float s = ((x - a.x) * dx + (y - a.y) * dy) / len2;
        s = std::max(0.0f, std::min(1.0f, s));

        const float px = a.x + s * dx;
        const float py = a.y + s * dy;
        const float d2 = (x - px) * (x - px) + (y - py) * (y - py);

        if (bestDist2 < 0 || d2 < bestDist2)
        {
            bestDist2 = d2;
            bestSeg = i;
            bestS = s;
        }
    }

    if (bestSeg < 0)
        return out;

    const RefPoint &a = line.pts[bestSeg];
    const RefPoint &b = line.pts[(bestSeg + 1) % n];

    const float dx = b.x - a.x;
    const float dy = b.y - a.y;
    const float len = sqrtf(dx * dx + dy * dy);

    // Cross product of travel direction with the offset to the car. Negative
    // when the car sits to the right of the direction of travel, so negate to
    // make "right of the line" positive.
    const float cross = dx * (y - a.y) - dy * (x - a.x);
    out.crossTrack = (len > 1e-6f) ? (-cross / len) : 0.0f;

    out.idx = (bestS < 0.5f) ? bestSeg : (bestSeg + 1) % n;
    out.refSpeed = a.speed + bestS * (b.speed - a.speed);

    // Guard the lap-boundary segment, where t jumps back to zero.
    out.refTime = (b.t >= a.t) ? (a.t + bestS * (b.t - a.t)) : a.t;

    out.speedDelta = speed - out.refSpeed;
    return out;
}

RefSample refAt(const RefLine &line, float pct)
{
    RefSample s;
    if (!line.valid())
        return s;

    const int n = (int)line.pts.size();

    int i = (int)(std::lower_bound(line.pts.begin(), line.pts.end(), pct,
                                   [](const RefPoint &p, float v) { return p.pct < v; }) -
                  line.pts.begin());
    if (i >= n)
        i = 0;

    const int h = ((i - 1) % n + n) % n;
    const RefPoint &a = line.pts[h];
    const RefPoint &b = line.pts[i];

    // Fraction between the bracketing samples, guarding the lap wrap where pct
    // jumps from ~1 back to ~0.
    float f = 0;
    const float span = b.pct - a.pct;
    if (span > 1e-9f)
        f = std::max(0.0f, std::min(1.0f, (pct - a.pct) / span));

    s.idx = (f < 0.5f) ? h : i;
    s.x = a.x + f * (b.x - a.x);
    s.y = a.y + f * (b.y - a.y);
    s.speed = a.speed + f * (b.speed - a.speed);
    s.t = (b.t >= a.t) ? (a.t + f * (b.t - a.t)) : a.t;

    // Interpolate the bearing the short way round the circle.
    const float db = wrapPi(b.bearing - a.bearing);
    s.bearing = a.bearing + f * db;

    return s;
}

float LateralEstimator::update(const RefLine &line, float pct, float speed,
                               float bearingVel, float dt)
{
    if (!line.valid() || dt <= 0 || dt > 0.5f)
        return d; // a long gap means a pause or a teleport, not real motion

    const RefSample s = refAt(line, pct);
    const float dpsi = wrapPi(bearingVel - s.bearing);

    d += speed * sinf(dpsi) * dt;

    // The car cannot be further from the racing line than the track is wide.
    // Clamping stops a bad patch of integration running away.
    const float kMaxOffset = 25.0f;
    d = std::max(-kMaxOffset, std::min(kMaxOffset, d));

    primed = true;
    return d;
}

void positionFrom(const RefLine &line, float pct, float lateral, float &x, float &y)
{
    const RefSample s = refAt(line, pct);
    // Right of travel for a bearing B is (cos B, -sin B) in (east, north).
    x = s.x + lateral * cosf(s.bearing);
    y = s.y - lateral * sinf(s.bearing);
}

void MapFit::toScreen(float x, float y, int &px, int &py) const
{
    px = (int)lroundf(offX + x * scale);
    py = (int)lroundf(offY - y * scale);
}

MapFit fitMap(const RefLine &line, int w, int h, int pad)
{
    MapFit fit;
    if (!line.valid())
        return fit;

    const float spanX = std::max(line.maxX - line.minX, 1.0f);
    const float spanY = std::max(line.maxY - line.minY, 1.0f);

    const float availW = (float)std::max(w - 2 * pad, 1);
    const float availH = (float)std::max(h - 2 * pad, 1);

    fit.scale = std::min(availW / spanX, availH / spanY);

    // Centre the track's bounding box in the box.
    const float midX = 0.5f * (line.minX + line.maxX);
    const float midY = 0.5f * (line.minY + line.maxY);

    fit.offX = 0.5f * (float)w - midX * fit.scale;
    fit.offY = 0.5f * (float)h + midY * fit.scale;

    return fit;
}
