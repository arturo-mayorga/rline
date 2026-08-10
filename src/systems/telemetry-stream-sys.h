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
    std::vector<wire::ChannelDesc> _channels;
    std::vector<float> _frame;

    // Text arriving from the relay, split into complete lines.
    std::string _inbox;

    void closeSocket();
    bool tryConnect();
    bool sendAll(const char *data, int len);
    bool sendText(const std::string &line);
    void buildChannelList();
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
