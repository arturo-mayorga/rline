#ifndef corner_coach_h_
#define corner_coach_h_

#include "refline.h"

#include <string>

// Judges each corner the moment the car is through it, against what the
// reference driver did in the same corner.
//
// Runs entirely on the rig from data already on disk, so a call arrives while
// the corner is still fresh rather than a lap later. Portable and testable: no
// Windows, no audio, no network - it only decides what to say.
//
// The order of the checks matters. Braking too hard is diagnosed before
// releasing too early, because being told to carry the brake longer while
// already at full pressure produces overslowing - which is exactly what
// happened when the advice was given as a single per-lap instruction.
struct CornerVerdict
{
    int corner = 0;
    std::string note;   // empty when there is nothing worth saying
    bool good = false;  // clean corner, nothing to fix
};

class CornerCoach
{
public:
    // Tolerances. Anything inside these is treated as a corner driven well.
    float brakeOverPressure = 0.08f; // above the reference's peak, 0..1
    float releaseEarlyM = 15.0f;     // brake off this far before the reference
    float vminDeficitMs = 2.2f;      // ~8 km/h under the reference
    float steerExcess = 1.15f;       // multiple of the reference's peak lock
    float throttleLateM = 20.0f;

    // Feed every telemetry sample. Returns a verdict on the tick the car
    // leaves a corner, otherwise an empty one.
    CornerVerdict update(const RefLine &line, float pct, float speed,
                         float brake, float throttle, float steer,
                         float learnedPeakSteer);

    void reset();

    int currentCorner() const { return _corner; }

private:
    int _corner = -1;      // index into line.corners, -1 when between corners
    float _vmin = 0;       // measured from turn-in, not from entry
    bool _pastTurnIn = false;
    float _peakBrake = 0;
    float _peakSteer = 0;
    float _learnedPeak = -1;
    float _releasePct = -1;
    float _lastPct = -1;

    CornerVerdict judge(const RefLine &line, int idx) const;
};

#endif
