#include "window-rendering-sys.h"
#include "../components/rendering-comp.h"

#include <cstdio>
#include <string>
#include <tchar.h>
#include <vector>
#include <windows.h>

namespace
{
    const TCHAR *kClassName = _T("rline-overlay");

    // Pixels in this colour are punched out of the window entirely, so no
    // drawing command may use it.
    const COLORREF kColourKey = RGB(255, 0, 255);
    const BYTE kAlpha = 230; // ~90% opaque for everything else

    // Present on every supported Windows version, unlike the reference
    // project's "Myriad Pro", which silently falls back to whatever GDI picks.
    const TCHAR *kFontFace = _T("Segoe UI");

    const float kRedrawIntervalMs = 16.0f; // ~60 Hz

    // Global hotkey, so it works while iRacing has focus.
    const int kHotkeyId = 1;

    // Position is remembered between runs; nobody wants to re-place an overlay
    // every session.
    std::string positionFilePath()
    {
        char exe[MAX_PATH] = {};
        if (!GetModuleFileNameA(NULL, exe, MAX_PATH))
            return "";
        std::string dir(exe);
        const size_t slash = dir.find_last_of("\\/");
        if (slash == std::string::npos)
            return "";
        return dir.substr(0, slash + 1) + "rline-pos.txt";
    }
}

bool loadSavedOverlayPosition(int &x, int &y)
{
    const std::string path = positionFilePath();
    if (path.empty())
        return false;
    FILE *f = fopen(path.c_str(), "r");
    if (!f)
        return false;
    int rx = 0, ry = 0;
    const bool ok = (fscanf(f, "%d %d", &rx, &ry) == 2);
    fclose(f);
    if (ok)
    {
        x = rx;
        y = ry;
    }
    return ok;
}

void WindowRenderingSystem::savePosition(HWND hwnd)
{
    RECT r = {};
    if (!GetWindowRect(hwnd, &r))
        return;
    const std::string path = positionFilePath();
    if (path.empty())
        return;
    FILE *f = fopen(path.c_str(), "w");
    if (!f)
        return;
    fprintf(f, "%d %d\n", (int)r.left, (int)r.top);
    fclose(f);
}

void WindowRenderingSystem::setUnlocked(bool unlocked)
{
    _unlocked = unlocked;

    for (auto &kv : _hwndToEntityMap)
    {
        HWND hwnd = kv.first;

        // WS_EX_TRANSPARENT is what makes the window click-through, so it has
        // to come off before the window can be grabbed, and go back on after.
        LONG_PTR ex = GetWindowLongPtr(hwnd, GWL_EXSTYLE);
        if (unlocked)
            ex &= ~(WS_EX_TRANSPARENT);
        else
            ex |= WS_EX_TRANSPARENT;
        SetWindowLongPtr(hwnd, GWL_EXSTYLE, ex);

        kv.second->with<CanvasConfigComponentSP>(
            [&](ECS::ComponentHandle<CanvasConfigComponentSP> cfgH)
            { cfgH.get()->unlocked = unlocked; });

        InvalidateRect(hwnd, NULL, FALSE);
    }
}

WindowRenderingSystem::~WindowRenderingSystem()
{
    for (auto &kv : _hwndToEntityMap)
        DestroyWindow(kv.first);
    _hwndToEntityMap.clear();
}

static HWND openWindow(const CanvasConfigComponent &cfg, WindowRenderingSystem *self)
{
    HWND hwnd = CreateWindowEx(
        // LAYERED for the colour key, TRANSPARENT + NOACTIVATE so clicks and
        // focus pass straight through to the game, TOOLWINDOW to stay out of
        // the alt-tab list.
        WS_EX_LAYERED | WS_EX_TOPMOST | WS_EX_TRANSPARENT | WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW,
        kClassName,
        _T("rline"),
        WS_POPUP,
        cfg.x, cfg.y, cfg.w, cfg.h,
        NULL, NULL, GetModuleHandle(NULL), self);

    if (!hwnd)
        return NULL;

    SetLayeredWindowAttributes(hwnd, kColourKey, kAlpha, LWA_COLORKEY | LWA_ALPHA);
    ShowWindow(hwnd, SW_SHOWNOACTIVATE);
    UpdateWindow(hwnd);

    return hwnd;
}

