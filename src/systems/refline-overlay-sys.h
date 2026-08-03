#ifndef refline_overlay_sys_h_
#define refline_overlay_sys_h_

#include "../ecs.h"

// Turns the live ego state plus the reference lap into a draw list: a
// car-centred track-up view of the reference line, a cross-track error bar and
// a numeric readout.
class ReflineOverlaySystem : public ECS::EntitySystem
{
public:
    virtual ~ReflineOverlaySystem();

    virtual void configure(class ECS::World *world) override;
    virtual void unconfigure(class ECS::World *world) override;
    virtual void tick(class ECS::World *world, float deltaTime) override;
};

#endif
