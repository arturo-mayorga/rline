#include "voice-input-sys.h"

#include "../components/overlay-comp.h"
#include "../components/rendering-comp.h"
#include "../irsdk/irsdk_client.h"
#include "../voice-line.h"

#include <cstdio>
#include <string>
#include <vector>

#include <windows.h>

#include <mmsystem.h> // joyGetPosEx; WIN32_LEAN_AND_MEAN keeps it out of windows.h
#include <objbase.h>
#include <sapi.h>

#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "winmm.lib")

namespace
{
    // A button that reads as held for longer than this is stuck, or the driver
    // has forgotten it. Recognition is stopped rather than left running for the
    // rest of the session.
    const float kMaxHoldMs = 30000.0f;

    // The legacy joystick API exposes at most this many of each.
    const int kMaxDevices = 16;
    const int kMaxButtons = 32;

    std::string toUtf8(const wchar_t *w)
    {
        if (!w)
            return std::string();
        const int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, NULL, 0, NULL, NULL);
        if (n <= 1)
            return std::string();
        std::vector<char> buf((size_t)n);
        WideCharToMultiByte(CP_UTF8, 0, w, -1, buf.data(), n, NULL, NULL);
        return std::string(buf.data()); // n includes the terminator
    }

    // SPEI_RECOGNITION carries an object; other events can carry allocations.
    // sphelper.h's SpClearEvent would do this, but that header ships with the
    // old Speech SDK rather than the Windows SDK, and this build has no
    // third-party dependencies.
    void clearEvent(SPEVENT &ev)
    {
        if (!ev.lParam)
            return;
        if (ev.elParamType == SPET_LPARAM_IS_OBJECT)
            ((IUnknown *)ev.lParam)->Release();
        else if (ev.elParamType == SPET_LPARAM_IS_POINTER ||
                 ev.elParamType == SPET_LPARAM_IS_STRING)
            CoTaskMemFree((void *)ev.lParam);
        ev.lParam = 0;
    }
}

VoiceInputSystem::VoiceInputSystem(const talk::Spec &button, const std::string &configPath,
                                   bool bindMode, const std::string &channel, bool inproc)
    : _button(button), _configPath(configPath), _channel(channel), _explicitBind(bindMode),
      _inproc(inproc)
{
}

void VoiceInputSystem::publishState(class ECS::World *world)
{
    world->each<DriverSpeechComponentSP>(
        [&](ECS::Entity *, ECS::ComponentHandle<DriverSpeechComponentSP> h)
        {
            DriverSpeechComponent &d = *h.get();
            d.binding = _binding;
            d.buttonLabel = _channel.empty() ? talk::formatSpec(_button) : _channel;
        });
}

bool VoiceInputSystem::buttonDown() const
{
    if (!_button.valid() || _button.button >= kMaxButtons)
        return false;

    JOYINFOEX ji = {};
    ji.dwSize = sizeof(ji);
    ji.dwFlags = JOY_RETURNBUTTONS;
    if (joyGetPosEx((UINT)_button.device, &ji) != JOYERR_NOERROR)
        return false;

    return (ji.dwButtons & (1u << _button.button)) != 0;
}

bool VoiceInputSystem::scanForPress(talk::Spec *found) const
{
    const UINT devices = joyGetNumDevs();
    for (UINT d = 0; d < devices && d < (UINT)kMaxDevices; ++d)
    {
        JOYINFOEX ji = {};
        ji.dwSize = sizeof(ji);
        ji.dwFlags = JOY_RETURNBUTTONS;
        if (joyGetPosEx(d, &ji) != JOYERR_NOERROR)
            continue; // nothing plugged into this id

        for (int b = 0; b < kMaxButtons; ++b)
        {
            if (ji.dwButtons & (1u << b))
            {
                if (found)
                {
                    found->device = (int)d;
                    found->button = b;
                }
                return true;
            }
        }
    }
    return false;
}

