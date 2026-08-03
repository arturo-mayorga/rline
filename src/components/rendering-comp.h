#ifndef rendering_comp_h_
#define rendering_comp_h_

// Backend-agnostic draw list. Systems append primitives; the window system is
// the only thing that knows about GDI.

#include "../ecs.h"

#include <memory>
#include <string>
#include <vector>

struct CanvasConfigComponent
{
    ECS_DECLARE_TYPE;

    CanvasConfigComponent() { id = CanvasConfigComponent::autoID++; }

    int x = 0;
    int y = 0;
    int w = 0;
    int h = 0;
    int id = 0;

    // Unlocked means the window accepts the mouse so it can be dragged; locked
    // means clicks pass through to the game. Owned by the window system, read
    // by the overlay so it can show that it is grabbable.
    bool unlocked = false;

    static int autoID;
};
ECS_DEFINE_TYPE(CanvasConfigComponent);
typedef std::shared_ptr<CanvasConfigComponent> CanvasConfigComponentSP;

struct DrawPoint
{
    int x = 0;
    int y = 0;
};

struct RectCmd
{
    int x, y, w, h;
    int r, g, b;
};

struct TextCmd
{
    std::wstring text;
    int x, y;
    int h; // cell height in pixels
    int r, g, b;
    bool bold;
};

struct PolyCmd
{
    std::vector<DrawPoint> pts;
    int r, g, b;
    int width;
};

struct DiscCmd
{
    int cx, cy, radius;
    int r, g, b;
};

// One list per canvas, rebuilt every frame.
struct DrawListComponent
{
    ECS_DECLARE_TYPE;

    std::vector<RectCmd> rects;
    std::vector<PolyCmd> polys;
    std::vector<DiscCmd> discs;
    std::vector<TextCmd> texts;

    void clear()
    {
        rects.clear();
        polys.clear();
        discs.clear();
        texts.clear();
    }

    void rect(int x, int y, int w, int h, int r, int g, int b)
    {
        rects.push_back(RectCmd{x, y, w, h, r, g, b});
    }

    void disc(int cx, int cy, int radius, int r, int g, int b)
    {
        discs.push_back(DiscCmd{cx, cy, radius, r, g, b});
    }

    void text(const std::wstring &s, int x, int y, int h, int r, int g, int b, bool bold = true)
    {
        texts.push_back(TextCmd{s, x, y, h, r, g, b, bold});
    }
};
ECS_DEFINE_TYPE(DrawListComponent);
typedef std::shared_ptr<DrawListComponent> DrawListComponentSP;

#endif
