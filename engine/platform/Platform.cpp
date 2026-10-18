#include "platform/Platform.h"

#include <SDL3/SDL_clipboard.h>
#include <SDL3/SDL_events.h>
#include <SDL3/SDL_filesystem.h>
#include <SDL3/SDL_gamepad.h>
#include <SDL3/SDL_init.h>
#include <SDL3/SDL_messagebox.h>
#include <SDL3/SDL_mouse.h>

namespace en::platform {

Platform::~Platform() noexcept {
    Shutdown();
}

Result<void, PlatformErrc> Platform::Init(std::string_view appName, std::string_view orgName) noexcept {
    if (m_initialized) return {};

    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMEPAD)) {
        return Err(PlatformErrc::SdlInitFailed);
    }

    if (const char* base = SDL_GetBasePath()) {
        (void)m_basePath.AssignNormalizedDir(base);
    }

    const FixedString<128> org(orgName);
    const FixedString<128> app(appName);
    if (char* pref = SDL_GetPrefPath(org.CStr(), app.CStr())) {
        (void)m_prefPath.AssignNormalizedDir(pref);
        SDL_free(pref);
    }

    m_activeGamepadMask = 0;
    m_rumbleGamepadMask = 0;
    m_frameEvents.ClearInput(false);
    m_initialized = true;
    m_quit = false;
    return {};
}

void Platform::Shutdown() noexcept {
    if (!m_initialized) return;

    for (u32 m = m_activeGamepadMask; m != 0; m &= (m - 1u)) {
        const u8 slot = static_cast<u8>(std::countr_zero(m));
        if (m_gamepadHandles[slot]) {
            SDL_CloseGamepad(m_gamepadHandles[slot]);
            m_gamepadHandles[slot] = nullptr;
            m_gamepadIds[slot] = 0;
        }
    }

    m_activeGamepadMask = 0;
    m_rumbleGamepadMask = 0;
    SDL_Quit();
    m_initialized = false;
}

void Platform::OpenGamepad(u32 instanceId) noexcept {
    if (FindGamepadSlot(instanceId) < kMaxGamepads) return;

    const u32 freeSlot = static_cast<u32>(std::countr_one(m_activeGamepadMask));
    if (freeSlot >= kMaxGamepads) return;

    if (SDL_Gamepad* pad = SDL_OpenGamepad(instanceId)) {
        (void)SDL_SetGamepadPlayerIndex(pad, static_cast<int>(freeSlot));
        m_gamepadHandles[freeSlot] = pad;
        m_gamepadIds[freeSlot] = instanceId;
        m_activeGamepadMask |= static_cast<u8>(1u << freeSlot);
    }
}

u8 Platform::CloseGamepad(u32 instanceId) noexcept {
    const u8 slot = FindGamepadSlot(instanceId);
    if (slot >= kMaxGamepads) return 0xFFu;

    SDL_CloseGamepad(m_gamepadHandles[slot]);
    m_gamepadHandles[slot] = nullptr;
    m_gamepadIds[slot] = 0;

    const u8 clearBit = static_cast<u8>(~(1u << slot));
    m_activeGamepadMask &= clearBit;
    m_rumbleGamepadMask &= clearBit;
    return slot;
}

bool Platform::RumbleGamepad(u8 padSlot, u16 lowFreq, u16 highFreq, u32 durationMs) noexcept {
    if (padSlot >= kMaxGamepads) return false;
    const u8 bit = static_cast<u8>(1u << padSlot);
    if ((m_activeGamepadMask & bit) == 0) return false;

    const bool ok = SDL_RumbleGamepad(m_gamepadHandles[padSlot], lowFreq, highFreq, durationMs);
    if (ok) {
        if ((lowFreq | highFreq) != 0 && durationMs != 0) {
            m_rumbleGamepadMask |= bit;
        } else {
            m_rumbleGamepadMask &= static_cast<u8>(~bit);
        }
    }
    return ok;
}

