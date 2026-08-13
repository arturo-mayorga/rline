// relay - receives telemetry from the sim rig and spools it to disk.
//
// This runs on the analysis machine, not the rig, which is the whole point:
// the rig executable stays frozen while this can be rewritten whenever the
// analysis needs something different. The rig sends every scalar channel; what
// is worth keeping is decided here.
//
// Writes one CSV per lap into <dir>/laps/, plus live.csv holding the most
// recent few seconds for anything that wants near-real-time data.

#include <windows.h>
#include <ws2tcpip.h>

#include <cstdio>
#include <cstring>
#include <ctime>
#include <deque>
#include <string>
#include <vector>

#include "../src/build-id.h"
#include "../src/data-sync.h"
#include "../src/voice-line.h"
#include "../src/wire.h"

#pragma comment(lib, "Ws2_32.lib")

namespace
{
    const int kLiveSeconds = 10;
    const int kLiveRows = kLiveSeconds * 60;

    std::string g_dir = "C:\\rline-coach";

    bool recvAll(SOCKET s, char *dst, int len)
    {
        int got = 0;
        while (got < len)
        {
            const int n = recv(s, dst + got, len - got, 0);
            if (n <= 0)
                return false;
            got += n;
        }
        return true;
    }

    void ensureDir(const std::string &path)
    {
        CreateDirectoryA(path.c_str(), NULL);
    }

    std::string trimField(const char *raw, int max)
    {
        std::string s(raw, strnlen(raw, max));
        while (!s.empty() && (s.back() == ' ' || s.back() == '\0'))
            s.pop_back();
        return s;
    }

    void writeHeader(FILE *f, const std::vector<wire::ChannelDesc> &ch)
    {
        for (size_t i = 0; i < ch.size(); ++i)
            fprintf(f, "%s%s", trimField(ch[i].name, wire::kNameLen).c_str(),
                    i + 1 < ch.size() ? "," : "\n");
    }

    void writeRow(FILE *f, const std::vector<float> &v)
    {
        for (size_t i = 0; i < v.size(); ++i)
            fprintf(f, "%.6g%s", v[i], i + 1 < v.size() ? "," : "\n");
    }

    std::string stamp()
    {
        const time_t t = time(NULL);
        char buf[32] = {};
        strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", localtime(&t));
        return std::string(buf);
    }

    // What the driver said, appended for the coaching side to pick up. This is
    // the mirror of outbox.txt going the other way, with one difference: it is
    // never truncated. A note to the driver is consumed once; a thing he said
    // is a record, and the transcript of a session is worth keeping.
    void appendHeard(const std::string &line)
    {
        const std::string path = g_dir + "\\inbox.txt";
        FILE *f = fopen(path.c_str(), "a");
        if (!f)
            return;
        fprintf(f, "%s %s\n", stamp().c_str(), line.c_str());
        fclose(f);
    }

    // What this machine believes it deployed. Compiled in from the same CMake
    // variable that chooses which reference lap ships, so the two cannot drift
    // apart in the one direction that matters - relay and rig are built from
    // one tree, the rig is then copied by hand, and it is the copy that goes
    // stale.
#ifdef RLINE_TRACK_NAME
    const char *kExpectTrack = RLINE_TRACK_NAME;
#else
    const char *kExpectTrack = "";
#endif

    // Where the files the rig may fetch live. Beside relay.exe, which is where
    // CMake already copies the pair chosen by RLINE_TRACK - so what the relay
    // serves and what this machine built cannot disagree.
    std::string g_dataDir;

    std::string besideExe()
    {
        char buf[MAX_PATH] = {};
        const DWORD n = GetModuleFileNameA(NULL, buf, MAX_PATH);
        if (n == 0 || n >= MAX_PATH)
            return ".";
        std::string p(buf, n);
        const size_t slash = p.find_last_of("\\/");
        return slash == std::string::npos ? std::string(".") : p.substr(0, slash);
    }

    bool sendAllBytes(SOCKET s, const char *data, int len)
    {
        int sent = 0;
        while (sent < len)
        {
            const int n = send(s, data + sent, len - sent, 0);
            if (n <= 0)
                return false;
            sent += n;
        }
        return true;
    }

    bool sendLine(SOCKET s, const std::string &line)
    {
        const std::string withNl = line + "\n";
        return sendAllBytes(s, withNl.data(), (int)withNl.size());
    }

    bool readFileBytes(const std::string &path, std::string *out)
    {
        FILE *f = fopen(path.c_str(), "rb");
        if (!f)
            return false;
        std::string body;
        char buf[65536];
        size_t n;
        while ((n = fread(buf, 1, sizeof(buf), f)) > 0)
            body.append(buf, n);
        fclose(f);
        *out = body;
        return true;
    }

