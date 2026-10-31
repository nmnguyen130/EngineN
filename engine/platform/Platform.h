#pragma once

#include "platform/Window.h"

struct SDL_Gamepad;

namespace en {

// SDL3 platform runtime managing event pumping, gamepad lifecycle, paths, and atomic file I/O.
class Platform {
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

    WindowFlags Pump(Window& window, EventCapture captured = EventCapture::None) noexcept;

    template <typename InputSystemT>
    WindowFlags Pump(
        Window& window, InputSystemT& input, EventCapture captured = EventCapture::None) noexcept {
        const WindowFlags flags = Pump(window, captured);
        (void)input.Ingest(m_events);
        return flags;
    }

    [[nodiscard]] constexpr const FrameEvents& Events() const noexcept { return m_events; }

    [[nodiscard]] constexpr bool ShouldQuit() const noexcept { return m_quit; }
    constexpr void RequestQuit() noexcept { m_quit = true; }

    [[nodiscard]] constexpr bool HasGamepad() const noexcept { return m_gamepadId != 0U; }
    bool RumbleGamepad(u16 lowFreq, u16 highFreq, u32 durationMs) noexcept;
    void StopRumble() noexcept;

    template <typename EventBusT>
    void ConsumeEvents(EventBusT& bus) noexcept {
        for (const auto& ev : bus.rumbles.Drain()) {
            if (!m_headless && m_initialized) {
                const u16 freq = static_cast<u16>(std::clamp(ev.intensity * 65535.0f, 0.0f, 65535.0f));
                (void)RumbleGamepad(freq, freq, static_cast<u32>(ev.durationTicks) * 8U);
            }
        }
    }

    [[nodiscard]] constexpr Result<FilePath, PlatformErrc> ResolveAsset(
        std::string_view rel) const noexcept {
        FilePath out{};
        if (!ResolvePath(m_basePath, rel, out)) return Err(PlatformErrc::PathRejected);
        return out;
    }
    [[nodiscard]] constexpr Result<FilePath, PlatformErrc> ResolveUser(
        std::string_view rel) const noexcept {
        FilePath out{};
        if (!ResolvePath(m_prefPath, rel, out)) return Err(PlatformErrc::PathRejected);
        return out;
    }
    [[nodiscard]] constexpr std::string_view BasePath() const noexcept { return m_basePath.View(); }
    [[nodiscard]] constexpr std::string_view PrefPath() const noexcept { return m_prefPath.View(); }

    [[nodiscard]] static Result<usize, PlatformErrc> ReadFile(
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

    FrameEvents  m_events{};
    FilePath     m_basePath{};
    FilePath     m_prefPath{};
    SDL_Gamepad* m_gamepad{nullptr};
    u32          m_gamepadId{0};
    bool         m_initialized{false};
    bool         m_quit{false};
    bool         m_headless{false};
    bool         m_rumbling{false};
};
static_assert(sizeof(Platform) == 784);

} // namespace en
