#ifndef voice_input_sys_h_
#define voice_input_sys_h_

#include "../ecs.h"
#include "../talk-button.h"

#include <string>

// Push to talk, from the wheel.
//
// The driver holds a spare wheel button and speaks; the recognised line is
// queued for the relay.
//
// The button is read straight off the joystick. Watching iRacing's PushToTalk
// telemetry channel instead would need no button code at all, but that button
// opens voice chat to everyone in the session - talking to your coach is not
// something to broadcast to the people you are racing. --talk-channel still
// allows that route for anyone who wants it, and it is off by default.
//
// joyGetPosEx rather than DirectInput: winmm is already linked for the braking
// countdown, so this adds no dependency, and a wheel presents its first 32
// buttons through it. If a button turns out to be invisible this way, that is
// the point at which DirectInput becomes worth its extra surface.
//
// Recognition happens here, on the rig, rather than by shipping audio to the
// analysis machine. That is a deliberate exception to keeping intelligence off
// the rig: audio is orders of magnitude larger than telemetry, and SAPI is
// already linked in for the coaching voice. What crosses the network is one
// short line per utterance.
//
// The dictation grammar is only active while the button is held. Leaving it
// listening would transcribe engine noise for a whole session.
struct ISpRecognizer;
struct ISpRecoContext;
struct ISpRecoGrammar;
struct ISpAudio;

class VoiceInputSystem : public ECS::EntitySystem
{
private:
    ISpRecognizer *_reco = nullptr;
    ISpRecoContext *_ctx = nullptr;
    ISpRecoGrammar *_grammar = nullptr;
    ISpAudio *_audio = nullptr;

    // The shared recogniser takes its audio from SAPI's own input setting,
    // which is separate from the Windows default recording device and is
    // frequently unset - the symptom is total silence with no error anywhere.
    // In-process instead lets us attach the default multimedia input by hand.
    bool _inproc = false;

    bool _comReady = false;
    bool _listening = false;
    bool _prevDown = false;
    float _heldMs = 0;

    // Where the press comes from. Exactly one of these is in use.
    talk::Spec _button;
    std::string _configPath;
    std::string _channel; // non-empty: read a telemetry channel instead

    // Discovery: wait for the driver to press the button he wants.
    //
    // Normally driven by the overlay's move-window mode (Ctrl+Shift+M) - that
    // is where he already is when configuring, and he can read the prompt on
    // screen. --bind-talk forces it on for a headless run.
    bool _explicitBind = false;
    bool _binding = false;
    bool _bindSawIdle = false;
    bool _wasUnlocked = false;

    int _varIdx = -1;
    bool _idxResolved = false;
    bool _warnedNoButton = false;

    bool buttonDown() const;
    bool scanForPress(talk::Spec *found) const;
    void saveBinding(const talk::Spec &s);
    void publishState(class ECS::World *world);

    void setListening(class ECS::World *world, bool on);
    void drainEvents(class ECS::World *world);

public:
    VoiceInputSystem(const talk::Spec &button, const std::string &configPath,
                     bool bindMode, const std::string &channel, bool inproc);
    virtual ~VoiceInputSystem();

    virtual void configure(class ECS::World *world) override;
    virtual void unconfigure(class ECS::World *world) override;
    virtual void tick(class ECS::World *world, float deltaTime) override;

    bool available() const { return _grammar != nullptr; }
};

#endif
