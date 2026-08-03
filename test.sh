#!/bin/bash
# Runs the platform-independent tests. Works on Windows, Linux or macOS - no
# Windows SDK and no running copy of iRacing required.
set -e

g++ -std=c++17 -O2 -Wall -o /tmp/rline-test-refline \
    tests/test-refline.cpp src/refline.cpp

g++ -std=c++17 -O2 -Wall -o /tmp/rline-test-overlay \
    tests/test-overlay.cpp src/refline.cpp \
    src/components/rendering-comp.cpp src/systems/refline-overlay-sys.cpp

/tmp/rline-test-refline data/lap.csv
echo
/tmp/rline-test-overlay data/lap.csv

g++ -std=c++17 -O2 -Wall -o /tmp/rline-test-lateral \
    tests/test-lateral.cpp src/refline.cpp
echo
/tmp/rline-test-lateral data/lap.csv data/drive-roadamerica.csv

g++ -std=c++17 -O2 -Wall -o /tmp/rline-test-event-cue \
    tests/test-event-cue.cpp src/event-cue.cpp src/refline.cpp
echo
/tmp/rline-test-event-cue data/lap.csv
