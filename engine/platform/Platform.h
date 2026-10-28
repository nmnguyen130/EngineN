#pragma once

#include "platform/Window.h"

struct SDL_Gamepad;

namespace en {

// SDL3 owner: init, one window pump, one gamepad, paths, atomic file IO.
// Invariant: m_gamepadId != 0 iff a controller is connected (or headless slot).
class alignas(16) Platform {
public:
    Platform() noexcept = default;
    ~Platform() noexcept { Shutdown(); }

    Platform(const Platform&) = delete;
    Platform& operator=(const Platform&) = delete;

    [[nodiscard]] Result<void, PlatformErrc> Init(
        std::string_view appName = "EngineN",
        std::string_view orgName = "EngineN",
        bool headless = false) noexcept;
    void Shutdown() noexcept;

    [[nodiscard]] constexpr bool IsInitialized() const noexcept { return m_initialized; }
    [[nodiscard]] constexpr bool IsHeadless() const noexcept { return m_headless; }

    // Drain SDL queue -> Window lifecycle + FrameEvents. Returns coalesced window flags.
    WindowFlags Pump(Window& window, EventCapture captured = EventCapture::None) noexcept;

    template <typename InputSinkT>
    WindowFlags Pump(
        Window& window, InputSinkT& input, EventCapture captured = EventCapture::None) noexcept {
        const WindowFlags flags = Pump(window, captured);
        input.IngestPlatformFrame(m_frameEvents);
        return flags;
    }

    [[nodiscard]] constexpr const PlatformFrameEvents& FrameEvents() const noexcept {
        return m_frameEvents;
    }

    [[nodiscard]] constexpr bool ShouldQuit() const noexcept { return m_quit; }
    constexpr void RequestQuit() noexcept { m_quit = true; }

    [[nodiscard]] constexpr bool HasGamepad() const noexcept { return m_gamepadId != 0U; }
    bool RumbleGamepad(u16 lowFreq, u16 highFreq, u32 durationMs) noexcept;
    void StopRumble() noexcept;

    [[nodiscard]] constexpr Result<FilePath, PlatformErrc> ResolveAsset(
        std::string_view rel) const noexcept {
        FilePath out{};
        if (!m_basePath.Resolve(rel, out)) return Err(PlatformErrc::PathRejected);
        return out;
    }
    [[nodiscard]] constexpr Result<FilePath, PlatformErrc> ResolveUser(
        std::string_view rel) const noexcept {
        FilePath out{};
        if (!m_prefPath.Resolve(rel, out)) return Err(PlatformErrc::PathRejected);
        return out;
    }
    [[nodiscard]] constexpr std::string_view BasePath() const noexcept { return m_basePath.View(); }
    [[nodiscard]] constexpr std::string_view PrefPath() const noexcept { return m_prefPath.View(); }

    [[nodiscard]] static Result<usize, PlatformErrc> ReadFileBuffer(
        const FilePath& path, std::span<u8> outBuf) noexcept;
    [[nodiscard]] static Result<void, PlatformErrc> WriteFileAtomic(
        const FilePath& path, std::span<const u8> data) noexcept;

    static bool SetClipboardText(std::string_view text) noexcept;
    [[nodiscard]] static usize GetClipboardText(std::span<char> outBuf) noexcept;
    static void ShowErrorBox(
        std::string_view title, std::string_view message, const Window* parent = nullptr) noexcept;

private:
    bool OpenGamepad(u32 instanceId) noexcept;
    void CloseGamepad(u32 instanceId) noexcept;

    PlatformFrameEvents m_frameEvents{};
    FilePath m_basePath{};
    FilePath m_prefPath{};
    SDL_Gamepad* m_gamepad{nullptr};
    u32 m_gamepadId{0};
    bool m_initialized{false};
    bool m_quit{false};
    bool m_headless{false};
    bool m_rumbling{false};
};
static_assert(sizeof(Platform) == 784);

} // namespace en
