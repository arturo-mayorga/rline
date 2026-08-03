#ifndef window_rendering_sys_h_
#define window_rendering_sys_h_

#include "../ecs.h"

#include <map>
#include <set>
#include <windows.h>

// Click-through, always-on-top layered window painted with GDI. Colour-key
// transparency, so it needs iRacing in windowed or borderless mode; an
// exclusive-fullscreen swap chain will cover it.
// Position the user last dragged the overlay to, if any. Written whenever a
// drag finishes, so it survives restarts.
bool loadSavedOverlayPosition(int &x, int &y);

class WindowRenderingSystem : public ECS::EntitySystem
{
private:
    std::map<HWND, ECS::Entity *> _hwndToEntityMap;
    std::set<int> _openCanvasIds;
    float _redrawAcc = 0;
    bool _unlocked = false;

    bool _startUnlocked = false;

public:
    // Opens unlocked, for when the hotkey is unavailable.
    void startUnlocked() { _startUnlocked = true; }

private:
    void setUnlocked(bool unlocked);
    void savePosition(HWND hwnd);

public:
    virtual ~WindowRenderingSystem();

    virtual void configure(class ECS::World *world) override;
    virtual void unconfigure(class ECS::World *world) override;
    virtual void tick(class ECS::World *world, float deltaTime) override;

    LRESULT CALLBACK windowProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam);

    static LRESULT CALLBACK staticWindowProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam)
    {
        if (uMsg == WM_NCCREATE)
        {
            CREATESTRUCT *pCreate = (CREATESTRUCT *)lParam;
            SetWindowLongPtr(hwnd, GWLP_USERDATA, (LONG_PTR)pCreate->lpCreateParams);
            return DefWindowProc(hwnd, uMsg, wParam, lParam);
        }

        WindowRenderingSystem *pThis =
            (WindowRenderingSystem *)GetWindowLongPtr(hwnd, GWLP_USERDATA);
        if (pThis)
            return pThis->windowProc(hwnd, uMsg, wParam, lParam);

        return DefWindowProc(hwnd, uMsg, wParam, lParam);
    }
};

#endif
