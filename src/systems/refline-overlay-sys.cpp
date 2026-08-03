#include "refline-overlay-sys.h"

#include "../components/overlay-comp.h"
#include "../components/rendering-comp.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace
{
    // Reserved as the layered window's colour key, so nothing may draw in it.
    const int kKeyR = 255, kKeyG = 0, kKeyB = 255;

    struct Rgb
    {
        int r, g, b;
    };

    const Rgb kLine = {70, 200, 255};  // reference line
    const Rgb kDim = {150, 160, 172};  // panel chrome
    const Rgb kText = {235, 240, 245};

    // Text must never antialias directly against the colour key or its glyph
    // edges blend toward magenta. Everything textual sits on this panel, which
    // also keeps the readout legible over a bright, busy game scene.
    const Rgb kPanel = {18, 20, 24};
    // Corner cues. None may be the colour key (255, 0, 255).
    const Rgb kBrakeMark = {250, 70, 60};
    const Rgb kTurnMark = {250, 190, 60};
    const Rgb kApexMark = {190, 130, 255};

    const Rgb kGood = {60, 220, 120};
    const Rgb kOk = {245, 200, 70};
    const Rgb kBad = {250, 90, 90};

    Rgb errorColour(float absErr, const OverlayConfigComponent &cfg)
    {
        if (absErr <= cfg.goodM)
            return kGood;
        if (absErr <= cfg.okM)
            return kOk;
        return kBad;
    }

    // Direction of travel at reference point i, from a centred difference.
    void headingAt(const RefLine &line, int i, float &hx, float &hy)
    {
        const int n = (int)line.pts.size();
        const RefPoint &p = line.pts[((i - 1) % n + n) % n];
        const RefPoint &q = line.pts[(i + 1) % n];
        float dx = q.x - p.x, dy = q.y - p.y;
        const float len = sqrtf(dx * dx + dy * dy);
        if (len > 1e-6f)
        {
            dx /= len;
            dy /= len;
        }
        else
        {
            dx = 0;
            dy = 1;
        }
        hx = dx;
        hy = dy;
    }

    float segLen(const RefLine &line, int i)
    {
        const int n = (int)line.pts.size();
        const RefPoint &a = line.pts[i % n];
        const RefPoint &b = line.pts[(i + 1) % n];
        return sqrtf((b.x - a.x) * (b.x - a.x) + (b.y - a.y) * (b.y - a.y));
    }
}

ReflineOverlaySystem::~ReflineOverlaySystem() {}
void ReflineOverlaySystem::configure(class ECS::World *world) {}
void ReflineOverlaySystem::unconfigure(class ECS::World *world) {}

