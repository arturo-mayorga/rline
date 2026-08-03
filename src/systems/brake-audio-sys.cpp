#include "brake-audio-sys.h"

#include "../components/overlay-comp.h"

#include <cmath>
#include <cstdint>
#include <cstring>

#include <windows.h>
#include <mmsystem.h>

namespace
{
    const int kSampleRate = 22050;

    // Braking is C5, the C above middle C: both beeps share the pitch so the
    // rhythm alone carries the countdown, with the last held longer so it still
    // reads as the one to act on.
    const float kC5 = 523.25f;

    // Turn-in rises a fifth, G4 to G5; the apex falls back, G5 to G4. Going in
    // rises, coming out falls, and neither clashes with the flat C5 braking
    // rhythm.
    const float kG4 = 392.00f;
    const float kG5 = 783.99f;

    const int kShortMs = 70;

    // Held longer than the counting beeps, but short enough that the turn-in
    // countdown starting an interval later cannot cut it off: the tightest
    // braking-to-turn-in gap on the reference lap is 0.60 s, so with a 0.5 s
    // interval the next tone begins 100 ms after this one.
    const int kFinalMs = 90;

    void appendLE(std::vector<char> &v, uint32_t value, int bytes)
    {
        for (int i = 0; i < bytes; ++i)
            v.push_back((char)((value >> (8 * i)) & 0xFF));
    }

    // A 16-bit mono PCM WAV in memory, so PlaySound can take it directly.
    std::vector<char> makeBeep(float freqHz, int ms, float amplitude)
    {
        const int samples = (kSampleRate * ms) / 1000;
        const int dataBytes = samples * 2;

        std::vector<char> w;
        w.reserve(44 + dataBytes);

        const char *riff = "RIFF";
        w.insert(w.end(), riff, riff + 4);
        appendLE(w, 36 + dataBytes, 4);
        const char *wave = "WAVEfmt ";
        w.insert(w.end(), wave, wave + 8);
        appendLE(w, 16, 4);            // fmt chunk size
        appendLE(w, 1, 2);             // PCM
        appendLE(w, 1, 2);             // mono
        appendLE(w, kSampleRate, 4);
        appendLE(w, kSampleRate * 2, 4); // byte rate
        appendLE(w, 2, 2);             // block align
        appendLE(w, 16, 2);            // bits per sample
        const char *data = "data";
        w.insert(w.end(), data, data + 4);
        appendLE(w, dataBytes, 4);

        // A raised-cosine attack and release; a bare square-edged tone clicks.
        const int fade = std::min(samples / 4, (kSampleRate * 5) / 1000);
        for (int i = 0; i < samples; ++i)
        {
            float env = 1.0f;
            if (fade > 0)
            {
                if (i < fade)
                    env = 0.5f * (1.0f - cosf(3.14159265f * (float)i / (float)fade));
                else if (i >= samples - fade)
                    env = 0.5f * (1.0f - cosf(3.14159265f * (float)(samples - 1 - i) / (float)fade));
            }

            const float s = sinf(2.0f * 3.14159265f * freqHz * (float)i / (float)kSampleRate);
            const int16_t v = (int16_t)(s * env * amplitude * 32767.0f);
            appendLE(w, (uint32_t)(uint16_t)v, 2);
        }

        return w;
    }
}

BrakeAudioSystem::~BrakeAudioSystem() {}

BrakeAudioSystem::BrakeAudioSystem()
{
    _brakeCue.kind = RefEventKind::Brake;
    _brakeCue.beeps = 3;

    // Two beeps, so the countdown is half as long and fits between the braking
    // point and turn-in.
    _turnCue.kind = RefEventKind::TurnIn;
    _turnCue.beeps = 2;
    _turnCue.maxLookM = 250.0f;

    _apexCue.kind = RefEventKind::Apex;
    _apexCue.beeps = 2;
    _apexCue.maxLookM = 250.0f;
}

void BrakeAudioSystem::configure(class ECS::World *world)
{
    _brakeCount = makeBeep(kC5, kShortMs, 0.30f);
    _brakeFinal = makeBeep(kC5, kFinalMs, 0.40f);
    _turnFirst = makeBeep(kG4, kShortMs, 0.30f);
    _turnFinal = makeBeep(kG5, kFinalMs, 0.40f);
    _apexFirst = makeBeep(kG5, kShortMs, 0.30f);
    _apexFinal = makeBeep(kG4, kFinalMs, 0.40f);
}

void BrakeAudioSystem::play(const std::vector<char> &wav)
{
    // ASYNC so the 60 Hz loop is never blocked waiting on audio, and NODEFAULT
    // so a failure is silent rather than the system ding.
    PlaySound(reinterpret_cast<LPCTSTR>(wav.data()), NULL,
              SND_MEMORY | SND_ASYNC | SND_NODEFAULT);
}

void BrakeAudioSystem::unconfigure(class ECS::World *world)
{
    PlaySound(NULL, NULL, 0); // stop anything still playing
}

void BrakeAudioSystem::tick(class ECS::World *world, float deltaTime)
{
    world->each<EgoStateComponentSP>(
        [&](ECS::Entity *ent, ECS::ComponentHandle<EgoStateComponentSP> egoH)
        {
            const EgoStateComponent &ego = *egoH.get();

            ECS::ComponentHandle<RefLineComponentSP> refH = ent->get<RefLineComponentSP>();
            if (!refH.isValid() || !refH.get()->loaded)
                return;

            if (!ego.connected || !ego.onTrack)
            {
                _brakeCue.reset();
                _turnCue.reset();
                _apexCue.reset();
                return;
            }

            const RefLine &line = refH.get()->line;
            const float dt = deltaTime / 1000.0f; // tick delta is milliseconds

            const int brake = _brakeCue.update(line, ego.pct, ego.speed, dt);
            if (brake > 0)
                play(brake >= _brakeCue.beeps ? _brakeFinal : _brakeCount);

            const int turn = _turnCue.update(line, ego.pct, ego.speed, dt);
            if (turn > 0)
                play(turn >= _turnCue.beeps ? _turnFinal : _turnFirst);

            const int apex = _apexCue.update(line, ego.pct, ego.speed, dt);
            if (apex > 0)
                play(apex >= _apexCue.beeps ? _apexFinal : _apexFirst);
        });
}
