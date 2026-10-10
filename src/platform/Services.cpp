#include "platform/Services.h"
#include "platform/Window.h"
#include <SDL3/SDL_platform.h>
#include <SDL3/SDL_filesystem.h>
#include <SDL3/SDL_clipboard.h>
#include <SDL3/SDL_cpuinfo.h>
#include <SDL3/SDL_messagebox.h>
#include <SDL3/SDL_events.h>
#include <SDL3/SDL_stdinc.h>
#include <SDL3/SDL_version.h>
#include <cstring>
#include <utility>

namespace EngineN {

// PathService

Result<void, EngineError> PathService::Init(
    std::string_view organization,
    std::string_view application) noexcept
{
    const char* base = SDL_GetBasePath();
    if (!base) {
        return Unexpected(MakePlatformError(PlatformErrc::PathUnavailable, "SDL_GetBasePath returned null"));
    }
    m_base.Assign(base);

    std::array<char, 128> orgBuf{};
    std::array<char, 128> appBuf{};
    const usize orgLen = std::min(organization.size(), orgBuf.size() - 1);
    const usize appLen = std::min(application.size(), appBuf.size() - 1);
    for (usize i = 0; i < orgLen; ++i) orgBuf[i] = organization[i];
    for (usize i = 0; i < appLen; ++i) appBuf[i] = application[i];
    orgBuf[orgLen] = '\0';
    appBuf[appLen] = '\0';

    char* pref = SDL_GetPrefPath(orgBuf.data(), appBuf.data());
    if (!pref) {
        return Unexpected(MakePlatformError(PlatformErrc::PathUnavailable, "SDL_GetPrefPath returned null"));
    }
    m_pref.Assign(pref);
    SDL_free(pref);

    return Ok;
}

namespace {

bool IsSandboxSafe(std::string_view relative) noexcept {
    if (relative.empty()) return false;

    // Reject drive letters like "C:" or "D:"
    if (relative.size() >= 2 && relative[1] == ':') return false;

    // Reject leading slashes
    if (relative[0] == '/' || relative[0] == '\\') return false;

    // Reject ".." traversal segments
    if (relative == ".." || relative.starts_with("../") || relative.starts_with("..\\")) return false;
    if (relative.find("/../") != std::string_view::npos || relative.find("\\..\\") != std::string_view::npos) return false;
    if (relative.ends_with("/..") || relative.ends_with("\\..")) return false;

    // Reject null characters
    if (relative.find('\0') != std::string_view::npos) return false;

    return true;
}

Result<FilePath, EngineError> CombinePath(std::string_view root, std::string_view relative) noexcept {
    if (!IsSandboxSafe(relative)) {
        return Unexpected(MakePlatformError(PlatformErrc::PathRejected, "Path contains sandbox escape or illegal format"));
    }

    FilePath path{};
    std::array<char, kMaxPathBytes> buf{};
    usize idx = 0;

    for (char c : root) {
        if (idx >= buf.size() - 1) return Unexpected(MakePlatformError(PlatformErrc::CapacityExceeded));
        buf[idx++] = (c == '\\') ? '/' : c;
    }

    if (idx > 0 && buf[idx - 1] != '/') {
        if (idx >= buf.size() - 1) return Unexpected(MakePlatformError(PlatformErrc::CapacityExceeded));
        buf[idx++] = '/';
    }

    for (char c : relative) {
        if (idx >= buf.size() - 1) return Unexpected(MakePlatformError(PlatformErrc::CapacityExceeded));
        buf[idx++] = (c == '\\') ? '/' : c;
    }
    buf[idx] = '\0';

    path.Assign(std::string_view(buf.data(), idx));
    return path;
}

} // namespace

Result<FilePath, EngineError> PathService::ResolveUser(std::string_view relative) const noexcept {
    return CombinePath(m_pref.View(), relative);
}

Result<FilePath, EngineError> PathService::ResolveAsset(std::string_view relative) const noexcept {
    return CombinePath(m_base.View(), relative);
}

// Clipboard

ClipboardText::~ClipboardText() {
    Reset();
}

ClipboardText::ClipboardText(ClipboardText&& o) noexcept
    : m_text(std::exchange(o.m_text, nullptr)) {}

ClipboardText& ClipboardText::operator=(ClipboardText&& o) noexcept {
    if (this != &o) {
        Reset();
        m_text = std::exchange(o.m_text, nullptr);
    }
    return *this;
}

std::string_view ClipboardText::View() const noexcept {
    return m_text ? std::string_view(m_text) : std::string_view{};
}

void ClipboardText::Reset() noexcept {
    if (m_text) {
        SDL_free(m_text);
        m_text = nullptr;
    }
}

bool Clipboard::HasText() const noexcept {
    return SDL_HasClipboardText();
}

ClipboardText Clipboard::GetText() const noexcept {
    char* text = SDL_GetClipboardText();
    return ClipboardText(text);
}

Result<void, EngineError> Clipboard::SetText(std::string_view utf8) noexcept {
    if (utf8.size() > kMaxSetBytes) {
        return Unexpected(MakePlatformError(PlatformErrc::CapacityExceeded, "Clipboard payload exceeds limit"));
    }

    std::string text(utf8);
    if (!SDL_SetClipboardText(text.c_str())) {
        return Unexpected(MakePlatformError(PlatformErrc::ClipboardFailed));
    }
    return Ok;
}

// System Specs

SystemSpecs QuerySystemSpecs() noexcept {
    SystemSpecs specs{};

    const char* plat = SDL_GetPlatform();
    if (plat) specs.platformName.Assign(plat);

    specs.ramMiB = static_cast<u32>(SDL_GetSystemRAM());
    specs.logicalCores = static_cast<u32>(SDL_GetNumLogicalCPUCores());
    specs.cacheLineBytes = static_cast<u32>(SDL_GetCPUCacheLineSize());
    specs.simdAlignment = static_cast<u32>(SDL_GetSIMDAlignment());
    specs.sdlVersion = static_cast<u32>(SDL_GetVersion());

    const char* steamDeckEnv = SDL_getenv("SteamDeck");
    specs.steamDeckHint = (steamDeckEnv && std::strcmp(steamDeckEnv, "1") == 0);

    return specs;
}

// Dialogs

namespace Dialogs {

Result<void, EngineError> ShowMessage(
    MessageSeverity severity,
    std::string_view title,
    std::string_view text,
    const Window* parent) noexcept
{
    SDL_MessageBoxFlags flags = 0;
    switch (severity) {
        case MessageSeverity::Info:    flags = SDL_MESSAGEBOX_INFORMATION; break;
        case MessageSeverity::Warning: flags = SDL_MESSAGEBOX_WARNING;     break;
        case MessageSeverity::Error:   flags = SDL_MESSAGEBOX_ERROR;       break;
    }

    FixedString<256> titleBuf(title);
    FixedString<1024> textBuf(text);
    SDL_Window* parentHandle = parent ? parent->Native() : nullptr;

    if (!SDL_ShowSimpleMessageBox(flags, titleBuf.CStr(), textBuf.CStr(), parentHandle)) {
        return Unexpected(MakePlatformError(PlatformErrc::InvalidState));
    }
    return Ok;
}

void ShowFatal(std::string_view title, std::string_view text) noexcept {
    FixedString<256> titleBuf(title);
    FixedString<1024> textBuf(text);
    (void)SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, titleBuf.CStr(), textBuf.CStr(), nullptr);
}

} // namespace Dialogs

} // namespace EngineN
