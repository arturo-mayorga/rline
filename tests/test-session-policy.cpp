// How much the coach may say, per session.
//
// Worth its own suite because this is the only thing standing between a
// feed-forward corner cue and a driver mid-overtake. Every assertion below is
// about failing SAFE: an unknown session closes down rather than opening up,
// and the pit lane and a caution beat whatever the session type says.

#include "../src/session-policy.h"

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string>

static int g_fail = 0;

static void check(bool ok, const char *what)
{
    printf("  %-5s %s\n", ok ? "ok" : "FAIL", what);
    if (!ok)
        ++g_fail;
}

// irsdk_defines.h cannot be included here - it pulls in <tchar.h> and this
// suite runs on Linux. So the header is read as text and the constants pulled
// out of it, which guards against a renumbering just as well as including it
// would, and keeps the portability that makes this testable at all.
static unsigned sdkFlag(const char *name)
{
    std::ifstream in("src/irsdk/irsdk_defines.h");
    if (!in)
        in.open("../src/irsdk/irsdk_defines.h");
    std::string line;
    const std::string key = name;
    while (std::getline(in, line))
    {
        const size_t at = line.find(key);
        if (at == std::string::npos)
            continue;
        // Must be the whole identifier, not a prefix: irsdk_yellow would
        // otherwise match irsdk_yellowWaving and silently test the wrong bit.
        const size_t after = at + key.size();
        if (after < line.size() && (isalnum((unsigned char)line[after]) || line[after] == '_'))
            continue;
        const size_t hex = line.find("0x", after);
        if (hex == std::string::npos)
            continue;
        return (unsigned)strtoul(line.c_str() + hex, NULL, 16);
    }
    return 0;
}

