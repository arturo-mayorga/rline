// Headless tests for the overlay geometry. The overlay system draws into a
// backend-agnostic draw list and never includes windows.h, so its output can be
// checked on any platform without opening a window or running iRacing.
//
// g++ -std=c++17 -O2 -o test-overlay tests/test-overlay.cpp src/refline.cpp
//     src/components/rendering-comp.cpp src/systems/refline-overlay-sys.cpp

#include "../src/components/overlay-comp.h"
#include "../src/components/rendering-comp.h"
#include "../src/ecs.h"
#include "../src/refline.h"
#include "../src/systems/refline-overlay-sys.h"

#include <cmath>
#include <cstdio>
#include <string>

static int g_fail = 0;

static void check(bool ok, const char *what)
{
    printf("%s  %s\n", ok ? "  ok  " : "  FAIL", what);
    if (!ok)
        ++g_fail;
}

namespace
{
    const int kW = 360;
    const int kH = 420;

    struct Fixture
    {
        ECS::World *world = nullptr;
        ECS::Entity *ent = nullptr;
        EgoStateComponentSP ego;
        RefLineComponentSP ref;
        OverlayConfigComponentSP cfg;
        DrawListComponentSP dl;
        ReflineOverlaySystem sys;

        bool build(const std::string &csv)
        {
            world = ECS::World::createWorld();
            ent = world->create();

            CanvasConfigComponentSP canvas(new CanvasConfigComponent());
            canvas->x = 0;
            canvas->y = 0;
            canvas->w = kW;
            canvas->h = kH;
            ent->assign<CanvasConfigComponentSP>(canvas);

            dl = DrawListComponentSP(new DrawListComponent());
            ent->assign<DrawListComponentSP>(dl);

            ego = EgoStateComponentSP(new EgoStateComponent());
            ego->connected = true;
            ego->onTrack = true;
            ent->assign<EgoStateComponentSP>(ego);

            cfg = OverlayConfigComponentSP(new OverlayConfigComponent());
            ent->assign<OverlayConfigComponentSP>(cfg);

            ref = RefLineComponentSP(new RefLineComponent());
            std::string err;
            ref->loaded = loadRefLineCsv(csv, ref->line, &err);
            ref->error = err;
            ent->assign<RefLineComponentSP>(ref);

            return ref->loaded;
        }

        // Places the car at reference point i, displaced sideways by offsetM
        // (positive = right of the direction of travel), then redraws.
        void placeAt(size_t i, float offsetM)
        {
            const RefLine &L = ref->line;
            const RefPoint &a = L.pts[i];
            const RefPoint &p = L.pts[i - 1];
            const RefPoint &q = L.pts[i + 1];

            float dx = q.x - p.x, dy = q.y - p.y;
            const float len = sqrtf(dx * dx + dy * dy);
            dx /= len;
            dy /= len;

            ego->pct = a.pct;
            ego->speed = a.speed;
            ego->lateral = offsetM;       // what the overlay reads
            ego->x = a.x + offsetM * dy;  // right of travel, for the view
            ego->y = a.y - offsetM * dx;

            sys.tick(world, 16.0f);
        }
    };

    // Where the drawn reference line crosses the car's own latitude on screen.
    bool lineXAtCarRow(const DrawListComponent &dl, int cy, int &outX)
    {
        if (dl.polys.empty())
            return false;
        const PolyCmd &poly = dl.polys.front();

        int best = -1, bestDy = 1 << 30;
        for (size_t i = 0; i < poly.pts.size(); ++i)
        {
            const int d = abs(poly.pts[i].y - cy);
            if (d < bestDy)
            {
                bestDy = d;
                best = (int)i;
            }
        }
        if (best < 0 || bestDy > 4)
            return false;
        outX = poly.pts[best].x;
        return true;
    }

    bool usesColourKey(const DrawListComponent &dl)
    {
        auto isKey = [](int r, int g, int b)
        { return r == 255 && g == 0 && b == 255; };

        for (const RectCmd &c : dl.rects)
            if (isKey(c.r, c.g, c.b))
                return true;
        for (const PolyCmd &c : dl.polys)
            if (isKey(c.r, c.g, c.b))
                return true;
        for (const DiscCmd &c : dl.discs)
            if (isKey(c.r, c.g, c.b))
                return true;
        for (const TextCmd &c : dl.texts)
            if (isKey(c.r, c.g, c.b))
                return true;
        return false;
    }
}

