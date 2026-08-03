#ifndef overlay_comp_h_
#define overlay_comp_h_

#include "../ecs.h"
#include "../refline.h"

#include <memory>
#include <string>
#include <vector>

// Live state of the player's car. Only the ego car: this tool never looks at
// the CarIdx* arrays that a broadcast overlay would need.
struct EgoStateComponent
{
    ECS_DECLARE_TYPE;

    bool connected = false;
    bool onTrack = false;

    float pct = 0;   // LapDistPct
    float speed = 0; // m/s
    int lap = -1;

    // Metres right of the reference line. iRacing publishes no absolute
    // position live, so this is dead reckoned; see LateralEstimator.
    float lateral = 0;

    // Position implied by pct and lateral, in the reference line's frame, so
    // the overlay can work in absolute metres as if it had been measured.
    float x = 0;
    float y = 0;
};
ECS_DEFINE_TYPE(EgoStateComponent);
typedef std::shared_ptr<EgoStateComponent> EgoStateComponentSP;

// The loaded reference lap.
struct RefLineComponent
{
    ECS_DECLARE_TYPE;

    RefLine line;
    bool loaded = false;
    std::string error;
};
ECS_DEFINE_TYPE(RefLineComponent);
typedef std::shared_ptr<RefLineComponent> RefLineComponentSP;

struct OverlayConfigComponent
{
    ECS_DECLARE_TYPE;

    bool mph = false;

    // Car-centred, track-up view. A whole-lap map is useless for this job:
    // 6.4 km across a 360 px box is ~18 m per pixel, so a 2 m line error would
    // be a tenth of a pixel.
    float forwardM = 120.0f; // reference line drawn ahead of the car
    float behindM = 40.0f;   // and behind it

    // Lateral distance is drawn at this multiple of the longitudinal scale, so
    // a couple of metres off line is legible at a glance instead of ~4 px.
    float lateralExaggeration = 3.0f;

    float barRangeM = 5.0f; // full-scale deflection of the cross-track bar

    // Show the braking countdown once the next brake point is this close.
    float brakeCueM = 160.0f;

    // Cross-track thresholds for the green / amber / red colouring, in metres.
    float goodM = 0.5f;
    float okM = 1.5f;

    // Past this the reconstruction cannot be describing a car on the track, so
    // the readout blanks rather than reporting a confident wrong number.
    float implausibleM = 30.0f;
};
ECS_DEFINE_TYPE(OverlayConfigComponent);
typedef std::shared_ptr<OverlayConfigComponent> OverlayConfigComponentSP;

#endif