int main()
{
    using namespace sessionpolicy;

    const unsigned irsdk_yellow = sdkFlag("irsdk_yellow");
    const unsigned irsdk_yellowWaving = sdkFlag("irsdk_yellowWaving");
    const unsigned irsdk_caution = sdkFlag("irsdk_caution");
    const unsigned irsdk_cautionWaving = sdkFlag("irsdk_cautionWaving");
    const unsigned irsdk_green = sdkFlag("irsdk_green");
    const unsigned irsdk_checkered = sdkFlag("irsdk_checkered");
    const unsigned irsdk_blue = sdkFlag("irsdk_blue");

    printf("-- the flag mask really is iRacing's --\n");
    {
        // session-policy.h copies these values rather than including the SDK,
        // so it stays testable on Linux. This is the only thing that would
        // catch the SDK renumbering them.
        check(irsdk_yellow && irsdk_yellowWaving && irsdk_caution && irsdk_cautionWaving &&
                  irsdk_green && irsdk_checkered && irsdk_blue,
              "the SDK header was found and parsed");
        check((kYellowFlagMask & irsdk_yellow) == irsdk_yellow, "yellow is covered");
        check((kYellowFlagMask & irsdk_yellowWaving) == irsdk_yellowWaving,
              "waved yellow is covered");
        check((kYellowFlagMask & irsdk_caution) == irsdk_caution, "caution is covered");
        check((kYellowFlagMask & irsdk_cautionWaving) == irsdk_cautionWaving,
              "waved caution is covered");
        check(!isYellow(irsdk_green), "green is not yellow");
        check(!isYellow(irsdk_checkered), "checkered is not yellow");
        check(!isYellow(irsdk_blue), "blue is not yellow");
        check(isYellow(irsdk_green | irsdk_yellow), "a yellow among other flags still counts");
    }

    printf("\n-- session type --\n");
    {
        check(fromSessionType("Practice") == kFull, "practice is wide open");
        check(fromSessionType("Offline Testing") == kFull, "testing is wide open");
        check(fromSessionType("Race") == kSilent, "a race says nothing");
        check(fromSessionType("Qualify") == kSilent, "qualifying says nothing");
        check(fromSessionType("Lone Qualify") == kSilent, "lone qualifying says nothing");
        check(fromSessionType("Open Qualify") == kSilent, "open qualifying says nothing");
        check(fromSessionType("Warmup") == kConfirm, "the warmup confirms only");

        check(fromSessionType("PRACTICE") == kFull, "matching ignores case");
        check(fromSessionType("race") == kSilent, "matching ignores case both ways");

        // The race-day order that matters: iRacing's warmup session must not
        // fall through to practice just because someone names it
        // "Practice/Warmup".
        check(fromSessionType("Practice / Warmup") == kConfirm,
              "warmup beats practice when both appear");

        check(fromSessionType("") == kConfirm, "an empty type closes down");
        check(fromSessionType("Heat Two Feature") == kConfirm,
              "an unknown session closes down, never opens up");
    }

    printf("\n-- session type out of the YAML --\n");
    {
        // Tomorrow's league format, as iRacing lays it out.
        const std::string y =
            "---\n"
            "WeekendInfo:\n"
            " TrackName: mugello\n"
            "SessionInfo:\n"
            " Sessions:\n"
            " - SessionNum: 0\n"
            "   SessionLaps: unlimited\n"
            "   SessionType: Practice\n"
            " - SessionNum: 1\n"
            "   SessionLaps: 14\n"
            "   SessionType: Race\n"
            " - SessionNum: 2\n"
            "   SessionType: Warmup\n"
            " - SessionNum: 3\n"
            "   SessionType: Race\n"
            "...\n";

        check(typeFromYaml(y, 0) == "Practice", "session 0 is practice");
        check(typeFromYaml(y, 1) == "Race", "session 1 is the sprint");
        check(typeFromYaml(y, 2) == "Warmup", "session 2 is the warmup");
        check(typeFromYaml(y, 3) == "Race", "session 3 is the feature");
        check(typeFromYaml(y, 9).empty(), "a session that is not there yields nothing");
        check(typeFromYaml("", 0).empty(), "empty yaml yields nothing");

        // End to end: the whole point is that a race day drives itself.
        check(decide(typeFromYaml(y, 0), false, false, 0) == kFull, "practice runs full");
        check(decide(typeFromYaml(y, 1), false, false, 0) == kSilent, "the sprint is silent");
        check(decide(typeFromYaml(y, 2), false, false, 0) == kConfirm, "the warmup confirms");
        check(decide(typeFromYaml(y, 3), false, false, 0) == kSilent, "the feature is silent");

        // A session with no SessionType must not borrow the next one's - that
        // would report "Race" during a practice.
        const std::string gap =
            "SessionInfo:\n Sessions:\n - SessionNum: 0\n   SessionLaps: 5\n"
            " - SessionNum: 1\n   SessionType: Race\n";
        check(typeFromYaml(gap, 0).empty(),
              "a session without a type does not borrow the next session's");
        check(typeFromYaml(gap, 1) == "Race", "the next session still reads correctly");

        // Two-digit session numbers, and a value with a trailing space.
        const std::string wide =
            "SessionInfo:\n Sessions:\n - SessionNum: 12\n   SessionType: Open Qualify  \n";
        check(typeFromYaml(wide, 12) == "Open Qualify",
              "multi-digit numbers parse and trailing space is trimmed");
        check(typeFromYaml(wide, 1).empty(), "12 is not matched by asking for 1");
    }

    printf("\n-- the overrides --\n");
    {
        check(decide("Practice", true, false, 0) == kSilent, "the pit lane silences practice");
        check(decide("Practice", false, false, irsdk_yellow) == kSilent,
              "a yellow silences practice");
        check(decide("Practice", false, true, 0) == kConfirm,
              "an out-lap in practice drops to confirm");
        check(decide("Practice", false, false, 0) == kFull, "a clean practice lap is full");

        check(decide("Warmup", false, true, 0) == kConfirm, "an out-lap cannot raise the mode");
        check(decide("Race", false, false, 0) == kSilent, "a race is silent regardless");
        check(decide("Race", false, true, irsdk_green) == kSilent, "green does not open a race up");
    }

    printf("\n-- relay override --\n");
    {
        Mode m = kFull;
        bool automatic = true;

        check(parseCommand("COACH|mode=silent", &m, &automatic) && m == kSilent && !automatic,
              "silent parses");
        check(parseCommand("COACH|mode=full", &m, &automatic) && m == kFull && !automatic,
              "full parses");
        check(parseCommand("COACH|mode=confirm", &m, &automatic) && m == kConfirm,
              "confirm parses");
        check(parseCommand("COACH|mode=off", &m, &automatic) && m == kSilent,
              "off is a synonym for silent");
        check(parseCommand("COACH|mode=on", &m, &automatic) && m == kFull,
              "on is a synonym for full");

        // auto must be distinguishable from "no command", because it is what
        // clears a previous override.
        check(parseCommand("COACH|mode=auto", &m, &automatic) && automatic,
              "auto reports itself as auto");

        check(!parseCommand("SAY|secs=10|text=hello", NULL, NULL), "a SAY is not a COACH");
        check(!parseCommand("COACH|mode=", NULL, NULL), "an empty mode is refused");
        check(!parseCommand("COACH|mode=banana", NULL, NULL),
              "an unrecognised mode is refused, not guessed");
        check(!parseCommand("", NULL, NULL), "an empty line is refused");

        // The failure that would matter: a refused command must leave the
        // caller's mode untouched rather than zeroing it to kFull.
        Mode keep = kSilent;
        parseCommand("COACH|mode=banana", &keep, NULL);
        check(keep == kSilent, "a refused command does not disturb the current mode");
    }

    printf("\n%s\n", g_fail ? "FAILED" : "all session-policy tests passed");
    return g_fail ? 1 : 0;
}