void Platform::StopAllRumble() noexcept {
    if (m_rumbleGamepadMask == 0) return;

    for (u32 m = m_rumbleGamepadMask; m != 0; m &= (m - 1u)) {
        const u8 slot = static_cast<u8>(std::countr_zero(m));
        if (m_gamepadHandles[slot]) {
            (void)SDL_RumbleGamepad(m_gamepadHandles[slot], 0, 0, 0);
        }
    }
    m_rumbleGamepadMask = 0;
}

WindowFlags Platform::Pump(Window& window, EventTap tap) noexcept {
    m_frameEvents.ClearInput(false);

    WindowFlags flags = WindowFlags::None;
    bool needMetricRefresh = false;
    const u32 winId = window.Id();

    SDL_Event ev;
    while (SDL_PollEvent(&ev)) {
        if (ev.type >= SDL_EVENT_WINDOW_FIRST && ev.type <= SDL_EVENT_WINDOW_LAST) {
            const WindowFlags winFlags = window.ProcessEvent(ev, needMetricRefresh);
            flags |= winFlags;
            if (HasFlag(winFlags, WindowFlags::FocusLost)) {
                StopAllRumble();
                m_frameEvents.ClearInput(true);
            }
            if (HasFlag(winFlags, WindowFlags::CloseRequested)) {
                m_quit = true;
            }
            continue;
        }

        switch (ev.type) {
            case SDL_EVENT_POLL_SENTINEL:
                continue;

            case SDL_EVENT_QUIT:
            case SDL_EVENT_TERMINATING:
                m_quit = true;
                continue;

            case SDL_EVENT_WILL_ENTER_BACKGROUND:
                StopAllRumble();
                m_frameEvents.ClearInput(true);
                needMetricRefresh = true;
                continue;

            case SDL_EVENT_GAMEPAD_ADDED:
                OpenGamepad(ev.gdevice.which);
                continue;

            case SDL_EVENT_GAMEPAD_REMOVED:
                if (const u8 slot = CloseGamepad(ev.gdevice.which); slot < kMaxGamepads) {
                    m_frameEvents.disconnectedPadMask |= static_cast<u8>(1u << slot);
                }
                continue;

            case SDL_EVENT_DISPLAY_ADDED:
            case SDL_EVENT_DISPLAY_REMOVED:
            case SDL_EVENT_DISPLAY_DESKTOP_MODE_CHANGED:
            case SDL_EVENT_DISPLAY_CURRENT_MODE_CHANGED:
            case SDL_EVENT_DISPLAY_CONTENT_SCALE_CHANGED:
                needMetricRefresh = true;
                flags |= WindowFlags::DisplayChanged | WindowFlags::Resized;
                continue;

            default:
                break;
        }

        const EventCapture capture = tap.callback ? tap.callback(tap.user, ev) : EventCapture::None;
        const bool isFocused = window.State().focused;

        switch (ev.type) {
            case SDL_EVENT_KEY_DOWN:
            case SDL_EVENT_KEY_UP:
                if (!ev.key.repeat && (ev.key.windowID == 0 || ev.key.windowID == winId)) {
                    if (!ev.key.down || (isFocused && !HasCapture(capture, EventCapture::Keyboard))) {
                        m_frameEvents.PushEvent(
                            InputEventKind::Key, 0, static_cast<u16>(ev.key.scancode), ev.key.down ? i16{1} : i16{0});
                    }
                }
                break;

            case SDL_EVENT_MOUSE_BUTTON_DOWN:
            case SDL_EVENT_MOUSE_BUTTON_UP:
                if (ev.button.which != SDL_TOUCH_MOUSEID && (ev.button.windowID == 0 || ev.button.windowID == winId)) {
                    if (!ev.button.down || (isFocused && !HasCapture(capture, EventCapture::Mouse))) {
                        m_frameEvents.PushEvent(
                            InputEventKind::MouseButton, 0, ev.button.button, ev.button.down ? i16{1} : i16{0});
                    }
                }
                break;

            case SDL_EVENT_MOUSE_MOTION:
                if (isFocused &&
                    ev.motion.which != SDL_TOUCH_MOUSEID &&
                    (ev.motion.windowID == 0 || ev.motion.windowID == winId) &&
                    !HasCapture(capture, EventCapture::Mouse)) {
                    m_frameEvents.mouseX = ev.motion.x;
                    m_frameEvents.mouseY = ev.motion.y;
                    m_frameEvents.mouseDx += ev.motion.xrel;
                    m_frameEvents.mouseDy += ev.motion.yrel;
                    m_frameEvents.hasMouseMotion = true;
                }
                break;

            case SDL_EVENT_MOUSE_WHEEL:
                if (isFocused &&
                    ev.wheel.which != SDL_TOUCH_MOUSEID &&
                    (ev.wheel.windowID == 0 || ev.wheel.windowID == winId) &&
                    !HasCapture(capture, EventCapture::Mouse)) {
                    const f32 flip = (ev.wheel.direction == SDL_MOUSEWHEEL_FLIPPED) ? -1.0f : 1.0f;
                    m_frameEvents.wheelX += ev.wheel.x * flip;
                    m_frameEvents.wheelY += ev.wheel.y * flip;
                }
                break;

            case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
            case SDL_EVENT_GAMEPAD_BUTTON_UP:
                if (!ev.gbutton.down || (isFocused && !HasCapture(capture, EventCapture::Gamepad))) {
                    if (const u8 slot = FindGamepadSlot(ev.gbutton.which); slot < kMaxGamepads) {
                        m_frameEvents.PushEvent(
                            InputEventKind::GamepadButton, slot, ev.gbutton.button, ev.gbutton.down ? i16{1} : i16{0});
                    }
                }
                break;

            case SDL_EVENT_GAMEPAD_AXIS_MOTION:
                if (isFocused) {
                    if (const u8 slot = FindGamepadSlot(ev.gaxis.which); slot < kMaxGamepads) {
                        m_frameEvents.PushEvent(InputEventKind::GamepadAxis, slot, ev.gaxis.axis, ev.gaxis.value);
                    }
                }
                break;

            default:
                break;
        }
    }

    if (needMetricRefresh) {
        window.RefreshMetrics();
    }

    if (m_frameEvents.hasMouseMotion) {
        const f32 density = window.State().pixelDensity;
        m_frameEvents.mouseX *= density;
        m_frameEvents.mouseY *= density;
        m_frameEvents.mouseDx *= density;
        m_frameEvents.mouseDy *= density;
    }

    return flags;
}

std::string_view Platform::GetClipboard(std::span<char> outBuf) noexcept {
    if (outBuf.empty() || !SDL_HasClipboardText()) return {};

    char* text = SDL_GetClipboardText();
    if (!text) return {};

    usize i = 0;
    const usize maxLen = outBuf.size() - 1;
    while (i < maxLen && text[i] != '\0') {
        outBuf[i] = text[i];
        ++i;
    }
    outBuf[i] = '\0';
    SDL_free(text);
    return {outBuf.data(), i};
}

Result<void, PlatformErrc> Platform::SetClipboard(std::string_view utf8) noexcept {
    const FixedString<4096> buf(utf8);
    if (!SDL_SetClipboardText(buf.CStr())) {
        return Err(PlatformErrc::ClipboardFailed);
    }
    return {};
}

void Platform::ShowErrorBox(std::string_view title, std::string_view message, const Window* parent) noexcept {
    const FixedString<256> t(title);
    const FixedString<1024> m(message);
    SDL_Window* nativeParent = parent ? parent->Native() : nullptr;
    (void)SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, t.CStr(), m.CStr(), nativeParent);
}

} // namespace en::platform
