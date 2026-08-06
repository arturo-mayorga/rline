#!/bin/bash
# Runs the platform-independent tests. Works on Windows, Linux or macOS - no
# Windows SDK and no running copy of iRacing required.
set -e

g++ -std=c++17 -O2 -Wall -o /tmp/rline-test-refline \
    tests/test-refline.cpp src/refline.cpp

g++ -std=c++17 -O2 -Wall -o /tmp/rline-test-overlay \
    tests/test-overlay.cpp src/refline.cpp src/grip-curve.cpp \
    src/corner-trace.cpp \
    src/components/rendering-comp.cpp src/systems/refline-overlay-sys.cpp

g++ -std=c++17 -O2 -Wall -o /tmp/rline-test-corner-trace \
    tests/test-corner-trace.cpp src/corner-trace.cpp src/refline.cpp

/tmp/rline-test-refline data/lap.csv
echo
/tmp/rline-test-overlay data/lap.csv
echo
/tmp/rline-test-corner-trace data/lap.csv

g++ -std=c++17 -O2 -Wall -o /tmp/rline-test-lateral \
    tests/test-lateral.cpp src/refline.cpp
echo
/tmp/rline-test-lateral data/lap.csv data/drive-roadamerica.csv

g++ -std=c++17 -O2 -Wall -o /tmp/rline-test-event-cue \
    tests/test-event-cue.cpp src/event-cue.cpp src/refline.cpp
echo
/tmp/rline-test-event-cue data/lap.csv

g++ -std=c++17 -O2 -Wall -o /tmp/rline-test-grip-curve \
    tests/test-grip-curve.cpp src/grip-curve.cpp
echo
/tmp/rline-test-grip-curve

# What the rig says out loud. Worth a suite of its own: two of these cues were
# removed because they told this driver to do more of what was hurting him.
g++ -std=c++17 -O2 -Wall -o /tmp/rline-test-corner-coach \
    tests/test-corner-coach.cpp src/corner-coach.cpp src/grip-curve.cpp \
    src/refline.cpp
echo
/tmp/rline-test-corner-coach data/lap.csv

# What the driver's own voice is allowed to put on the wire. Recognition is
# SAPI and rig-only, but a misrecognition must never be able to forge a field.
g++ -std=c++17 -O2 -Wall -o /tmp/rline-test-voice-line \
    tests/test-voice-line.cpp src/voice-line.cpp
echo
/tmp/rline-test-voice-line

# Which wheel button means "listen to me". A typo here would bind button 0,
# which is usually a paddle, and look exactly like broken hardware.
g++ -std=c++17 -O2 -Wall -o /tmp/rline-test-talk-button \
    tests/test-talk-button.cpp src/talk-button.cpp
echo
/tmp/rline-test-talk-button

# The fixed vocabulary. Built and tested but NOT yet loaded by rline: dictation
# is being given a chance first, at the driver's request.
g++ -std=c++17 -O2 -Wall -o /tmp/rline-test-voice-grammar \
    tests/test-voice-grammar.cpp src/voice-grammar.cpp
echo
/tmp/rline-test-voice-grammar data/voice-grammar.txt

# The order the rig says things in. Two producers used to share one slot and
# the speaker purged whatever was talking, cutting the driver off mid-sentence.
g++ -std=c++17 -O2 -Wall -o /tmp/rline-test-speech-queue \
    tests/test-speech-queue.cpp src/speech-queue.cpp
echo
/tmp/rline-test-speech-queue
