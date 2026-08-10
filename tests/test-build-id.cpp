// What the rig reports about itself on connection.
//
// Worth its own suite because the whole point of the feature is to be trusted
// without checking: if this line is wrong or silently truncated, it produces
// confident false reassurance, which is worse than the silence it replaces.

#include "../src/build-id.h"

#include <cstdio>
#include <cstring>
#include <string>

static int g_fail = 0;

static void check(bool ok, const char *what)
{
    printf("  %-5s %s\n", ok ? "ok" : "FAIL", what);
    if (!ok)
        ++g_fail;
}

int main()
{
    printf("-- hashing --\n");
    {
        const char *a = "hello";
        const char *b = "hellp";
        check(buildid::hashBytes(a, 5) == buildid::hashBytes(a, 5), "the same bytes hash the same");
        check(buildid::hashBytes(a, 5) != buildid::hashBytes(b, 5), "one changed byte changes the hash");
        check(buildid::hex8(0) == "00000000", "hex is zero padded to eight");
        check(buildid::hex8(0xdeadbeef) == "deadbeef", "hex is lowercase and full width");
        // A missing file must be distinguishable from an empty one: reporting
        // a hash for a reference that is not there is exactly the false
        // reassurance this whole feature exists to prevent.
        check(buildid::hashFile("/nonexistent/definitely/not/here.csv").empty(),
              "an unreadable file yields no hash, not a hash of nothing");
    }

    printf("\n-- round trip --\n");
    {
        buildid::Info in;
        in.track = "mugello";
        in.exeHash = "1a2b3c4d";
        in.built = "Aug  9 2026 20:21:03";
        in.proto = 2;
        in.refHash = "8be64c39";
        in.refPoints = 5021;
        in.refLength = 5189.0f;
        in.refLapTime = 83.63f;
        in.refCorners = 6;
        in.nameCount = 6;
        in.firstName = "San Donato";

        const std::string line = buildid::buildLine(in);
        printf("       %s\n", line.c_str());

        buildid::Info out;
        check(buildid::parseLine(line, &out), "a built line parses");
        check(out.track == "mugello", "track survives");
        check(out.exeHash == "1a2b3c4d", "exe hash survives");
        check(out.built == "Aug  9 2026 20:21:03", "a timestamp with spaces survives");
        check(out.proto == 2, "protocol version survives");
        check(out.refHash == "8be64c39", "reference hash survives");
        check(out.refPoints == 5021, "point count survives");
        check(out.refLength > 5188 && out.refLength < 5190, "track length survives");
        check(out.refLapTime > 83.6f && out.refLapTime < 83.7f, "lap time survives");
        check(out.refCorners == 6, "corner count survives");
        check(out.nameCount == 6, "name count survives");
        check(out.firstName == "San Donato", "a corner name with a space survives");
    }

    printf("\n-- what is not a build line --\n");
    {
        buildid::Info o;
        check(!buildid::parseLine("HEAR|conf=0.9|text=understeer", &o), "a HEAR line is refused");
        check(!buildid::parseLine("", &o), "an empty line is refused");
        check(!buildid::parseLine("BUILD", &o), "the bare word is refused");
        check(buildid::parseLine("BUILD|track=x", &o), "a minimal build line is accepted");
    }

    printf("\n-- a corner name cannot forge a field --\n");
    {
        // corner-names.txt is hand-edited on a machine with no dev tools. A
        // stray '|' in it must not be able to fabricate a track or a hash.
        buildid::Info in;
        in.track = "mugello";
        in.firstName = "San Donato|track=roadamerica|ref=deadbeef";

        buildid::Info out;
        check(buildid::parseLine(buildid::buildLine(in), &out), "it still parses");
        check(out.track == "mugello", "the injected track did not take");
        check(out.refHash.empty() || out.refHash == "?", "the injected hash did not take");
        check(out.firstName.find('|') == std::string::npos, "the pipe was stripped from the name");
    }

    printf("\n-- missing fields degrade, they do not fail --\n");
    {
        // An older rig talking to a newer relay, or the reverse. Partial
        // information beats refusing the line.
        buildid::Info o;
        check(buildid::parseLine("BUILD|track=mugello|first=San Donato", &o), "a sparse line parses");
        check(o.track == "mugello", "what is present is read");
        check(o.refPoints == 0 && o.proto == 0, "what is absent stays at its default");
        check(buildid::parseLine("BUILD|track=x|newfield=7|first=y", &o), "an unknown field is ignored");
        check(o.firstName == "y", "fields after an unknown one still parse");
    }

    printf("\n-- the report calls out the failures that have happened --\n");
    {
        buildid::Info wrong;
        wrong.track = "roadamerica";
        wrong.refCorners = 11;
        wrong.nameCount = 11;
        wrong.firstName = "Turn one";
        wrong.refPoints = 6000;
        const std::string d = buildid::describe(wrong, "mugello");
        printf("%s\n", d.c_str());
        check(d.find("WRONG TRACK") != std::string::npos, "a track mismatch is shouted about");

        buildid::Info noNames;
        noNames.track = "mugello";
        noNames.refCorners = 6;
        noNames.nameCount = 0;
        noNames.refPoints = 5021;
        check(buildid::describe(noNames, "mugello").find("detector numbers") != std::string::npos,
              "missing corner names are shouted about");

        buildid::Info noRef;
        noRef.track = "mugello";
        check(buildid::describe(noRef, "mugello").find("not on track") != std::string::npos,
              "a missing reference lap is shouted about");

        buildid::Info shortNames;
        shortNames.track = "mugello";
        shortNames.refCorners = 6;
        shortNames.nameCount = 4;
        shortNames.refPoints = 5021;
        check(buildid::describe(shortNames, "mugello").find("speak as numbers") != std::string::npos,
              "too few names is shouted about");

        buildid::Info right;
        right.track = "mugello";
        right.refCorners = 6;
        right.nameCount = 6;
        right.refPoints = 5021;
        right.firstName = "San Donato";
        const std::string ok = buildid::describe(right, "mugello");
        check(ok.find("***") == std::string::npos, "a correct rig draws no warnings");
    }

    printf("\n%s (%d failures)\n", g_fail == 0 ? "ALL PASSED" : "FAILURES", g_fail);
    return g_fail == 0 ? 0 : 1;
}
