#ifndef voice_line_h_
#define voice_line_h_

#include <string>

// The driver's voice on its way back to the analysis machine.
//
// The rig recognises speech locally and sends one line per utterance up the
// same socket the telemetry uses:
//
//   HEAR|conf=0.87|text=that felt like understeer at the exit
//
// Kept deliberately in the same shape as the SAY lines going the other way, so
// the whole protocol stays splittable on '|' and '=' with no parser.
//
// This half is free of windows.h on purpose. Recognition itself needs SAPI and
// only builds on the rig, but everything that decides what actually reaches the
// wire - and therefore what a coaching decision can be based on - is testable
// on any machine.
namespace voice
{
    // Long enough for anything said in one breath between corners; short
    // enough that a stuck recogniser cannot flood the link.
    const size_t kMaxTextLen = 400;

    // Make an arbitrary recognised string safe to put in one wire field.
    // Removes '|' (which would forge a field boundary), flattens control
    // characters and newlines to spaces, collapses runs of whitespace, trims,
    // and truncates to kMaxTextLen without splitting a UTF-8 sequence.
    std::string sanitize(const std::string &raw);

    // "HEAR|conf=0.87|via=cmd|text=..." - confidence clamped to 0..1. Returns
    // an empty string if the text sanitises away to nothing, so callers can
    // simply not send it rather than putting an empty utterance on the wire.
    //
    // via says which engine produced it: "cmd" for the fixed-phrase grammar,
    // "free" for dictation. That distinction matters more than the confidence
    // number, because dictation on this rig returns confident nonsense on short
    // phrases while a rule match is drawn from a list of known-good cues.
    std::string buildLine(const std::string &text, float confidence, bool fromGrammar);

    // Parse one such line. text is taken as the whole remainder after
    // "|text=", so it needs no escaping. Returns false for anything that is
    // not a HEAR line or that carries no text. Unknown fields are ignored, so
    // an older reader still parses a newer line.
    bool parseLine(const std::string &line, std::string *text, float *confidence,
                   std::string *via = nullptr);
}

#endif
