#pragma once
#include "SDL_video.h"
#include "SDL_mouse.h"
#include "SDL_gamepad.h"
#include <array>

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
    SDL_EVENT_TEXT_INPUT = 0x303,
    SDL_EVENT_MOUSE_MOTION = 0x400,
    SDL_EVENT_MOUSE_BUTTON_DOWN = 0x401,
    SDL_EVENT_MOUSE_BUTTON_UP = 0x402,
    SDL_EVENT_MOUSE_WHEEL = 0x403,
    SDL_EVENT_GAMEPAD_AXIS_MOTION = 0x650,
    SDL_EVENT_GAMEPAD_BUTTON_DOWN = 0x651,
    SDL_EVENT_GAMEPAD_BUTTON_UP = 0x652,
    SDL_EVENT_GAMEPAD_ADDED = 0x653,
    SDL_EVENT_GAMEPAD_REMOVED = 0x654,
    SDL_EVENT_DROP_FILE = 0x1000,
    SDL_EVENT_POLL_SENTINEL = 0x7F00
};

struct SDL_WindowEvent { Uint32 type; Uint32 reserved; Uint64 timestamp; SDL_WindowID windowID; Sint32 data1; Sint32 data2; };
struct SDL_KeyboardEvent { Uint32 type; Uint32 reserved; Uint64 timestamp; SDL_WindowID windowID; Uint32 which; int scancode; int key; Uint16 mod; Uint16 raw; bool down; bool repeat; };
struct SDL_TextInputEvent { Uint32 type; Uint32 reserved; Uint64 timestamp; SDL_WindowID windowID; const char* text; };
struct SDL_MouseMotionEvent { Uint32 type; Uint32 reserved; Uint64 timestamp; SDL_WindowID windowID; Uint32 which; Uint32 state; float x; float y; float xrel; float yrel; };
struct SDL_MouseButtonEvent { Uint32 type; Uint32 reserved; Uint64 timestamp; SDL_WindowID windowID; Uint32 which; Uint8 button; bool down; Uint8 clicks; Uint8 padding; float x; float y; };
struct SDL_MouseWheelEvent { Uint32 type; Uint32 reserved; Uint64 timestamp; SDL_WindowID windowID; Uint32 which; float x; float y; int direction; float mouse_x; float mouse_y; };
struct SDL_GamepadDeviceEvent { Uint32 type; Uint32 reserved; Uint64 timestamp; Uint32 which; };
struct SDL_GamepadButtonEvent { Uint32 type; Uint32 reserved; Uint64 timestamp; Uint32 which; Uint8 button; bool down; };
struct SDL_GamepadAxisEvent { Uint32 type; Uint32 reserved; Uint64 timestamp; Uint32 which; Uint8 axis; Sint16 value; };
struct SDL_DropEvent { Uint32 type; Uint32 reserved; Uint64 timestamp; SDL_WindowID windowID; float x; float y; const char* source; const char* data; };

union SDL_Event {
    Uint32 type;
    SDL_WindowEvent window;
    SDL_KeyboardEvent key;
    SDL_TextInputEvent text;
    SDL_MouseMotionEvent motion;
    SDL_MouseButtonEvent button;
    SDL_MouseWheelEvent wheel;
    SDL_GamepadDeviceEvent gdevice;
    SDL_GamepadButtonEvent gbutton;
    SDL_GamepadAxisEvent gaxis;
    SDL_DropEvent drop;
    Uint8 padding[128];
};

using SDL_EventFilter = bool (SDLCALL *)(void* userdata, SDL_Event* event);
inline bool SDL_AddEventWatch(SDL_EventFilter, void*) noexcept { return true; }
inline void SDL_RemoveEventWatch(SDL_EventFilter, void*) noexcept {}

struct SdlFixedEventRing {
    static constexpr std::size_t kCapacity = 256;
    std::array<SDL_Event, kCapacity> events{};
    std::size_t head{0};
    std::size_t tail{0};

    constexpr bool Push(const SDL_Event& ev) noexcept {
        if (tail - head >= kCapacity) return false;
        events[tail & (kCapacity - 1U)] = ev;
        ++tail;
        return true;
    }

    constexpr bool Pop(SDL_Event& outEv) noexcept {
        if (head == tail) return false;
        outEv = events[head & (kCapacity - 1U)];
        ++head;
        return true;
    }
};

inline SdlFixedEventRing& GlobalSdlEventRing() noexcept {
    static SdlFixedEventRing s_ring{};
    return s_ring;
}

inline bool SDL_PushEvent(SDL_Event* ev) noexcept {
    return ev ? GlobalSdlEventRing().Push(*ev) : false;
}

inline SDL_Window* SDL_CreateWindow(const char* title, int w, int h, SDL_WindowFlags flags) noexcept {
    (void)title;
    auto* win = new SDL_Window{};
    win->id = 1;
    win->w = w;
    win->h = h;
    win->flags = flags | SDL_WINDOW_INPUT_FOCUS;
    return win;
}

inline void SDL_DestroyWindow(SDL_Window* win) noexcept {
    delete win;
}

inline bool SDL_PollEvent(SDL_Event* ev) noexcept {
    SDL_Event popped{};
    if (!GlobalSdlEventRing().Pop(popped)) return false;
    if (ev) *ev = popped;
    return true;
}
