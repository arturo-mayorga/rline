#ifndef session_policy_h_
#define session_policy_h_

#include <string>

// How much the coach is allowed to say, decided by which session is running.
//
// Until 2026-08-12 `CornerCoach` talked identically in practice, in a warmup
// and in a race, and that is a safety problem rather than a pace one. Two
// things are established about this driver from his own sessions: he loses the
// corner he is not thinking about, and incidents cluster within a lap or two of
// a new cue - three of the four on 2026-08-09 did. In practice a mistimed cue
// costs half a second, and the worst one measured cost 4.4 s. In a race it
// costs contact.
//
// So the rule is not "coach less when it matters", it is "introduce nothing
// when there is no lap left to correct it in":
//
//   full      practice and testing. Everything, including new faults.
//   confirm   warmup, and the out-laps of anything else. Only cues already
//             established - no corner may be raised for the first time. A ten
//             minute warmup is one out-lap and three or four flying laps, which
//             is enough to check a cue still lands and nowhere near enough to
//             teach one.
//   silent    race sessions. Nothing about technique at all. The relay can
//             still push notes; what is suppressed is the rig inventing them.
//
// Portable and testable: this file only decides, it never speaks.
namespace sessionpolicy
{
    enum Mode
    {
        kFull = 0,
        kConfirm,
        kSilent
    };

    const char *name(Mode m);

    // Parses the mode out of a relay command, so the policy can be overridden
    // from the analysis machine without a rebuild and without another exe copy
    // onto a machine with no dev tools.
    //
    //   "COACH|mode=full"     force it wide open
    //   "COACH|mode=confirm"
    //   "COACH|mode=silent"   the panic button
    //   "COACH|mode=auto"     back to deciding from the session type
    //
    // `isAuto` distinguishes "the relay asked for auto" from "the relay said
    // nothing", which are different: the first clears a previous override.
    bool parseCommand(const std::string &line, Mode *out, bool *isAuto);

    // iRacing reports the session type as free text in the session info YAML -
    // "Practice", "Qualify", "Race", "Warmup", "Lone Qualify", "Open Qualify",
    // "Offline Testing". Matched case-insensitively on substrings because the
    // exact strings vary by session configuration and a new one must degrade to
    // something safe rather than to nothing.
    //
    // Anything unrecognised returns kConfirm, never kFull: an unknown session
    // is more likely to be a race variant than a practice one, and the cost of
    // being wrong is asymmetric.
    Mode fromSessionType(const std::string &sessionType);

    // Pulls a session's type out of iRacing's session-info YAML.
    //
    // Not done with the SDK's parseYaml. That helper is opaque, only exercisable
    // against a live sim, and every documented spelling of its array syntax
    // ("Sessions:SessionNum:{2}:SessionType:" and the brace-attached variant)
    // returned nothing on 2026-08-12 - which is why the coach sat on `confirm`
    // for a whole race day and had to be pinned by hand. One field does not
    // justify a general parser, and a purpose-built extractor can be tested on
    // Linux like everything else here that decides what the rig says.
    //
    // Scans for the session whose "SessionNum:" matches, then takes the
    // "SessionType:" that follows it and before the next SessionNum. Returns
    // empty when it cannot find one, which `fromSessionType` reads as unknown
    // and closes down.
    std::string typeFromYaml(const std::string &yaml, int sessionNum);

    // The whole decision, including the states that override the session type.
    //
    //   onPitRoad     nothing is worth saying, and a pit lap is not a lap
    //   underYellow   his attention belongs on the track, not on a brake release
    //   outLap        the first flying lap has not happened yet, so any fault
    //                 remembered from before is stale - confirm at most
    //
    // `flags` is iRacing's SessionFlags bitfield; only the caution bits are
    // read, via kYellowFlagMask.
    Mode decide(const std::string &sessionType, bool onPitRoad, bool outLap,
                unsigned flags);

    // irsdk_yellow | irsdk_yellowWaving | irsdk_caution | irsdk_cautionWaving,
    // copied from src/irsdk/irsdk_defines.h:155-167 rather than included, so
    // this file stays free of the SDK and testable on Linux. A test asserts the
    // four values, which is the only guard against the SDK renumbering them.
    const unsigned kYellowFlagMask = 0x00000008u | 0x00000100u | 0x00004000u | 0x00008000u;

    bool isYellow(unsigned flags);
}

#endif
