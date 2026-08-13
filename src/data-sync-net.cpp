#include "data-sync-net.h"

#include "build-id.h"
#include "data-sync.h"

#include <winsock2.h>
#include <ws2tcpip.h>

#include <windows.h>

#include <cstdio>
#include <cstring>
#include <vector>

#pragma comment(lib, "Ws2_32.lib")

namespace
{
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

    bool sendAll(SOCKET s, const char *data, int len)
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

    // One reply line, read a byte at a time. Slow in principle and irrelevant
    // in practice - there are two of them per startup - and it means the body
    // that follows a DATA line is never swallowed into a read buffer we would
    // then have to unpick.
    bool recvLine(SOCKET s, std::string *out, size_t cap = 1024)
    {
        std::string line;
        for (;;)
        {
            char c = 0;
            if (!recvAll(s, &c, 1))
                return false;
            if (c == '\n')
                break;
            if (c != '\r')
                line += c;
            if (line.size() > cap)
                return false;
        }
        *out = line;
        return true;
    }

    // Write to a temp name and rename over the target, so a download that dies
    // half way cannot leave a truncated reference lap where a good one was.
    // The rig would then start, load it, and coach against nonsense.
    bool installFile(const std::string &dir, const std::string &name,
                     const std::string &body)
    {
        const std::string finalPath = dir + "\\" + name;
        const std::string tmpPath = finalPath + ".part";

        FILE *f = fopen(tmpPath.c_str(), "wb");
        if (!f)
            return false;
        const size_t wrote = fwrite(body.data(), 1, body.size(), f);
        const bool flushed = (fflush(f) == 0);
        fclose(f);

        if (wrote != body.size() || !flushed)
        {
            DeleteFileA(tmpPath.c_str());
            return false;
        }

        if (!MoveFileExA(tmpPath.c_str(), finalPath.c_str(), MOVEFILE_REPLACE_EXISTING))
        {
            DeleteFileA(tmpPath.c_str());
            return false;
        }
        return true;
    }

    bool connectWithTimeout(SOCKET s, const sockaddr_in &addr, int timeoutMs)
    {
        u_long nonblocking = 1;
        ioctlsocket(s, FIONBIO, &nonblocking);

        connect(s, (const sockaddr *)&addr, sizeof(addr));

        fd_set wr;
        FD_ZERO(&wr);
        FD_SET(s, &wr);
        timeval tv = {};
        tv.tv_sec = timeoutMs / 1000;
        tv.tv_usec = (timeoutMs % 1000) * 1000;

        const int ready = select(0, NULL, &wr, NULL, &tv);

        u_long blocking = 0;
        ioctlsocket(s, FIONBIO, &blocking);

        if (ready != 1)
            return false;

        int err = 0;
        int elen = sizeof(err);
        if (getsockopt(s, SOL_SOCKET, SO_ERROR, (char *)&err, &elen) != 0 || err != 0)
            return false;
        return true;
    }
}

namespace datasyncnet
{
    Result fetch(const std::string &host, int port, const std::string &dir,
                 int timeoutMs, std::string *log)
    {
        Result r;

        WSADATA wsa = {};
        if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0)
            return r;

        SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (s == INVALID_SOCKET)
        {
            WSACleanup();
            return r;
        }

        sockaddr_in addr = {};
        addr.sin_family = AF_INET;
        addr.sin_port = htons((u_short)port);
        inet_pton(AF_INET, host.c_str(), &addr.sin_addr);

        if (!connectWithTimeout(s, addr, timeoutMs))
        {
            closesocket(s);
            WSACleanup();
            return r;
        }

        // Past the connect, every read has the same deadline. A relay that is
        // up but wedged must not hold the overlay off the screen.
        DWORD tmo = (DWORD)timeoutMs;
        setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char *)&tmo, sizeof(tmo));
        setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, (const char *)&tmo, sizeof(tmo));

        // What we have now. An unreadable or absent file hashes to empty, which
        // the relay reads as "send it regardless".
        std::vector<datasync::FileState> have;
        for (int i = 0; i < datasync::kSyncFileCount; ++i)
        {
            datasync::FileState f;
            f.name = datasync::kSyncFiles[i];
            f.hash = buildid::hashFile(dir + "\\" + f.name);
            have.push_back(f);
        }

        const std::string want = datasync::wantLine(have);

        const uint32_t magic = datasync::kMagic;
        const uint16_t version = datasync::kVersion;
        const uint16_t len = (uint16_t)want.size();

        bool ok = sendAll(s, (const char *)&magic, sizeof(magic)) &&
                  sendAll(s, (const char *)&version, sizeof(version)) &&
                  sendAll(s, (const char *)&len, sizeof(len)) &&
                  sendAll(s, want.data(), (int)want.size());

        if (!ok)
        {
            closesocket(s);
            WSACleanup();
            return r;
        }

        r.contacted = true;

        // One reply per file we asked about, in the order we asked.
        for (size_t i = 0; i < have.size() && ok; ++i)
        {
            std::string line;
            if (!recvLine(s, &line))
                break;

            datasync::Header h;
            if (!datasync::parseHeader(line, &h))
            {
                ++r.failed;
                if (log)
                    *log += "rline:   unparseable sync reply, keeping what I had\n";
                break; // the stream is no longer trustworthy
            }

            if (h.reply == datasync::kReplySame)
            {
                ++r.current;
                if (log)
                    *log += "rline:   " + h.name + " already current\n";
                continue;
            }

            if (h.reply == datasync::kReplyNone)
            {
                if (log)
                    *log += "rline:   " + h.name + " not offered, keeping mine\n";
                continue;
            }

            std::string body(h.len, '\0');
            if (!recvAll(s, &body[0], (int)h.len))
            {
                ++r.failed;
                if (log)
                    *log += "rline:   " + h.name + " download cut short, kept the old one\n";
                break;
            }

            // Verified before it is allowed anywhere near the real filename.
            if (!datasync::verify(h, body.data(), body.size()))
            {
                ++r.failed;
                if (log)
                    *log += "rline:   " + h.name + " FAILED its hash, kept the old one\n";
                continue;
            }

            if (!installFile(dir, h.name, body))
            {
                ++r.failed;
                if (log)
                    *log += "rline:   " + h.name + " could not be written, kept the old one\n";
                continue;
            }

            ++r.updated;
            if (log)
                *log += "rline:   " + h.name + " updated [" + h.hash + "]\n";
        }

        closesocket(s);
        WSACleanup();
        return r;
    }
}
