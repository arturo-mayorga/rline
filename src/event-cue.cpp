#include "event-cue.h"

#include <algorithm>
#include <cmath>

void EventCue::reset()
{
    _targetPct = -1;
    _fired = 0;
}

int EventCue::update(const RefLine &line, float pct, float speed, float dt)
{
    // A long gap is a pause or a teleport, not driving.
    if (!line.valid() || dt <= 0 || dt > 0.5f)
        return 0;

    // Nearest event of this kind ahead. Once passed, distanceAhead wraps to
    // nearly a full lap, so a passed event drops out of range on its own.
    float bestD = -1;
    float bestPct = -1;
    for (const RefEvent &e : line.events)
    {
        if (e.kind != kind)
            continue;
        const float d = distanceAhead(line, pct, e.pct);
        if (d > maxLookM)
            continue;
        if (bestD < 0 || d < bestD)
        {
            bestD = d;
            bestPct = e.pct;
        }
    }

    if (bestD < 0)
    {
        _targetPct = -1;
        _fired = 0;
        return 0;
    }

    if (speed < minSpeed)
        return 0;

    const float timeToGo = bestD / speed;
    const float fullCountdown = (float)(beeps - 1) * intervalS;

    if (bestPct != _targetPct)
    {
        _targetPct = bestPct;

        // Coming into range with less time left than the countdown needs would
        // fire every beep on consecutive ticks - a burst rather than a rhythm.
        // Better to stay silent for this corner than to mislead.
        _fired = (timeToGo < fullCountdown) ? beeps : 0;
    }

    const int k = _fired + 1;
    if (k > beeps)
        return 0;

    // Beep k is due once we would cross its threshold before the next tick, so
    // the final beep lands on the point rather than just after it.
    const float threshold = (float)(beeps - k) * intervalS;
    if (timeToGo - dt <= threshold)
    {
        _fired = k;
        return k;
    }

    return 0;
}
