// Prints the detector's corner table for a reference lap.
//
// A new track means a new corner-names.txt, and that file can only be written
// against what the detector actually found - not against the circuit's own turn
// numbers, which it does not use and does not know. This prints exactly the
// RefCorner fields the mapping turns on, so the table in SKILL.md can be
// rebuilt from the reference lap rather than remembered.
//
// It also names the two things that have caused wrong callouts before: corners
// the reference takes with no brake (the anchor for the mapping) and spans that
// overlap their neighbour (which is why per-corner measurement must keep every
// corner live at once).
//
//   g++ -std=c++17 -O2 -o /tmp/dump-corners tools/dump-corners.cpp src/refline.cpp
//   /tmp/dump-corners data/muguello-ref.csv

#include "../src/refline.h"

#include <cstdio>
#include <string>

int main(int argc, char **argv)
{
    if (argc < 2)
    {
        printf("usage: dump-corners <reference-lap.csv>\n");
        return 2;
    }

    RefLine line;
    std::string err;
    if (!loadRefLineCsv(argv[1], line, &err))
    {
        printf("cannot load %s: %s\n", argv[1], err.c_str());
        return 1;
    }

    printf("%s\n  %d corners, %.0f m, %.2f s lap, brake=%d steer=%d\n\n",
           argv[1], (int)line.corners.size(), line.length, line.lapTime,
           (int)line.hasBrake, (int)line.hasSteer);

    printf("  n  entry turnin  apex   exit |  turnin_m  vmin  peakBrk peakStr  relPct   wotPct\n");
    for (size_t i = 0; i < line.corners.size(); ++i)
    {
        const RefCorner &c = line.corners[i];
        printf("%3d  %.3f %.3f  %.3f  %.3f |  %7.0f %5.0f    %.2f    %.2f  %7.3f  %7.3f%s\n",
               c.n, c.pctEntry, c.pctTurnIn, c.pctApex, c.pctExit,
               c.pctTurnIn * line.length, c.vmin * 3.6f, c.peakBrake, c.peakSteer,
               c.releasePct, c.fullThrottlePct,
               c.peakBrake < 0.01f ? "   <-- NO BRAKE (mapping anchor)" : "");
    }

    // Overlap is a property of the reference lap, so say out loud whether this
    // one has it rather than leaving the next reader to assume it does not.
    int overlaps = 0;
    printf("\n");
    for (size_t i = 1; i < line.corners.size(); ++i)
        if (line.corners[i].pctEntry < line.corners[i - 1].pctExit)
        {
            ++overlaps;
            printf("  overlap: corner %d opens at %.3f before corner %d closes at %.3f\n",
                   (int)i + 1, line.corners[i].pctEntry,
                   (int)i, line.corners[i - 1].pctExit);
        }
    printf("  %d of %d spans overlap their predecessor\n",
           overlaps, (int)line.corners.size());
    return 0;
}
