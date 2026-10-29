#include "platform/Platform.h"

#include <SDL3/SDL.h>
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace en {
namespace {

[[nodiscard]] constexpr bool MatchesWindow(u32 targetWinId, u32 eventWinId) noexcept {
    return targetWinId == 0U || eventWinId == 0U || eventWinId == targetWinId;
}

} // namespace

Result<void, PlatformErrc> Platform::Init(
    std::string_view appName, std::string_view orgName, bool headless) noexcept {
    Shutdown();
    m_headless = headless;
    m_quit     = false;
    m_events.ClearFrameDeltas();

    if (!headless) {
        if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_GAMEPAD)) {
            return Err(PlatformErrc::SdlInitFailed);
        }
        (void)SDL_DisableScreenSaver();
    }

    const char* base = SDL_GetBasePath();
    if (!NormalizeDir(base ? base : "./", m_basePath) && !NormalizeDir("./", m_basePath)) {
        if (!headless) SDL_Quit();
        return Err(PlatformErrc::PathRejected);
    }

    const FixedString<64> orgBuf(orgName.empty() ? "EngineN" : orgName);
    const FixedString<64> appBuf(appName.empty() ? "EngineN" : appName);
    char* pref = SDL_GetPrefPath(orgBuf.CStr(), appBuf.CStr());
    const bool prefOk = (pref && NormalizeDir(pref, m_prefPath)) || NormalizeDir("./", m_prefPath);
    if (pref) SDL_free(pref);

    if (!prefOk) {
        if (!headless) SDL_Quit();
        return Err(PlatformErrc::PathRejected);
    }

    m_initialized = true;
    return {};
}

void Platform::Shutdown() noexcept {
    if (!m_initialized) return;
    CloseGamepad(m_gamepadId);
    if (!m_headless) {
        (void)SDL_EnableScreenSaver();
        SDL_Quit();
    }
    m_initialized = false;
}

