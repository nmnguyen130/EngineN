#include "platform/Platform.h"
#include <SDL3/SDL.h>
#include <filesystem>
#include <fstream>
#include <system_error>

namespace en {

u64 FileWatcher::QueryFileModTimeTicks(const char* cpath) noexcept {
    if (!cpath || cpath[0] == '\0') return 0;
    std::error_code ec{};
    const auto ftime = std::filesystem::last_write_time(cpath, ec);
    if (ec) return 0;
    return static_cast<u64>(ftime.time_since_epoch().count());
}

bool FileWatcher::Watch(const FilePath& path, StringId assetId) noexcept {
    if (path.Empty() || !assetId.IsValid()) return false;
    const u64 mod = QueryFileModTimeTicks(path.CStr());
    for (auto& s : m_slots) {
        if (!s.active || s.assetId == assetId) {
            s = WatchSlot{path, mod, assetId, true};
            return true;
        }
    }
    return false;
}

u32 FileWatcher::PollModified(std::span<StringId> outChangedIds) noexcept {
    u32 count = 0;
    for (auto& s : m_slots) {
        if (!s.active) continue;
        const u64 mod = QueryFileModTimeTicks(s.path.CStr());
        if (mod != 0 && mod != s.lastModTick) {
            s.lastModTick = mod;
            if (count < outChangedIds.size()) outChangedIds[count] = s.assetId;
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
    if (!headless) {
        if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMEPAD)) {
            return Err(PlatformErrc::SdlInitFailed);
        }
    }
    const char* base = SDL_GetBasePath();
    if (!m_basePath.AssignNormalizedDir(base ? base : "./")) {
        (void)m_basePath.AssignNormalizedDir("./");
    }
    FixedString<64> orgBuf(orgName);
    FixedString<64> appBuf(appName);
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
    StopAllRumble();
    u8 mask = m_activeGamepadMask;
    while (mask != 0) {
        const u8 slot = static_cast<u8>(std::countr_zero(mask));
        mask &= static_cast<u8>(mask - 1U);
        if (m_gamepadHandles[slot]) {
            SDL_CloseGamepad(m_gamepadHandles[slot]);
            m_gamepadHandles[slot] = nullptr;
        }
    }
    m_activeGamepadMask = 0;
    if (!m_headless) {
        SDL_Quit();
    }
    m_initialized = false;
}

WindowFlags Platform::Pump(Window& window, EventCapture captured) noexcept {
    m_frameEvents.ClearFrameDeltas();
    if (!m_initialized || m_headless) return WindowFlags::None;

    WindowFlags winFlags = WindowFlags::None;
    bool needRefresh = false;
    const bool isFocused = window.State().focused;

    SDL_Event ev{};
    while (SDL_PollEvent(&ev)) {
        winFlags |= window.ProcessEvent(ev, needRefresh);
        if (HasFlag(winFlags, WindowFlags::CloseRequested)) {
            m_quit = true;
        }
        switch (ev.type) {
            case SDL_EVENT_KEY_DOWN:
            case SDL_EVENT_KEY_UP: {
                const bool down = (ev.type == SDL_EVENT_KEY_DOWN);
                if (!down || (isFocused && !HasFlag(captured, EventCapture::Keyboard))) {
                    m_frameEvents.PushEvent(
                        InputEventKind::Key, 0, static_cast<u16>(ev.key.scancode), down ? 1 : 0);
                }
                break;
            }
            case SDL_EVENT_MOUSE_BUTTON_DOWN:
            case SDL_EVENT_MOUSE_BUTTON_UP: {
                const bool down = (ev.type == SDL_EVENT_MOUSE_BUTTON_DOWN);
                if (!down || (isFocused && !HasFlag(captured, EventCapture::Mouse))) {
                    m_frameEvents.PushEvent(
                        InputEventKind::MouseButton, 0, static_cast<u16>(ev.button.button), down ? 1 : 0);
                }
                break;
            }
            case SDL_EVENT_MOUSE_MOTION:
                if (isFocused && !HasFlag(captured, EventCapture::Mouse)) {
                    m_frameEvents.mouseX = ev.motion.x;
                    m_frameEvents.mouseY = ev.motion.y;
                    m_frameEvents.mouseDx += ev.motion.xrel;
                    m_frameEvents.mouseDy += ev.motion.yrel;
                    m_frameEvents.hasMouseMotion = 1;
                }
                break;
            case SDL_EVENT_MOUSE_WHEEL:
                if (isFocused && !HasFlag(captured, EventCapture::Mouse)) {
                    m_frameEvents.wheelX += ev.wheel.x;
                    m_frameEvents.wheelY += ev.wheel.y;
                }
                break;
            default:
                break;
        }
    }

    if (needRefresh) {
        window.RefreshMetrics();
    }
    if (HasFlag(winFlags, WindowFlags::FocusLost)) {
        StopAllRumble();
        m_frameEvents.ClearInput(true);
    } else if (m_frameEvents.hasMouseMotion != 0) {
        const f32 d = window.State().pixelDensity;
        m_frameEvents.mouseX *= d;
        m_frameEvents.mouseY *= d;
        m_frameEvents.mouseDx *= d;
        m_frameEvents.mouseDy *= d;
    }
    return winFlags;
}

bool Platform::RumbleGamepad(u8 padSlot, u16 lowFreq, u16 highFreq, u32 durationMs) noexcept {
    if (padSlot >= 4U) return false;
    const u8 bit = static_cast<u8>(1U << padSlot);
    if ((m_activeGamepadMask & bit) == 0 || !m_gamepadHandles[padSlot]) {
        m_rumbleGamepadMask |= bit;
        return true;
    }
    m_rumbleGamepadMask |= bit;
    return SDL_RumbleGamepad(m_gamepadHandles[padSlot], lowFreq, highFreq, durationMs);
}

void Platform::StopAllRumble() noexcept {
    u8 mask = m_rumbleGamepadMask;
    while (mask != 0) {
        const u8 slot = static_cast<u8>(std::countr_zero(mask));
        mask &= static_cast<u8>(mask - 1U);
        if (m_gamepadHandles[slot]) {
            SDL_RumbleGamepad(m_gamepadHandles[slot], 0, 0, 0);
        }
    }
    m_rumbleGamepadMask = 0;
}

Result<usize, PlatformErrc> Platform::ReadFileBuffer(const FilePath& path, std::span<u8> outBuf) noexcept {
    if (path.Empty()) return Err(PlatformErrc::PathRejected);
    std::error_code ec{};
    const auto fsize = std::filesystem::file_size(path.CStr(), ec);
    if (ec || fsize > outBuf.size()) return Err(PlatformErrc::FileIoFailed);

    std::ifstream ifs(path.CStr(), std::ios::binary);
    if (!ifs.is_open()) return Err(PlatformErrc::FileIoFailed);
    if (fsize > 0) {
        ifs.read(reinterpret_cast<char*>(outBuf.data()), static_cast<std::streamsize>(fsize));
        if (!ifs.good() && !ifs.eof()) return Err(PlatformErrc::FileIoFailed);
    }
    return static_cast<usize>(fsize);
}

Result<void, PlatformErrc> Platform::WriteFileAtomic(const FilePath& path, std::span<const u8> data) noexcept {
    if (path.Empty()) return Err(PlatformErrc::PathRejected);
    std::filesystem::path targetFsPath(path.View());
    std::filesystem::path tempFsPath = targetFsPath;
    tempFsPath += ".tmp";

    {
        std::ofstream ofs(tempFsPath, std::ios::binary | std::ios::trunc);
        if (!ofs.is_open()) return Err(PlatformErrc::FileIoFailed);
        if (!data.empty()) {
            ofs.write(reinterpret_cast<const char*>(data.data()), static_cast<std::streamsize>(data.size()));
            if (!ofs.good()) return Err(PlatformErrc::FileIoFailed);
        }
        ofs.flush();
    }
    std::error_code ec{};
    std::filesystem::remove(targetFsPath, ec);
    ec.clear();
    std::filesystem::rename(tempFsPath, targetFsPath, ec);
    if (ec) {
        std::filesystem::remove(tempFsPath, ec);
        return Err(PlatformErrc::FileIoFailed);
    }
    return {};
}

bool Platform::PushDroppedFile(std::string_view filePath) noexcept {
    if (filePath.empty() || m_droppedFileCount >= m_droppedFiles.size()) return false;
    m_droppedFiles[m_droppedFileCount++].Assign(filePath);
    return true;
}

u32 Platform::PollDroppedFiles(std::span<FilePath> outFiles) noexcept {
    const u32 count = std::min<u32>(m_droppedFileCount, static_cast<u32>(outFiles.size()));
    for (u32 i = 0; i < count; ++i) {
        outFiles[i] = m_droppedFiles[i];
    }
    m_droppedFileCount = 0;
    return count;
}

namespace {
char g_clipboardBuffer[1024]{};
} // namespace

bool Platform::SetClipboardText(std::string_view text) noexcept {
    const usize len = std::min<usize>(text.size(), sizeof(g_clipboardBuffer) - 1U);
    if (len > 0) std::memcpy(g_clipboardBuffer, text.data(), len);
    g_clipboardBuffer[len] = '\0';
    return true;
}

usize Platform::GetClipboardText(std::span<char> outBuf) noexcept {
    if (outBuf.empty()) return 0;
    const usize srcLen = std::strlen(g_clipboardBuffer);
    const usize copyLen = std::min<usize>(srcLen, outBuf.size() - 1U);
    if (copyLen > 0) std::memcpy(outBuf.data(), g_clipboardBuffer, copyLen);
    outBuf[copyLen] = '\0';
    return copyLen;
}

} // namespace en
