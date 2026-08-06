// The fixed vocabulary the driver can say. Free dictation was measured on the
// rig and produced nonsense on short phrases, which are the ones that matter
// while driving; this list is what replaces it, so a phrase that silently
// fails to load is a coaching cue that can never be spoken.

#include "../src/voice-grammar.h"

#include <cstdio>
#include <fstream>
#include <set>
#include <string>

static int g_fail = 0;
static void check(bool ok, const char *what)
{
    printf("%s  %s\n", ok ? "  ok  " : "  FAIL", what);
    if (!ok) ++g_fail;
}

int main(int argc, char **argv)
{
    printf("-- normalising a phrase for SAPI --\n");
    {
        check(voice::normalisePhrase("Mark That") == "mark that", "lower-cased");
        check(voice::normalisePhrase("  understeer  ") == "understeer", "trimmed");
        check(voice::normalisePhrase("that   was    worse") == "that was worse",
              "inner whitespace collapses");
        check(voice::normalisePhrase("I lost the front.") == "i lost the front",
              "trailing punctuation is dropped, not left stuck to a word");
        check(voice::normalisePhrase("don't coach") == "don't coach",
              "apostrophes are kept - they are part of the word");
        check(voice::normalisePhrase("turn-in") == "turn in",
              "a hyphen separates words rather than joining them");
        check(voice::normalisePhrase("turn 8") == "turn 8", "digits survive");
        check(voice::normalisePhrase("").empty(), "empty stays empty");
        check(voice::normalisePhrase("!!!").empty(), "punctuation only yields nothing");
        check(voice::normalisePhrase(std::string(voice::kMaxPhraseLen + 1, 'a')).empty(),
              "an absurdly long phrase is refused");
    }

    printf("\n-- parsing the file --\n");
    {
        const std::string src =
            "# a comment\n"
            "\n"
            "   # an indented comment\n"
            "mark\n"
            "Understeer\n"
            "understeer\n"          // duplicate after normalising
            "  that was worse  \n"
            "!!!\n"                 // unusable
            "the carousel\n";

        const std::vector<std::string> p = voice::parseGrammar(src);
        check(p.size() == 4, "comments, blanks, duplicates and junk are all dropped");
        check(p[0] == "mark", "order follows the file");
        check(p[1] == "understeer", "first spelling wins");
        check(p[2] == "that was worse", "phrases with spaces survive");
        check(p[3] == "the carousel", "and the last line is not lost");

        std::set<std::string> uniq(p.begin(), p.end());
        check(uniq.size() == p.size(), "no duplicates reach SAPI");
    }

    printf("\n-- a mangled file cannot build an unbounded grammar --\n");
    {
        std::string huge;
        for (size_t i = 0; i < voice::kMaxPhrases * 3; ++i)
            huge += "phrase " + std::to_string(i) + "\n";
        check(voice::parseGrammar(huge).size() <= voice::kMaxPhrases,
              "the phrase count is capped");
    }

    printf("\n-- the real file that ships beside the exe --\n");
    {
        const char *path = argc > 1 ? argv[1] : "data/voice-grammar.txt";
        std::string err;
        const std::vector<std::string> p = voice::loadGrammar(path, &err);
        printf("       %s: %d phrases%s%s\n", path, (int)p.size(),
               err.empty() ? "" : ", error: ", err.c_str());

        check(!p.empty(), "the shipped grammar loads");
        check(err.empty(), "with no error");

        // The cues the coaching side actually depends on. If one of these were
        // dropped by a typo, the driver would say it and nothing would happen.
        const std::set<std::string> have(p.begin(), p.end());
        check(have.count("mark") > 0, "'mark' is present - the most valuable thing he can say");
        check(have.count("understeer") > 0, "'understeer' is present");
        check(have.count("oversteer") > 0, "'oversteer' is present");
        check(have.count("that was better") > 0, "'that was better' is present");
        check(have.count("that was worse") > 0, "'that was worse' is present");
        check(have.count("say again") > 0, "'say again' is present");

        // Corner names must match what the rig speaks, or he will name one
        // corner and be answered about another.
        check(have.count("turn eight") > 0, "'turn eight' is present");
        check(have.count("the carousel") > 0, "'the carousel' is present");

        for (const std::string &s : p)
        {
            if (s != voice::normalisePhrase(s))
            {
                check(false, "every shipped phrase is already normalised");
                break;
            }
        }
        check(true, "every shipped phrase survives normalisation unchanged");
    }

    printf("\n-- an unreadable file is reported, not silently empty --\n");
    {
        std::string err;
        const std::vector<std::string> p =
            voice::loadGrammar("no-such-grammar-file-here.txt", &err);
        check(p.empty(), "nothing is returned");
        check(!err.empty(), "and the reason is given");
    }

    printf("\n%s (%d failures)\n", g_fail == 0 ? "ALL PASSED" : "FAILURES", g_fail);
    return g_fail == 0 ? 0 : 1;
}
