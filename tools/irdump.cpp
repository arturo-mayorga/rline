// Diagnostic: lists the telemetry channels a live iRacing session actually
// exposes, and the current value of the ones this overlay depends on.
// Not part of the overlay; built only when you need to check the SDK contract.

// windows.h first: irsdk_client.h refers to NULL without defining it, and only
// compiles where something else has already pulled it in.
#include <windows.h>

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "../src/irsdk/irsdk_client.h"
#include "../src/irsdk/irsdk_defines.h"

static const char *typeName(int t)
{
    switch (t)
    {
    case irsdk_char: return "char";
    case irsdk_bool: return "bool";
    case irsdk_int: return "int";
    case irsdk_bitField: return "bitField";
    case irsdk_float: return "float";
    case irsdk_double: return "double";
    default: return "?";
    }
}

// Logs the channels needed to work out iRacing's yaw / velocity sign
// conventions and to test whether lateral offset can be dead-reckoned.
static int runLog(const char *path, int seconds)
{
    static const char *chans[] = {
        "SessionTime", "Lap", "LapDist", "LapDistPct", "Speed",
        "Yaw", "YawNorth", "VelocityX", "VelocityY", "VelocityZ",
        "YawRate", "SteeringWheelAngle", "LatAccel", "IsOnTrack"};
    const int nChans = (int)(sizeof(chans) / sizeof(chans[0]));

    FILE *f = fopen(path, "w");
    if (!f)
    {
        printf("cannot write %s\n", path);
        return 1;
    }

    for (int i = 0; i < nChans; ++i)
        fprintf(f, "%s%s", chans[i], i + 1 < nChans ? "," : "\n");

    // irsdkCVar has no default constructor, so build the list rather than
    // declaring a fixed-size array.
    std::vector<irsdkCVar> vars;
    vars.reserve(nChans);
    for (int i = 0; i < nChans; ++i)
        vars.push_back(irsdkCVar(chans[i]));

    printf("logging %d channels to %s for %d s -- drive at least one full lap\n",
           nChans, path, seconds);

    const DWORD tEnd = GetTickCount() + (DWORD)seconds * 1000;
    int rows = 0;
    while (GetTickCount() < tEnd)
    {
        if (!irsdkClient::instance().waitForData(16))
            continue;

        for (int i = 0; i < nChans; ++i)
            fprintf(f, "%.9f%s", vars[i].getDouble(), i + 1 < nChans ? "," : "\n");

        if (++rows % 600 == 0)
        {
            printf("  %d rows, lap %d, %.1f%%\n",
                   rows, (int)vars[1].getDouble(), 100.0 * vars[3].getDouble());
            fflush(stdout);
            fflush(f);
        }
    }

    fclose(f);
    printf("done: %d rows\n", rows);
    return 0;
}

int main(int argc, char **argv)
{
    const bool listAll = (argc > 1 && !strcmp(argv[1], "--all"));
    const bool doLog = (argc > 3 && !strcmp(argv[1], "--log"));

    printf("waiting for iRacing...\n");
    for (int i = 0; i < 100 && !irsdkClient::instance().isConnected(); ++i)
        irsdkClient::instance().waitForData(100);

    if (!irsdkClient::instance().isConnected())
    {
        printf("not connected\n");
        return 1;
    }

    irsdkClient::instance().waitForData(100);

    if (doLog)
        return runLog(argv[2], atoi(argv[3]));

    const irsdk_header *hdr = irsdk_getHeader();
    printf("connected. numVars = %d\n\n", hdr ? hdr->numVars : -1);

    // The channels the overlay reads.
    const char *wanted[] = {"Lat", "Lon", "Alt", "LapDistPct", "Speed",
                            "IsOnTrack", "IsOnTrackCar", "PlayerCarIdx"};

    printf("%-16s %5s %-9s %6s  %s\n", "NAME", "IDX", "TYPE", "COUNT", "VALUE");
    printf("-------------------------------------------------------------\n");
    for (const char *name : wanted)
    {
        const int idx = irsdk_varNameToIndex(name);
        if (idx < 0)
        {
            printf("%-16s %5d %-9s %6s  <NOT PRESENT>\n", name, idx, "-", "-");
            continue;
        }

        const irsdk_varHeader *vh = irsdk_getVarHeaderEntry(idx);
        irsdkCVar v(name);
        printf("%-16s %5d %-9s %6d  %.9f\n",
               name, idx, vh ? typeName(vh->type) : "?", vh ? vh->count : 0,
               v.getDouble());
    }

    if (listAll && hdr)
    {
        printf("\nall channels:\n");
        for (int i = 0; i < hdr->numVars; ++i)
        {
            const irsdk_varHeader *vh = irsdk_getVarHeaderEntry(i);
            if (!vh)
                continue;
            printf("  %-32s %-9s count=%-3d  %s\n",
                   vh->name, typeName(vh->type), vh->count, vh->unit);
        }
    }

    return 0;
}