void VoiceInputSystem::saveBinding(const talk::Spec &s)
{
    if (_configPath.empty())
        return;
    FILE *f = fopen(_configPath.c_str(), "w");
    if (!f)
    {
        printf("rline: could not write %s, the binding will not survive a restart\n",
               _configPath.c_str());
        return;
    }
    fprintf(f, "%s\n", talk::formatSpec(s).c_str());
    fclose(f);
}

VoiceInputSystem::~VoiceInputSystem()
{
    if (_grammar)
        _grammar->Release();
    if (_ctx)
        _ctx->Release();
    if (_audio)
        _audio->Release();
    if (_reco)
        _reco->Release();
    if (_comReady)
        CoUninitialize();
}

void VoiceInputSystem::configure(class ECS::World *world)
{
    const HRESULT hr = CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
    _comReady = SUCCEEDED(hr) || hr == RPC_E_CHANGED_MODE;

    // Shared uses the machine's configured recognition profile and SAPI's own
    // audio input. In-process lets us attach the default multimedia microphone
    // explicitly, which is the remedy when SAPI's input is not configured and
    // the shared engine simply never receives any audio.
    const CLSID which = _inproc ? CLSID_SpInprocRecognizer : CLSID_SpSharedRecognizer;
    if (FAILED(CoCreateInstance(which, NULL, CLSCTX_ALL, IID_ISpRecognizer,
                                (void **)&_reco)))
    {
        _reco = nullptr;
        printf("rline: no speech recogniser available, push-to-talk disabled\n");
        return;
    }

    if (_inproc)
    {
        // CLSID_SpMMAudioIn is the default Windows recording device, the same
        // one everything else on the rig uses.
        HRESULT ah = CoCreateInstance(CLSID_SpMMAudioIn, NULL, CLSCTX_ALL, IID_ISpAudio,
                                      (void **)&_audio);
        if (SUCCEEDED(ah) && _audio)
            ah = _reco->SetInput(_audio, TRUE);
        if (FAILED(ah))
            printf("rline: could not attach the default microphone (0x%08lx)\n",
                   (unsigned long)ah);
        else
            printf("rline: using the in-process recogniser with the default microphone\n");
    }

    if (FAILED(_reco->CreateRecoContext(&_ctx)) || !_ctx)
    {
        printf("rline: could not create a recognition context, push-to-talk disabled\n");
        return;
    }

    // SOUND_START/END say whether the microphone is reaching SAPI at all, and
    // FALSE_RECOGNITION says it heard something it could not turn into words.
    // Without these two, a rig where the mic is not wired to the recogniser and
    // a rig where recognition simply fails look identical: total silence.
    const ULONGLONG interest = SPFEI(SPEI_RECOGNITION) | SPFEI(SPEI_FALSE_RECOGNITION) |
                               SPFEI(SPEI_SOUND_START) | SPFEI(SPEI_SOUND_END);
    _ctx->SetInterest(interest, interest);

    if (FAILED(_ctx->CreateGrammar(1, &_grammar)) || !_grammar)
    {
        _grammar = nullptr;
        printf("rline: could not create a dictation grammar, push-to-talk disabled\n");
        return;
    }

    if (FAILED(_grammar->LoadDictation(NULL, SPLO_STATIC)))
    {
        _grammar->Release();
        _grammar = nullptr;
        printf("rline: dictation is not installed, push-to-talk disabled\n");
        return;
    }

    // The shared engine can be sitting inactive on a machine where Windows
    // Speech Recognition has never been started, and it then produces nothing
    // for ever without reporting an error anywhere.
    const HRESULT rs = _reco->SetRecoState(SPRST_ACTIVE);
    if (FAILED(rs))
        printf("rline: could not activate the recogniser (0x%08lx) - speech will "
               "not be heard\n",
               (unsigned long)rs);

    // Silent until the button goes down.
    _grammar->SetDictationState(SPRS_INACTIVE);

    if (!_channel.empty())
        printf("rline: push-to-talk ready, reading the %s telemetry channel\n"
               "rline: NOTE - if that is iRacing's push-to-talk, everyone in the "
               "session hears you\n",
               _channel.c_str());
    else if (_explicitBind)
        printf("rline: push-to-talk ready - press the wheel button you want to "
               "talk with\n");
    else if (_button.valid())
        printf("rline: push-to-talk ready, wheel button %s\n",
               talk::formatSpec(_button).c_str());
    else
        printf("rline: push-to-talk ready but no button is bound - "
               "unlock the overlay with Ctrl+Shift+M and press one\n");

    publishState(world);
}

