// rline - reference driving line overlay for iRacing.

#include "ecs.h"
#include "refline.h"
#include "talk-button.h"

#include "components/overlay-comp.h"
#include "components/rendering-comp.h"

#include "systems/brake-audio-sys.h"
#include "systems/coach-speech-sys.h"
#include "systems/corner-coach-sys.h"
#include "systems/telemetry-stream-sys.h"
#include "systems/demo-telemetry-sys.h"
#include "systems/irtelemetry-sys.h"
#include "systems/refline-overlay-sys.h"
#include "systems/voice-input-sys.h"
#include "systems/window-rendering-sys.h"

#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <string>
#include <windows.h>

namespace
{
    struct Options
    {
        std::string csv = "lap.csv";
        int x = -1; // -1 means "place it near the wheel"
        int y = -1;
        int w = 360;
        int h = 420;
        bool mph = false;
        float exaggeration = 3.0f;
        bool demo = false;
        bool verbose = false;
        bool unlocked = false;
        bool noAudio = false;
        bool dumpBeeps = false;
        float beepInterval = 0.5f;
        // Streams to the coaching relay by default so the rig needs no
        // arguments. rline-relay.txt beside the exe overrides this, and
        // --relay overrides that, so a changed IP never needs a rebuild.
        std::string relay = "192.168.1.161";
        bool noRelay = false;
        bool noSpeech = false;
        bool noCornerCoach = false;
        bool noVoiceInput = false;
        // The talk button is read straight off the wheel, as <device>:<button>,
        // remembered in rline-talk.txt beside the exe and discovered by
        // --bind-talk. Empty talkChannel means "use the wheel"; setting it
        // reads an iRacing telemetry channel instead, which is off by default
        // because the obvious channel - PushToTalk - also opens voice chat to
        // everyone in the session.
        std::string talkButton;
        bool bindTalk = false;
        bool voiceInproc = false;
        std::string talkChannel;
        int exitAfterMs = 0; // 0 = run until killed
        bool help = false;
    };

    // Resolves a relative path against the executable's own directory, so the
    // overlay works no matter what the working directory is when launched.
    std::string resolveBesideExe(const std::string &path)
    {
        if (path.size() > 1 && (path[1] == ':' || path[0] == '\\' || path[0] == '/'))
            return path;

        char exe[MAX_PATH] = {};
        if (!GetModuleFileNameA(NULL, exe, MAX_PATH))
            return path;

        std::string dir(exe);
        const size_t slash = dir.find_last_of("\\/");
        if (slash == std::string::npos)
            return path;

        return dir.substr(0, slash + 1) + path;
    }

    Options parseArgs(int argc, char **argv)
    {
        Options o;
        for (int i = 1; i < argc; ++i)
        {
            const char *a = argv[i];
            auto next = [&](int &out)
            { if (i + 1 < argc) out = atoi(argv[++i]); };

            if (!strcmp(a, "--csv") && i + 1 < argc)
                o.csv = argv[++i];
            else if (!strcmp(a, "--x"))
                next(o.x);
            else if (!strcmp(a, "--y"))
                next(o.y);
            else if (!strcmp(a, "--w"))
                next(o.w);
            else if (!strcmp(a, "--h"))
                next(o.h);
            else if (!strcmp(a, "--mph"))
                o.mph = true;
            else if (!strcmp(a, "--exaggeration") && i + 1 < argc)
                o.exaggeration = (float)atof(argv[++i]);
            else if (!strcmp(a, "--demo"))
                o.demo = true;
            else if (!strcmp(a, "--verbose") || !strcmp(a, "-v"))
                o.verbose = true;
            else if (!strcmp(a, "--unlocked"))
                o.unlocked = true;
            else if (!strcmp(a, "--no-audio"))
                o.noAudio = true;
            else if (!strcmp(a, "--dump-beeps"))
                o.dumpBeeps = true;
            else if (!strcmp(a, "--relay") && i + 1 < argc)
                o.relay = argv[++i];
            else if (!strcmp(a, "--no-relay"))
                o.noRelay = true;
            else if (!strcmp(a, "--no-speech"))
                o.noSpeech = true;
            else if (!strcmp(a, "--no-corner-coach"))
                o.noCornerCoach = true;
            else if (!strcmp(a, "--no-voice-input"))
                o.noVoiceInput = true;
            else if (!strcmp(a, "--talk-button") && i + 1 < argc)
                o.talkButton = argv[++i];
            else if (!strcmp(a, "--bind-talk"))
                o.bindTalk = true;
            else if (!strcmp(a, "--voice-inproc"))
                o.voiceInproc = true;
            else if (!strcmp(a, "--talk-channel") && i + 1 < argc)
                o.talkChannel = argv[++i];
            else if (!strcmp(a, "--beep-interval") && i + 1 < argc)
                o.beepInterval = (float)atof(argv[++i]);
            else if (!strcmp(a, "--exit-after"))
                next(o.exitAfterMs);
            else if (!strcmp(a, "--help") || !strcmp(a, "-h"))
                o.help = true;
        }
        return o;
    }