    // A rig asking what its track data should be, on its own short-lived
    // connection. Serving this is the whole reason the driver no longer copies
    // a folder: the executable is still his to install, the data is ours to
    // push.
    //
    // Deliberately synchronous and inline in the accept loop. It happens once
    // at rig startup, before any telemetry connection exists, so there is
    // nothing for it to block - and keeping it single-threaded means it cannot
    // interleave with the 60 Hz path at all.
    void serveSync(SOCKET s)
    {
        uint16_t version = 0;
        uint16_t lineLen = 0;
        if (!recvAll(s, (char *)&version, 2) || !recvAll(s, (char *)&lineLen, 2))
            return;
        if (lineLen == 0 || lineLen > wire::kMaxTextLen)
            return;

        std::string line(lineLen, '\0');
        if (!recvAll(s, &line[0], lineLen))
            return;

        std::vector<datasync::FileState> want;
        if (!datasync::parseWant(line, &want))
        {
            printf("relay: <- unparseable sync request, ignored\n");
            return;
        }

        printf("relay: <- rig asking for track data (protocol %u)\n", (unsigned)version);

        for (const datasync::FileState &f : want)
        {
            const std::string path = g_dataDir + "\\" + f.name;

            std::string body;
            if (!readFileBytes(path, &body) || body.empty())
            {
                // The relay genuinely has not got it. Say so rather than
                // sending nothing: the rig keeps whatever it had cached, and
                // the reason is visible in both logs.
                printf("relay:    %s - not here, rig keeps its own\n", f.name.c_str());
                if (!sendLine(s, datasync::noneLine(f.name)))
                    return;
                continue;
            }

            const std::string hash =
                buildid::hex8(buildid::hashBytes(body.data(), body.size()));

            if (hash == f.hash)
            {
                printf("relay:    %s - already current [%s]\n", f.name.c_str(), hash.c_str());
                if (!sendLine(s, datasync::sameLine(f.name)))
                    return;
                continue;
            }

            printf("relay:    %s - sending %zu bytes [%s], rig had [%s]\n",
                   f.name.c_str(), body.size(), hash.c_str(),
                   f.hash.empty() ? "none" : f.hash.c_str());

            if (!sendLine(s, datasync::dataLine(f.name, hash, (uint32_t)body.size())))
                return;
            if (!sendAllBytes(s, body.data(), (int)body.size()))
                return;
        }
    }

    // The rig's self-report, left where it can be read without scraping the
    // log. Truncated each connection: this is current state, not a history.
    void writeRigBuild(const std::string &line, const std::string &report)
    {
        const std::string path = g_dir + "\\rig-build.txt";
        FILE *f = fopen(path.c_str(), "w");
        if (!f)
            return;
        fprintf(f, "%s connected\n%s\n\n%s\n", stamp().c_str(), report.c_str(), line.c_str());
        fclose(f);
    }
}

