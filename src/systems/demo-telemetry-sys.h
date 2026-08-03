#ifndef demo_telemetry_sys_h_
#define demo_telemetry_sys_h_

#include "../ecs.h"

// Drives the ego state from the reference lap itself instead of the iRacing
// SDK, weaving side to side so the whole render path can be exercised without a
// session running. Swap in for IrTelemetrySystem with --demo.
class DemoTelemetrySystem : public ECS::EntitySystem
{
private:
    float _elapsed = 0; // seconds into the reference lap

public:
    virtual ~DemoTelemetrySystem();

    virtual void configure(class ECS::World *world) override;
    virtual void unconfigure(class ECS::World *world) override;
    virtual void tick(class ECS::World *world, float deltaTime) override;
};

#endif
