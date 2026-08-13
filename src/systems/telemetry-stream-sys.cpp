#include "telemetry-stream-sys.h"

#include "../build-id.h"
#include "../components/overlay-comp.h"
#include "../irsdk/irsdk_client.h"
#include "../irsdk/irsdk_defines.h"

#include <cstdio>
#include <cstring>

#include <ws2tcpip.h>

#pragma comment(lib, "Ws2_32.lib")

namespace
{
    const float kRetryIntervalMs = 2000.0f;
    const float kReportIntervalMs = 10000.0f;
}

TelemetryStreamSystem::TelemetryStreamSystem(const std::string &host, int port)
    : _host(host), _port(port)
{
}

TelemetryStreamSystem::~TelemetryStreamSystem()
{
    closeSocket();
    if (_wsaReady)
        WSACleanup();
}

void TelemetryStreamSystem::configure(class ECS::World *world)
{
    WSADATA wsa = {};
    _wsaReady = (WSAStartup(MAKEWORD(2, 2), &wsa) == 0);
    if (!_wsaReady)
        printf("rline: winsock unavailable, telemetry streaming disabled\n");
    else
        printf("rline: streaming telemetry to %s:%d\n", _host.c_str(), _port);
}

void TelemetryStreamSystem::unconfigure(class ECS::World *world)
{
    closeSocket();
}

void TelemetryStreamSystem::closeSocket()
{
    if (_sock != INVALID_SOCKET)
    {
        closesocket(_sock);
        _sock = INVALID_SOCKET;
    }
    _helloSent = false;
    _inbox.clear();
}

bool TelemetryStreamSystem::tryConnect()
{
    addrinfo hints = {};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;

    char portStr[16];
    snprintf(portStr, sizeof(portStr), "%d", _port);

    addrinfo *res = NULL;
    if (getaddrinfo(_host.c_str(), portStr, &hints, &res) != 0 || !res)
        return false;

    SOCKET s = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    if (s == INVALID_SOCKET)
    {
        freeaddrinfo(res);
        return false;
    }

    // Connect while still blocking - a LAN connect resolves in milliseconds and
    // this only runs every couple of seconds when disconnected.
    const bool ok = (connect(s, res->ai_addr, (int)res->ai_addrlen) == 0);
    freeaddrinfo(res);

    if (!ok)
    {
        closesocket(s);
        return false;
    }

    // From here on nothing may block the driving loop.
    u_long nonblocking = 1;
    ioctlsocket(s, FIONBIO, &nonblocking);

    // Coaching data is small and latency matters more than packing.
    BOOL nodelay = TRUE;
    setsockopt(s, IPPROTO_TCP, TCP_NODELAY, (const char *)&nodelay, sizeof(nodelay));

    _sock = s;
    return true;
}

bool TelemetryStreamSystem::sendAll(const char *data, int len)
{
    int sent = 0;
    while (sent < len)
    {
        const int n = send(_sock, data + sent, len - sent, 0);
        if (n > 0)
        {
            sent += n;
            continue;
        }

        if (n == SOCKET_ERROR && WSAGetLastError() == WSAEWOULDBLOCK)
            return false; // receiver is behind; drop this frame rather than wait

        closeSocket();
        return false;
    }
    return true;
}

void TelemetryStreamSystem::sendBuildId(class ECS::World *world)
{
    buildid::Info info;
    info.proto = wire::kVersion;
    info.built = __DATE__ " " __TIME__;
#ifdef RLINE_TRACK_NAME
    info.track = RLINE_TRACK_NAME;
#endif

    // Hash the running binary rather than trusting the compile stamp: a
    // translation unit that did not need recompiling keeps an old __DATE__,
    // and the whole point here is a number that cannot quietly go stale.
    char exePath[MAX_PATH] = {};
    if (GetModuleFileNameA(NULL, exePath, MAX_PATH))
        info.exeHash = buildid::hashFile(exePath);

    // Everything else is read from what is loaded in memory, so a stale file
    // beside the exe reports itself instead of what we hoped was there.
    world->each<RefLineComponentSP>(
        [&](ECS::Entity *, ECS::ComponentHandle<RefLineComponentSP> h)
        {
            const RefLineComponent &r = *h.get();
            info.refHash = r.refHash;
            info.refPoints = r.line.pts.size();
            info.refLength = r.line.length;
            info.refLapTime = r.line.lapTime;
            info.refCorners = (int)r.line.corners.size();
            info.nameCount = (int)r.cornerNames.size();
            if (!r.cornerNames.empty())
                info.firstName = r.cornerNames.front();
        });

    const std::string line = buildid::buildLine(info);
    if (sendText(line))
        printf("rline: reported build to relay - %s\n", line.c_str());
}

