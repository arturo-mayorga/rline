#include "demo-telemetry-sys.h"

#include "../components/overlay-comp.h"

#include <algorithm>
#include <cmath>

namespace
{
    // How far the demo car weaves either side of the line, and how quickly.
    const float kWeaveAmplitudeM = 2.5f;
    const float kWeavePeriodS = 8.0f;
}

DemoTelemetrySystem::~DemoTelemetrySystem() {}
void DemoTelemetrySystem::configure(class ECS::World *world) {}
void DemoTelemetrySystem::unconfigure(class ECS::World *world) {}

void DemoTelemetrySystem::tick(class ECS::World *world, float deltaTime)
{
    world->each<EgoStateComponentSP>(
        [&](ECS::Entity *ent, ECS::ComponentHandle<EgoStateComponentSP> egoH)
        {
            ECS::ComponentHandle<RefLineComponentSP> refH = ent->get<RefLineComponentSP>();
            if (!refH.isValid() || !refH.get()->loaded)
                return;

            EgoStateComponent &ego = *egoH.get();
            const RefLine &line = refH.get()->line;

            ego.connected = true;
            ego.onTrack = true;

            _elapsed += deltaTime / 1000.0f; // tick delta is milliseconds
            if (line.lapTime > 0)
                _elapsed = fmodf(_elapsed, line.lapTime);

            // Point the reference lap had reached at this time.
            const size_t i = (size_t)(std::lower_bound(
                                          line.pts.begin(), line.pts.end(), _elapsed,
                                          [](const RefPoint &p, float v) { return p.t < v; }) -
                                      line.pts.begin());

            const size_t n = line.pts.size();
            const RefPoint &a = line.pts[std::min(i, n - 1)];
            const RefPoint &b = line.pts[std::min(i + 1, n - 1)];

            // Weave across the line so the bar and the readout visibly move.
            float dx = b.x - a.x, dy = b.y - a.y;
            const float len = sqrtf(dx * dx + dy * dy);
            if (len > 1e-6f)
            {
                dx /= len;
                dy /= len;
            }

            const float offset =
                kWeaveAmplitudeM * sinf(_elapsed * 2.0f * 3.14159265f / kWeavePeriodS);

            ego.pct = a.pct;
            ego.speed = a.speed;
            ego.lateral = offset;
            ego.x = a.x + offset * dy; // + is right of travel
            ego.y = a.y - offset * dx;
        });
}
