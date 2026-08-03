#ifndef brake_audio_sys_h_
#define brake_audio_sys_h_

#include "../ecs.h"
#include "../event-cue.h"

#include <vector>

// Plays the corner countdowns. The timing lives in EventCue, which is portable
// and tested; this system only turns "beep k is due" into sound.
//
// Braking gets three beeps on C5; turn-in two, rising G4 to G5; the apex two,
// falling G5 to G4. The last beep of each lands on its point. The two-beep
// countdowns are short because turn-in follows braking, and the apex follows
// turn-in, by well under a second - a longer countdown would collide with the
// tone before it.
//
// Tones are synthesised into memory at startup rather than shipped as files, so
// the executable stays self-contained.
class BrakeAudioSystem : public ECS::EntitySystem
{
private:
    EventCue _brakeCue;
    EventCue _turnCue;
    EventCue _apexCue;

    std::vector<char> _brakeCount; // C5, braking beeps 1 and 2
    std::vector<char> _brakeFinal; // C5 held longer, on the braking point
    std::vector<char> _turnFirst;  // G4, one interval before turn-in
    std::vector<char> _turnFinal;  // G5, on the turn-in point
    std::vector<char> _apexFirst;  // G5, one interval before the apex
    std::vector<char> _apexFinal;  // G4, on the apex

    void play(const std::vector<char> &wav);

public:
    BrakeAudioSystem();
    virtual ~BrakeAudioSystem();

    virtual void configure(class ECS::World *world) override;
    virtual void unconfigure(class ECS::World *world) override;
    virtual void tick(class ECS::World *world, float deltaTime) override;

    void setInterval(float seconds)
    {
        _brakeCue.intervalS = seconds;
        _turnCue.intervalS = seconds;
        _apexCue.intervalS = seconds;
    }

    // The synthesised tones, so their pitch can be checked rather than assumed.
    const std::vector<char> &brakeCountWav() const { return _brakeCount; }
    const std::vector<char> &brakeFinalWav() const { return _brakeFinal; }
    const std::vector<char> &turnFirstWav() const { return _turnFirst; }
    const std::vector<char> &turnFinalWav() const { return _turnFinal; }
    const std::vector<char> &apexFirstWav() const { return _apexFirst; }
    const std::vector<char> &apexFinalWav() const { return _apexFinal; }
};

#endif
