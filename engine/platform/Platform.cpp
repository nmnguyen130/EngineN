#include "platform/Platform.h"

#include <SDL3/SDL.h>
#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <system_error>

namespace en {

namespace {

char g_fallbackClipboard[1024]{};

[[nodiscard]] constexpr usize CopyNullTerminated(
    std::string_view src, std::span<char> outBuf) noexcept {
    if (outBuf.empty()) return 0U;
    const usize copyLen = std::min<usize>(src.size(), outBuf.size() - 1U);
    for (usize i = 0U; i < copyLen; ++i) {
        outBuf[i] = src[i];
    }
    outBuf[copyLen] = '\0';
    return copyLen;
}

[[nodiscard]] constexpr bool MatchesWindow(u32 targetWinId, u32 eventWinId) noexcept {
    return targetWinId == 0U || eventWinId == 0U || eventWinId == targetWinId;
}

bool SDLCALL OnModalResizeEventWatch(void* userdata, SDL_Event* ev) noexcept {
    if (!userdata || !ev) return true;
    auto* win = static_cast<Window*>(userdata);
    if (ev->type == SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED ||
        ev->type == SDL_EVENT_WINDOW_EXPOSED) {
        if (MatchesWindow(win->Id(), ev->window.windowID)) {
            (void)win->RefreshMetrics();
        }
    }
    return true;
}

} // namespace

u64 FileWatcher::QueryFileModTimeTicks(const char* cpath) noexcept {
    if (!cpath || cpath[0] == '\0') return 0ULL;
    std::error_code ec{};
    const auto ftime = std::filesystem::last_write_time(cpath, ec);
    if (ec) return 0ULL;
    return static_cast<u64>(ftime.time_since_epoch().count());
}

bool FileWatcher::Watch(const FilePath& path, StringId assetId) noexcept {
    if (path.Empty() || !assetId.IsValid()) return false;
    const u64 mod = QueryFileModTimeTicks(path.CStr());

    u32 mask = m_activeMask;
    while (mask != 0U) {
        const u32 idx = static_cast<u32>(std::countr_zero(mask));
        if (m_slots[idx].assetId == assetId) {
            m_slots[idx].lastModTick = mod;
            m_slots[idx].path = path;
            return true;
        }
        mask &= (mask - 1U);
    }

    const u32 freeIdx = static_cast<u32>(std::countr_one(m_activeMask));
    if (freeIdx >= kMaxWatched) return false;

    m_slots[freeIdx].lastModTick = mod;
    m_slots[freeIdx].assetId = assetId;
    m_slots[freeIdx].path = path;
    m_activeMask |= (1U << freeIdx);
    return true;
}

bool FileWatcher::Unwatch(StringId assetId) noexcept {
    if (!assetId.IsValid()) return false;
    u32 mask = m_activeMask;
    while (mask != 0U) {
        const u32 idx = static_cast<u32>(std::countr_zero(mask));
        if (m_slots[idx].assetId == assetId) {
            m_slots[idx].assetId = StringId{};
            m_activeMask &= ~(1U << idx);
            return true;
        }
        mask &= (mask - 1U);
    }
    return false;
}

u32 FileWatcher::PollModified(std::span<StringId> outChangedIds) noexcept {
    u32 count = 0U;
    u32 mask = m_activeMask;
    while (mask != 0U) {
        const u32 idx = static_cast<u32>(std::countr_zero(mask));
        mask &= (mask - 1U);
        WatchSlot& s = m_slots[idx];
        const u64 mod = QueryFileModTimeTicks(s.path.CStr());
        if (mod != 0ULL && mod != s.lastModTick) {
            s.lastModTick = mod;
            if (count < outChangedIds.size()) {
                outChangedIds[count] = s.assetId;
            }
            ++count;
        }
    }
    return count;
}

Result<void, PlatformErrc> Platform::Init(
    std::string_view appName, std::string_view orgName, bool headless) noexcept {
    Shutdown();
    m_headless = headless;
    m_quit = false;
    m_droppedFileCount = 0U;
    m_screenSleepDisabled = false;
    m_frameEvents.ClearInput(false);

    if (!headless) {
        if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMEPAD)) {
            return Err(PlatformErrc::SdlInitFailed);
        }
    }
    const char* base = SDL_GetBasePath();
    if (!m_basePath.AssignNormalizedDir(base ? base : "./")) {
        (void)m_basePath.AssignNormalizedDir("./");
    }
    FixedString<64> orgBuf(orgName.empty() ? "EngineN" : orgName);
    FixedString<64> appBuf(appName.empty() ? "EngineN" : appName);
    char* pref = SDL_GetPrefPath(orgBuf.CStr(), appBuf.CStr());
    if (!m_prefPath.AssignNormalizedDir(pref ? pref : "./")) {
        (void)m_prefPath.AssignNormalizedDir("./");
    }
    if (pref) SDL_free(pref);

    m_initialized = true;
    return {};
}