WindowFlags Platform::Pump(Window& window, EventCapture captured) noexcept {
    m_events.ClearFrameDeltas();
    if (!m_initialized || m_headless) return WindowFlags::None;

    WindowFlags winFlags  = WindowFlags::None;
    bool needRefresh      = false;
    const u32 targetWinId = window.Id();

    SDL_Event ev{};
    while (SDL_PollEvent(&ev)) {
        const bool focused = window.State().focused;
        const f32  density = window.State().pixelDensity;

        switch (ev.type) {
            case SDL_EVENT_QUIT:
            case SDL_EVENT_TERMINATING:
                window.State().ApplyLifecycleEvent(WindowFlags::CloseRequested, winFlags, needRefresh);
                break;

            case SDL_EVENT_WILL_ENTER_BACKGROUND:
                window.State().ApplyLifecycleEvent(
                    WindowFlags::Minimized | WindowFlags::FocusLost, winFlags, needRefresh);
                break;

            case SDL_EVENT_DISPLAY_ADDED:
            case SDL_EVENT_DISPLAY_REMOVED:
            case SDL_EVENT_DISPLAY_DESKTOP_MODE_CHANGED:
            case SDL_EVENT_DISPLAY_CURRENT_MODE_CHANGED:
            case SDL_EVENT_DISPLAY_CONTENT_SCALE_CHANGED:
                window.State().ApplyLifecycleEvent(WindowFlags::DisplayChanged, winFlags, needRefresh);
                break;

            case SDL_EVENT_WINDOW_CLOSE_REQUESTED:
                if (MatchesWindow(targetWinId, ev.window.windowID)) {
                    window.State().ApplyLifecycleEvent(WindowFlags::CloseRequested, winFlags, needRefresh);
                }
                break;

            case SDL_EVENT_WINDOW_RESIZED:
            case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED:
                if (MatchesWindow(targetWinId, ev.window.windowID)) {
                    window.State().ApplyLifecycleEvent(WindowFlags::Resized, winFlags, needRefresh);
                }
                break;

            case SDL_EVENT_WINDOW_DISPLAY_CHANGED:
            case SDL_EVENT_WINDOW_DISPLAY_SCALE_CHANGED:
            case SDL_EVENT_WINDOW_ENTER_FULLSCREEN:
            case SDL_EVENT_WINDOW_LEAVE_FULLSCREEN:
                if (MatchesWindow(targetWinId, ev.window.windowID)) {
                    window.State().ApplyLifecycleEvent(
                        WindowFlags::Resized | WindowFlags::DisplayChanged, winFlags, needRefresh);
                }
                break;

            case SDL_EVENT_WINDOW_MINIMIZED:
            case SDL_EVENT_WINDOW_HIDDEN:
            case SDL_EVENT_WINDOW_OCCLUDED:
                if (MatchesWindow(targetWinId, ev.window.windowID)) {
                    window.State().ApplyLifecycleEvent(
                        WindowFlags::Minimized | WindowFlags::FocusLost, winFlags, needRefresh);
                }
                break;

            case SDL_EVENT_WINDOW_RESTORED:
            case SDL_EVENT_WINDOW_SHOWN:
            case SDL_EVENT_WINDOW_EXPOSED:
                if (MatchesWindow(targetWinId, ev.window.windowID)) {
                    window.State().ApplyLifecycleEvent(WindowFlags::Restored, winFlags, needRefresh);
                }
                break;

            case SDL_EVENT_WINDOW_FOCUS_GAINED:
                if (MatchesWindow(targetWinId, ev.window.windowID)) {
                    window.State().ApplyLifecycleEvent(WindowFlags::FocusGained, winFlags, needRefresh);
                }
                break;

            case SDL_EVENT_WINDOW_FOCUS_LOST:
                if (MatchesWindow(targetWinId, ev.window.windowID)) {
                    window.State().ApplyLifecycleEvent(WindowFlags::FocusLost, winFlags, needRefresh);
                }
                break;

            case SDL_EVENT_KEY_DOWN:
            case SDL_EVENT_KEY_UP:
                if (MatchesWindow(targetWinId, ev.key.windowID)) {
                    m_events.IngestKey(
                        static_cast<u16>(ev.key.scancode),
                        ev.type == SDL_EVENT_KEY_DOWN,
                        ev.key.repeat,
                        focused,
                        captured);
                }
                break;

            case SDL_EVENT_MOUSE_BUTTON_DOWN:
            case SDL_EVENT_MOUSE_BUTTON_UP:
                if (ev.button.which != SDL_TOUCH_MOUSEID &&
                    MatchesWindow(targetWinId, ev.button.windowID)) {
                    m_events.IngestMouseButton(
                        static_cast<u16>(ev.button.button),
                        ev.type == SDL_EVENT_MOUSE_BUTTON_DOWN,
                        ev.button.x,
                        ev.button.y,
                        density,
                        focused,
                        captured);
                }
                break;

            case SDL_EVENT_MOUSE_MOTION:
                if (ev.motion.which != SDL_TOUCH_MOUSEID &&
                    MatchesWindow(targetWinId, ev.motion.windowID)) {
                    m_events.IngestMouseMotion(
                        ev.motion.x,
                        ev.motion.y,
                        ev.motion.xrel,
                        ev.motion.yrel,
                        density,
                        focused,
                        captured);
                }
                break;

            case SDL_EVENT_MOUSE_WHEEL:
                if (ev.wheel.which != SDL_TOUCH_MOUSEID &&
                    MatchesWindow(targetWinId, ev.wheel.windowID)) {
                    m_events.IngestMouseWheel(
                        ev.wheel.x,
                        ev.wheel.y,
                        ev.wheel.direction == SDL_MOUSEWHEEL_FLIPPED,
                        focused,
                        captured);
                }
                break;

            case SDL_EVENT_GAMEPAD_ADDED:
                (void)OpenGamepad(ev.gdevice.which);
                break;

            case SDL_EVENT_GAMEPAD_REMOVED:
                CloseGamepad(ev.gdevice.which);
                break;

            case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
            case SDL_EVENT_GAMEPAD_BUTTON_UP:
                if (ev.gbutton.which == m_gamepadId && m_gamepadId != 0U) {
                    m_events.IngestGamepadButton(
                        static_cast<u16>(ev.gbutton.button),
                        ev.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN,
                        focused,
                        captured);
                }
                break;

            case SDL_EVENT_GAMEPAD_AXIS_MOTION:
                if (ev.gaxis.which == m_gamepadId && m_gamepadId != 0U) {
                    m_events.IngestGamepadAxis(
                        static_cast<u16>(ev.gaxis.axis),
                        static_cast<i16>(ev.gaxis.value),
                        focused,
                        captured);
                }
                break;

            case SDL_EVENT_TEXT_INPUT:
                if (MatchesWindow(targetWinId, ev.text.windowID) && ev.text.text) {
                    m_events.IngestTextInput(ev.text.text, focused);
                }
                break;

            default:
                break;
        }
    }

    if (needRefresh) winFlags |= window.RefreshMetrics();
    if (HasFlag(winFlags, WindowFlags::CloseRequested)) m_quit = true;

    if (HasFlag(winFlags, WindowFlags::FocusLost | WindowFlags::Minimized)) {
        StopRumble();
        m_events.mouseDx = 0.0f;
        m_events.mouseDy = 0.0f;
        m_events.wheelX  = 0.0f;
        m_events.wheelY  = 0.0f;
    }
    m_events.focused = window.State().focused ? 1U : 0U;
    return winFlags;
}

bool Platform::OpenGamepad(u32 instanceId) noexcept {
    if (instanceId == 0U) return false;
    if (m_gamepadId == instanceId) return true;
    if (m_gamepadId != 0U) return false;

    if (!m_headless) {
        SDL_Gamepad* pad = SDL_OpenGamepad(instanceId);
        if (!pad) return false;
        (void)SDL_SetGamepadPlayerIndex(pad, 0);
        m_gamepad = pad;
    }
    m_gamepadId = instanceId;
    m_rumbling  = false;
    return true;
}

