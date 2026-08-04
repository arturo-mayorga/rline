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

void CoachSpeechSystem::tick(class ECS::World *world, float deltaTime)
{
    if (!_voice)
        return;

    world->each<CoachMessageComponentSP>(
        [&](ECS::Entity *, ECS::ComponentHandle<CoachMessageComponentSP> mH)
        {
            CoachMessageComponent &m = *mH.get();
            if (!m.speakPending || m.text.empty())
                return;

            m.speakPending = false;

            const std::wstring w(m.text.begin(), m.text.end());

            // ASYNC so the loop continues immediately; PURGEBEFORESPEAK so a
            // newer note replaces one still being read rather than queueing
            // behind it.
            _voice->Speak(w.c_str(), SPF_ASYNC | SPF_PURGEBEFORESPEAK, NULL);
        });
}