int main(int argc, char **argv)
{
    int port = wire::kDefaultPort;
    for (int i = 1; i < argc; ++i)
    {
        if (!strcmp(argv[i], "--dir") && i + 1 < argc)
            g_dir = argv[++i];
        else if (!strcmp(argv[i], "--port") && i + 1 < argc)
            port = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--data") && i + 1 < argc)
            g_dataDir = argv[++i];
        else if (!strcmp(argv[i], "--help"))
        {
            printf("relay [--dir <path>] [--port <n>] [--data <path>]\n"
                   "  Receives rline telemetry and writes <dir>/laps/lap-NNNN.csv\n"
                   "  plus <dir>/live.csv. Lines typed here are sent to the rig.\n"
                   "  Serves lap.csv and corner-names.txt to a rig that asks for\n"
                   "  them, from --data (default: beside relay.exe).\n");
            return 0;
        }
    }

    if (g_dataDir.empty())
        g_dataDir = besideExe();

    setvbuf(stdout, NULL, _IONBF, 0);

    ensureDir(g_dir);
    ensureDir(g_dir + "\\laps");

    WSADATA wsa = {};
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0)
    {
        printf("relay: winsock failed\n");
        return 1;
    }

    SOCKET listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    BOOL reuse = TRUE;
    setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, (const char *)&reuse, sizeof(reuse));

    sockaddr_in addr = {};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY; // the rig connects in from the LAN
    addr.sin_port = htons((u_short)port);

    if (bind(listener, (sockaddr *)&addr, sizeof(addr)) != 0 ||
        listen(listener, 1) != 0)
    {
        printf("relay: cannot listen on port %d\n", port);
        return 1;
    }

    printf("relay: listening on port %d, writing to %s\n", port, g_dir.c_str());

    // What a rig will be given if it asks. Printed at startup rather than only
    // on request, because the failure this feature exists to prevent is a
    // wrong-track file nobody looked at - and an empty or missing pair here is
    // the one state where the rig silently keeps whatever it already had.
    printf("relay: serving track data from %s\n", g_dataDir.c_str());
    for (int i = 0; i < datasync::kSyncFileCount; ++i)
    {
        const std::string name = datasync::kSyncFiles[i];
        const std::string hash = buildid::hashFile(g_dataDir + "\\" + name);
        if (hash.empty())
            printf("relay:   *** %s MISSING - a rig asking for it keeps its own ***\n",
                   name.c_str());
        else
            printf("relay:   %s [%s]\n", name.c_str(), hash.c_str());
    }

    for (;;)
    {
        printf("relay: waiting for the rig...\n");
        SOCKET s = accept(listener, NULL, NULL);
        if (s == INVALID_SOCKET)
            continue;

        // A client that connects and then says nothing must not be able to
        // wedge the relay: without a timeout the handshake read blocks forever
        // and no other connection can be accepted.
        DWORD handshakeTimeout = 10000;
        setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char *)&handshakeTimeout,
                   sizeof(handshakeTimeout));

        // Two kinds of client arrive on this port and the first four bytes say
        // which. A telemetry connection opens with wire::kMagic and lasts the
        // session; a data-sync connection opens with datasync::kMagic, is
        // served in a few hundred milliseconds at rig startup, and closes.
        wire::HelloHeader h = {};
        if (!recvAll(s, (char *)&h.magic, sizeof(h.magic)))
        {
            printf("relay: bad handshake (or silent client), dropping\n");
            closesocket(s);
            continue;
        }

        if (h.magic == datasync::kMagic)
        {
            serveSync(s);
            closesocket(s);
            continue;
        }

        if (h.magic != wire::kMagic ||
            !recvAll(s, (char *)&h.version, sizeof(h) - sizeof(h.magic)))
        {
            printf("relay: bad handshake (or silent client), dropping\n");
            closesocket(s);
            continue;
        }

        // Streaming can legitimately pause - in the pits, in menus - so no
        // read timeout from here. Keepalive still reaps a rig that vanished
        // without closing the connection.
        DWORD noTimeout = 0;
        setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char *)&noTimeout, sizeof(noTimeout));
        BOOL keepalive = TRUE;
        setsockopt(s, SOL_SOCKET, SO_KEEPALIVE, (const char *)&keepalive, sizeof(keepalive));

        std::vector<wire::ChannelDesc> channels(h.channelCount);
        if (!recvAll(s, (char *)channels.data(),
                     (int)(channels.size() * sizeof(wire::ChannelDesc))))
        {
            closesocket(s);
            continue;
        }

        printf("relay: connected, %d channels\n", (int)channels.size());

        // Locate the channels the relay itself needs to split laps.
        int lapCol = -1, pctCol = -1;
        for (size_t i = 0; i < channels.size(); ++i)
        {
            const std::string n = trimField(channels[i].name, wire::kNameLen);
            if (n == "Lap")
                lapCol = (int)i;
            else if (n == "LapDistPct")
                pctCol = (int)i;
        }

        const std::string outbox = g_dir + "\\outbox.txt";
        printf("relay: append lines to %s to send them to the rig\n", outbox.c_str());

        std::vector<float> row(channels.size());
        std::deque<std::vector<float>> live;

        FILE *lapFile = NULL;
        int currentLap = -1;
        long rowsThisLap = 0;
        uint32_t lastSeq = 0;
        bool first = true;
        bool sawBuild = false;

        for (;;)
        {
            uint8_t marker = 0;
            if (!recvAll(s, (char *)&marker, 1))
                break;

            // Recognised speech, interleaved with the telemetry.
            if (marker == wire::kTextMarker)
            {
                uint16_t tlen = 0;
                if (!recvAll(s, (char *)&tlen, 2))
                    break;
                if (tlen == 0 || tlen > wire::kMaxTextLen)
                {
                    // The stream is no longer trustworthy: we cannot know how
                    // many bytes to skip, so resynchronising would be guessing.
                    printf("relay: bogus text length %u, dropping the connection\n",
                           (unsigned)tlen);
                    break;
                }

                std::string text(tlen, '\0');
                if (!recvAll(s, &text[0], tlen))
                    break;

                // The rig's own identity, sent once per connection ahead of any
                // telemetry. Checked before speech because a BUILD line is not
                // something the driver said and must never reach the transcript.
                buildid::Info bi;
                if (buildid::parseLine(text, &bi))
                {
                    sawBuild = true;
                    const std::string report = buildid::describe(bi, kExpectTrack);
                    printf("%s\n", report.c_str());
                    writeRigBuild(text, report);
                    continue;
                }

                std::string said;
                float conf = 0;
                if (voice::parseLine(text, &said, &conf))
                {
                    printf("relay: <- heard (%.0f%%) %s\n", conf * 100.0f, said.c_str());
                    appendHeard(text);
                }
                else
                {
                    printf("relay: <- unparseable text frame, ignored\n");
                }
                continue;
            }

            if (marker != wire::kFrameMarker)
                continue; // resynchronise

            uint32_t seq = 0;
            if (!recvAll(s, (char *)&seq, 4))
                break;
            if (!recvAll(s, (char *)row.data(), (int)(row.size() * sizeof(float))))
                break;

            // The rig reports itself before its first frame, so by the time
            // telemetry arrives its silence is conclusive rather than a race.
            // Silence means an exe built before this existed - which is exactly
            // the case that cost an evening: a rig quietly running the previous
            // track's reference lap and corner names, indistinguishable from a
            // driver ignoring his coaching.
            if (first && !sawBuild)
            {
                printf("relay: *** the rig sent no build id ***\n");
                printf("relay:   this exe predates build reporting, so what reference lap\n");
                printf("relay:   and corner names it loaded CANNOT be confirmed from here.\n");
                printf("relay:   copy rline-dist to the rig and restart it.\n");
                writeRigBuild("(none)",
                              "relay: *** the rig sent no build id - pre-reporting exe, "
                              "track files unconfirmable ***");
            }

            if (!first && seq != lastSeq + 1)
                printf("relay: %u frames missing\n", seq - lastSeq - 1);
            lastSeq = seq;
            first = false;

            // Split on the lap counter so each file is one flying lap.
            const int lap = (lapCol >= 0) ? (int)row[lapCol] : 0;
            if (lap != currentLap)
            {
                if (lapFile)
                {
                    fclose(lapFile);
                    printf("relay: lap %d written, %ld rows\n", currentLap, rowsThisLap);
                }
                char path[512];
                snprintf(path, sizeof(path), "%s\\laps\\lap-%04d.csv", g_dir.c_str(), lap);
                lapFile = fopen(path, "w");
                if (lapFile)
                    writeHeader(lapFile, channels);
                currentLap = lap;
                rowsThisLap = 0;
            }

            if (lapFile)
            {
                writeRow(lapFile, row);
                ++rowsThisLap;
                if ((rowsThisLap % 60) == 0)
                    fflush(lapFile); // survive a hard kill mid-session
            }

            // Anything written to the outbox goes to the rig, then the file is
            // truncated so each line is sent exactly once. Checked between
            // frames rather than on a thread: frames arrive every 16 ms, so
            // this is already a fine-grained enough tick.
            if ((rowsThisLap % 30) == 0)
            {
                FILE *ob = fopen(outbox.c_str(), "r");
                if (ob)
                {
                    std::string pending;
                    char line[1024];
                    while (fgets(line, sizeof(line), ob))
                        pending += line;
                    fclose(ob);

                    if (!pending.empty())
                    {
                        if (pending.back() != '\n')
                            pending += '\n';
                        int sent = 0;
                        const int len = (int)pending.size();
                        while (sent < len)
                        {
                            const int n = send(s, pending.data() + sent, len - sent, 0);
                            if (n <= 0)
                                break;
                            sent += n;
                        }
                        printf("relay: -> %s", pending.c_str());
                        fclose(fopen(outbox.c_str(), "w")); // consumed
                    }
                }
            }

            // A short rolling window, rewritten once a second, for anything
            // that wants the current state without opening the lap file.
            live.push_back(row);
            while ((int)live.size() > kLiveRows)
                live.pop_front();

            if ((rowsThisLap % 60) == 0)
            {
                const std::string tmp = g_dir + "\\live.csv.tmp";
                FILE *lf = fopen(tmp.c_str(), "w");
                if (lf)
                {
                    writeHeader(lf, channels);
                    for (const std::vector<float> &r : live)
                        writeRow(lf, r);
                    fclose(lf);
                    // Replace atomically so a reader never sees a half file.
                    MoveFileExA(tmp.c_str(), (g_dir + "\\live.csv").c_str(),
                                MOVEFILE_REPLACE_EXISTING);
                }
            }
        }

        if (lapFile)
            fclose(lapFile);
        closesocket(s);
        printf("relay: rig disconnected\n");
    }
}
