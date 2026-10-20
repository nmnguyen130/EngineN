#pragma once

#include "platform/Window.h"

struct SDL_Gamepad;

namespace en {

// Cross-platform C++23 std::filesystem file modification watcher (works on Windows MSVC/MinGW, Linux, and macOS).
class FileWatcher {
public:
    static constexpr usize kMaxWatched = 32;
    struct WatchSlot {
        FilePath path{};
        u64 lastModTick{0};
        StringId assetId{};
        bool active{false};
    };

    bool Watch(const FilePath& path, StringId assetId) noexcept;
    [[nodiscard]] u32 PollModified(std::span<StringId> outChangedIds) noexcept;
    [[nodiscard]] static u64 QueryFileModTimeTicks(const char* cpath) noexcept;

private:
    std::array<WatchSlot, kMaxWatched> m_slots{};
};

// OS platform coordinator managing SDL lifecycle, coalesced event pumping, gamepads, and sandbox I/O.
class alignas(64) Platform {
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

    WindowFlags Pump(Window& window, EventCapture captured = EventCapture::None) noexcept;

    template <typename InputSinkT>
    WindowFlags Pump(Window& window, InputSinkT& input, EventCapture captured = EventCapture::None) noexcept {
        const WindowFlags flags = Pump(window, captured);
        input.IngestPlatformFrame(m_frameEvents);
        return flags;
    }

    [[nodiscard]] constexpr const PlatformFrameEvents& FrameEvents() const noexcept { return m_frameEvents; }
    [[nodiscard]] constexpr bool ShouldQuit() const noexcept { return m_quit; }
    constexpr void RequestQuit() noexcept { m_quit = true; }

    bool RumbleGamepad(u8 padSlot, u16 lowFreq, u16 highFreq, u32 durationMs) noexcept;
    void StopAllRumble() noexcept;
    [[nodiscard]] constexpr u8 ActiveGamepadMask() const noexcept { return m_activeGamepadMask; }

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

    [[nodiscard]] static Result<usize, PlatformErrc> ReadFileBuffer(const FilePath& path, std::span<u8> outBuf) noexcept;
    [[nodiscard]] static Result<void, PlatformErrc> WriteFileAtomic(const FilePath& path, std::span<const u8> data) noexcept;

    bool PushDroppedFile(std::string_view filePath) noexcept;
    [[nodiscard]] u32 PollDroppedFiles(std::span<FilePath> outFiles) noexcept;
    [[nodiscard]] constexpr u32 DroppedFileCount() const noexcept { return m_droppedFileCount; }

    static bool SetClipboardText(std::string_view text) noexcept;
    [[nodiscard]] static usize GetClipboardText(std::span<char> outBuf) noexcept;

    [[nodiscard]] constexpr u64 ComputeAuditHash() const noexcept { return m_frameEvents.ComputeAuditHash(); }

private:
    std::array<SDL_Gamepad*, 4> m_gamepadHandles{};
    std::array<u32, 4> m_gamepadIds{};
    u8 m_activeGamepadMask{0};
    u8 m_rumbleGamepadMask{0};
    bool m_initialized{false};
    bool m_quit{false};
    bool m_headless{false};

    PlatformFrameEvents m_frameEvents{};
    FilePath m_basePath{};
    FilePath m_prefPath{};
    std::array<FilePath, 8> m_droppedFiles{};
    u32 m_droppedFileCount{0};
};

} // namespace en