    FILE *g_log = nullptr;

    // Writes to stdout and, in verbose mode, to a log file - flushed every
    // line so nothing is lost if the process dies unexpectedly.
    void logLine(const char *fmt, ...)
    {
        char buf[512];
        va_list ap;
        va_start(ap, fmt);
        vsnprintf(buf, sizeof(buf), fmt, ap);
        va_end(ap);

        printf("%s\n", buf);
        if (g_log)
        {
            fprintf(g_log, "%s\n", buf);
            fflush(g_log);
        }
    }

    // Defaults to lower-centre of the primary monitor, roughly where the
    // steering wheel sits in a cockpit view, rather than a corner.
    void defaultPlacement(Options &o)
    {
        const int sw = GetSystemMetrics(SM_CXSCREEN);
        const int sh = GetSystemMetrics(SM_CYSCREEN);
        if (sw <= 0 || sh <= 0)
        {
            if (o.x < 0) o.x = 40;
            if (o.y < 0) o.y = 40;
            return;
        }

        int px = (sw - o.w) / 2;
        int py = (int)(sh * 0.62f) - o.h / 2;

        // A position the user dragged to wins over the default, but an
        // explicit --x/--y wins over both.
        int sx = px, sy = py;
        if (loadSavedOverlayPosition(sx, sy))
        {
            px = sx;
            py = sy;
        }

        if (o.x < 0) o.x = std::max(0, px);
        if (o.y < 0) o.y = std::max(0, py);
    }

    void printUsage()
    {
        printf(
            "rline - reference driving line overlay for iRacing\n"
            "\n"
            "  --csv <path>          reference lap CSV (default: lap.csv beside the exe)\n"
            "  --x --y               overlay position in pixels (default 40, 40)\n"
            "  --w --h               overlay size in pixels (default 360, 420)\n"
            "  --mph                 show speed delta in mph instead of km/h\n"
            "  --exaggeration <n>    lateral zoom multiplier (default 3.0)\n"
            "  --demo                replay the reference lap instead of reading iRacing,\n"
            "                        weaving across the line; for testing without a session\n"
            "  --exit-after <ms>     quit after this long (for automated checks)\n"
            "  --verbose             log state to the console and rline.log\n"
            "  --unlocked            start unlocked so it can be dragged straight away\n"
            "  --relay <host[:port]> coaching relay to stream to; overrides the default\n"
            "                        and rline-relay.txt\n"
            "  --no-relay            do not stream telemetry anywhere\n"
            "  --no-speech           show coaching notes but do not read them aloud\n"
            "  --no-corner-coach     stop calling out each corner as you exit it\n"
            "  --bind-talk           bind the talk button from the command line;\n"
            "                        normally done in move mode (Ctrl+Shift+M)\n"
            "  --talk-button <d:b>   set it directly, as device:button\n"
            "  --voice-inproc        use the in-process recogniser with the\n"
            "                        default microphone; try this if the console\n"
            "                        never says \"microphone audio started\"\n"
            "  --no-voice-input      do not listen for push-to-talk\n"
            "  --talk-channel <name> read a telemetry channel instead of the\n"
            "                        wheel. Note that iRacing's own PushToTalk\n"
            "                        also transmits to everyone in the session\n"
            "  --no-audio            silence the braking countdown\n"
            "  --beep-interval <s>   spacing between countdown beeps (default 0.5)\n"
            "\n"
            "Press Ctrl+Shift+M to unlock the overlay and drag it with the mouse;\n"
            "while unlocked, pressing any wheel button binds it as the talk button.\n"
            "press it again to lock it and make it click-through. The position is\n"
            "remembered in rline-pos.txt beside the exe.\n"
            "\n"
            "The CSV needs Lat, Lon, LapDistPct and Speed columns; order does not matter.\n"
            "iRacing must run in windowed or borderless mode - an exclusive fullscreen\n"
            "swap chain draws over layered windows like this one.\n"
            "\n"
            "Close the overlay from the console window, or with Ctrl+C.\n");
    }
}

