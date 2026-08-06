#ifndef coach_speech_sys_h_
#define coach_speech_sys_h_

#include "../ecs.h"

// Speaks coaching notes through the Windows speech engine.
//
// SAPI ships with Windows, so this costs no new dependency and the executable
// stays self-contained. Speech is asynchronous - a coaching note must never
// stall the 60 Hz loop.
//
// Notes are taken from SpeechQueueComponent, never spoken directly, and a
// sentence in progress is never interrupted. It used to speak with
// SPF_PURGEBEFORESPEAK on the theory that stale advice is worse than none -
// but that cut the driver off mid-sentence whenever the corner coach and the
// relay both had something to say, which he reported directly. Staleness is
// now handled where it belongs: each note carries a shelf life and is dropped
// if it cannot be delivered in time. See src/speech-queue.h.
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

private:
    // True while the engine still has audio to play, so nothing is started on
    // top of it.
    bool speaking() const;
};

#endif
