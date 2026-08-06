#include "coach-speech-sys.h"

#include "../components/overlay-comp.h"

#include <cstdio>
#include <string>

#include <windows.h>

#include <objbase.h>
#include <sapi.h>

#pragma comment(lib, "ole32.lib")

CoachSpeechSystem::~CoachSpeechSystem()
{
    if (_voice)
        _voice->Release();
    if (_comReady)
        CoUninitialize();
}

void CoachSpeechSystem::configure(class ECS::World *world)
{
    // Apartment-threaded: everything here runs on the one loop thread.
    const HRESULT hr = CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
    _comReady = SUCCEEDED(hr) || hr == RPC_E_CHANGED_MODE;

    if (FAILED(CoCreateInstance(CLSID_SpVoice, NULL, CLSCTX_ALL, IID_ISpVoice,
                                (void **)&_voice)))
    {
        _voice = nullptr;
        printf("rline: no speech engine available, coaching notes will be silent\n");
        return;
    }

    // Coaching is terse and time-critical; the default rate reads too slowly
    // to finish a note before the corner arrives.
    _voice->SetRate(2);
    printf("rline: speech ready\n");
}

void CoachSpeechSystem::unconfigure(class ECS::World *world)
{
    if (_voice)
        _voice->Speak(NULL, SPF_PURGEBEFORESPEAK, NULL);
}

bool CoachSpeechSystem::speaking() const
{
    if (!_voice)
        return false;

    SPVOICESTATUS st = {};
    if (FAILED(_voice->GetStatus(&st, NULL)))
        return false;

    return (st.dwRunningState & SPRS_IS_SPEAKING) != 0;
}

void CoachSpeechSystem::tick(class ECS::World *world, float deltaTime)
{
    if (!_voice)
        return;

    world->each<SpeechQueueComponentSP>(
        [&](ECS::Entity *ent, ECS::ComponentHandle<SpeechQueueComponentSP> qH)
        {
            speech::Queue &q = qH.get()->queue;

            // Shelf lives run down whether or not anything is being said, so a
            // corner cue that waited out a long note is dropped rather than
            // delivered after its corner.
            q.age(deltaTime);

            // The fix for being cut off mid-sentence: wait for the engine to
            // finish rather than purging it. This used to speak with
            // SPF_PURGEBEFORESPEAK, so every new note - from either the corner
            // coach or the relay - truncated whatever was already talking.
            if (speaking())
                return;

            speech::Item item;
            if (!q.pop(&item))
                return;

            // The panel shows what is being said, as it is said, so the screen
            // and the voice can never disagree.
            ECS::ComponentHandle<CoachMessageComponentSP> mH =
                ent->get<CoachMessageComponentSP>();
            if (mH.isValid())
            {
                CoachMessageComponent &m = *mH.get();
                m.text = item.text;
                m.ttl = item.displaySecs;
                m.speakPending = false;
            }

            const std::wstring w(item.text.begin(), item.text.end());

            // ASYNC so the loop continues immediately. No PURGEBEFORESPEAK:
            // nothing may interrupt a sentence in progress.
            _voice->Speak(w.c_str(), SPF_ASYNC, NULL);
        });
}
