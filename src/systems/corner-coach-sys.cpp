#include "corner-coach-sys.h"

#include "../components/overlay-comp.h"

namespace
{
    // A call must not land on top of the braking countdown for the next
    // corner, so a verdict is dropped rather than queued if one is too recent.
    const float kMinGapMs = 1500.0f;
}

CornerCoachSystem::~CornerCoachSystem() {}
void CornerCoachSystem::configure(class ECS::World *world) {}
void CornerCoachSystem::unconfigure(class ECS::World *world) {}

void CornerCoachSystem::tick(class ECS::World *world, float deltaTime)
{
    if (_quietFor > 0)
        _quietFor -= deltaTime;

    world->each<EgoStateComponentSP>(
        [&](ECS::Entity *ent, ECS::ComponentHandle<EgoStateComponentSP> egoH)
        {
            const EgoStateComponent &ego = *egoH.get();

            ECS::ComponentHandle<RefLineComponentSP> refH = ent->get<RefLineComponentSP>();
            if (!refH.isValid() || !refH.get()->loaded)
                return;

            if (!ego.connected || !ego.onTrack)
            {
                _coach.reset();
                return;
            }

            const CornerVerdict v = _coach.update(refH.get()->line, ego.pct,
                                                  ego.speed, ego.brake, ego.throttle, ego.steer,
                                                  ego.grip.peakSteer());
            if (v.corner == 0 || v.good || v.note.empty())
                return;

            if (_quietFor > 0)
                return;
            _quietFor = kMinGapMs;

            ECS::ComponentHandle<CoachMessageComponentSP> mH =
                ent->get<CoachMessageComponentSP>();
            if (!mH.isValid())
                return;

            CoachMessageComponent &m = *mH.get();
            m.text = v.note;
            m.ttl = 5.0f;
            m.speakPending = true;
        });
}
