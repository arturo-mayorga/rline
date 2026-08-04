#include "grip-curve.h"

#include <cmath>

void GripCurve::reset()
{
    for (int i = 0; i < kBins; ++i)
    {
        _sum[i] = 0;
        _count[i] = 0;
    }
    _total = 0;
}

void GripCurve::add(float steerRad, float latAccel, float speed)
{
    if (speed < minSpeed)
        return;

    const float a = fabsf(steerRad);
    const int b = (int)(a / binWidth);
    if (b < 0 || b >= kBins)
        return;

    _sum[b] += fabsf(latAccel);
    ++_count[b];
    ++_total;
}

float GripCurve::binMean(int i) const
{
    if (i < 0 || i >= kBins || _count[i] == 0)
        return 0.0f;
    return (float)(_sum[i] / _count[i]);
}

// A single bin's mean is noisy enough that the raw maximum lands on whichever
// bin got lucky. Smoothing over neighbours recovers the shape of the curve.
float GripCurve::smoothed(int i) const
{
    double sum = 0;
    int n = 0;
    for (int k = i - 1; k <= i + 1; ++k)
    {
        if (k < 0 || k >= kBins || _count[k] < kMinSamplesPerBin)
            continue;
        sum += binMean(k);
        ++n;
    }
    return n ? (float)(sum / n) : 0.0f;
}

bool GripCurve::analyse(float &thresholdOut, bool &fallOffSeen) const
{
    float best = 0;
    int bestBin = -1;
    for (int i = 0; i < kBins; ++i)
    {
        if (_count[i] < kMinSamplesPerBin)
            continue;
        const float m = smoothed(i);
        if (bestBin < 0 || m > best)
        {
            best = m;
            bestBin = i;
        }
    }
    if (bestBin < 0 || best <= 0)
        return false;

    // The useful number is not where grip is highest, but the most lock that
    // still produces most of it - past there, turning the wheel further is
    // costing grip rather than buying it.
    const float floorMean = best * kUsableFraction;

    int last = bestBin;
    for (int i = bestBin; i < kBins; ++i)
        if (_count[i] >= kMinSamplesPerBin && smoothed(i) >= floorMean)
            last = i;

    thresholdOut = (last + 1) * binWidth;

    // Evidence of fall-off: a populated bin beyond the threshold that is
    // measurably worse. Without one, the driver simply never steered that far.
    fallOffSeen = false;
    for (int i = last + 1; i < kBins; ++i)
        if (_count[i] >= kMinSamplesPerBin && smoothed(i) < floorMean)
            fallOffSeen = true;

    return true;
}

float GripCurve::peakSteer() const
{
    float t = 0;
    bool fall = false;
    return analyse(t, fall) ? t : -1.0f;
}

bool GripCurve::confident() const
{
    float t = 0;
    bool fall = false;
    return analyse(t, fall) && fall;
}
