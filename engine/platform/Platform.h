#pragma once

#include "platform/PlatformTypes.h"
#include "platform/Window.h"

struct SDL_Gamepad;

namespace en::platform {

struct EventTap {
    using Fn = EventCapture (*)(void* user, const SDL_Event& ev) noexcept;
    void* user{nullptr};
    Fn callback{nullptr};

    template <auto Method, typename T>
    [[nodiscard]] static constexpr EventTap Bind(T& instance) noexcept {
        return EventTap{
            .user = &instance,
            .callback = [](void* self, const SDL_Event& e) noexcept -> EventCapture {
                return (static_cast<T*>(self)->*Method)(e);
            }
        };
    }
};

// Owns SDL3 subsystem lifecycle, SoA gamepad handles, sandboxed filesystem roots, and coalesced frame pumping.
// Places all hot runtime state (gamepad SoA arrays, active/rumble bitmasks, flags) in Cache Line 0 (offset 0..51).
// Invariant: Active SDL_Gamepad slots match set bits in m_activeGamepadMask and are closed prior to SDL_Quit().
class alignas(64) Platform {
public:
    static constexpr usize kMaxGamepads = en::platform::kMaxGamepads;

    constexpr Platform() noexcept = default;
    ~Platform() noexcept;

    Platform(const Platform&) = delete;
    Platform& operator=(const Platform&) = delete;

    [[nodiscard]] Result<void, PlatformErrc> Init(
        std::string_view appName = "EngineN",
        std::string_view orgName = "EngineN") noexcept;
    void Shutdown() noexcept;

    WindowFlags Pump(Window& window, EventTap tap = {}) noexcept;

    template <typename InputSinkT>
        requires requires(InputSinkT& sink, const PlatformFrameEvents& ev) { sink.IngestPlatformFrame(ev); }
    WindowFlags Pump(Window& window, InputSinkT& input, EventTap tap = {}) noexcept {
        const WindowFlags flags = Pump(window, tap);
        input.IngestPlatformFrame(m_frameEvents);
        return flags;
    }

    [[nodiscard]] constexpr const PlatformFrameEvents& FrameEvents() const noexcept { return m_frameEvents; }
    [[nodiscard]] constexpr bool ShouldQuit() const noexcept { return m_quit; }
    constexpr void RequestQuit() noexcept { m_quit = true; }
    [[nodiscard]] constexpr u8 ActiveGamepadMask() const noexcept { return m_activeGamepadMask; }

    bool RumbleGamepad(u8 padSlot, u16 lowFreq, u16 highFreq, u32 durationMs) noexcept;
    void StopAllRumble() noexcept;

    [[nodiscard]] constexpr Result<FilePath, PlatformErrc> ResolveAsset(std::string_view relative) const noexcept {
        return m_basePath.Resolve(relative);
    }

    [[nodiscard]] constexpr Result<FilePath, PlatformErrc> ResolveUser(std::string_view relative) const noexcept {
        return m_prefPath.Resolve(relative);
    }

    [[nodiscard]] constexpr std::string_view BasePath() const noexcept { return m_basePath.View(); }
    [[nodiscard]] constexpr std::string_view PrefPath() const noexcept { return m_prefPath.View(); }

    [[nodiscard]] static std::string_view GetClipboard(std::span<char> outBuf) noexcept;
    [[nodiscard]] static Result<void, PlatformErrc> SetClipboard(std::string_view utf8) noexcept;
    static void ShowErrorBox(std::string_view title, std::string_view message, const Window* parent = nullptr) noexcept;

    [[nodiscard]] constexpr u64 ComputeAuditHash() const noexcept {
        const u64 stateBits =
            (static_cast<u64>(m_activeGamepadMask) << 24) |
            (static_cast<u64>(m_rumbleGamepadMask) << 16) |
            (static_cast<u64>(m_initialized) << 8) |
            static_cast<u64>(m_quit);
        return detail::MixAuditBits(m_frameEvents.ComputeAuditHash() ^ detail::MixAuditBits(stateBits));
    }

private:
    void OpenGamepad(u32 instanceId) noexcept;
    [[nodiscard]] u8 CloseGamepad(u32 instanceId) noexcept;

    [[nodiscard]] constexpr u8 FindGamepadSlot(u32 instanceId) const noexcept {
        for (u32 m = m_activeGamepadMask; m != 0; m &= (m - 1u)) {
            const u8 slot = static_cast<u8>(std::countr_zero(m));
            if (m_gamepadIds[slot] == instanceId) return slot;
        }
        return 0xFFu;
    }

    // Cache Line 0 (offset 0..51): Hot runtime state and SoA gamepad lookup tables.
    std::array<SDL_Gamepad*, kMaxGamepads> m_gamepadHandles{};
    std::array<u32, kMaxGamepads> m_gamepadIds{};
    u8 m_activeGamepadMask{0};
    u8 m_rumbleGamepadMask{0};
    bool m_initialized{false};
    bool m_quit{false};

    // Cache Lines 1..4 (offset 64..319, 256B): Coalesced frame events.
    PlatformFrameEvents m_frameEvents{};

    // Cold Cache Lines 5..36 (offset 320..2367, 2048B): Pre-normalized filesystem roots.
    FilePath m_basePath{};
    FilePath m_prefPath{};
};
static_assert(sizeof(Platform) == 2368);

} // namespace en::platform
