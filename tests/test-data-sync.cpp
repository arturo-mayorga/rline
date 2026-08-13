// The rig fetching its track data from the relay.
//
// Worth its own suite for the same reason test-build-id is: this code decides
// what gets written over the reference lap the coaching is measured against. A
// bug here does not crash - it silently installs the wrong track, which is
// indistinguishable from a driver ignoring his coaching and cost an evening
// once already.
//
// The two properties under test are the ones a bad actor or a bad cable would
// go after: only the two known filenames can ever be written, and no bytes are
// accepted that do not hash to what was advertised.

#include "../src/build-id.h"
#include "../src/data-sync.h"

#include <cstdio>
#include <string>
#include <vector>

static int g_fail = 0;

static void check(bool ok, const char *what)
{
    printf("  %-5s %s\n", ok ? "ok" : "FAIL", what);
    if (!ok)
        ++g_fail;
}

int main()
{
    using namespace datasync;

    printf("-- the allowlist --\n");
    {
        check(isSyncFile("lap.csv"), "lap.csv is syncable");
        check(isSyncFile("corner-names.txt"), "corner-names.txt is syncable");
        check(isSyncFile("car-channels.txt"), "car-channels.txt is syncable");
        check(!isSyncFile("rline.exe"), "the executable is not syncable");
        check(!isSyncFile(""), "an empty name is not syncable");

        // Path traversal is not sanitised, it is unrepresentable: none of
        // these are equal to either allowed name, so all of them are refused
        // by the same comparison that refuses "banana".
        check(!isSyncFile("../lap.csv"), "a relative escape is refused");
        check(!isSyncFile("..\\..\\windows\\system32\\lap.csv"), "a windows escape is refused");
        check(!isSyncFile("/etc/passwd"), "an absolute path is refused");
        check(!isSyncFile("C:\\rline-coach\\lap.csv"), "a drive-qualified path is refused");
        check(!isSyncFile("lap.csv "), "a trailing space is refused");
        check(!isSyncFile("LAP.CSV"), "case must match exactly");
    }

    printf("\n-- WANT round trip --\n");
    {
        std::vector<FileState> have;
        have.push_back({"lap.csv", "455bbfae"});
        have.push_back({"corner-names.txt", ""});

        const std::string line = wantLine(have);

        std::vector<FileState> back;
        check(parseWant(line, &back), "a WANT line parses");
        check(back.size() == 2, "both files survive the trip");
        check(back.size() > 0 && back[0].name == "lap.csv" && back[0].hash == "455bbfae",
              "a hash survives exactly");

        // Absent and stale must stay distinguishable: one is a fresh install,
        // the other is the failure this feature exists to catch.
        check(back.size() > 1 && back[1].name == "corner-names.txt" && back[1].hash.empty(),
              "an absent file reports an empty hash, not a missing field");

        check(!parseWant("BUILD|track=mugello", NULL), "a BUILD line is not a WANT line");
        check(!parseWant("", NULL), "an empty line is not a WANT line");
        check(!parseWant("WANT|v=1|lap.csv=zzzzzzzz", NULL), "a non-hex hash is refused");
        check(!parseWant("WANT|v=1|lap.csv=455bb", NULL), "a short hash is refused");

        // A rig that asks for something not on the list gets nothing for it,
        // rather than the relay trying to serve a path it chose.
        std::vector<FileState> bad;
        bad.push_back({"../../secrets.txt", "455bbfae"});
        std::vector<FileState> parsed;
        parseWant(wantLine(bad), &parsed);
        check(parsed.empty(), "an off-list request is dropped, not served");
    }

    printf("\n-- reply headers --\n");
    {
        Header h;
        check(parseHeader(sameLine("lap.csv"), &h) && h.reply == kReplySame,
              "SAME parses");
        check(parseHeader(noneLine("lap.csv"), &h) && h.reply == kReplyNone,
              "NONE parses");

        check(parseHeader(dataLine("lap.csv", "455bbfae", 788134), &h) &&
                  h.reply == kReplyData && h.name == "lap.csv" &&
                  h.hash == "455bbfae" && h.len == 788134,
              "DATA carries name, hash and length");

        check(!parseHeader("HELLO|name=lap.csv", NULL), "an unknown verb is refused");
        check(!parseHeader("DATA|name=rline.exe|hash=455bbfae|len=10", NULL),
              "DATA for an off-list name is refused");
        check(!parseHeader("DATA|name=../lap.csv|hash=455bbfae|len=10", NULL),
              "DATA for a traversal name is refused");

        // Every one of these would leave the rig unable to check what it wrote.
        check(!parseHeader("DATA|name=lap.csv|len=10", NULL), "DATA without a hash is refused");
        check(!parseHeader("DATA|name=lap.csv|hash=|len=10", NULL), "DATA with an empty hash is refused");
        check(!parseHeader("DATA|name=lap.csv|hash=nothex11|len=10", NULL),
              "DATA with a non-hex hash is refused");
        check(!parseHeader("DATA|name=lap.csv|hash=455bbfae", NULL), "DATA without a length is refused");
        check(!parseHeader("DATA|name=lap.csv|hash=455bbfae|len=0", NULL),
              "a zero length is refused");
        check(!parseHeader("DATA|name=lap.csv|hash=455bbfae|len=-5", NULL),
              "a negative length is refused");
        check(!parseHeader("DATA|name=lap.csv|hash=455bbfae|len=999999999", NULL),
              "a length over the cap is refused");
        check(parseHeader("DATA|name=lap.csv|hash=455bbfae|len=33554432", NULL),
              "a length exactly at the cap is allowed");
    }

    printf("\n-- verification --\n");
    {
        const std::string body = "LapDist,Speed\n0,0\n1,50\n";
        const std::string hash = buildid::hex8(buildid::hashBytes(body.data(), body.size()));

        Header h;
        parseHeader(dataLine("lap.csv", hash, (uint32_t)body.size()), &h);

        check(verify(h, body.data(), body.size()), "the advertised bytes verify");

        // The failure that motivates the temp-file-then-rename: a short read
        // must never be renamed over a good reference lap.
        check(!verify(h, body.data(), body.size() - 1), "a truncated body fails");

        std::string flipped = body;
        flipped[3] ^= 0x01;
        check(!verify(h, flipped.data(), flipped.size()), "one flipped bit fails");

        Header same;
        parseHeader(sameLine("lap.csv"), &same);
        check(!verify(same, body.data(), body.size()),
              "only a DATA reply can verify, so SAME can never install bytes");
    }

    printf("\n%s\n", g_fail ? "FAILED" : "all data-sync tests passed");
    return g_fail ? 1 : 0;
}