void VoiceInputSystem::unconfigure(class ECS::World *world)
{
    if (_grammar)
        _grammar->SetDictationState(SPRS_INACTIVE);
    _listening = false;
}

void VoiceInputSystem::setListening(class ECS::World *world, bool on)
{
    if (!_grammar || on == _listening)
        return;

    _listening = on;
    const HRESULT hr = _grammar->SetDictationState(on ? SPRS_ACTIVE : SPRS_INACTIVE);
    _heldMs = 0;

    if (FAILED(hr))
        printf("rline: dictation would not %s (0x%08lx)\n",
               on ? "start" : "stop", (unsigned long)hr);
    else
        printf("rline: %s\n", on ? "listening..." : "listening stopped");

    world->each<DriverSpeechComponentSP>(
        [&](ECS::Entity *, ECS::ComponentHandle<DriverSpeechComponentSP> h)
        { h.get()->listening = on; });
}

void VoiceInputSystem::drainEvents(class ECS::World *world)
{
    if (!_ctx)
        return;

    SPEVENT ev = {};
    ULONG fetched = 0;

    // GetEvents returns S_FALSE once the queue is empty, so this never blocks
    // the 60 Hz loop.
    while (_ctx->GetEvents(1, &ev, &fetched) == S_OK && fetched == 1)
    {
        // Diagnostics first: these are how we tell "the mic never reached SAPI"
        // apart from "SAPI heard me and made nothing of it".
        if (ev.eEventId == SPEI_SOUND_START)
            printf("rline: microphone audio started\n");
        else if (ev.eEventId == SPEI_SOUND_END)
            printf("rline: microphone audio ended\n");
        else if (ev.eEventId == SPEI_FALSE_RECOGNITION)
            printf("rline: heard sound but could not make out any words\n");

        if (ev.eEventId == SPEI_RECOGNITION && ev.elParamType == SPET_LPARAM_IS_OBJECT &&
            ev.lParam)
        {
            ISpRecoResult *res = (ISpRecoResult *)ev.lParam;

            WCHAR *w = NULL;
            if (SUCCEEDED(res->GetText(SP_GETWHOLEPHRASE, SP_GETWHOLEPHRASE, TRUE, &w, NULL)) && w)
            {
                // Average the engine's per-word confidence. A single number is
                // all the coaching side needs: it decides whether to act on an
                // utterance or ask for it again.
                float conf = 0.0f;
                SPPHRASE *ph = NULL;
                if (SUCCEEDED(res->GetPhrase(&ph)) && ph)
                {
                    double sum = 0;
                    ULONG n = 0;
                    for (ULONG i = 0; i < ph->Rule.ulCountOfElements; ++i)
                    {
                        sum += ph->pElements[i].SREngineConfidence;
                        ++n;
                    }
                    if (n)
                        conf = (float)(sum / n);
                    CoTaskMemFree(ph);
                }

                // false: this came from dictation, not the fixed-phrase
                // grammar, so the coaching side knows to weigh it accordingly.
                const std::string line = voice::buildLine(toUtf8(w), conf, false);
                CoTaskMemFree(w);

                if (!line.empty())
                {
                    printf("rline: -> %s\n", line.c_str());
                    world->each<DriverSpeechComponentSP>(
                        [&](ECS::Entity *, ECS::ComponentHandle<DriverSpeechComponentSP> h)
                        {
                            DriverSpeechComponent &d = *h.get();
                            d.pending.push_back(line);
                            // Drop the oldest rather than grow without limit if
                            // the relay is down while the driver keeps talking.
                            while (d.pending.size() > DriverSpeechComponent::kMaxPending)
                                d.pending.erase(d.pending.begin());
                            voice::parseLine(line, &d.lastHeard, &d.lastConfidence);
                        });
                }
            }
        }

        clearEvent(ev);
        fetched = 0;
    }

    clearEvent(ev);
}