int main(int argc, char **argv)
{
    Options opt = parseArgs(argc, argv);
    if (opt.help)
    {
        printUsage();
        return 0;
    }

    // The console is the only feedback channel this tool has, and a redirected
    // stdout is block buffered - so anything printed before an abnormal exit
    // would be lost exactly when it is most needed.
    setvbuf(stdout, NULL, _IONBF, 0);

    // Without this Windows bitmap-scales the overlay on a display with DPI
    // scaling - blurry text, and GetSystemMetrics reporting virtualised pixels
    // so the default placement lands in the wrong spot. Resolved at runtime so
    // the exe still starts on Windows versions that predate it.
    {
        typedef BOOL(WINAPI * SetCtxFn)(HANDLE);
        HMODULE user32 = GetModuleHandleA("user32.dll");
        if (user32)
        {
            SetCtxFn setCtx =
                (SetCtxFn)GetProcAddress(user32, "SetProcessDpiAwarenessContext");
            if (setCtx)
            {
                // -4 is DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2.
                setCtx((HANDLE)-4);
            }
            else
            {
                SetProcessDPIAware(); // good enough on older builds
            }
        }
    }

    // A one-line rline-relay.txt beside the exe overrides the built-in host.
    // Skipped when --relay was given explicitly.
    {
        bool explicitRelay = false;
        for (int i = 1; i < argc; ++i)
            if (!strcmp(argv[i], "--relay"))
                explicitRelay = true;

        if (!explicitRelay)
        {
            FILE *f = fopen(resolveBesideExe("rline-relay.txt").c_str(), "r");
            if (f)
            {
                char buf[128] = {};
                if (fgets(buf, sizeof(buf), f))
                {
                    std::string v(buf);
                    while (!v.empty() && (v.back() == '\n' || v.back() == '\r' ||
                                          v.back() == ' ' || v.back() == '\t'))
                        v.pop_back();
                    if (!v.empty())
                        opt.relay = v;
                }
                fclose(f);
            }
        }
    }

    defaultPlacement(opt);

    // stdout also disappears entirely when launched detached or through WSL
    // interop, so --verbose additionally writes a log beside the exe.
    if (opt.verbose)
    {
        g_log = fopen(resolveBesideExe("rline.log").c_str(), "w");
        logLine("rline: starting");
    }

    // Writes the countdown tones out so their pitch can be measured.
    if (opt.dumpBeeps)
    {
        BrakeAudioSystem probe;
        probe.configure(nullptr);
        const std::vector<char> *data[] = {&probe.brakeCountWav(), &probe.brakeFinalWav(),
                                           &probe.turnFirstWav(), &probe.turnFinalWav(),
                                           &probe.apexFirstWav(), &probe.apexFinalWav()};
        const char *names[] = {"beep-brake-count.wav", "beep-brake-final.wav",
                               "beep-turn-first.wav", "beep-turn-final.wav",
                               "beep-apex-first.wav", "beep-apex-final.wav"};
        for (int i = 0; i < 6; ++i)
        {
            FILE *f = fopen(resolveBesideExe(names[i]).c_str(), "wb");
            if (f)
            {
                fwrite(data[i]->data(), 1, data[i]->size(), f);
                fclose(f);
                printf("wrote %s (%zu bytes)\n", names[i], data[i]->size());
            }
        }
        return 0;
    }

    ECS::World *world = ECS::World::createWorld();

    // One entity carries the whole overlay: its window, its draw list, the
    // reference lap and the live car state.
    ECS::Entity *ent = world->create();

    CanvasConfigComponentSP canvas(new CanvasConfigComponent());
    canvas->x = opt.x;
    canvas->y = opt.y;
    canvas->w = opt.w;
    canvas->h = opt.h;
    ent->assign<CanvasConfigComponentSP>(canvas);

    ent->assign<DrawListComponentSP>(new DrawListComponent());

    EgoStateComponentSP ego(new EgoStateComponent());
    ent->assign<EgoStateComponentSP>(ego);

    ent->assign<CoachMessageComponentSP>(new CoachMessageComponent());
    ent->assign<DriverSpeechComponentSP>(new DriverSpeechComponent());
    ent->assign<SpeechQueueComponentSP>(new SpeechQueueComponent());

    OverlayConfigComponentSP cfg(new OverlayConfigComponent());
    cfg->mph = opt.mph;
    cfg->lateralExaggeration = opt.exaggeration;
    ent->assign<OverlayConfigComponentSP>(cfg);

    RefLineComponentSP ref(new RefLineComponent());
    {
        const std::string path = resolveBesideExe(opt.csv);
        std::string err;
        ref->loaded = loadRefLineCsv(path, ref->line, &err);
        ref->error = err;

        if (ref->loaded)
        {
            logLine("rline: loaded %zu points, %.0f m, reference lap %.2f s, from %s",
                    ref->line.pts.size(), ref->line.length, ref->line.lapTime, path.c_str());
        }
        else
        {
            // Not fatal: the overlay opens and shows the error, which is easier
            // to notice than a console line behind a fullscreen game.
            logLine("rline: %s", err.c_str());
        }
    }
    ent->assign<RefLineComponentSP>(ref);

    if (opt.demo)
    {
        logLine("rline: demo mode, replaying the reference lap");
        world->registerSystem(new DemoTelemetrySystem());
    }
    else
    {
        world->registerSystem(new IrTelemetrySystem());
    }
    if (!opt.noAudio)
    {
        BrakeAudioSystem *audio = new BrakeAudioSystem();
        audio->setInterval(opt.beepInterval);
        world->registerSystem(audio);
    }
    if (!opt.relay.empty() && !opt.noRelay)
    {
        std::string host = opt.relay;
        int port = wire::kDefaultPort;
        const size_t colon = host.find(':');
        if (colon != std::string::npos)
        {
            port = atoi(host.c_str() + colon + 1);
            host = host.substr(0, colon);
        }
        world->registerSystem(new TelemetryStreamSystem(host, port));
    }

    if (!opt.noCornerCoach)
        world->registerSystem(new CornerCoachSystem());

    // One voice, whatever produced the note: the relay pushes some, the corner
    // coach produces others, and two ISpVoice instances would talk over
    // each other.
    if (!opt.noSpeech)
        world->registerSystem(new CoachSpeechSystem());

    // Registered after the stream system so an utterance recognised this tick
    // waits for the next one before going up the wire. That costs 16 ms and
    // keeps the ordering obvious: speech is always queued, never sent from
    // inside the recogniser's own tick.
    if (!opt.noVoiceInput)
    {
        const std::string talkCfg = resolveBesideExe("rline-talk.txt");

        // --talk-button beats rline-talk.txt, which beats nothing bound at all.
        talk::Spec button;
        std::string source = opt.talkButton;
        if (source.empty())
        {
            FILE *f = fopen(talkCfg.c_str(), "r");
            if (f)
            {
                char buf[64] = {};
                if (fgets(buf, sizeof(buf), f))
                    source = buf;
                fclose(f);
            }
        }
        if (!source.empty() && !talk::parseSpec(source, &button))
            printf("rline: '%s' is not a device:button binding, ignoring it\n",
                   source.c_str());

        world->registerSystem(
            new VoiceInputSystem(button, talkCfg, opt.bindTalk, opt.talkChannel,
                                 opt.voiceInproc));
    }

    world->registerSystem(new ReflineOverlaySystem());
    WindowRenderingSystem *windowSys = new WindowRenderingSystem();
    if (opt.unlocked)
        windowSys->startUnlocked();
    world->registerSystem(windowSys);

    // IrTelemetrySystem blocks up to 16 ms waiting on iRacing, which paces the
    // loop at the sim's 60 Hz. Sleep keeps the CPU sane when it is not running,
    // and carries the whole pace in demo mode.
    const DWORD tStart = GetTickCount();
    DWORD tPrev = tStart;
    DWORD tLastLog = tStart;
    long long ticks = 0;

    for (;;)
    {
        const DWORD t = GetTickCount();
        world->tick((float)(t - tPrev));
        tPrev = t;
        ++ticks;

        if (opt.verbose && (t - tLastLog) >= 500)
        {
            tLastLog = t;
            logLine("t=%5.1fs ticks=%-8lld conn=%d track=%d lap=%-3d pct=%.4f "
                    "speed=%5.1f lat=%+6.2f xy=(%8.1f,%8.1f)",
                   (t - tStart) / 1000.0, ticks, (int)ego->connected,
                   (int)ego->onTrack, ego->lap, ego->pct, ego->speed,
                   ego->lateral, ego->x, ego->y);
        }

        if (opt.exitAfterMs > 0 && (t - tStart) >= (DWORD)opt.exitAfterMs)
            break;

        Sleep(opt.demo ? 8 : 1);
    }

    world->destroyWorld();
    return 0;
}
