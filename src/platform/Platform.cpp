#include "platform/Platform.h"
#include "platform/input/SdlInput.h"

#include <SDL3/SDL.h>

namespace EngineN {

EngineError MakePlatformError(PlatformErrc code, std::string_view detail) noexcept {
    if (!detail.empty()) {
        return EngineError(detail, static_cast<i32>(code));
    }
    if (const char* sdlErr = SDL_GetError(); sdlErr && sdlErr[0] != '\0') {
        return EngineError(sdlErr, static_cast<i32>(code));
    }
    return EngineError("Platform error", static_cast<i32>(code));
}

namespace {

[[nodiscard]] constexpr bool IsSafeRelativePath(std::string_view rel) noexcept {
    if (rel.empty() || rel[0] == '/' || rel[0] == '\\') return false;
    if (rel.size() >= 2 && rel[1] == ':') return false;
    if (rel == ".." || rel.starts_with("../") || rel.starts_with("..\\")) return false;
    if (rel.find("/../") != std::string_view::npos || rel.find("\\..\\") != std::string_view::npos) return false;
    if (rel.ends_with("/..") || rel.ends_with("\\..")) return false;
    return rel.find('\0') == std::string_view::npos;
}

[[nodiscard]] Result<FilePath, EngineError> JoinSandboxed(std::string_view root, std::string_view rel) noexcept {
    if (!IsSafeRelativePath(rel)) {
        return Unexpected(MakePlatformError(PlatformErrc::PathRejected));
    }

    std::array<char, kMaxPathBytes> buf{};
    usize len = 0;

    auto appendNormalized = [&](std::string_view s) noexcept -> bool {
        for (char c : s) {
            if (len >= buf.size() - 1) return false;
            buf[len++] = (c == '\\') ? '/' : c;
        }
        return true;
    };

    if (!appendNormalized(root)) {
        return Unexpected(MakePlatformError(PlatformErrc::PathUnavailable));
    }
    if (len > 0 && buf[len - 1] != '/') {
        if (len >= buf.size() - 1) return Unexpected(MakePlatformError(PlatformErrc::PathUnavailable));
        buf[len++] = '/';
    }
    if (!appendNormalized(rel)) {
        return Unexpected(MakePlatformError(PlatformErrc::PathUnavailable));
    }
    buf[len] = '\0';

    return FilePath(std::string_view(buf.data(), len));
}

// Anti-sticking filter: only suppresses Press and Continuous motion events.
// Release events (KEY_UP, BUTTON_UP) always pass through so InputPipeline never retains stuck bits.
[[nodiscard]] constexpr bool ShouldSuppressInput(const SDL_Event& ev, EventCapture cap, u32 winId) noexcept {
    switch (ev.type) {
        case SDL_EVENT_KEY_DOWN:
            return (ev.key.windowID != 0 && ev.key.windowID != winId) ||
                   HasCapture(cap, EventCapture::Keyboard);

        case SDL_EVENT_KEY_UP:
            return (ev.key.windowID != 0 && ev.key.windowID != winId);

        case SDL_EVENT_MOUSE_BUTTON_DOWN:
            return (ev.button.windowID != 0 && ev.button.windowID != winId) ||
                   HasCapture(cap, EventCapture::Mouse);

        case SDL_EVENT_MOUSE_BUTTON_UP:
            return (ev.button.windowID != 0 && ev.button.windowID != winId);

        case SDL_EVENT_MOUSE_MOTION:
            return (ev.motion.windowID != 0 && ev.motion.windowID != winId) ||
                   HasCapture(cap, EventCapture::Mouse);

        case SDL_EVENT_MOUSE_WHEEL:
            return (ev.wheel.windowID != 0 && ev.wheel.windowID != winId) ||
                   HasCapture(cap, EventCapture::Mouse);

        case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
            return HasCapture(cap, EventCapture::Gamepad);

        default:
            return false;
    }
}

} // namespace

Platform::~Platform() noexcept {
    Shutdown();
}

Result<void, EngineError> Platform::Init(std::string_view appName, std::string_view orgName) noexcept {
    if (m_initialized) return Ok;

    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMEPAD)) {
        return Unexpected(MakePlatformError(PlatformErrc::SdlInitFailed));
    }

    if (const char* base = SDL_GetBasePath()) {
        m_basePath.Assign(base);
    }

    FixedString<128> org(orgName);
    FixedString<128> app(appName);
    if (char* pref = SDL_GetPrefPath(org.CStr(), app.CStr())) {
        m_prefPath.Assign(pref);
        SDL_free(pref);
    }

    m_initialized = true;
    m_quit = false;
    return Ok;
}

void Platform::Shutdown() noexcept {
    if (!m_initialized) return;

    for (auto& slot : m_gamepads) {
        if (slot.handle) {
            SDL_CloseGamepad(slot.handle);
            slot = {};
        }
    }

    SDL_Quit();
    m_initialized = false;
}

