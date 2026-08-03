#include "irtelemetry-sys.h"

#include "../components/overlay-comp.h"
#include "../irsdk/irsdk_client.h"

#include <cmath>

// Player-car channels. iRacing publishes no absolute position live - there is
// no Lat/Lon/Alt and no world X/Y/Z among the 300-odd channels - so lateral
// position is reconstructed from heading and velocity instead.
static irsdkCVar g_sessionTime("SessionTime");
static irsdkCVar g_lap("Lap");
static irsdkCVar g_lapDistPct("LapDistPct");
static irsdkCVar g_speed("Speed");
static irsdkCVar g_yawNorth("YawNorth");
static irsdkCVar g_velocityX("VelocityX");
static irsdkCVar g_velocityY("VelocityY");
static irsdkCVar g_isOnTrack("IsOnTrack");

namespace
{
    // VelocityY is positive to the LEFT. Established by integrating a real
    // captured lap both ways: negating the slip term closed the lap to 0.00 m,
    // not negating it did not.
    const float kSlipSign = -1.0f;
}

IrTelemetrySystem::~IrTelemetrySystem() {}

void IrTelemetrySystem::configure(class ECS::World *world) {}

void IrTelemetrySystem::unconfigure(class ECS::World *world) {}

void IrTelemetrySystem::tick(class ECS::World *world, float deltaTime)
{
    // Blocks until the next 60 Hz sample or the timeout, so this doubles as the
    // frame pacer while iRacing is running.
    const bool fresh = irsdkClient::instance().waitForData(16);
    const bool connected = irsdkClient::instance().isConnected();

    world->each<EgoStateComponentSP>(
        [&](ECS::Entity *ent, ECS::ComponentHandle<EgoStateComponentSP> egoH)
        {
            EgoStateComponent &ego = *egoH.get();

            ego.connected = connected;

            if (!connected)
            {
                ego.onTrack = false;
                _haveTime = false;
                return;
            }

            if (!fresh)
                return; // hold the last good sample rather than blanking

            ECS::ComponentHandle<RefLineComponentSP> refH = ent->get<RefLineComponentSP>();
            if (!refH.isValid() || !refH.get()->loaded)
                return;

            const RefLine &line = refH.get()->line;

            ego.onTrack = g_isOnTrack.getBool();
            ego.pct = g_lapDistPct.getFloat();
            ego.speed = g_speed.getFloat();

            const int lap = g_lap.getInt();
            const double now = g_sessionTime.getDouble();

            // A tow, a reset to pits or a jump in the replay moves the car
            // without driving it there, so track position jumps discontinuously.
            // Carrying the old offset across that would place the car somewhere
            // it never was.
            float dpct = fabsf(ego.pct - _lastPct);
            if (dpct > 0.5f)
                dpct = 1.0f - dpct; // the lap wrap is not a jump
            const bool teleported = _haveTime && dpct > 0.02f;

            // Dead reckoning accumulates error, so start each lap from the
            // reference line rather than letting it grow all session. Also
            // restart cleanly whenever the car leaves the track.
            if (lap != ego.lap || !ego.onTrack || teleported)
            {
                _lateral.reset();
                _haveTime = false;
                ego.lap = lap;
                // Without this the display keeps showing the last offset from
                // before the car was moved.
                ego.lateral = 0;
            }

            if (_haveTime && ego.onTrack)
            {
                const float dt = (float)(now - _lastTime);

                // Direction the car is actually travelling: where the nose
                // points, plus slip. Using the heading alone integrates the
                // slip angle, which is systematic through corners and drifted
                // 22 m over a lap in testing.
                const float slip = atan2f(g_velocityY.getFloat(), g_velocityX.getFloat());
                const float bearingVel = g_yawNorth.getFloat() + kSlipSign * slip;

                ego.lateral = _lateral.update(line, ego.pct, ego.speed, bearingVel, dt);
            }

            _lastTime = now;
            _lastPct = ego.pct;
            _haveTime = true;

            // Hand the rest of the overlay an absolute position, as if it had
            // been measured directly.
            positionFrom(line, ego.pct, ego.lateral, ego.x, ego.y);
        });
}