bool TelemetryStreamSystem::sendText(const std::string &line)
{
    if (line.empty() || line.size() > wire::kMaxTextLen)
        return true; // unsendable; report it consumed rather than retry forever

    std::vector<char> pkt;
    pkt.reserve(3 + line.size());
    pkt.push_back((char)wire::kTextMarker);
    const uint16_t len = (uint16_t)line.size();
    const char *lp = (const char *)&len;
    pkt.insert(pkt.end(), lp, lp + 2);
    pkt.insert(pkt.end(), line.begin(), line.end());

    // Not sendAll: a telemetry frame can be abandoned half-written because the
    // reader resynchronises on the next marker, but a half-written *text* frame
    // leaves a length field with telemetry behind it, and the reader would take
    // that for speech. So either it all goes, or the connection does.
    int sent = 0;
    const int total = (int)pkt.size();
    while (sent < total)
    {
        const int n = send(_sock, pkt.data() + sent, total - sent, 0);
        if (n > 0)
        {
            sent += n;
            continue;
        }
        if (n == SOCKET_ERROR && WSAGetLastError() == WSAEWOULDBLOCK && sent == 0)
            return false; // nothing left yet: keep it queued and try next tick

        closeSocket();
        return false;
    }
    return true;
}

// Per-car arrays the analysis machine has asked for, one name per line, read
// from car-channels.txt beside the exe. Syncs from the relay like the reference
// lap does, so changing what is streamed is an edit there and a restart here -
// never a rebuild, on a machine with no dev tools.
void TelemetryStreamSystem::loadCarChannels()
{
    _carChannels.clear();

    FILE *f = fopen("car-channels.txt", "r");
    if (!f)
        return;

    char line[256];
    while (fgets(line, sizeof(line), f))
    {
        std::string s(line);
        const size_t hash = s.find('#');
        if (hash != std::string::npos)
            s.erase(hash);
        while (!s.empty() && (s.back() == '\n' || s.back() == '\r' || s.back() == ' ' ||
                              s.back() == '\t'))
            s.pop_back();
        size_t b = 0;
        while (b < s.size() && (s[b] == ' ' || s[b] == '\t'))
            ++b;
        s.erase(0, b);
        if (!s.empty())
            _carChannels.push_back(s);
    }
    fclose(f);
}

void TelemetryStreamSystem::buildChannelList()
{
    _varIdx.clear();
    _varEntry.clear();
    _channels.clear();

    const irsdk_header *hdr = irsdk_getHeader();
    if (!hdr)
        return;

    loadCarChannels();

    int expanded = 0;
    for (int i = 0; i < hdr->numVars; ++i)
    {
        const irsdk_varHeader *vh = irsdk_getVarHeaderEntry(i);
        if (!vh)
            continue;

        if (vh->count == 1)
        {
            wire::ChannelDesc d = {};
            strncpy(d.name, vh->name, wire::kNameLen - 1);
            strncpy(d.unit, vh->unit, wire::kUnitLen - 1);

            _channels.push_back(d);
            _varIdx.push_back(i);
            _varEntry.push_back(0);
            continue;
        }

        // An array. Only the ones named in car-channels.txt are sent, and each
        // is flattened into one channel per slot - CarIdxLapDistPct_00 .. _63 -
        // so the wire protocol, the relay, the CSV writer and every analysis
        // tool stay exactly as they were. They are just more columns.
        //
        // Not all arrays: the _ST channels are 6-wide at 360 Hz, and the full
        // CarIdx set is around twenty names, which at 64 slots each would
        // quadruple every lap file for data nothing has asked for.
        bool wanted = false;
        for (const std::string &want : _carChannels)
            if (want == vh->name)
            {
                wanted = true;
                break;
            }
        if (!wanted)
            continue;

        for (int e = 0; e < vh->count; ++e)
        {
            wire::ChannelDesc d = {};
            // Truncation would collide two slots into one column name, so the
            // name is built to fit rather than clipped after the fact.
            char nm[wire::kNameLen];
            snprintf(nm, sizeof(nm), "%.*s_%02d", wire::kNameLen - 5, vh->name, e);
            strncpy(d.name, nm, wire::kNameLen - 1);
            strncpy(d.unit, vh->unit, wire::kUnitLen - 1);

            _channels.push_back(d);
            _varIdx.push_back(i);
            _varEntry.push_back(e);
        }
        ++expanded;
    }

    // Derived channels, appended after everything iRacing publishes. _varIdx
    // of -1 marks them; _varEntry then indexes _derived.
    static const char *const kDerivedNames[kDerivedCount] = {
        "SessionFlagsLo", "SessionFlagsHi", "CoachYellow", "CoachOutLap", "CoachPolicy"};
    for (int d = 0; d < kDerivedCount; ++d)
    {
        wire::ChannelDesc dc = {};
        strncpy(dc.name, kDerivedNames[d], wire::kNameLen - 1);
        strncpy(dc.unit, "rline", wire::kUnitLen - 1);
        _channels.push_back(dc);
        _varIdx.push_back(-1);
        _varEntry.push_back(d);
    }

    printf("rline: streaming %d channels (%d per-car arrays expanded, %d derived)\n",
           (int)_channels.size(), expanded, (int)kDerivedCount);

    _frame.assign(_channels.size(), 0.0f);
}