void Platform::OpenGamepad(u32 id) noexcept {
    for (const auto& slot : m_gamepads) {
        if (slot.handle && slot.id == id) return;
    }
    for (auto& slot : m_gamepads) {
        if (!slot.handle) {
            if (SDL_Gamepad* pad = SDL_OpenGamepad(id)) {
                slot.id = id;
                slot.handle = pad;
            }
            return;
        }
    }
}

void Platform::CloseGamepad(u32 id) noexcept {
    for (auto& slot : m_gamepads) {
        if (slot.handle && slot.id == id) {
            SDL_CloseGamepad(slot.handle);
            slot = {};
            return;
        }
    }
}

bool Platform::RumbleGamepad(u8 padSlot, u16 lowFreq, u16 highFreq, u32 durationMs) noexcept {
    if (padSlot >= kMaxGamepads || !m_gamepads[padSlot].handle) return false;
    return SDL_RumbleGamepad(m_gamepads[padSlot].handle, lowFreq, highFreq, durationMs);
}

void Platform::StopAllRumble() noexcept {
    for (auto& slot : m_gamepads) {
        if (slot.handle) {
            (void)SDL_RumbleGamepad(slot.handle, 0, 0, 0);
        }
    }
}

WindowFlags Platform::Pump(Window& window, InputPipeline& input, EventTap tap) noexcept {
    WindowFlags flags = WindowFlags::None;
    input.BeginFrame();

    SDL_Event ev;
    while (SDL_PollEvent(&ev)) {
        const EventCapture capture = tap.callback ? tap.callback(tap.user, ev) : EventCapture::None;

        switch (ev.type) {
            case SDL_EVENT_QUIT:
            case SDL_EVENT_TERMINATING:
                m_quit = true;
                break;

            case SDL_EVENT_WILL_ENTER_BACKGROUND:
                StopAllRumble();
                input.ClearPhysicalState();
                break;

            case SDL_EVENT_GAMEPAD_ADDED:
                OpenGamepad(ev.gdevice.which);
                break;

            case SDL_EVENT_GAMEPAD_REMOVED:
                input.DisconnectGamepad(ev.gdevice.which);
                CloseGamepad(ev.gdevice.which);
                break;

            case SDL_EVENT_DISPLAY_ADDED:
            case SDL_EVENT_DISPLAY_REMOVED:
            case SDL_EVENT_DISPLAY_DESKTOP_MODE_CHANGED:
            case SDL_EVENT_DISPLAY_CURRENT_MODE_CHANGED:
            case SDL_EVENT_DISPLAY_CONTENT_SCALE_CHANGED:
                window.RefreshMetrics();
                flags |= WindowFlags::DisplayChanged | WindowFlags::Resized;
                break;

            default:
                break;
        }

        const WindowFlags winFlags = window.ProcessEvent(ev);
        flags |= winFlags;

        if (HasFlag(winFlags, WindowFlags::FocusLost)) {
            StopAllRumble();
            input.ClearPhysicalState();
        }

        if (!ShouldSuppressInput(ev, capture, window.Id())) {
            const f32 density = window.State().pixelDensity;
            if (density != 1.0f) {
                if (ev.type == SDL_EVENT_MOUSE_MOTION) {
                    ev.motion.x    *= density;
                    ev.motion.y    *= density;
                    ev.motion.xrel *= density;
                    ev.motion.yrel *= density;
                } else if (ev.type == SDL_EVENT_MOUSE_BUTTON_DOWN || ev.type == SDL_EVENT_MOUSE_BUTTON_UP) {
                    ev.button.x *= density;
                    ev.button.y *= density;
                }
            }
            IngestSdlEvent(input, ev);
        }
    }

    (void)input.EndFrame();
    return flags;
}

Result<FilePath, EngineError> Platform::ResolveAsset(std::string_view relative) const noexcept {
    return JoinSandboxed(m_basePath.View(), relative);
}

Result<FilePath, EngineError> Platform::ResolveUser(std::string_view relative) const noexcept {
    return JoinSandboxed(m_prefPath.View(), relative);
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

Result<void, EngineError> Platform::SetClipboard(std::string_view utf8) noexcept {
    FixedString<4096> buf(utf8);
    if (!SDL_SetClipboardText(buf.CStr())) {
        return Unexpected(MakePlatformError(PlatformErrc::ClipboardFailed));
    }
    return Ok;
}

void Platform::ShowErrorBox(std::string_view title, std::string_view message, const Window* parent) noexcept {
    FixedString<256> t(title);
    FixedString<1024> m(message);
    SDL_Window* nativeParent = parent ? parent->Native() : nullptr;
    (void)SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, t.CStr(), m.CStr(), nativeParent);
}

} // namespace EngineN