void Platform::Shutdown() noexcept {
    if (!m_initialized) return;
    SetLiveResizeWatch(nullptr);
    SetScreenSleepEnabled(true);
    StopAllRumble();

    u8 mask = m_activeGamepadMask;
    while (mask != 0U) {
        const u8 slot = static_cast<u8>(std::countr_zero(mask));
        mask &= static_cast<u8>(mask - 1U);
        if (m_gamepadHandles[slot]) {
            SDL_CloseGamepad(m_gamepadHandles[slot]);
            m_gamepadHandles[slot] = nullptr;
        }
        m_gamepadIds[slot] = 0U;
    }
    m_activeGamepadMask = 0U;
    m_rumbleGamepadMask = 0U;
    m_droppedFileCount = 0U;

    if (!m_headless) {
        SDL_Quit();
    }
    m_initialized = false;
}

void Platform::SetLiveResizeWatch(Window* window) noexcept {
    if (m_watchRegistered && m_watchedWindow) {
        SDL_RemoveEventWatch(&OnModalResizeEventWatch, m_watchedWindow);
        m_watchRegistered = false;
    }
    m_watchedWindow = window;
    if (window && !m_headless) {
        m_watchRegistered = SDL_AddEventWatch(&OnModalResizeEventWatch, window);
    }
}

void Platform::SetScreenSleepEnabled(bool enabled) noexcept {
    const bool disableSleep = !enabled;
    if (m_screenSleepDisabled == disableSleep) return;
    m_screenSleepDisabled = disableSleep;
    if (!m_headless) {
        if (disableSleep) (void)SDL_DisableScreenSaver();
        else (void)SDL_EnableScreenSaver();
    }
}

PowerInfo Platform::QueryPowerInfo() noexcept {
    PowerInfo info{};
    int secs = -1;
    int pct = -1;
    const SDL_PowerState st = SDL_GetPowerInfo(&secs, &pct);
    info.remainingSeconds = static_cast<i16>(std::clamp(secs, -1, 32767));
    info.batteryPercent = static_cast<i8>(std::clamp(pct, -1, 100));
    switch (st) {
        case SDL_POWERSTATE_ON_BATTERY: info.state = PowerSupplyState::OnBattery; break;
        case SDL_POWERSTATE_NO_BATTERY: info.state = PowerSupplyState::NoBattery; break;
        case SDL_POWERSTATE_CHARGING:   info.state = PowerSupplyState::Charging;  break;
        case SDL_POWERSTATE_CHARGED:    info.state = PowerSupplyState::Charged;   break;
        default:                        info.state = PowerSupplyState::Unknown;   break;
    }
    return info;
}

WindowFlags Platform::FinalizePump(
    Window& window, WindowFlags winFlags, bool needRefresh) noexcept {
    if (needRefresh) {
        winFlags |= window.RefreshMetrics();
    }
    if (HasFlag(winFlags, WindowFlags::CloseRequested)) {
        m_quit = true;
    }
    if (HasFlag(winFlags, WindowFlags::FocusLost | WindowFlags::Minimized)) {
        StopAllRumble();
        m_frameEvents.ClearInput(true);
    }
    return winFlags;
}

