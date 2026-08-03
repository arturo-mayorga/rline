#ifndef event_cue_h_
#define event_cue_h_

#include "refline.h"

// Schedules a countdown of beeps so the last one lands on a given kind of
// reference event - a braking point, a turn-in point.
//
// The beeps are spaced in time rather than distance on purpose: an evenly
// spaced countdown is anticipatory, so you predict the last beep from the ones
// before it and act in sync with it. A single tone on the point would instead
// cost a full reaction time - about 21 m at 300 km/h.
//
// Deliberately free of Windows and of any audio API, so the timing can be
// tested without a sound card.
struct EventCue
{
    RefEventKind kind = RefEventKind::Brake;

    int beeps = 3;
    float intervalS = 0.5f;  // spacing between beeps
    float maxLookM = 400.0f; // ignore events further away than this
    float minSpeed = 5.0f;   // no countdown while crawling

    // Returns 1..beeps when that beep is due this tick, otherwise 0.
    int update(const RefLine &line, float pct, float speed, float dt);

    void reset();

    float targetPct() const { return _targetPct; }
    int fired() const { return _fired; }

private:
    float _targetPct = -1;
    int _fired = 0;
};

#endif
