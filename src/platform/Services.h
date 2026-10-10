#pragma once

#include "platform/PlatformTypes.h"
#include <atomic>
#include <string_view>

struct SDL_Window;

namespace EngineN {

class Window;

using FilePath = FixedString<kMaxPathBytes>;

/**
 * PathService: sandboxed path resolver for read-only assets and writable save/prefs.
 */
class PathService {
public:
    [[nodiscard]] Result<void, EngineError> Init(
        std::string_view organization,
        std::string_view application) noexcept;

    [[nodiscard]] std::string_view BasePath() const noexcept { return m_base.View(); }
    [[nodiscard]] std::string_view PrefPath() const noexcept { return m_pref.View(); }

    [[nodiscard]] Result<FilePath, EngineError> ResolveUser(std::string_view relative) const noexcept;
    [[nodiscard]] Result<FilePath, EngineError> ResolveAsset(std::string_view relative) const noexcept;

private:
    FilePath m_base{};
    FilePath m_pref{};
};

/**
 * ClipboardText: zero-leak RAII wrapper over SDL-allocated clipboard string buffer.
 */
class ClipboardText {
public:
    ClipboardText() noexcept = default;
    ~ClipboardText();

    ClipboardText(const ClipboardText&) = delete;
    ClipboardText& operator=(const ClipboardText&) = delete;

    ClipboardText(ClipboardText&& o) noexcept;
    ClipboardText& operator=(ClipboardText&& o) noexcept;

    [[nodiscard]] std::string_view View() const noexcept;
    [[nodiscard]] bool Empty() const noexcept { return m_text == nullptr || *m_text == '\0'; }
    [[nodiscard]] usize Size() const noexcept { return View().size(); }
    [[nodiscard]] const char* CStr() const noexcept { return m_text ? m_text : ""; }

    void Reset() noexcept;

    operator std::string_view() const noexcept { return View(); }

private:
    friend class Clipboard;
    explicit ClipboardText(char* sdlOwned) noexcept : m_text(sdlOwned) {}
    char* m_text{nullptr};
};

class Clipboard {
public:
    [[nodiscard]] bool HasText() const noexcept;
    [[nodiscard]] ClipboardText GetText() const noexcept;
    [[nodiscard]] Result<void, EngineError> SetText(std::string_view utf8) noexcept;

    static constexpr usize kMaxSetBytes = 8 * 1024;
};

struct SystemSpecs {
    FixedString<32> platformName{};
    u32 ramMiB{0};
    u32 logicalCores{0};
    u32 cacheLineBytes{64};
    u32 simdAlignment{16};
    u32 sdlVersion{0};
    bool steamDeckHint{false};
};

[[nodiscard]] SystemSpecs QuerySystemSpecs() noexcept;

enum class MessageSeverity : u8 { Info, Warning, Error };

namespace Dialogs {

[[nodiscard]] Result<void, EngineError> ShowMessage(
    MessageSeverity severity,
    std::string_view title,
    std::string_view text,
    const Window* parent = nullptr) noexcept;

void ShowFatal(std::string_view title, std::string_view text) noexcept;

} // namespace Dialogs

} // namespace EngineN