int main(int argc, char **argv)
{
    const std::string csv = (argc > 1) ? argv[1] : "data/lap.csv";

    Fixture fx;
    if (!fx.build(csv))
    {
        printf("FAILED TO LOAD %s: %s\n", csv.c_str(), fx.ref->error.c_str());
        return 1;
    }

    // Must track the layout in refline-overlay-sys.cpp.
    const int readoutH = 34, barH = 26, traceH = 78, frozenH = 56;
    const int viewH = kH - readoutH - barH - traceH - frozenH;
    const int cx = kW / 2;
    const int cy = (int)(viewH * 0.78f);

    const size_t mid = fx.ref->line.pts.size() / 2;

    printf("-- on the line --\n");
    {
        fx.placeAt(mid, 0.0f);
        const DrawListComponent &dl = *fx.dl;

        check(!dl.polys.empty(), "a reference polyline was emitted");
        check(dl.polys.front().pts.size() > 20, "the polyline has real geometry");
        check(!dl.discs.empty(), "the car marker was emitted");
        check(!dl.texts.empty(), "a readout was emitted");
        check(!usesColourKey(dl), "nothing is drawn in the transparent colour key");

        int lx = 0;
        const bool found = lineXAtCarRow(dl, cy, lx);
        check(found, "the line is drawn across the car's row");
        check(found && abs(lx - cx) <= 2, "on the line, the line passes through the car marker");

        check(dl.discs.front().cx == cx && dl.discs.front().cy == cy,
              "the car marker sits at the view origin");
    }

    printf("\n-- displaced right --\n");
    {
        fx.placeAt(mid, 2.0f);
        const DrawListComponent &dl = *fx.dl;

        int lx = 0;
        const bool found = lineXAtCarRow(dl, cy, lx);
        check(found, "the line is drawn across the car's row");

        // Car is right of the line, so the line must be drawn to its left.
        check(found && lx < cx - 5, "line appears left of the car when the car is right");

        // 2 m at 3x lateral exaggeration should be clearly more than 2 m at 1x.
        const float pxPerMFwd = (float)cy / fx.cfg->forwardM;
        const int expect = (int)lroundf(2.0f * pxPerMFwd * fx.cfg->lateralExaggeration);
        check(found && abs((cx - lx) - expect) <= 3,
              "lateral offset is drawn at the exaggerated scale");

        // The bar marker is the last rect: panel, centre line, tick, marker.
        check(dl.rects.size() >= 3, "bar chrome and marker were emitted");
        // Fly-to: the car is right of the line, so the marker - which shows
        // where the line is - must sit left, and you steer toward it.
        check(dl.rects.back().x < cx, "bar marker points left when the car is right");
    }

    printf("\n-- displaced left --\n");
    {
        fx.placeAt(mid, -2.0f);
        const DrawListComponent &dl = *fx.dl;

        int lx = 0;
        const bool found = lineXAtCarRow(dl, cy, lx);
        check(found && lx > cx + 5, "line appears right of the car when the car is left");
        check(dl.rects.back().x > cx, "bar marker points right when the car is left");
    }

    printf("\n-- bar saturation --\n");
    {
        fx.placeAt(mid, 50.0f); // far beyond full-scale deflection
        const DrawListComponent &dl = *fx.dl;
        const int barMid = kW / 2;
        const int barHalf = kW / 2 - 12;
        const int markerHalf = 3;
        // rects carry a left edge, so allow for the marker's own width.
        const int left = dl.rects.back().x;
        const int right = left + dl.rects.back().w;
        check(left >= barMid - barHalf - markerHalf &&
                  right <= barMid + barHalf + markerHalf * 2,
              "bar marker stays inside the bar when far off line");
    }

    printf("\n-- connection states --\n");
    {
        fx.ego->onTrack = false;
        fx.sys.tick(fx.world, 16.0f);
        check(fx.dl->polys.empty() && !fx.dl->texts.empty(),
              "off track shows a message instead of a line");

        fx.ego->onTrack = true;
        fx.ego->connected = false;
        fx.sys.tick(fx.world, 16.0f);
        check(fx.dl->polys.empty() && !fx.dl->texts.empty(),
              "disconnected shows a message instead of a line");

        fx.ego->connected = true;
        fx.ref->loaded = false;
        fx.ref->error = "boom";
        fx.sys.tick(fx.world, 16.0f);
        check(fx.dl->polys.empty() && !fx.dl->texts.empty(),
              "a missing reference lap shows the error");
    }

    printf("\n-- full lap sweep --\n");
    {
        fx.ref->loaded = true;
        fx.ego->connected = true;
        fx.ego->onTrack = true;

        int emptyPoly = 0, offCanvasCar = 0, keyUse = 0;
        const size_t n = fx.ref->line.pts.size();

        for (size_t i = 1; i + 1 < n; i += 11)
        {
            fx.placeAt(i, 0.0f);
            if (fx.dl->polys.empty() || fx.dl->polys.front().pts.size() < 5)
                ++emptyPoly;
            if (fx.dl->discs.empty() ||
                fx.dl->discs.front().cx < 0 || fx.dl->discs.front().cx > kW)
                ++offCanvasCar;
            if (usesColourKey(*fx.dl))
                ++keyUse;
        }

        printf("       (%zu positions swept)\n", (n - 2) / 11);
        check(emptyPoly == 0, "every position on the lap produces a line");
        check(offCanvasCar == 0, "the car marker is always on canvas");
        check(keyUse == 0, "the colour key is never drawn anywhere on the lap");
    }

    printf("\n%s (%d failures)\n", g_fail == 0 ? "ALL PASSED" : "FAILURES", g_fail);
    return g_fail == 0 ? 0 : 1;
}
