#ifndef voice_grammar_h_
#define voice_grammar_h_

#include <string>
#include <vector>

// The fixed vocabulary the driver can say and have recognised exactly.
//
// Free dictation was tried first and measured on the rig on 2026-08-05. It
// fails on exactly the utterances this needs: long sentences came back around
// 44% confidence and roughly right, but short phrases - "hate all factory",
// "her as", "that restriction is very for" for "the transcription is very
// poor" - came back as nonsense. Dictation leans on sentence context, and a
// three-word phrase gives it none. Three-word phrases under load are the whole
// use case.
//
// A rule grammar searches only these phrases instead of the whole language, so
// a match is near-certain. Dictation still runs alongside it for anything not
// listed, and is reported at its own low confidence.
//
// The list lives in data/voice-grammar.txt, copied beside the exe, so the
// vocabulary changes without rebuilding anything on the rig.
namespace voice
{
    // Bounded so a mangled file cannot build a grammar big enough to slow
    // recognition down or exhaust memory on the rig.
    const size_t kMaxPhrases = 200;
    const size_t kMaxPhraseLen = 60;

    // Lower-cases, drops anything that is not a letter, digit, space or
    // apostrophe, and collapses whitespace. SAPI wants plain words: punctuation
    // in a rule phrase silently produces a transition that can never match.
    // Returns empty for anything unusable.
    std::string normalisePhrase(const std::string &raw);

    // Whole file contents -> the phrases to load. Skips blank lines and '#'
    // comments, normalises, and drops duplicates while keeping the file's
    // order so the list stays readable next to what the driver sees.
    std::vector<std::string> parseGrammar(const std::string &contents);

    // Same, reading from disk. Returns empty and sets error if unreadable.
    std::vector<std::string> loadGrammar(const std::string &path, std::string *error);
}

#endif