LRESULT WindowRenderingSystem::windowProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam)
{
    switch (uMsg)
    {
    case WM_PAINT:
    {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hwnd, &ps);

        // Always repaint the whole client area. The back buffer's origin is
        // (0,0), so blitting it to a partial rect's corner would shift the
        // image; the reference project only got away with this because it
        // always invalidated the entire window.
        RECT client;
        GetClientRect(hwnd, &client);
        const int width = client.right - client.left;
        const int height = client.bottom - client.top;

        HDC memDC = CreateCompatibleDC(hdc);
        HBITMAP memBitmap = CreateCompatibleBitmap(hdc, width, height);
        HBITMAP oldBitmap = (HBITMAP)SelectObject(memDC, memBitmap);

        // A compatible bitmap starts uninitialised. Without this fill the
        // window renders as a black box instead of a transparent one.
        {
            HBRUSH keyBrush = CreateSolidBrush(kColourKey);
            FillRect(memDC, &client, keyBrush);
            DeleteObject(keyBrush);
        }

        auto it = _hwndToEntityMap.find(hwnd);
        if (it != _hwndToEntityMap.end())
        {
            it->second->with<DrawListComponentSP>(
                [&](ECS::ComponentHandle<DrawListComponentSP> dlH)
                {
                    const DrawListComponent &dl = *dlH.get();

                    for (const RectCmd &c : dl.rects)
                    {
                        HBRUSH brush = CreateSolidBrush(RGB(c.r, c.g, c.b));
                        RECT rc = {c.x, c.y, c.x + c.w, c.y + c.h};
                        FillRect(memDC, &rc, brush);
                        DeleteObject(brush);
                    }

                    for (const PolyCmd &c : dl.polys)
                    {
                        if (c.pts.size() < 2)
                            continue;

                        std::vector<POINT> pts;
                        pts.reserve(c.pts.size());
                        for (const DrawPoint &p : c.pts)
                            pts.push_back(POINT{p.x, p.y});

                        HPEN pen = CreatePen(PS_SOLID, c.width, RGB(c.r, c.g, c.b));
                        HPEN oldPen = (HPEN)SelectObject(memDC, pen);
                        Polyline(memDC, pts.data(), (int)pts.size());
                        SelectObject(memDC, oldPen);
                        DeleteObject(pen);
                    }

                    for (const DiscCmd &c : dl.discs)
                    {
                        HBRUSH brush = CreateSolidBrush(RGB(c.r, c.g, c.b));
                        HPEN pen = CreatePen(PS_SOLID, 1, RGB(c.r, c.g, c.b));
                        HBRUSH oldBrush = (HBRUSH)SelectObject(memDC, brush);
                        HPEN oldPen = (HPEN)SelectObject(memDC, pen);

                        Ellipse(memDC, c.cx - c.radius, c.cy - c.radius,
                                c.cx + c.radius, c.cy + c.radius);

                        SelectObject(memDC, oldBrush);
                        SelectObject(memDC, oldPen);
                        DeleteObject(brush);
                        DeleteObject(pen);
                    }

                    SetBkMode(memDC, TRANSPARENT);
                    for (const TextCmd &c : dl.texts)
                    {
                        LOGFONT lf = {};
                        lf.lfHeight = -c.h; // negative: character height
                        lf.lfWeight = c.bold ? FW_BOLD : FW_NORMAL;
                        // Greyscale, not ClearType. Subpixel antialiasing tints
                        // glyph edges toward whatever is behind them, and on a
                        // colour-keyed layered window that means magenta fringes
                        // on every character.
                        lf.lfQuality = ANTIALIASED_QUALITY;
                        _tcscpy_s(lf.lfFaceName, kFontFace);

                        HFONT font = CreateFontIndirect(&lf);
                        HFONT oldFont = (HFONT)SelectObject(memDC, font);

                        SetTextColor(memDC, RGB(c.r, c.g, c.b));
                        TextOutW(memDC, c.x, c.y, c.text.c_str(), (int)c.text.length());

                        SelectObject(memDC, oldFont);
                        DeleteObject(font);
                    }
                });
        }

        BitBlt(hdc, 0, 0, width, height, memDC, 0, 0, SRCCOPY);

        SelectObject(memDC, oldBitmap);
        DeleteObject(memBitmap);
        DeleteDC(memDC);

        EndPaint(hwnd, &ps);
        return 0;
    }

    case WM_NCHITTEST:
        // Reporting the whole client area as the title bar lets Windows run
        // its own drag loop, so moving the overlay needs no mouse tracking of
        // our own.
        return _unlocked ? HTCAPTION : HTTRANSPARENT;

    case WM_HOTKEY:
        if (wParam == kHotkeyId)
        {
            setUnlocked(!_unlocked);
            return 0;
        }
        break;

    case WM_EXITSIZEMOVE:
        savePosition(hwnd);
        return 0;

    case WM_ERASEBKGND:
        return 1; // fully repainted in WM_PAINT

    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;

    default:
        break;
    }

    // Also catches the cases above that break out rather than returning.
    return DefWindowProc(hwnd, uMsg, wParam, lParam);
}

