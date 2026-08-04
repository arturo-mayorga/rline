#ifndef corner_coach_sys_h_
#define corner_coach_sys_h_

#include "../corner-coach.h"
#include "../ecs.h"

// Speaks a verdict on each corner as the car leaves it.
//
// All of it runs on the rig from the reference lap already on disk, so a call
// arrives while the corner is still fresh - no relay, no network, no analysis
// machine. Clean corners say nothing: constant chatter would drown out the
// calls that matter.
class CornerCoachSystem : public ECS::EntitySystem
{
private:
    CornerCoach _coach;
    float _quietFor = 0; // ms until another call is allowed

public:
    virtual ~CornerCoachSystem();

    virtual void configure(class ECS::World *world) override;
    virtual void unconfigure(class ECS::World *world) override;
    virtual void tick(class ECS::World *world, float deltaTime) override;
};

#endif