void ReflineOverlaySystem::tick(class ECS::World *world, float deltaTime)
{
    // Single overlay entity carries every component this system needs.
    world->each<DrawListComponentSP>(
        [&](ECS::Entity *ent,
            ECS::ComponentHandle<DrawListComponentSP> dlH)
        {
            DrawListComponentSP dl = dlH.get();

            ECS::ComponentHandle<CanvasConfigComponentSP> canvasH = ent->get<CanvasConfigComponentSP>();
            ECS::ComponentHandle<EgoStateComponentSP> egoH = ent->get<EgoStateComponentSP>();
            ECS::ComponentHandle<RefLineComponentSP> refH = ent->get<RefLineComponentSP>();
            ECS::ComponentHandle<OverlayConfigComponentSP> cfgH = ent->get<OverlayConfigComponentSP>();

            if (!canvasH.isValid() || !egoH.isValid() || !refH.isValid() || !cfgH.isValid())
                return;

            const CanvasConfigComponent &canvas = *canvasH.get();
            const EgoStateComponent &ego = *egoH.get();
            const RefLineComponent &ref = *refH.get();
            const OverlayConfigComponent &cfg = *cfgH.get();

            dl->clear();

            const int W = canvas.w;
            const int H = canvas.h;

            // While unlocked the window swallows the mouse, so say so plainly
            // rather than leaving the driver wondering why clicks stopped
            // reaching the game.
            auto drawUnlockedChrome = [&]()
            {
                if (!canvas.unlocked)
                    return;
                const Rgb c = kTurnMark;
                dl->rect(0, 0, W, 2, c.r, c.g, c.b);
                dl->rect(0, H - 2, W, 2, c.r, c.g, c.b);
                dl->rect(0, 0, 2, H, c.r, c.g, c.b);
                dl->rect(W - 2, 0, 2, H, c.r, c.g, c.b);

                dl->rect(0, H / 2 - 16, W, 34, kPanel.r, kPanel.g, kPanel.b);
                dl->text(L"drag to move - Ctrl+Shift+M to lock", 10, H / 2 - 9, 16,
                         c.r, c.g, c.b);
            };

            // Layout: view on top, then the bar, then the readout.
            const int readoutH = 34;
            const int barH = 26;
            const int viewH = H - readoutH - barH;

            wchar_t buf[128];

            // Status messages get their own backing panel for the same reason.
            auto status = [&](const wchar_t *msg, const Rgb &c)
            {
                dl->rect(0, H / 2 - 18, W, 40, kPanel.r, kPanel.g, kPanel.b);
                dl->text(msg, 10, H / 2 - 11, 18, c.r, c.g, c.b);
            };

            if (canvas.unlocked)
            {
                drawUnlockedChrome();
                return;
            }

            if (!ref.loaded)
            {
                swprintf(buf, 128, L"no reference lap: %hs", ref.error.c_str());
                status(buf, kBad);
                return;
            }

            if (!ego.connected)
            {
                status(L"waiting for iRacing...", kDim);
                return;
            }

            if (!ego.onTrack)
            {
                status(L"not on track", kDim);
                return;
            }

            const RefLine &line = ref.line;

            // The estimator's own offset is authoritative. Recomputing it from
            // the synthesized position is a round trip that can only lose
            // information, and the two drifted apart in practice.
            const RefSample ref0 = refAt(line, ego.pct);
            if (ref0.idx < 0)
                return;

            const float crossTrack = ego.lateral;
            const float speedDelta = ego.speed - ref0.speed;

            const int n = (int)line.pts.size();

            // --- car-centred, track-up view ---------------------------------
            const int cx = W / 2;
            const int cy = (int)(viewH * 0.78f); // car sits low, most room ahead

            const float pxPerMFwd = (float)cy / std::max(cfg.forwardM, 1.0f);
            const float pxPerMLat = pxPerMFwd * cfg.lateralExaggeration;

            float hx = 0, hy = 1;
            headingAt(line, ref0.idx, hx, hy);

            // Reference line in car-relative, heading-up coordinates.
            PolyCmd poly;
            poly.r = kLine.r;
            poly.g = kLine.g;
            poly.b = kLine.b;
            poly.width = 3;

            // Reference-frame metres -> screen, car-centred and heading-up.
            auto toScreen = [&](float wx, float wy, int &sx, int &sy)
            {
                const float dx = wx - ego.x;
                const float dy = wy - ego.y;

                const float fwd = dx * hx + dy * hy;   // along the track
                const float lat = dx * hy - dy * hx;   // + is right of travel

                sx = cx + (int)lroundf(lat * pxPerMLat);
                sy = cy - (int)lroundf(fwd * pxPerMFwd);
            };

            auto pushPoint = [&](const RefPoint &p)
            {
                DrawPoint sp;
                toScreen(p.x, p.y, sp.x, sp.y);
                poly.pts.push_back(sp);
            };

            // Walk back from the car, then forward, so the polyline is ordered.
            int startIdx = ref0.idx;
            {
                float acc = 0;
                while (acc < cfg.behindM)
                {
                    const int prev = ((startIdx - 1) % n + n) % n;
                    acc += segLen(line, prev);
                    startIdx = prev;
                }
            }

            {
                float acc = 0;
                int i = startIdx;
                pushPoint(line.pts[i]);
                const float total = cfg.behindM + cfg.forwardM;
                while (acc < total)
                {
                    acc += segLen(line, i);
                    i = (i + 1) % n;
                    pushPoint(line.pts[i]);
                }
            }

            dl->polys.push_back(poly);

            // --- brake / turn-in / apex markers -------------------------------
            // These come from the reference lap indexed by track position, so
            // unlike the lateral offset they involve no dead reckoning.
            {
                const std::vector<RefEvent> near =
                    eventsNear(line, ego.pct, cfg.forwardM, cfg.behindM);

                for (const RefEvent &e : near)
                {
                    int ex = 0, ey = 0;
                    toScreen(e.x, e.y, ex, ey);
                    if (ey < -20 || ey > viewH + 20)
                        continue;

                    Rgb c = kBrakeMark;
                    const wchar_t *tag = L"B";
                    if (e.kind == RefEventKind::TurnIn)
                    {
                        c = kTurnMark;
                        tag = L"T";
                    }
                    else if (e.kind == RefEventKind::Apex)
                    {
                        c = kApexMark;
                        tag = L"A";
                    }

                    // A gate across the line reads better than a dot on it.
                    dl->rect(ex - 9, ey - 1, 18, 3, c.r, c.g, c.b);
                    dl->text(tag, ex + 12, ey - 9, 15, c.r, c.g, c.b);
                }

                // Countdown to the next braking point, the cue that is hardest
                // to judge by eye and easiest to get from the data.
                for (const RefEvent &e : near)
                {
                    if (e.kind != RefEventKind::Brake)
                        continue;
                    const float d = distanceAhead(line, ego.pct, e.pct);
                    if (d > cfg.brakeCueM)
                        break;

                    dl->rect(0, 6, W, 30, kPanel.r, kPanel.g, kPanel.b);
                    swprintf(buf, 128, L"BRAKE IN %3.0f m", d);
                    dl->text(buf, 10, 10, 21, kBrakeMark.r, kBrakeMark.g, kBrakeMark.b);
                    break;
                }
            }

            // --- car marker ---------------------------------------------------
            const float absErr = fabsf(crossTrack);
            const Rgb col = errorColour(absErr, cfg);

            // The car is at the origin of this view by construction, so its
            // offset from the line is shown by the line, not by the marker.
            // Dark ring first: a bare coloured dot disappears into a busy scene.
            dl->disc(cx, cy, 7, kPanel.r, kPanel.g, kPanel.b);
            dl->disc(cx, cy, 5, col.r, col.g, col.b);

            // --- backing panel for the bar and readout ------------------------
            // Emitted before the bar chrome so it sits behind it: the renderer
            // draws all rects in order, then polylines, discs and text.
            dl->rect(0, viewH, W, barH + readoutH, kPanel.r, kPanel.g, kPanel.b);

            // --- cross-track bar ----------------------------------------------
            const int barY = viewH + 4;
            const int barMid = W / 2;
            const int barHalf = W / 2 - 12;

            dl->rect(barMid - barHalf, barY + barH / 2 - 1, barHalf * 2, 2,
                     kDim.r, kDim.g, kDim.b);
            dl->rect(barMid - 1, barY + 2, 2, barH - 4, kDim.r, kDim.g, kDim.b);

            {
                // Fly-to, like a localizer needle: centre is the car and the
                // marker is where the reference line is, so the correction is
                // always "steer toward the marker". Showing the car's own
                // displacement instead would have to be mentally inverted
                // every time.
                float f = -crossTrack / std::max(cfg.barRangeM, 0.1f);
                f = std::max(-1.0f, std::min(1.0f, f));
                const int mx = barMid + (int)lroundf(f * (float)barHalf);
                dl->rect(mx - 3, barY + 2, 6, barH - 4, col.r, col.g, col.b);
            }

            // --- readout -------------------------------------------------------
            const int textY = viewH + barH + 4;

            // Positive cross-track means the car is right of the line, so the
            // correction is to move left. Beyond any plausible distance from
            // the racing line the reconstruction has lost track of the car, and
            // saying nothing beats showing a confident wrong number.
            if (absErr > cfg.implausibleM)
            {
                dl->text(L"-- m", 10, textY, 22, kDim.r, kDim.g, kDim.b);
            }
            else
            {
                // The direction named is the one to steer, not the side the car
                // is on: being right of the line means "go LEFT".
                swprintf(buf, 128, L"%s %.1f m", absErr < 0.1f ? L" " : (crossTrack > 0 ? L"◀" : L"▶"),
                         absErr);
                dl->text(buf, 10, textY, 22, col.r, col.g, col.b);
            }

            {
                const float k = cfg.mph ? 2.2369363f : 3.6f;
                const float d = speedDelta * k;
                const Rgb sc = (d >= 0) ? kGood : kBad;
                // Wide enough for the longest form, "-207 km/h", at 22 px bold.
                swprintf(buf, 128, L"%+.0f %s", d, cfg.mph ? L"mph" : L"km/h");
                dl->text(buf, W - 132, textY, 22, sc.r, sc.g, sc.b);
            }
        });
}
