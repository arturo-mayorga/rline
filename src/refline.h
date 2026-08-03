#ifndef refline_h_
#define refline_h_

// Reference-lap geometry. Deliberately free of Windows headers so the math can
// be built and tested on any platform; everything else in this project is Win32.

#include <string>
#include <vector>

struct RefPoint
{
    float pct;     // LapDistPct, 0..1, ascending
    float x;       // local metres, +east of origin
    float y;       // local metres, +north of origin
    float speed;   // m/s
    float t;       // seconds since the lap's first sample
    float bearing; // direction of travel, radians clockwise from north
    float brake;   // 0..1, zero when the CSV has no Brake column
    float steer;   // radians, zero when the CSV has no steering column
};

// Where the reference driver braked, turned in and clipped the apex. Derived
// from the reference lap and indexed by track position, so unlike the lateral
// offset these do not depend on any dead reckoning - they are exact.
enum class RefEventKind
{
    Brake,
    TurnIn,
    Apex
};

struct RefEvent
{
    float pct = 0;
    float x = 0, y = 0;
    RefEventKind kind = RefEventKind::Brake;
    int corner = 0; // corners numbered in lap order from 1
};

struct RefLine
{
    std::vector<RefPoint> pts;      // sorted by pct
    std::vector<RefEvent> events;   // sorted by pct
    bool hasBrake = false;          // whether the CSV carried the channels
    bool hasSteer = false;

    // Local tangent-plane projection origin. Good to a few cm over a track.
    double lat0 = 0, lon0 = 0;
    double mPerLat = 0, mPerLon = 0;

    float minX = 0, maxX = 0, minY = 0, maxY = 0;
    float lapTime = 0; // seconds
    float length = 0;  // metres

    bool valid() const { return pts.size() >= 2; }
};

// How the live car sits against the reference lap.
struct RefCompare
{
    int idx = -1;          // nearest reference point
    float crossTrack = 0;  // signed metres, + = car is right of the line
    float refSpeed = 0;    // reference speed here, m/s
    float refTime = 0;     // reference elapsed time here, s
    float speedDelta = 0;  // live speed - reference speed, m/s
};

// Parses a telemetry CSV. Columns are located by header name, so column order
// and extra channels don't matter. Requires Lat, Lon, LapDistPct and Speed.
// Returns false and fills err on failure.
bool loadRefLineCsv(const std::string &path, RefLine &out, std::string *err);

// Same parser over an in-memory buffer, so callers can embed the lap.
bool parseRefLineCsv(const std::string &text, RefLine &out, std::string *err);

// Populates line.events. Called by the parser; exposed for testing.
void detectRefEvents(RefLine &line);

// Shortest signed angle, in (-pi, pi].
float wrapPi(float a);

// World position -> local metres, using the line's projection origin.
void projectLatLon(const RefLine &line, double lat, double lon, float &x, float &y);

// The reference lap interpolated at a track position.
struct RefSample
{
    float x = 0, y = 0;
    float bearing = 0;
    float speed = 0;
    float t = 0;
    int idx = -1;
};

RefSample refAt(const RefLine &line, float pct);

// Distance along the reference line from pct to an event ahead of the car,
// in metres. Handles the lap wrap.
float distanceAhead(const RefLine &line, float fromPct, float toPct);

// Events within `aheadM` in front of the car (and `behindM` behind it),
// in the order they will be reached.
std::vector<RefEvent> eventsNear(const RefLine &line, float pct,
                                 float aheadM, float behindM);

// Recovers lateral offset from the reference line without absolute position,
// which iRacing does not publish live. Integrates the Frenet relation
// dd/dt = V*sin(psi_vel - psi_ref), so it is dead reckoning: exact in
// principle, but accumulating error in practice. Reset it every lap to bound
// the growth.
struct LateralEstimator
{
    float d = 0;       // metres right of the reference line
    bool primed = false;

    void reset() { d = 0; }

    // bearingVel is the direction the car is actually travelling (its heading
    // plus slip), radians clockwise from north. dt in seconds.
    float update(const RefLine &line, float pct, float speed, float bearingVel, float dt);
};

// Position implied by a track position and a lateral offset. Lets the rest of
// the overlay keep working in absolute metres.
void positionFrom(const RefLine &line, float pct, float lateral, float &x, float &y);

// Compares a live sample against the reference. pct seeds the search so the
// result can't snap to a physically-near but unrelated part of the track.
RefCompare compareToRefLine(const RefLine &line, float pct, float x, float y, float speed);

// Uniform scale + offset that fits the line's bounds into a w*h box with the
// given padding, preserving aspect. Used for the top-down map.
struct MapFit
{
    float scale = 1; // metres -> pixels
    float offX = 0;
    float offY = 0;

    // Local metres -> pixel coordinates (y flipped: north is up on screen).
    void toScreen(float x, float y, int &px, int &py) const;
};

MapFit fitMap(const RefLine &line, int w, int h, int pad);

#endif
