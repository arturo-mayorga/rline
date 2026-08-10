#include "corner-coach-sys.h"

#include "../components/overlay-comp.h"

namespace
{
    // A call must not land on top of the braking countdown for the next
    // corner, so a verdict is dropped rather than queued if one is too recent.
    const float kMinGapMs = 1500.0f;

    // How long a corner cue stays worth saying. It names a corner that is
    // arriving, so once the driver is past it the cue is not merely useless but
    // actively misleading - it points him at the wrong piece of road. Long
    // enough to wait out one note already being spoken, short enough that it
    // never survives into the next corner.
    const float kCueShelfLifeMs = 4000.0f;
}

CornerCoachSystem::~CornerCoachSystem() {}
void CornerCoachSystem::configure(class ECS::World *world)
{
    // Sits next to the exe, like the reference lap. Absent is not an error:
    // the coach falls back to the detector's own numbering.
    _coach.loadNames("corner-names.txt");

    // Publish what will actually be spoken, so the rig can report it to the
    // relay on connection. Read from the coach rather than the file, because
    // the question being answered is "what will this rig say out loud" - and
    // an unreadable or half-parsed file must show up as missing names here.
    world->each<RefLineComponentSP>(
        [&](ECS::Entity *, ECS::ComponentHandle<RefLineComponentSP> h)
        { h.get()->cornerNames = _coach.names; });
}
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

            // Queued rather than written straight to the slot: this used to
            // cut off whatever was already being said. A corner cue is given a
            // short shelf life on purpose - delivered after the corner it names,
            // it sends the driver to the wrong piece of road, so it is better
            // dropped than spoken late.
            ECS::ComponentHandle<SpeechQueueComponentSP> qH =
                ent->get<SpeechQueueComponentSP>();
            if (qH.isValid())
                qH.get()->queue.push(v.note, speech::PriorityCorner, kCueShelfLifeMs, 5.0f);
        });
}