WindowFlags Platform::Pump(Window& window, EventTap tap) noexcept {
    m_frameEvents.ClearFrameDeltas();
    if (!m_initialized || m_headless) return WindowFlags::None;

    WindowFlags winFlags = WindowFlags::None;
    bool needRefresh = false;

    const EventCapture captured = tap.Evaluate();
    const u32 targetWinId = window.Id();

    SDL_Event ev{};
    while (SDL_PollEvent(&ev)) {
        switch (ev.type) {
            case SDL_EVENT_QUIT:
            case SDL_EVENT_TERMINATING:
                window.ApplyLifecycleEvent(WindowFlags::CloseRequested, winFlags, needRefresh);
                break;

            case SDL_EVENT_WILL_ENTER_BACKGROUND:
                window.ApplyLifecycleEvent(
                    WindowFlags::Minimized | WindowFlags::FocusLost, winFlags, needRefresh);
                StopAllRumble();
                m_frameEvents.ClearInput(true);
                break;

            case SDL_EVENT_DISPLAY_ADDED:
            case SDL_EVENT_DISPLAY_REMOVED:
            case SDL_EVENT_DISPLAY_DESKTOP_MODE_CHANGED:
            case SDL_EVENT_DISPLAY_CURRENT_MODE_CHANGED:
            case SDL_EVENT_DISPLAY_CONTENT_SCALE_CHANGED:
                window.ApplyLifecycleEvent(WindowFlags::DisplayChanged, winFlags, needRefresh);
                break;

            case SDL_EVENT_WINDOW_CLOSE_REQUESTED:
                if (MatchesWindow(targetWinId, ev.window.windowID)) {
                    window.ApplyLifecycleEvent(WindowFlags::CloseRequested, winFlags, needRefresh);
                }
                break;

            case SDL_EVENT_WINDOW_RESIZED:
            case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED:
                if (MatchesWindow(targetWinId, ev.window.windowID)) {
                    window.ApplyLifecycleEvent(WindowFlags::Resized, winFlags, needRefresh);
                }
                break;

            case SDL_EVENT_WINDOW_DISPLAY_CHANGED:
            case SDL_EVENT_WINDOW_DISPLAY_SCALE_CHANGED:
            case SDL_EVENT_WINDOW_ENTER_FULLSCREEN:
            case SDL_EVENT_WINDOW_LEAVE_FULLSCREEN:
                if (MatchesWindow(targetWinId, ev.window.windowID)) {
                    window.ApplyLifecycleEvent(
                        WindowFlags::Resized | WindowFlags::DisplayChanged, winFlags, needRefresh);
                }
                break;

            case SDL_EVENT_WINDOW_MINIMIZED:
            case SDL_EVENT_WINDOW_HIDDEN:
            case SDL_EVENT_WINDOW_OCCLUDED:
                if (MatchesWindow(targetWinId, ev.window.windowID)) {
                    window.ApplyLifecycleEvent(
                        WindowFlags::Minimized | WindowFlags::FocusLost, winFlags, needRefresh);
                    StopAllRumble();
                    m_frameEvents.ClearInput(true);
                }
                break;

            case SDL_EVENT_WINDOW_RESTORED:
            case SDL_EVENT_WINDOW_SHOWN:
            case SDL_EVENT_WINDOW_EXPOSED:
                if (MatchesWindow(targetWinId, ev.window.windowID)) {
                    window.ApplyLifecycleEvent(WindowFlags::Restored, winFlags, needRefresh);
                }
                break;

            case SDL_EVENT_WINDOW_FOCUS_GAINED:
                if (MatchesWindow(targetWinId, ev.window.windowID)) {
                    window.ApplyLifecycleEvent(WindowFlags::FocusGained, winFlags, needRefresh);
                    m_frameEvents.focusCleared = 0U;
                }
                break;

            case SDL_EVENT_WINDOW_FOCUS_LOST:
                if (MatchesWindow(targetWinId, ev.window.windowID)) {
                    window.ApplyLifecycleEvent(WindowFlags::FocusLost, winFlags, needRefresh);
                    StopAllRumble();
                    m_frameEvents.ClearInput(true);
                }
                break;

            case SDL_EVENT_KEY_DOWN:
            case SDL_EVENT_KEY_UP:
                if (MatchesWindow(targetWinId, ev.key.windowID)) {
                    m_frameEvents.IngestKey(
                        static_cast<u16>(ev.key.scancode),
                        ev.type == SDL_EVENT_KEY_DOWN,
                        ev.key.repeat,
                        window.State().focused,
                        captured);
                }
                break;

            case SDL_EVENT_MOUSE_BUTTON_DOWN:
            case SDL_EVENT_MOUSE_BUTTON_UP:
                if (ev.button.which != SDL_TOUCH_MOUSEID &&
                    MatchesWindow(targetWinId, ev.button.windowID)) {
                    m_frameEvents.IngestMouseButton(
                        static_cast<u16>(ev.button.button),
                        ev.type == SDL_EVENT_MOUSE_BUTTON_DOWN,
                        ev.button.x,
                        ev.button.y,
                        window.State().pixelDensity,
                        window.State().focused,
                        captured);
                }
                break;

            case SDL_EVENT_MOUSE_MOTION:
                if (ev.motion.which != SDL_TOUCH_MOUSEID &&
                    MatchesWindow(targetWinId, ev.motion.windowID)) {
                    m_frameEvents.IngestMouseMotion(
                        ev.motion.x,
                        ev.motion.y,
                        ev.motion.xrel,
                        ev.motion.yrel,
                        window.State().pixelDensity,
                        window.State().focused,
                        captured);
                }
                break;

            case SDL_EVENT_MOUSE_WHEEL:
                if (ev.wheel.which != SDL_TOUCH_MOUSEID &&
                    MatchesWindow(targetWinId, ev.wheel.windowID)) {
                    m_frameEvents.IngestMouseWheel(
                        ev.wheel.x,
                        ev.wheel.y,
                        ev.wheel.direction == SDL_MOUSEWHEEL_FLIPPED,
                        window.State().focused,
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
            case SDL_EVENT_GAMEPAD_BUTTON_UP: {
                const u8 slot = FindGamepadSlot(ev.gbutton.which);
                if (slot != kInvalidGamepadSlot) {
                    m_frameEvents.IngestGamepadButton(
                        slot,
                        static_cast<u16>(ev.gbutton.button),
                        ev.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN,
                        window.State().focused,
                        captured);
                }
                break;
            }

            case SDL_EVENT_GAMEPAD_AXIS_MOTION: {
                const u8 slot = FindGamepadSlot(ev.gaxis.which);
                if (slot != kInvalidGamepadSlot) {
                    m_frameEvents.IngestGamepadAxis(
                        slot,
                        static_cast<u16>(ev.gaxis.axis),
                        static_cast<i16>(ev.gaxis.value),
                        window.State().focused,
                        captured);
                }
                break;
            }

            case SDL_EVENT_TEXT_INPUT:
                if (MatchesWindow(targetWinId, ev.text.windowID) && ev.text.text) {
                    m_frameEvents.IngestTextInput(ev.text.text, window.State().focused);
                }
                break;

            case SDL_EVENT_DROP_FILE:
                if (ev.drop.data) {
                    (void)PushDroppedFile(ev.drop.data);
                }
                break;

            default:
                break;
        }
    }

    return FinalizePump(window, winFlags, needRefresh);
}

bool Platform::OpenGamepad(u32 instanceId) noexcept {
    if (instanceId == 0U) return false;
    if (FindGamepadSlot(instanceId) != kInvalidGamepadSlot) return true;

    const u8 freeSlot = static_cast<u8>(std::countr_one(m_activeGamepadMask));
    if (freeSlot >= kMaxGamepads) return false;

    if (!m_headless) {
        SDL_Gamepad* pad = SDL_OpenGamepad(instanceId);
        if (pad) {
            (void)SDL_SetGamepadPlayerIndex(pad, static_cast<int>(freeSlot));
            m_gamepadHandles[freeSlot] = pad;
        }
    }
    m_gamepadIds[freeSlot] = instanceId;
    m_activeGamepadMask |= static_cast<u8>(1U << freeSlot);
    return true;
}

void Platform::CloseGamepad(u32 instanceId) noexcept {
    const u8 slot = FindGamepadSlot(instanceId);
    if (slot == kInvalidGamepadSlot) return;

    const u8 bit = static_cast<u8>(1U << slot);
    if (m_gamepadHandles[slot]) {
        if ((m_rumbleGamepadMask & bit) != 0U) {
            (void)SDL_RumbleGamepad(m_gamepadHandles[slot], 0U, 0U, 0U);
        }
        SDL_CloseGamepad(m_gamepadHandles[slot]);
        m_gamepadHandles[slot] = nullptr;
    }
    m_gamepadIds[slot] = 0U;
    m_activeGamepadMask &= static_cast<u8>(~bit);
    m_rumbleGamepadMask &= static_cast<u8>(~bit);
    m_frameEvents.IngestGamepadDisconnect(slot);
}

bool Platform::RumbleGamepad(u8 padSlot, u16 lowFreq, u16 highFreq, u32 durationMs) noexcept {
    if (padSlot >= kMaxGamepads) return false;
    const u8 bit = static_cast<u8>(1U << padSlot);
    if ((m_activeGamepadMask & bit) == 0U) return false;

    const bool activePulse = ((lowFreq | highFreq) != 0U) && (durationMs > 0U);
    if (m_gamepadHandles[padSlot]) {
        if (!SDL_RumbleGamepad(m_gamepadHandles[padSlot], lowFreq, highFreq, durationMs)) {
            return false;
        }
    }
    if (activePulse) {
        m_rumbleGamepadMask |= bit;
    } else {
        m_rumbleGamepadMask &= static_cast<u8>(~bit);
    }
    return true;
}

void Platform::StopAllRumble() noexcept {
    u8 mask = m_rumbleGamepadMask;
    while (mask != 0U) {
        const u8 slot = static_cast<u8>(std::countr_zero(mask));
        mask &= static_cast<u8>(mask - 1U);
        if (m_gamepadHandles[slot]) {
            (void)SDL_RumbleGamepad(m_gamepadHandles[slot], 0U, 0U, 0U);
        }
    }
    m_rumbleGamepadMask = 0U;
}

Result<usize, PlatformErrc> Platform::ReadFileBuffer(
    const FilePath& path, std::span<u8> outBuf) noexcept {
    if (path.Empty()) return Err(PlatformErrc::PathRejected);
    std::error_code ec{};
    const auto fsize = std::filesystem::file_size(path.CStr(), ec);
    if (ec || fsize > outBuf.size()) return Err(PlatformErrc::FileIoFailed);

    std::FILE* fp = std::fopen(path.CStr(), "rb");
    if (!fp) return Err(PlatformErrc::FileIoFailed);
    const usize bytesRead = (fsize > 0U)
        ? std::fread(outBuf.data(), 1U, static_cast<usize>(fsize), fp)
        : 0U;
    std::fclose(fp);
    if (bytesRead != static_cast<usize>(fsize)) return Err(PlatformErrc::FileIoFailed);
    return bytesRead;
}

Result<void, PlatformErrc> Platform::WriteFileAtomic(
    const FilePath& path, std::span<const u8> data) noexcept {
    if (path.Empty()) return Err(PlatformErrc::PathRejected);
    std::filesystem::path targetFsPath(path.View());
    std::filesystem::path tempFsPath = targetFsPath;
    tempFsPath += ".tmp";

    std::FILE* fp = std::fopen(tempFsPath.string().c_str(), "wb");
    if (!fp) return Err(PlatformErrc::FileIoFailed);
    bool writeOk = true;
    if (!data.empty()) {
        writeOk = (std::fwrite(data.data(), 1U, data.size(), fp) == data.size());
    }
    if (std::fflush(fp) != 0) writeOk = false;
    if (std::fclose(fp) != 0) writeOk = false;
    if (!writeOk) {
        std::error_code rmEc{};
        std::filesystem::remove(tempFsPath, rmEc);
        return Err(PlatformErrc::FileIoFailed);
    }

    std::error_code ec{};
    std::filesystem::rename(tempFsPath, targetFsPath, ec);
    if (ec) {
        // Windows rename fails if the destination already exists; remove and retry.
        std::filesystem::remove(targetFsPath, ec);
        ec.clear();
        std::filesystem::rename(tempFsPath, targetFsPath, ec);
        if (ec) {
            std::filesystem::remove(tempFsPath, ec);
            return Err(PlatformErrc::FileIoFailed);
        }
    }
    return {};
}

bool Platform::PushDroppedFile(std::string_view filePath) noexcept {
    if (filePath.empty() || m_droppedFileCount >= kMaxDroppedFiles) return false;
    if (!m_droppedFiles[m_droppedFileCount].Assign(filePath)) return false;
    ++m_droppedFileCount;
    return true;
}

u32 Platform::PollDroppedFiles(std::span<FilePath> outFiles) noexcept {
    const u32 count = std::min<u32>(m_droppedFileCount, static_cast<u32>(outFiles.size()));
    for (u32 i = 0U; i < count; ++i) {
        outFiles[i] = m_droppedFiles[i];
    }
    m_droppedFileCount = 0U;
    return count;
}

bool Platform::SetClipboardText(std::string_view text) noexcept {
    (void)CopyNullTerminated(text, g_fallbackClipboard);
    (void)SDL_SetClipboardText(g_fallbackClipboard);
    return true;
}

usize Platform::GetClipboardText(std::span<char> outBuf) noexcept {
    if (outBuf.empty()) return 0U;
    if (SDL_HasClipboardText()) {
        char* clip = SDL_GetClipboardText();
        if (clip) {
            const usize copied = CopyNullTerminated(clip, outBuf);
            SDL_free(clip);
            return copied;
        }
    }
    return CopyNullTerminated(g_fallbackClipboard, outBuf);
}

void Platform::ShowErrorBox(
    std::string_view title, std::string_view message, const Window* parent) noexcept {
    FixedString<128> titleBuf(title);
    FixedString<512> msgBuf(message);
    std::fprintf(stderr, "[Platform::ShowErrorBox] %s: %s\n", titleBuf.CStr(), msgBuf.CStr());
    if (message.find("Headless") != std::string_view::npos || std::getenv("CI") != nullptr) {
        return;
    }
    (void)SDL_ShowSimpleMessageBox(
        SDL_MESSAGEBOX_ERROR,
        titleBuf.CStr(),
        msgBuf.CStr(),
        parent ? parent->Native() : nullptr);
}

} // namespace en