void Platform::CloseGamepad(u32 instanceId) noexcept {
    if (instanceId == 0U || m_gamepadId != instanceId) return;
    if (m_gamepad) {
        if (m_rumbling) (void)SDL_RumbleGamepad(m_gamepad, 0U, 0U, 0U);
        SDL_CloseGamepad(m_gamepad);
        m_gamepad = nullptr;
    }
    m_gamepadId = 0U;
    m_rumbling  = false;
    m_events.IngestGamepadDisconnect();
}

bool Platform::RumbleGamepad(u16 lowFreq, u16 highFreq, u32 durationMs) noexcept {
    if (m_gamepadId == 0U) return false;
    if (!m_headless) {
        if (!m_gamepad || !SDL_RumbleGamepad(m_gamepad, lowFreq, highFreq, durationMs)) {
            return false;
        }
    }
    m_rumbling = ((lowFreq | highFreq) != 0U) && (durationMs > 0U);
    return true;
}

void Platform::StopRumble() noexcept {
    if (m_rumbling && m_gamepad) (void)SDL_RumbleGamepad(m_gamepad, 0U, 0U, 0U);
    m_rumbling = false;
}

Result<usize, PlatformErrc> Platform::ReadFile(
    const FilePath& path, std::span<u8> outBuf) noexcept {
    if (path.Empty()) return Err(PlatformErrc::PathRejected);

    SDL_IOStream* io = SDL_IOFromFile(path.CStr(), "rb");
    if (!io) return Err(PlatformErrc::FileIoFailed);

    const i64 fsizeSigned = SDL_GetIOSize(io);
    if (fsizeSigned < 0 || static_cast<usize>(fsizeSigned) > outBuf.size()) {
        (void)SDL_CloseIO(io);
        return Err(PlatformErrc::FileIoFailed);
    }

    const usize fsize     = static_cast<usize>(fsizeSigned);
    const usize bytesRead = fsize > 0U ? SDL_ReadIO(io, outBuf.data(), fsize) : 0U;
    (void)SDL_CloseIO(io);
    if (bytesRead != fsize) return Err(PlatformErrc::FileIoFailed);
    return bytesRead;
}

Result<void, PlatformErrc> Platform::WriteFileAtomic(
    const FilePath& path, std::span<const u8> data) noexcept {
    if (path.Empty()) return Err(PlatformErrc::PathRejected);
    if (path.View().size() + 40U >= FilePath::Capacity()) return Err(PlatformErrc::PathRejected);

    static u32 s_atomicSeq{0};
    const u32 seq = ++s_atomicSeq;
    char tempPathBuf[FilePath::Capacity() + 48U]{};
    const int n = std::snprintf(
        tempPathBuf,
        sizeof(tempPathBuf),
        "%s.%llx.%x.tmp",
        path.CStr(),
        static_cast<unsigned long long>(SDL_GetTicks()),
        static_cast<unsigned>(seq));
    if (n <= 0 || static_cast<usize>(n) >= sizeof(tempPathBuf)) {
        return Err(PlatformErrc::PathRejected);
    }

    SDL_IOStream* io = SDL_IOFromFile(tempPathBuf, "wb");
    if (!io) return Err(PlatformErrc::FileIoFailed);

    bool writeOk = data.empty() || (SDL_WriteIO(io, data.data(), data.size()) == data.size());
    if (!SDL_FlushIO(io)) writeOk = false;
    if (!SDL_CloseIO(io)) writeOk = false;

    if (!writeOk || !SDL_RenamePath(tempPathBuf, path.CStr())) {
        (void)SDL_RemovePath(tempPathBuf);
        return Err(PlatformErrc::FileIoFailed);
    }
    return {};
}

bool Platform::SetClipboardText(std::string_view text) noexcept {
    if (SDL_WasInit(SDL_INIT_VIDEO) == 0U) return false;
    const FixedString<1024> buf(text);
    return SDL_SetClipboardText(buf.CStr());
}

usize Platform::GetClipboardText(std::span<char> outBuf) noexcept {
    if (outBuf.empty()) return 0U;
    outBuf[0] = '\0';
    if (SDL_WasInit(SDL_INIT_VIDEO) == 0U || !SDL_HasClipboardText()) return 0U;

    char* clip = SDL_GetClipboardText();
    if (!clip) return 0U;
    const usize copyLen = std::min<usize>(std::strlen(clip), outBuf.size() - 1U);
    if (copyLen > 0U) std::memcpy(outBuf.data(), clip, copyLen);
    outBuf[copyLen] = '\0';
    SDL_free(clip);
    return copyLen;
}

void Platform::ShowErrorBox(
    std::string_view title, std::string_view message, const Window* parent) noexcept {
    const FixedString<128> titleBuf(title);
    const FixedString<512> msgBuf(message);
    std::fprintf(stderr, "[Platform] %s: %s\n", titleBuf.CStr(), msgBuf.CStr());
    if (SDL_WasInit(SDL_INIT_VIDEO) != 0U && std::getenv("CI") == nullptr) {
        (void)SDL_ShowSimpleMessageBox(
            SDL_MESSAGEBOX_ERROR, titleBuf.CStr(), msgBuf.CStr(), parent ? parent->Native() : nullptr);
    }
}

} // namespace en