void VoiceInputSystem::tick(class ECS::World *world, float deltaTime)
{
    if (!_grammar)
        return;

    // Move-window mode is bind mode. He is already there when configuring the
    // overlay, and it is the one place he has a prompt to read.
    bool unlocked = false;
    world->each<CanvasConfigComponentSP>(
        [&](ECS::Entity *, ECS::ComponentHandle<CanvasConfigComponentSP> h)
        { unlocked = h.get()->unlocked; });

    if (unlocked != _wasUnlocked)
    {
        _wasUnlocked = unlocked;
        // Require a release before the first bind, so the button press that is
        // still settling from unlocking cannot bind itself.
        _bindSawIdle = false;
        if (unlocked)
            printf("rline: move mode - press a wheel button to make it the talk button\n"
                   "rline: (speech is not recognised until you lock again)\n");
        else if (_button.valid())
            printf("rline: talk button %s armed - hold it and speak\n",
                   talk::formatSpec(_button).c_str());
        else
            printf("rline: locked with no talk button bound\n");
    }

    const bool wantBind = _explicitBind || unlocked;
    if (wantBind != _binding)
    {
        _binding = wantBind;
        if (!_binding)
            _bindSawIdle = false;
        publishState(world);
    }

    // Discovery. Deliberately waits to see every button released first, so a
    // button already held - a paddle resting against a stop, say - cannot bind
    // itself. Pressing again while still in move mode rebinds, so a mistake is
    // corrected on the spot rather than by editing a file.
    if (_binding)
    {
        if (_listening)
            setListening(world, false);

        talk::Spec found;
        const bool anyDown = scanForPress(&found);

        if (!_bindSawIdle)
        {
            if (!anyDown)
                _bindSawIdle = true;
        }
        else if (anyDown)
        {
            _button = found;
            _bindSawIdle = false; // release before rebinding
            _explicitBind = false;
            printf("rline: talk button bound to %s\n", talk::formatSpec(_button).c_str());
            saveBinding(_button);
            publishState(world);
        }

        _prevDown = false;
        return; // never listening for speech while binding
    }

    bool down = false;

    if (!_channel.empty())
    {
        // Telemetry-channel route: only meaningful while iRacing is up, and
        // channel indices are valid for one connection only.
        if (!irsdkClient::instance().isConnected())
        {
            _idxResolved = false;
            _varIdx = -1;
            if (_listening)
                setListening(world, false);
            _prevDown = false;
            return;
        }

        if (!_idxResolved)
        {
            _varIdx = irsdkClient::instance().getVarIdx(_channel.c_str());
            _idxResolved = true;
            if (_varIdx < 0)
                printf("rline: no '%s' channel in this session, push-to-talk inactive\n",
                       _channel.c_str());
        }

        if (_varIdx >= 0)
            down = irsdkClient::instance().getVarBool(_varIdx, 0);
    }
    else
    {
        // The wheel is readable whether or not iRacing is running, so he can
        // test the button from the desktop before going on track.
        if (!_button.valid())
        {
            if (!_warnedNoButton)
            {
                printf("rline: no talk button bound, run rline --bind-talk once\n");
                _warnedNoButton = true;
            }
            return;
        }
        down = buttonDown();
    }

    if (down && !_prevDown)
        setListening(world, true);
    else if (!down && _prevDown)
        setListening(world, false);

    if (_listening)
    {
        _heldMs += deltaTime;
        if (_heldMs > kMaxHoldMs)
        {
            printf("rline: talk button held for %.0f s, assuming it is stuck\n",
                   _heldMs / 1000.0f);
            setListening(world, false);
        }
    }

    _prevDown = down;

    // Always drain: a recognition lands shortly after the button is released.
    drainEvents(world);
}
