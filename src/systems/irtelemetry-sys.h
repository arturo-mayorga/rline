#ifndef irtelemetry_sys_h_
#define irtelemetry_sys_h_

#include "../ecs.h"
#include "../refline.h"

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

public:
    virtual ~IrTelemetrySystem();

    virtual void configure(class ECS::World *world) override;
    virtual void unconfigure(class ECS::World *world) override;
    virtual void tick(class ECS::World *world, float deltaTime) override;
};

#endif