void WindowRenderingSystem::configure(class ECS::World *world)
{
    WNDCLASS wc = {};
    wc.lpfnWndProc = WindowRenderingSystem::staticWindowProc;
    wc.hInstance = GetModuleHandle(NULL);
    wc.lpszClassName = kClassName;
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    RegisterClass(&wc);
}

void WindowRenderingSystem::unconfigure(class ECS::World *world)
{
    for (auto &kv : _hwndToEntityMap)
    {
        UnregisterHotKey(kv.first, kHotkeyId);
        DestroyWindow(kv.first);
    }
    _hwndToEntityMap.clear();
}

void WindowRenderingSystem::tick(class ECS::World *world, float deltaTime)
{
    MSG msg = {};
    while (PeekMessage(&msg, NULL, 0, 0, PM_REMOVE))
    {
        // Handled here as well as in the window proc: a hotkey message can
        // arrive with a null hwnd, in which case DispatchMessage drops it.
        if (msg.message == WM_HOTKEY && (int)msg.wParam == kHotkeyId)
        {
            setUnlocked(!_unlocked);
            continue;
        }

        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }

    // Open a window for any canvas that does not have one yet.
    for (ECS::Entity *ent : world->each<CanvasConfigComponentSP>())
    {
        ent->with<CanvasConfigComponentSP>(
            [&](ECS::ComponentHandle<CanvasConfigComponentSP> cfgH)
            {
                const CanvasConfigComponent &cfg = *cfgH.get();
                if (_openCanvasIds.count(cfg.id))
                    return;

                HWND hwnd = openWindow(cfg, this);
                if (hwnd)
                {
                    _openCanvasIds.insert(cfg.id);
                    _hwndToEntityMap[hwnd] = ent;

                    // Global, so it still reaches us while the sim has focus.
                    if (RegisterHotKey(hwnd, kHotkeyId,
                                       MOD_CONTROL | MOD_SHIFT | MOD_NOREPEAT, 'M'))
                    {
                        printf("rline: Ctrl+Shift+M unlocks the overlay for dragging\n");
                    }
                    else
                    {
                        printf("rline: could not register Ctrl+Shift+M (another app "
                               "owns it). Start with --unlocked to reposition.\n");
                    }

                    // Honour a start-unlocked request now the window exists.
                    if (_startUnlocked)
                        setUnlocked(true);
                }
            });
    }

    // Repaint at a fixed rate rather than as fast as the loop spins.
    _redrawAcc += deltaTime;
    if (_redrawAcc >= kRedrawIntervalMs)
    {
        _redrawAcc = 0;
        for (auto &kv : _hwndToEntityMap)
            InvalidateRect(kv.first, NULL, FALSE); // FALSE: no erase, no flicker
    }
}
