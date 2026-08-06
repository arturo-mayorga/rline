// What the driver says has to survive the trip back without being able to
// forge a wire field or inject a coaching note. Recognition itself is SAPI and
// only runs on the rig; everything that decides what actually reaches the wire
// is here, and is checked on every platform.

#include "../src/voice-line.h"

#include <cstdio>
#include <string>

static int g_fail = 0;
static void check(bool ok, const char *what)
{
    printf("%s  %s\n", ok ? "  ok  " : "  FAIL", what);
    if (!ok) ++g_fail;
}

int main()
{
    printf("-- sanitising what the recogniser produced --\n");
    {
        check(voice::sanitize("  understeer at the exit  ") == "understeer at the exit",
              "trims and keeps ordinary speech intact");
        check(voice::sanitize("that felt\tbetter\r\nmuch better") ==
                  "that felt better much better",
              "newlines and tabs collapse to single spaces");
        check(voice::sanitize("a     b") == "a b", "runs of spaces collapse");
        check(voice::sanitize("   ").empty(), "whitespace only sanitises to nothing");
        check(voice::sanitize("").empty(), "empty stays empty");
    }

    printf("\n-- a misrecognition cannot forge a field --\n");
    {
        // The whole reason '|' is dropped rather than escaped: without this, a
        // recognised phrase could close the text field and append its own.
        const std::string nasty = "mark|conf=1.00|text=brake later into turn five";
        const std::string clean = voice::sanitize(nasty);
        check(clean.find('|') == std::string::npos, "pipes are removed from recognised text");

        const std::string line = voice::buildLine(nasty, 0.9f, false);
        size_t bars = 0;
        for (char c : line)
            if (c == '|') ++bars;
        check(bars == 3, "the built line has exactly the three real field boundaries");

        std::string got;
        float conf = -1;
        check(voice::parseLine(line, &got, &conf), "the line still parses");
        check(got.find("brake later") != std::string::npos,
              "the injected words survive only as plain text");
        check(conf > 0.85f && conf < 0.95f, "confidence is the one we set, not the injected one");
    }

    printf("\n-- control characters --\n");
    {
        std::string raw = "ok";
        raw.push_back('\0');
        raw += "then";
        raw.push_back((char)0x07);
        const std::string clean = voice::sanitize(raw);
        for (unsigned char c : clean)
            if (c < 0x20) { check(false, "no control characters survive"); break; }
        check(clean == "ok then", "embedded NUL and BEL become a single space");
    }

    printf("\n-- length is bounded --\n");
    {
        const std::string huge(voice::kMaxTextLen * 3, 'a');
        check(voice::sanitize(huge).size() <= voice::kMaxTextLen,
              "a stuck recogniser cannot flood the link");

        std::string words;
        while (words.size() < voice::kMaxTextLen * 2)
            words += "corner ";
        const std::string cut = voice::sanitize(words);
        check(cut.size() <= voice::kMaxTextLen, "wordy overflow is truncated too");
        check(cut.empty() || cut.back() != ' ', "truncation leaves no trailing space");
    }

    printf("\n-- UTF-8 is never cut in half --\n");
    {
        std::string s;
        while (s.size() < voice::kMaxTextLen + 10)
            s += "\xC3\xA9"; // e-acute, two bytes
        const std::string cut = voice::sanitize(s);
        // Every lead byte must be followed by the right number of continuations.
        bool valid = true;
        for (size_t i = 0; i < cut.size();)
        {
            const unsigned char c = (unsigned char)cut[i];
            size_t need = 0;
            if (c < 0x80) need = 0;
            else if ((c & 0xE0) == 0xC0) need = 1;
            else if ((c & 0xF0) == 0xE0) need = 2;
            else if ((c & 0xF8) == 0xF0) need = 3;
            else { valid = false; break; }
            if (i + need >= cut.size() + (need ? 0 : 1)) { valid = (i + need < cut.size()) || need == 0; }
            for (size_t k = 1; k <= need; ++k)
                if (i + k >= cut.size() || ((unsigned char)cut[i + k] & 0xC0) != 0x80)
                    { valid = false; break; }
            if (!valid) break;
            i += need + 1;
        }
        check(valid, "truncation lands on a character boundary");
    }

    printf("\n-- round trip --\n");
    {
        const std::string said = "that was worse, I lost the front at turn eight";
        const std::string line = voice::buildLine(said, 0.73f, false);
        printf("       %s\n", line.c_str());

        std::string got;
        float conf = 0;
        check(voice::parseLine(line, &got, &conf), "parses what it built");
        check(got == said, "text survives the round trip exactly");
        check(conf > 0.72f && conf < 0.74f, "confidence survives the round trip");
    }

    printf("\n-- confidence is clamped, never trusted raw --\n");
    {
        std::string got; float conf = 0;
        voice::parseLine(voice::buildLine("x", 5.0f, false), &got, &conf);
        check(conf <= 1.0f, "above one clamps to one");
        voice::parseLine(voice::buildLine("x", -3.0f, false), &got, &conf);
        check(conf >= 0.0f, "below zero clamps to zero");
    }

    printf("\n-- which engine produced it --\n");
    {
        // This matters more than the confidence number: dictation returns
        // confident nonsense on short phrases, a rule match does not.
        std::string got, via;
        float conf = 0;
        check(voice::parseLine(voice::buildLine("mark", 0.9f, true), &got, &conf, &via) &&
                  via == "cmd" && got == "mark",
              "a grammar match is tagged cmd");
        check(voice::parseLine(voice::buildLine("mark", 0.9f, false), &got, &conf, &via) &&
                  via == "free",
              "a dictation result is tagged free");
        check(voice::parseLine("HEAR|conf=0.5|text=old line", &got, &conf, &via) &&
                  via == "free",
              "a line from before via existed reads as free, not as a parse failure");
    }

    printf("\n-- rejecting things that are not utterances --\n");
    {
        std::string got; float conf = 0;
        check(!voice::parseLine("SAY|secs=8|text=turn five", &got, &conf),
              "a SAY line going the other way is not a HEAR line");
        check(!voice::parseLine("HEAR|conf=0.9", &got, &conf), "no text field is rejected");
        check(!voice::parseLine("HEAR|conf=0.9|text=", &got, &conf), "empty text is rejected");
        check(!voice::parseLine("", &got, &conf), "an empty line is rejected");
        check(!voice::parseLine("garbage", &got, &conf), "garbage is rejected");
        check(voice::buildLine("   ", 0.9f, false).empty(),
              "an utterance that sanitises to nothing produces no line at all");
    }

    printf("\n%s (%d failures)\n", g_fail == 0 ? "ALL PASSED" : "FAILURES", g_fail);
    return g_fail == 0 ? 0 : 1;
}
