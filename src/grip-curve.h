#ifndef grip_curve_h_
#define grip_curve_h_

// Learns where this driver's front grip peaks, from their own driving.
//
// Cornering force rises with steering angle up to the tyre's peak slip angle
// and falls away past it. Beyond that peak, more lock produces less grip - so
// the instinct to add steering when the car will not turn is exactly wrong.
//
// The peak is a property of the car and the driver, not the circuit, so unlike
// everything else in this project this needs no reference lap and works on any
// track from the first few corners.
//
// Portable: no Windows, no telemetry API, so the binning can be tested against
// a captured lap.
class GripCurve
{
public:
    static const int kBins = 24;
    static const int kMinSamplesPerBin = 40;

    // Grip within this fraction of the measured best still counts as usable;
    // below it, the extra lock is costing more than it buys.
    static constexpr float kUsableFraction = 0.92f;

    float binWidth = 0.15f;  // radians per bin
    float minSpeed = 15.0f;  // m/s; parked cars teach nothing

    void add(float steerRad, float latAccel, float speed);

    // Steering angle beyond which measured grip is falling. Negative until
    // enough of the curve has been seen to be worth believing.
    float peakSteer() const;

    // True once the peak is supported by data either side of it.
    bool confident() const;

    void reset();

    int samples() const { return _total; }
    float binMean(int i) const;
    int binCount(int i) const { return (i >= 0 && i < kBins) ? _count[i] : 0; }

private:
    float smoothed(int i) const;
    bool analyse(float &thresholdOut, bool &fallOffSeen) const;

    double _sum[kBins] = {};
    int _count[kBins] = {};
    int _total = 0;
};

#endif
