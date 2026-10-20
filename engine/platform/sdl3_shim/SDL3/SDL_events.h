#pragma once
#include "SDL_video.h"
#include "SDL_mouse.h"
#include "SDL_gamepad.h"
#include <vector>

enum SDL_EventType : Uint32 {
    SDL_EVENT_FIRST = 0,
    SDL_EVENT_QUIT = 0x100,
    SDL_EVENT_TERMINATING = 0x101,
    SDL_EVENT_WILL_ENTER_BACKGROUND = 0x103,
    SDL_EVENT_DISPLAY_ADDED = 0x151,
    SDL_EVENT_DISPLAY_REMOVED = 0x152,
    SDL_EVENT_DISPLAY_DESKTOP_MODE_CHANGED = 0x153,
    SDL_EVENT_DISPLAY_CURRENT_MODE_CHANGED = 0x154,
    SDL_EVENT_DISPLAY_CONTENT_SCALE_CHANGED = 0x155,
    SDL_EVENT_WINDOW_FIRST = 0x202,
    SDL_EVENT_WINDOW_SHOWN = 0x202,
    SDL_EVENT_WINDOW_HIDDEN = 0x203,
    SDL_EVENT_WINDOW_EXPOSED = 0x204,
    SDL_EVENT_WINDOW_RESIZED = 0x206,
    SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED = 0x207,
    SDL_EVENT_WINDOW_MINIMIZED = 0x209,
    SDL_EVENT_WINDOW_RESTORED = 0x20B,
    SDL_EVENT_WINDOW_FOCUS_GAINED = 0x20E,
    SDL_EVENT_WINDOW_FOCUS_LOST = 0x20F,
    SDL_EVENT_WINDOW_CLOSE_REQUESTED = 0x210,
    SDL_EVENT_WINDOW_DISPLAY_CHANGED = 0x213,
    SDL_EVENT_WINDOW_DISPLAY_SCALE_CHANGED = 0x214,
    SDL_EVENT_WINDOW_OCCLUDED = 0x215,
    SDL_EVENT_WINDOW_ENTER_FULLSCREEN = 0x216,
    SDL_EVENT_WINDOW_LEAVE_FULLSCREEN = 0x217,
    SDL_EVENT_WINDOW_LAST = 0x218,
    SDL_EVENT_KEY_DOWN = 0x300,
    SDL_EVENT_KEY_UP = 0x301,
    SDL_EVENT_MOUSE_MOTION = 0x400,
    SDL_EVENT_MOUSE_BUTTON_DOWN = 0x401,
    SDL_EVENT_MOUSE_BUTTON_UP = 0x402,
    SDL_EVENT_MOUSE_WHEEL = 0x403,
    SDL_EVENT_GAMEPAD_AXIS_MOTION = 0x650,
    SDL_EVENT_GAMEPAD_BUTTON_DOWN = 0x651,
    SDL_EVENT_GAMEPAD_BUTTON_UP = 0x652,
    SDL_EVENT_GAMEPAD_ADDED = 0x653,
    SDL_EVENT_GAMEPAD_REMOVED = 0x654,
    SDL_EVENT_POLL_SENTINEL = 0x7F00
};

struct SDL_WindowEvent { Uint32 type; Uint32 reserved; Uint64 timestamp; SDL_WindowID windowID; Sint32 data1; Sint32 data2; };
struct SDL_KeyboardEvent { Uint32 type; Uint32 reserved; Uint64 timestamp; SDL_WindowID windowID; Uint32 which; int scancode; int key; Uint16 mod; Uint16 raw; bool down; bool repeat; };
struct SDL_MouseMotionEvent { Uint32 type; Uint32 reserved; Uint64 timestamp; SDL_WindowID windowID; Uint32 which; Uint32 state; float x; float y; float xrel; float yrel; };
struct SDL_MouseButtonEvent { Uint32 type; Uint32 reserved; Uint64 timestamp; SDL_WindowID windowID; Uint32 which; Uint8 button; bool down; Uint8 clicks; Uint8 padding; float x; float y; };
struct SDL_MouseWheelEvent { Uint32 type; Uint32 reserved; Uint64 timestamp; SDL_WindowID windowID; Uint32 which; float x; float y; int direction; float mouse_x; float mouse_y; };
struct SDL_GamepadDeviceEvent { Uint32 type; Uint32 reserved; Uint64 timestamp; Uint32 which; };
struct SDL_GamepadButtonEvent { Uint32 type; Uint32 reserved; Uint64 timestamp; Uint32 which; Uint8 button; bool down; };
struct SDL_GamepadAxisEvent { Uint32 type; Uint32 reserved; Uint64 timestamp; Uint32 which; Uint8 axis; Sint16 value; };

union SDL_Event {
    Uint32 type;
    SDL_WindowEvent window;
    SDL_KeyboardEvent key;
    SDL_MouseMotionEvent motion;
    SDL_MouseButtonEvent button;
    SDL_MouseWheelEvent wheel;
    SDL_GamepadDeviceEvent gdevice;
    SDL_GamepadButtonEvent gbutton;
    SDL_GamepadAxisEvent gaxis;
    Uint8 padding[128];
};

inline std::vector<SDL_Event>& MockEventQueue() noexcept {
    static std::vector<SDL_Event> q;
    return q;
}
inline bool SDL_PushEvent(SDL_Event* ev) noexcept {
    if (ev) MockEventQueue().push_back(*ev);
    return true;
}
#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include "SDL_hints.h"