void TelemetryStreamSystem::pumpIncoming(class ECS::World *world)
{
    char buf[1024];
    for (;;)
    {
        const int n = recv(_sock, buf, sizeof(buf), 0);
        if (n > 0)
        {
            _inbox.append(buf, n);
            continue;
        }
        if (n == 0)
        {
            closeSocket(); // relay closed the connection
            return;
        }
        if (WSAGetLastError() != WSAEWOULDBLOCK)
            closeSocket();
        break;
    }

    // Complete lines only; a partial line stays buffered for the next tick.
    for (;;)
    {
        const size_t nl = _inbox.find('\n');
        if (nl == std::string::npos)
            break;

        std::string line = _inbox.substr(0, nl);
        _inbox.erase(0, nl + 1);
        if (!line.empty() && line.back() == '\r')
            line.pop_back();
        if (line.empty())
            continue;

        printf("rline: <- %s\n", line.c_str());

        // COACH|mode=silent - pin how much the corner coach may say, or
        // COACH|mode=auto to hand it back to the session type. This is the only
        // way to change what the rig says without rebuilding it, on a machine
        // that deliberately has no command line.
        {
            sessionpolicy::Mode m = sessionpolicy::kFull;
            bool automatic = false;
            if (sessionpolicy::parseCommand(line, &m, &automatic))
            {
                world->each<CoachPolicyComponentSP>(
                    [&](ECS::Entity *, ECS::ComponentHandle<CoachPolicyComponentSP> h)
                    {
                        CoachPolicyComponent &p = *h.get();
                        p.pinned = !automatic;
                        p.pin = m;
                    });
                printf("rline: coach mode -> %s\n",
                       automatic ? "auto" : sessionpolicy::name(m));
                continue;
            }
        }

        // SAY|secs=8|text=brake later into turn five
        if (line.rfind("SAY|", 0) == 0 || line.rfind("SAY ", 0) == 0)
        {
            float secs = 8.0f;
            std::string text;

            size_t pos = 4;
            while (pos <= line.size())
            {
                const size_t bar = line.find('|', pos);
                const std::string field =
                    line.substr(pos, bar == std::string::npos ? std::string::npos : bar - pos);
                const size_t eq = field.find('=');
                if (eq != std::string::npos)
                {
                    const std::string k = field.substr(0, eq);
                    const std::string v = field.substr(eq + 1);
                    if (k == "secs")
                        secs = (float)atof(v.c_str());
                    else if (k == "text")
                        text = v; // last field, may contain anything but '|'
                }
                if (bar == std::string::npos)
                    break;
                pos = bar + 1;
            }

            if (!text.empty())
            {
                // Queued, not spoken over the top of whatever is talking. A
                // note from the relay is conversational - an answer, an
                // observation - so it never goes stale and it yields to a
                // corner cue that does.
                world->each<SpeechQueueComponentSP>(
                    [&](ECS::Entity *, ECS::ComponentHandle<SpeechQueueComponentSP> qH)
                    { qH.get()->queue.push(text, speech::PriorityNote, 0.0f, secs); });
            }
        }
    }
}

