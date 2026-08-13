#ifndef irtelemetry_sys_h_
#define irtelemetry_sys_h_

#include "../ecs.h"
#include "../refline.h"

#include <string>

// Pulls the player car's track position, speed, heading and velocity out of
// iRacing's shared memory, reconstructs lateral offset from them, and writes
// the result to EgoStateComponent. Nothing else.
class IrTelemetrySystem : public ECS::EntitySystem
{
private:
    LateralEstimator _lateral;
    double _lastTime = 0;
    float _lastPct = 0;
    bool _haveTime = false;

    // Session type, re-read from the YAML only when the session number changes.
    int _sessionNum = -1;
    std::string _sessionType;

    // The lap the car was on when it last left the pits. Until the counter
    // moves past it, this is an out-lap: whatever fault the coach remembers
    // came from before the stop, on tyres that are not these tyres.
    int _pitExitLap = -1;
    bool _wasOnPitRoad = false;

public:
    virtual ~IrTelemetrySystem();

    virtual void configure(class ECS::World *world) override;
    virtual void unconfigure(class ECS::World *world) override;
    virtual void tick(class ECS::World *world, float deltaTime) override;
};

#endif
