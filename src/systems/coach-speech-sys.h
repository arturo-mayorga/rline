#ifndef coach_speech_sys_h_
#define coach_speech_sys_h_

#include "../ecs.h"

// Speaks coaching notes through the Windows speech engine.
//
// SAPI ships with Windows, so this costs no new dependency and the executable
// stays self-contained. Speech is asynchronous - a coaching note must never
// stall the 60 Hz loop - and a new note interrupts the previous one, since
// stale advice is worse than none while the corner is already happening.
struct ISpVoice;

class CoachSpeechSystem : public ECS::EntitySystem
{
private:
    ISpVoice *_voice = nullptr;
    bool _comReady = false;

public:
    virtual ~CoachSpeechSystem();

    virtual void configure(class ECS::World *world) override;
    virtual void unconfigure(class ECS::World *world) override;
    virtual void tick(class ECS::World *world, float deltaTime) override;

    bool available() const { return _voice != nullptr; }
};

#endif