void TelemetryStreamSystem::tick(class ECS::World *world, float deltaTime)
{
    if (!_wsaReady)
        return;

    // Nothing to say until iRacing is up. Connecting earlier would leave the
    // relay holding an open but silent connection, which blocks it from
    // accepting anyone else.
    if (!irsdkClient::instance().isConnected())
    {
        if (_sock != INVALID_SOCKET)
            closeSocket();
        return;
    }

    if (_sock == INVALID_SOCKET)
    {
        _retryAcc += deltaTime;
        if (_retryAcc < kRetryIntervalMs)
            return;
        _retryAcc = 0;
        if (!tryConnect())
            return;
    }

    if (!_helloSent)
    {
        buildChannelList();
        if (_channels.empty())
            return;

        std::vector<char> hello;
        wire::HelloHeader h;
        h.magic = wire::kMagic;
        h.version = wire::kVersion;
        h.channelCount = (uint16_t)_channels.size();

        const char *hp = (const char *)&h;
        hello.insert(hello.end(), hp, hp + sizeof(h));
        const char *cp = (const char *)_channels.data();
        hello.insert(hello.end(), cp, cp + _channels.size() * sizeof(wire::ChannelDesc));

        if (!sendAll(hello.data(), (int)hello.size()))
            return;

        _helloSent = true;
        _seq = 0;
        printf("rline: relay connected, streaming %d channels\n", (int)_channels.size());

        // Before any telemetry, so the relay's log opens with what it is
        // talking to rather than having to infer it later.
        sendBuildId(world);
    }

    pumpIncoming(world);
    if (_sock == INVALID_SOCKET)
        return;

    // Anything the driver said goes up ahead of the next telemetry frame, so a
    // congested link delays his words by less than it delays data we can
    // reconstruct anyway. Stops at the first failure and keeps the rest queued.
    world->each<DriverSpeechComponentSP>(
        [&](ECS::Entity *, ECS::ComponentHandle<DriverSpeechComponentSP> h)
        {
            DriverSpeechComponent &d = *h.get();
            while (!d.pending.empty() && _sock != INVALID_SOCKET)
            {
                if (!sendText(d.pending.front()))
                    break;
                d.pending.erase(d.pending.begin());
            }
        });

    if (_sock == INVALID_SOCKET)
        return;

    // Fill the derived channels from state the rig still holds exactly. This is
    // the only place the true SessionFlags integer exists on the analysis path;
    // once it is a float32 on the wire the caution bits are gone.
    world->each<EgoStateComponentSP>(
        [&](ECS::Entity *ent, ECS::ComponentHandle<EgoStateComponentSP> h)
        {
            const EgoStateComponent &ego = *h.get();
            _derived[kDerivedFlagsLo] = (float)(ego.sessionFlags & 0xffffu);
            _derived[kDerivedFlagsHi] = (float)((ego.sessionFlags >> 16) & 0xffffu);
            _derived[kDerivedYellow] = sessionpolicy::isYellow(ego.sessionFlags) ? 1.0f : 0.0f;
            _derived[kDerivedOutLap] = ego.outLap ? 1.0f : 0.0f;

            ECS::ComponentHandle<CoachPolicyComponentSP> p = ent->get<CoachPolicyComponentSP>();
            if (p.isValid())
                _derived[kDerivedPolicy] = (float)(int)p.get()->mode;
        });

    for (size_t i = 0; i < _varIdx.size(); ++i)
    {
        if (_varIdx[i] < 0)
            _frame[i] = _derived[_varEntry[i]];
        else
            _frame[i] = (float)irsdkClient::instance().getVarDouble(_varIdx[i], _varEntry[i]);
    }

    std::vector<char> pkt;
    pkt.reserve(1 + 4 + _frame.size() * 4);
    pkt.push_back((char)wire::kFrameMarker);
    const char *sp = (const char *)&_seq;
    pkt.insert(pkt.end(), sp, sp + 4);
    const char *fp = (const char *)_frame.data();
    pkt.insert(pkt.end(), fp, fp + _frame.size() * sizeof(float));

    if (sendAll(pkt.data(), (int)pkt.size()))
        ++_seq;
    else
        ++_dropped;

    _reportAcc += deltaTime;
    if (_reportAcc >= kReportIntervalMs)
    {
        _reportAcc = 0;
        if (_dropped > 0)
        {
            printf("rline: %d frames dropped (receiver behind)\n", _dropped);
            _dropped = 0;
        }
    }
}
