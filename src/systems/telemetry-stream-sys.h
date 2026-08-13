#ifndef telemetry_stream_sys_h_
#define telemetry_stream_sys_h_

#include "../ecs.h"
#include "../wire.h"

#include <string>
#include <vector>

#include <winsock2.h>

// Streams every scalar iRacing channel to the relay.
//
// The socket is non-blocking and frames are dropped rather than queued if the
// receiver stalls: a coaching link must never be able to introduce a hitch in
// the driving loop. Reconnection is retried on a timer, so the relay can be
// restarted mid-session without touching the rig.
class TelemetryStreamSystem : public ECS::EntitySystem
{
private:
    std::string _host;
    int _port = wire::kDefaultPort;

    SOCKET _sock = INVALID_SOCKET;
    bool _wsaReady = false;
    bool _helloSent = false;

    float _retryAcc = 0;   // ms since the last connection attempt
    uint32_t _seq = 0;
    int _dropped = 0;
    float _reportAcc = 0;

    // Resolved once per connection: the scalar channels, in wire order.
    std::vector<int> _varIdx;
    std::vector<int> _varEntry; // array slot for _varIdx, 0 for scalars
    std::vector<std::string> _carChannels;

    // Channels the rig computes rather than reads. They exist because the wire
    // carries float32, and a 32-bit bitfield does not survive it: SessionFlags
    // with the start-light bits set sits near 2.7e8, where float32 spacing is
    // 32 - so irsdk_yellow (0x8) and irsdk_yellowWaving (0x100) are rounded
    // away before the relay ever sees them. Checking cautions on the analysis
    // side against the streamed SessionFlags produced a confident 100%-caution
    // verdict on a clean practice lap.
    //
    // So the flags are split into two 16-bit halves, each exact in float32, and
    // the rig also publishes what it concluded - it holds the true integer.
    enum Derived
    {
        kDerivedFlagsLo = 0, // SessionFlags & 0xffff
        kDerivedFlagsHi,     // SessionFlags >> 16
        kDerivedYellow,      // 1 while any caution bit is set
        kDerivedOutLap,      // 1 from pit exit until the lap counter moves
        kDerivedPolicy,      // sessionpolicy::Mode the coach is running under
        kDerivedCount
    };
    float _derived[kDerivedCount] = {0};
    std::vector<wire::ChannelDesc> _channels;
    std::vector<float> _frame;

    // Text arriving from the relay, split into complete lines.
    std::string _inbox;

    void closeSocket();
    bool tryConnect();
    bool sendAll(const char *data, int len);
    bool sendText(const std::string &line);
    void buildChannelList();
    void loadCarChannels();
    void pumpIncoming(class ECS::World *world);

    // Reports what this rig actually loaded, immediately after the hello. Sent
    // every connection rather than once, so restarting the relay mid-session
    // still tells the analysis machine what it is talking to.
    void sendBuildId(class ECS::World *world);

public:
    TelemetryStreamSystem(const std::string &host, int port);
    virtual ~TelemetryStreamSystem();

    virtual void configure(class ECS::World *world) override;
    virtual void unconfigure(class ECS::World *world) override;
    virtual void tick(class ECS::World *world, float deltaTime) override;

    bool connected() const { return _sock != INVALID_SOCKET && _helloSent; }
    int channelCount() const { return (int)_channels.size(); }
    int droppedFrames() const { return _dropped; }
};

#endif