inline int Win32VkToSdlScancode(WPARAM vk) noexcept {
    switch (vk) {
        case 'A': case VK_LEFT:   return 4;
        case 'D': case VK_RIGHT:  return 7;
        case 'W': case VK_UP:     return 26;
        case 'S': case VK_DOWN:   return 22;
        case VK_SPACE:            return 44;
        case VK_SHIFT: case VK_LSHIFT: case VK_RSHIFT: case 'L': return 225;
        case 'J': case 'Z':       return 13;
        case 'K': case 'X':       return 14;
        case 'U': case 'C':       return 24;
        case 'I': case 'F':       return 12;
        case VK_ESCAPE: case 'P': return 41;
        default: return 0;
    }
}

inline LRESULT CALLBACK EngineNWin32WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) noexcept {
    switch (msg) {
        case WM_CLOSE:
        case WM_DESTROY: {
            SDL_Event ev{};
            ev.type = SDL_EVENT_QUIT;
            SDL_PushEvent(&ev);
            return 0;
        }
        case WM_SIZE: {
            const int nw = LOWORD(lParam);
            const int nh = HIWORD(lParam);
            if (nw > 0 && nh > 0) {
                SDL_Event ev{};
                ev.type = SDL_EVENT_WINDOW_RESIZED;
                ev.window.windowID = 1;
                ev.window.data1 = nw;
                ev.window.data2 = nh;
                SDL_PushEvent(&ev);
            }
            return 0;
        }
        case WM_KEYDOWN:
        case WM_KEYUP: {
            const bool down = (msg == WM_KEYDOWN);
            const bool repeat = down && ((lParam & (1LL << 30)) != 0);
            const int sc = Win32VkToSdlScancode(wParam);
            if (sc != 0) {
                SDL_Event ev{};
                ev.type = down ? SDL_EVENT_KEY_DOWN : SDL_EVENT_KEY_UP;
                ev.key.windowID = 1;
                ev.key.scancode = sc;
                ev.key.down = down;
                ev.key.repeat = repeat;
                SDL_PushEvent(&ev);
            }
            return 0;
        }
        case WM_LBUTTONDOWN:
        case WM_LBUTTONUP: {
            SDL_Event ev{};
            ev.type = (msg == WM_LBUTTONDOWN) ? SDL_EVENT_MOUSE_BUTTON_DOWN : SDL_EVENT_MOUSE_BUTTON_UP;
            ev.button.windowID = 1;
            ev.button.button = 1;
            ev.button.down = (msg == WM_LBUTTONDOWN);
            SDL_PushEvent(&ev);
            return 0;
        }
        case WM_RBUTTONDOWN:
        case WM_RBUTTONUP: {
            SDL_Event ev{};
            ev.type = (msg == WM_RBUTTONDOWN) ? SDL_EVENT_MOUSE_BUTTON_DOWN : SDL_EVENT_MOUSE_BUTTON_UP;
            ev.button.windowID = 1;
            ev.button.button = 3;
            ev.button.down = (msg == WM_RBUTTONDOWN);
            SDL_PushEvent(&ev);
            return 0;
        }
        default:
            break;
    }
    return DefWindowProcA(hwnd, msg, wParam, lParam);
}
#endif

inline SDL_Window* SDL_CreateWindow(const char* title, int w, int h, SDL_WindowFlags flags) noexcept {
    auto* win = new SDL_Window{};
    win->w = w;
    win->h = h;
    win->flags = flags | SDL_WINDOW_INPUT_FOCUS;
#if defined(_WIN32)
    if (!SDL_IsDummyVideoDriver()) {
        HINSTANCE hInst = GetModuleHandleA(nullptr);
        static bool s_registered = false;
        if (!s_registered) {
            WNDCLASSA wc{};
            wc.lpfnWndProc = &EngineNWin32WndProc;
            wc.hInstance = hInst;
            wc.lpszClassName = "EngineN2026WindowClass";
            wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
            RegisterClassA(&wc);
            s_registered = true;
        }
        RECT r{0, 0, w, h};
        AdjustWindowRect(&r, WS_OVERLAPPEDWINDOW, FALSE);
        HWND hwnd = CreateWindowExA(
            0, "EngineN2026WindowClass", title ? title : "EngineN 2026",
            WS_OVERLAPPEDWINDOW | WS_VISIBLE,
            CW_USEDEFAULT, CW_USEDEFAULT, r.right - r.left, r.bottom - r.top,
            nullptr, nullptr, hInst, nullptr);
        win->nativeHwnd = hwnd;
        if (hwnd) ShowWindow(hwnd, SW_SHOW);
    }
#else
    (void)title;
#endif
    return win;
}

inline void SDL_DestroyWindow(SDL_Window* win) noexcept {
#if defined(_WIN32)
    if (win && win->nativeHwnd) {
        DestroyWindow(static_cast<HWND>(win->nativeHwnd));
        win->nativeHwnd = nullptr;
    }
#endif
    delete win;
}

inline bool SDL_PollEvent(SDL_Event* ev) noexcept {
#if defined(_WIN32)
    if (!SDL_IsDummyVideoDriver()) {
        MSG msg{};
        while (PeekMessageA(&msg, nullptr, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) {
                SDL_Event qev{};
                qev.type = SDL_EVENT_QUIT;
                SDL_PushEvent(&qev);
            }
            TranslateMessage(&msg);
            DispatchMessageA(&msg);
        }
    }
#endif
    auto& q = MockEventQueue();
    if (q.empty()) return false;
    if (ev) *ev = q.front();
    q.erase(q.begin());
    return true;
}
