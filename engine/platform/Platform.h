#pragma once

#include "platform/Window.h"

struct SDL_Gamepad;

namespace en {

// Bitmask-accelerated file modification watcher for hot-reloading cooked assets in development builds.
// Value type owning a fixed 32-slot table and 32-bit occupancy bitmask; zero dynamic allocation.
// Invariant: bit i of m_activeMask is 1 iff m_slots[i].assetId.IsValid() holds; PollModified is O(active).
class alignas(8) FileWatcher {
public:
    static constexpr usize kMaxWatched = 32;

    struct alignas(8) WatchSlot {
        u64 lastModTick{0};
        StringId assetId{};
        FilePath path{};
    };
    static_assert(sizeof(WatchSlot) == 272);

    bool Watch(const FilePath& path, StringId assetId) noexcept;
    bool Unwatch(StringId assetId) noexcept;

    constexpr void Clear() noexcept {
        m_activeMask = 0U;
    }

    [[nodiscard]] constexpr u32 ActiveCount() const noexcept {
        return static_cast<u32>(std::popcount(m_activeMask));
    }

    [[nodiscard]] u32 PollModified(std::span<StringId> outChangedIds) noexcept;
    [[nodiscard]] static u64 QueryFileModTimeTicks(const char* cpath) noexcept;

private:
    std::array<WatchSlot, kMaxWatched> m_slots{};
    u32 m_activeMask{0};
};

// OS platform coordinator managing SDL3 lifecycle, coalesced event pumping, gamepads, power state, and sandbox I/O.
// Single-instance move-disabled coordinator; packs all hot gamepad SoA and flags into Cache Line 0 (64B exact).
// Invariant: m_activeGamepadMask uses only bits [0..3], and m_frameEvents occupies Cache Lines 1..4 (offset 64..319).
class alignas(64) Platform {
public:
    static constexpr usize kMaxDroppedFiles = 4;

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

    WindowFlags Pump(Window& window, EventTap tap = {}) noexcept;

    template <typename InputSinkT>
    WindowFlags Pump(Window& window, InputSinkT& input, EventTap tap = {}) noexcept {
        const WindowFlags flags = Pump(window, tap);
        input.IngestPlatformFrame(m_frameEvents);
        return flags;
    }

    WindowFlags FinalizePump(Window& window, WindowFlags winFlags, bool needRefresh) noexcept;

    void SetLiveResizeWatch(Window* window) noexcept;
    void SetScreenSleepEnabled(bool enabled) noexcept;
    [[nodiscard]] constexpr bool IsScreenSleepEnabled() const noexcept { return !m_screenSleepDisabled; }

    [[nodiscard]] static PowerInfo QueryPowerInfo() noexcept;

    [[nodiscard]] constexpr PlatformFrameEvents& FrameEvents() noexcept { return m_frameEvents; }
    [[nodiscard]] constexpr const PlatformFrameEvents& FrameEvents() const noexcept { return m_frameEvents; }

    [[nodiscard]] constexpr bool ShouldQuit() const noexcept { return m_quit; }
    constexpr void RequestQuit() noexcept { m_quit = true; }

    bool OpenGamepad(u32 instanceId) noexcept;
    void CloseGamepad(u32 instanceId) noexcept;
    [[nodiscard]] constexpr u8 FindGamepadSlot(u32 instanceId) const noexcept {
        u8 mask = m_activeGamepadMask;
        while (mask != 0U) {
            const u8 slot = static_cast<u8>(std::countr_zero(mask));
            if (m_gamepadIds[slot] == instanceId) return slot;
            mask &= static_cast<u8>(mask - 1U);
        }
        return kInvalidGamepadSlot;
    }

    bool RumbleGamepad(u8 padSlot, u16 lowFreq, u16 highFreq, u32 durationMs) noexcept;
    void StopAllRumble() noexcept;
    [[nodiscard]] constexpr u8 ActiveGamepadMask() const noexcept { return m_activeGamepadMask; }
    [[nodiscard]] constexpr u8 RumbleGamepadMask() const noexcept { return m_rumbleGamepadMask; }

    [[nodiscard]] constexpr Result<FilePath, PlatformErrc> ResolveAsset(std::string_view rel) const noexcept {
        FilePath out{};
        if (!m_basePath.Resolve(rel, out)) return Err(PlatformErrc::PathRejected);
        return out;
    }

    [[nodiscard]] constexpr Result<FilePath, PlatformErrc> ResolveUser(std::string_view rel) const noexcept {
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

    bool PushDroppedFile(std::string_view filePath) noexcept;
    [[nodiscard]] u32 PollDroppedFiles(std::span<FilePath> outFiles) noexcept;
    [[nodiscard]] constexpr u8 DroppedFileCount() const noexcept { return m_droppedFileCount; }

    static bool SetClipboardText(std::string_view text) noexcept;
    [[nodiscard]] static usize GetClipboardText(std::span<char> outBuf) noexcept;
    static void ShowErrorBox(
        std::string_view title, std::string_view message, const Window* parent = nullptr) noexcept;

    [[nodiscard]] constexpr u64 ComputeAuditHash() const noexcept {
        return m_frameEvents.ComputeAuditHash();
    }

private:
    std::array<SDL_Gamepad*, kMaxGamepads> m_gamepadHandles{};
    std::array<u32, kMaxGamepads> m_gamepadIds{};
    Window* m_watchedWindow{nullptr};
    u8 m_activeGamepadMask{0};
    u8 m_rumbleGamepadMask{0};
    u8 m_droppedFileCount{0};
    bool m_initialized{false};
    bool m_quit{false};
    bool m_headless{false};
    bool m_watchRegistered{false};
    bool m_screenSleepDisabled{false};

    PlatformFrameEvents m_frameEvents{};
    FilePath m_basePath{};
    FilePath m_prefPath{};
    std::array<FilePath, kMaxDroppedFiles> m_droppedFiles{};
};
static_assert(sizeof(Platform) == 1856);

} // namespace en
