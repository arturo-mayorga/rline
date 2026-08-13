#ifndef corner_coach_h_
#define corner_coach_h_

#include "refline.h"
#include "session-policy.h"

#include <string>
#include <vector>

// Judges each corner the moment the car is through it, and uses what it learns
// to warn about the corner coming up.
//
// Runs entirely on the rig from data already on disk, so a call arrives while
// the corner is still fresh rather than a lap later. Portable and testable: no
// Windows, no audio, no network - it only decides what to say.
//
// Feed-forward by default. A note about the corner just finished can only be
// filed away; a note about the corner ahead can be acted on before arriving.
// So each corner's fault is remembered from the last time through, and spoken
// on the exit of the preceding corner. The retrospective form is only used on
// the first lap, when nothing is known about what is coming.
//
// The order of the checks matters, and it is ordered by what this driver
// actually does wrong rather than by what is most often said to a novice:
//
//  1. Dragging the brake - carrying light pressure 30-60 m past where the
//     reference released. This is his dominant fault, it is what makes the
//     front push, and on one occasion it put the car round. It is checked
//     first because every other symptom downstream is caused by it.
//  2. Too much pressure, which produces overslowing.
//  3. Too much lock, past the point where his own grip curve stops improving.
//  4. Only then, a plain speed deficit - and only when the wheel was inside
//     the grip peak, because "carry more speed" to a driver already at the
//     limit of front grip asks for more of what is not working. Never at all
//     on a corner the reference barely brakes for: those are aero-limited, the
//     grip curve cannot see it, and entry speed is not what is missing there.
//
// Two cues were deliberately removed. "Trail the brake in further" told him to
// do more of his worst habit. "Get it rotated on entry" is heard as rotating
// the car with the brake, which is the input that spun it.
struct CornerVerdict
{
    int corner = 0;     // the corner the note is about
    std::string note;   // empty when there is nothing worth saying
    bool good = false;  // clean corner, nothing to fix
    bool ahead = false; // the note is about a corner still to come
};

class CornerCoach
{
public:
    // Tolerances. Anything inside these is treated as a corner driven well.
    float brakeOverPressure = 0.08f; // above the reference's peak, 0..1
    float releaseLateM = 25.0f;      // brake still on this far past the reference
    float vminDeficitMs = 2.2f;      // ~8 km/h under the reference
    float steerExcess = 1.15f;       // multiple of the reference's peak lock

    // Below this reference peak brake a corner counts as aero-limited, and no
    // speed-deficit cue may be given for it at all. See check 4 in the .cpp:
    // the grip curve reads on lock, so a 250 km/h turn pulling 4.2 g on light
    // hands looks unthreatened when it is the most loaded corner on the lap.
    float aeroPeakBrake = 0.25f;

    // What each corner is called out loud - the whole subject phrase, so a
    // corner with a name rather than a number ("The carousel") reads properly.
    // Index i names line.corners[i]; empty falls back to the detected number.
    // Loaded from corner-names.txt, because the detector's numbering is not
    // the track's: it finds 11 corners across 16 numbered turns here, and
    // speaking its own numbering sends the driver to the wrong piece of road.
    std::vector<std::string> names;

    // How much may be spoken. Set from the session type each tick by
    // CornerCoachSystem, or pinned by a COACH|mode= line from the relay.
    //
    // Only *emission* is gated. The accumulators keep measuring and each
    // corner's remembered fault keeps updating whatever the mode, so coming out
    // of a race into practice has warm feed-forward state instead of a wasted
    // lap - and so the analysis machine still receives telemetry describing a
    // corner the rig chose not to mention.
    sessionpolicy::Mode policy = sessionpolicy::kFull;

    // Reads one label per line, skipping blanks and # comments. Returns false
    // if the file cannot be opened; a missing file is not an error, the coach
    // just falls back to detected numbers.
    bool loadNames(const std::string &path);

    // Feed every telemetry sample. Returns a verdict on the tick the car
    // leaves a corner, otherwise an empty one.
    CornerVerdict update(const RefLine &line, float pct, float speed,
                         float brake, float throttle, float steer,
                         float learnedPeakSteer);

    void reset();

    // Index of a corner currently being measured, or -1.
    int currentCorner() const;

private:
    // One of these per corner, all live at once. The reference lap's corner
    // spans overlap - 8 of 11 begin before their predecessor ends - so a
    // single "which corner am I in" index picks the later corner up only once
    // the earlier one releases it, well past its braking zone, and then judges
    // it on a fraction of the evidence.
    struct Acc
    {
        bool active = false;
        bool pastTurnIn = false;
        bool pastApex = false;
        float vmin = 0;
        float peakBrake = 0;
        float peakSteer = 0;
        float releasePct = -1;
    };

    std::vector<Acc> _acc;
    std::vector<std::string> _lastFault; // per corner, from the last time through
    // Corners this coach has already raised out loud. In kConfirm - the warmup,
    // and every out-lap - a corner that has never been mentioned may not be
    // mentioned now: ten minutes before a race is the worst moment to hand him
    // a corner he was not already working on.
    std::vector<bool> _raised;
    std::string _lastSpoken;             // to avoid saying the same thing twice running
    float _learnedPeak = -1;
    float _lastPct = -1;

    // What was wrong with this corner, as a spoken phrase. Empty when clean.
    std::string fault(const RefLine &line, int idx) const;

    // The next corner the driver has not already entered, so a warning about
    // it can still be acted on.
    int nextActionable(const RefLine &line, float pct, int from) const;

    std::string nameOf(const RefLine &line, int idx) const;
};

#endif
