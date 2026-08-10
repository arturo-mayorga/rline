#include "build-id.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace
{
    const uint32_t kFnvOffset = 2166136261u;
    const uint32_t kFnvPrime = 16777619u;

    std::string field(const std::string &line, const std::string &key)
    {
        // Fields are separated by '|' and the value runs to the next '|', so a
        // corner name with spaces needs no escaping. Anchored on "|key=" so
        // "names=" cannot match inside "firstnames=".
        const std::string needle = "|" + key + "=";
        size_t p = line.find(needle);
        if (p == std::string::npos)
            return std::string();
        p += needle.size();
        const size_t end = line.find('|', p);
        return line.substr(p, end == std::string::npos ? std::string::npos : end - p);
    }
}

namespace buildid
{
    uint32_t hashBytes(const void *data, size_t len)
    {
        const unsigned char *p = (const unsigned char *)data;
        uint32_t h = kFnvOffset;
        for (size_t i = 0; i < len; ++i)
        {
            h ^= p[i];
            h *= kFnvPrime;
        }
        return h;
    }

    std::string hex8(uint32_t h)
    {
        char buf[16];
        snprintf(buf, sizeof(buf), "%08x", (unsigned)h);
        return buf;
    }

    std::string hashFile(const std::string &path)
    {
        FILE *f = fopen(path.c_str(), "rb");
        if (!f)
            return std::string();

        uint32_t h = kFnvOffset;
        unsigned char buf[8192];
        size_t n;
        while ((n = fread(buf, 1, sizeof(buf), f)) > 0)
            for (size_t i = 0; i < n; ++i)
            {
                h ^= buf[i];
                h *= kFnvPrime;
            }
        fclose(f);
        return hex8(h);
    }

    std::string buildLine(const Info &in)
    {
        // Values must not contain '|' or they would forge a field boundary.
        // Only firstName can come from a file the driver edits, so it is the
        // only one worth guarding.
        std::string first = in.firstName;
        for (size_t i = 0; i < first.size(); ++i)
            if (first[i] == '|' || first[i] == '\n' || first[i] == '\r')
                first[i] = ' ';
        if (first.size() > 48)
            first.resize(48);

        char buf[512];
        snprintf(buf, sizeof(buf),
                 "BUILD|track=%s|proto=%d|exe=%s|built=%s"
                 "|ref=%s|pts=%zu|len=%.0f|lap=%.2f|corners=%d"
                 "|names=%d|first=%s",
                 in.track.empty() ? "?" : in.track.c_str(),
                 in.proto,
                 in.exeHash.empty() ? "?" : in.exeHash.c_str(),
                 in.built.empty() ? "?" : in.built.c_str(),
                 in.refHash.empty() ? "?" : in.refHash.c_str(),
                 in.refPoints, in.refLength, in.refLapTime, in.refCorners,
                 in.nameCount, first.c_str());
        return buf;
    }

    bool parseLine(const std::string &line, Info *out)
    {
        if (line.compare(0, 6, "BUILD|") != 0)
            return false;
        if (!out)
            return true;

        Info i;
        i.track = field(line, "track");
        i.exeHash = field(line, "exe");
        i.built = field(line, "built");
        i.refHash = field(line, "ref");
        i.firstName = field(line, "first");

        const std::string proto = field(line, "proto");
        const std::string pts = field(line, "pts");
        const std::string len = field(line, "len");
        const std::string lap = field(line, "lap");
        const std::string cor = field(line, "corners");
        const std::string nms = field(line, "names");

        if (!proto.empty()) i.proto = atoi(proto.c_str());
        if (!pts.empty()) i.refPoints = (size_t)atoll(pts.c_str());
        if (!len.empty()) i.refLength = (float)atof(len.c_str());
        if (!lap.empty()) i.refLapTime = (float)atof(lap.c_str());
        if (!cor.empty()) i.refCorners = atoi(cor.c_str());
        if (!nms.empty()) i.nameCount = atoi(nms.c_str());

        *out = i;
        return true;
    }

    std::string describe(const Info &in, const std::string &expectTrack)
    {
        char buf[1024];
        std::string s;

        snprintf(buf, sizeof(buf), "relay: rig build %s, compiled %s, protocol %d",
                 in.exeHash.c_str(), in.built.c_str(), in.proto);
        s += buf;

        snprintf(buf, sizeof(buf),
                 "\nrelay:   track %s - reference %s, %zu points, %.0f m, %.2f s, %d corners",
                 in.track.c_str(), in.refHash.c_str(), in.refPoints,
                 in.refLength, in.refLapTime, in.refCorners);
        s += buf;

        snprintf(buf, sizeof(buf), "\nrelay:   %d corner names, first is \"%s\"",
                 in.nameCount, in.firstName.c_str());
        s += buf;

        // The checks worth shouting about, in the order they have actually
        // caused trouble.
        if (!expectTrack.empty() && !in.track.empty() && in.track != expectTrack)
        {
            snprintf(buf, sizeof(buf),
                     "\nrelay:   *** WRONG TRACK: rig has %s, this machine deployed %s ***",
                     in.track.c_str(), expectTrack.c_str());
            s += buf;
        }
        if (in.nameCount > 0 && in.refCorners > 0 && in.nameCount < in.refCorners)
        {
            snprintf(buf, sizeof(buf),
                     "\nrelay:   *** only %d names for %d corners - the rest speak as numbers ***",
                     in.nameCount, in.refCorners);
            s += buf;
        }
        if (in.nameCount == 0)
            s += "\nrelay:   *** no corner names loaded - the rig will speak detector numbers ***";
        if (in.refPoints == 0)
            s += "\nrelay:   *** no reference lap loaded - the overlay will say 'not on track' ***";

        return s;
    }
}
