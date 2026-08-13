#include "data-sync.h"

#include "build-id.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace
{
    // Same shape as build-id.cpp's: fields separated by '|', value runs to the
    // next '|', anchored on "|key=" so one key cannot match inside another.
    std::string field(const std::string &line, const std::string &key)
    {
        const std::string needle = "|" + key + "=";
        size_t p = line.find(needle);
        if (p == std::string::npos)
            return std::string();
        p += needle.size();
        const size_t end = line.find('|', p);
        return line.substr(p, end == std::string::npos ? std::string::npos : end - p);
    }

    bool hasField(const std::string &line, const std::string &key)
    {
        return line.find("|" + key + "=") != std::string::npos;
    }

    // A hash is exactly eight lowercase hex characters, or empty. Anything else
    // is a garbled line, and accepting it would mean comparing a cached file
    // against a value that can never match - a permanent redownload loop that
    // looks like a working sync.
    bool wellFormedHash(const std::string &h)
    {
        if (h.empty())
            return true;
        if (h.size() != 8)
            return false;
        for (char c : h)
            if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f')))
                return false;
        return true;
    }
}

namespace datasync
{
    // main.cpp opens lap.csv, corner-coach-sys.cpp opens corner-names.txt, and
    // telemetry-stream-sys.cpp opens car-channels.txt. Those three names are the
    // entire contract, so they are the entire list.
    const char *const kSyncFiles[] = {"lap.csv", "corner-names.txt", "car-channels.txt"};
    const int kSyncFileCount = 3;

    bool isSyncFile(const std::string &name)
    {
        for (int i = 0; i < kSyncFileCount; ++i)
            if (name == kSyncFiles[i])
                return true;
        return false;
    }

    std::string wantLine(const std::vector<FileState> &have)
    {
        std::string s = "WANT|v=";
        s += std::to_string((int)kVersion);
        for (const FileState &f : have)
        {
            // Silently skipped rather than rejected: a caller that asks for
            // something unknown gets no answer for it, which is the same
            // outcome as asking and being told NONE.
            if (!isSyncFile(f.name))
                continue;
            s += "|" + f.name + "=" + f.hash;
        }
        return s;
    }

    bool parseWant(const std::string &line, std::vector<FileState> *out)
    {
        if (line.compare(0, 5, "WANT|") != 0)
            return false;

        // Validation runs whether or not the caller wants the result. An
        // early-out on a null `out` would make parseWant(line, NULL) answer
        // "yes, well formed" for a line it never looked at, and the only
        // callers that pass NULL are the ones asking exactly that question.
        std::vector<FileState> v;
        for (int i = 0; i < kSyncFileCount; ++i)
        {
            const std::string name = kSyncFiles[i];
            if (!hasField(line, name))
                continue; // this rig did not ask for it

            FileState f;
            f.name = name;
            f.hash = field(line, name);
            if (!wellFormedHash(f.hash))
                return false;
            v.push_back(f);
        }

        if (out)
            *out = v;
        return true;
    }

    std::string sameLine(const std::string &name)
    {
        return "SAME|name=" + name;
    }

    std::string noneLine(const std::string &name)
    {
        return "NONE|name=" + name;
    }

    std::string dataLine(const std::string &name, const std::string &hash, uint32_t len)
    {
        char buf[64];
        snprintf(buf, sizeof(buf), "%u", (unsigned)len);
        return "DATA|name=" + name + "|hash=" + hash + "|len=" + buf;
    }

    bool parseHeader(const std::string &line, Header *out)
    {
        Header h;
        if (line.compare(0, 5, "SAME|") == 0)
            h.reply = kReplySame;
        else if (line.compare(0, 5, "NONE|") == 0)
            h.reply = kReplyNone;
        else if (line.compare(0, 5, "DATA|") == 0)
            h.reply = kReplyData;
        else
            return false;

        h.name = field(line, "name");
        if (!isSyncFile(h.name))
            return false; // the allowlist, enforced before anything is written

        if (h.reply == kReplyData)
        {
            h.hash = field(line, "hash");
            // An unverifiable download is worse than none: it would replace a
            // known-good cached file with bytes nothing has vouched for.
            if (h.hash.size() != 8 || !wellFormedHash(h.hash))
                return false;

            const std::string len = field(line, "len");
            if (len.empty())
                return false;
            const long long n = atoll(len.c_str());
            if (n <= 0 || n > (long long)kMaxFileBytes)
                return false;
            h.len = (uint32_t)n;
        }

        if (out)
            *out = h;
        return true;
    }

    bool verify(const Header &h, const void *bytes, size_t len)
    {
        if (h.reply != kReplyData)
            return false;
        if (len != (size_t)h.len)
            return false;
        return buildid::hex8(buildid::hashBytes(bytes, len)) == h.hash;
    }
}
