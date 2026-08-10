#ifndef overlay_comp_h_
#define overlay_comp_h_

#include "../corner-trace.h"
#include "../ecs.h"
#include "../grip-curve.h"
#include "../refline.h"
#include "../speech-queue.h"

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
    float brake = 0;    // 0..1
    float throttle = 0; // 0..1
    float steer = 0;    // radians
    float latAccel = 0; // m/s2

    // Learned from this driver in this car, so it needs no reference lap and
    // works on any track from the first few corners.
    GripCurve grip;
    int lap = -1;

    // Metres right of the reference line. iRacing publishes no absolute
    // position live, so this is dead reckoned; see LateralEstimator.
    float lateral = 0;

    // Position implied by pct and lateral, in the reference line's frame, so
    // the overlay can work in absolute metres as if it had been measured.
    float x = 0;
    float y = 0;

    // Recent pedal history, so the overlay can draw the shape of a brake
    // application rather than just its current value. A ring rather than a
    // growing buffer: this is written 60 times a second, forever.
    struct PedalSample
    {
        float pct = 0;
        float brake = 0;
        float throttle = 0;
    };
    // Long enough to hold the longest corner on the reference lap end to end,
    // because the frozen trace is rebuilt from this buffer rather than
    // accumulated live. Road America's longest span is ~820 m, which is over
    // 25 s if the car is slow through it; 10 s would silently truncate it.
    static const int kHistory = 2400; // 40 s at 60 Hz
    PedalSample hist[kHistory];
    int histCount = 0;
    int histHead = 0;

    void pushHistory(float p, float b, float t)
    {
        hist[histHead] = PedalSample{p, b, t};
        histHead = (histHead + 1) % kHistory;
        if (histCount < kHistory)
            ++histCount;
    }

    // The corner just completed, held until the next one finishes. The live
    // trace above it scrolls past too quickly to read while cornering; this is
    // what the driver actually glances at on the way out.
    CornerTrace lastCorner;
    CornerExitWatcher exitWatch;
    std::vector<PedalPoint> scratch;

    // Call once per sample, after pushHistory. Rebuilding from history rather
    // than capturing live is what lets overlapping corners each be measured
    // from their own entry.
    void updateCornerTrace(const RefLine &line)
    {
        const int done = exitWatch.update(line, pct);
        if (done < 0)
            return;

        scratch.clear();
        scratch.reserve((size_t)histCount);
        for (int k = 0; k < histCount; ++k)
        {
            const int i = (histHead - histCount + k + 2 * kHistory) % kHistory;
            scratch.push_back(PedalPoint{hist[i].pct, hist[i].brake});
        }
        buildCornerTrace(line, done, scratch.data(), (int)scratch.size(),
                         lastCorner);
    }
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

    // Identity of what is actually in use, for the BUILD line the rig sends
    // the relay on connection. Deliberately measured rather than configured:
    // the failure this guards against is a stale lap.csv sitting beside the
    // exe, and anything derived from build settings would report the intent
    // instead of the reality. `cornerNames` is published by CornerCoachSystem
    // once it has loaded them, so it is exactly what will be spoken aloud.
    std::string refHash;
    std::vector<std::string> cornerNames;
};
ECS_DEFINE_TYPE(RefLineComponent);
typedef std::shared_ptr<RefLineComponent> RefLineComponentSP;

// A coaching note pushed from the relay. Held here rather than in the stream
// system so the overlay and the speech system both read the same state.
struct CoachMessageComponent
{
    ECS_DECLARE_TYPE;

    std::string text;      // what to show
    float ttl = 0;         // seconds of display left
    bool speakPending = false; // set when a new note arrives, cleared once spoken
};
ECS_DEFINE_TYPE(CoachMessageComponent);
typedef std::shared_ptr<CoachMessageComponent> CoachMessageComponentSP;

// Everything waiting to be said, in the order it will be said.
//
// CoachMessageComponent above is now only what is on *screen*. Anything that
// wants to be heard goes in here instead, because two producers writing one
// slot meant the second cut the first off mid-sentence - or overwrote it before
// it was ever spoken. See src/speech-queue.h for the ordering rules.
struct SpeechQueueComponent
{
    ECS_DECLARE_TYPE;

    speech::Queue queue;
};
ECS_DEFINE_TYPE(SpeechQueueComponent);
typedef std::shared_ptr<SpeechQueueComponent> SpeechQueueComponentSP;

// The other direction: what the driver said, waiting to go up the wire.
//
// VoiceInputSystem fills this from the speech recogniser and
// TelemetryStreamSystem drains it. They are decoupled through a component so
// that recognition still works with the relay down - the queue simply drains
// to nothing - and so neither system has to know the other exists.
struct DriverSpeechComponent
{
    ECS_DECLARE_TYPE;

    // Bounded: if the relay is disconnected while the driver keeps talking,
    // the oldest utterances are dropped rather than growing without limit.
    // Nothing said 30 seconds ago is worth a leak.
    static const size_t kMaxPending = 16;

    std::vector<std::string> pending; // complete HEAR| lines, oldest first

    bool listening = false;    // button is down right now
    std::string lastHeard;     // most recent utterance, for the overlay
    float lastConfidence = 0;

    // Binding happens in the overlay's move-window mode, where the driver
    // already is when configuring things and where he has a screen to read.
    // Asking him to run an exe with a flag would mean a command line on a rig
    // that deliberately has no dev tools on it.
    bool binding = false;      // waiting for him to press the button he wants
    std::string buttonLabel;   // "0:7", or empty when nothing is bound yet
};
ECS_DEFINE_TYPE(DriverSpeechComponent);
typedef std::shared_ptr<DriverSpeechComponent> DriverSpeechComponentSP;

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
