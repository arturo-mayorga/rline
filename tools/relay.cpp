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
#include <deque>
#include <string>
#include <vector>

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
        else if (!strcmp(argv[i], "--help"))
        {
            printf("relay [--dir <path>] [--port <n>]\n"
                   "  Receives rline telemetry and writes <dir>/laps/lap-NNNN.csv\n"
                   "  plus <dir>/live.csv. Lines typed here are sent to the rig.\n");
            return 0;
        }
    }

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

        wire::HelloHeader h = {};
        if (!recvAll(s, (char *)&h, sizeof(h)) || h.magic != wire::kMagic)
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

        for (;;)
        {
            uint8_t marker = 0;
            if (!recvAll(s, (char *)&marker, 1))
                break;
            if (marker != wire::kFrameMarker)
                continue; // resynchronise

            uint32_t seq = 0;
            if (!recvAll(s, (char *)&seq, 4))
                break;
            if (!recvAll(s, (char *)row.data(), (int)(row.size() * sizeof(float))))
                break;

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
