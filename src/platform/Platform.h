#pragma once

#include "core/Result.h"
#include "core/Types.h"
#include "platform/PlatformTypes.h"
#include <span>

namespace EngineN {

/// RAII-based SDL subsystem initialization and platform utility services.
/// Only ONE Platform instance may exist at any time.
class Platform {
public:
    Platform() = default;
    ~Platform();

    Platform(const Platform&) = delete;
    Platform& operator=(const Platform&) = delete;
    Platform(Platform&&) = delete;
    Platform& operator=(Platform&&) = delete;

    /// Initialize SDL subsystems (VIDEO | GAMEPAD).
    [[nodiscard]] Result<void> Init();

    /// Shutdown SDL subsystems. Safe to call multiple times.
    void Shutdown();

    [[nodiscard]] bool IsInitialized() const noexcept { return m_initialized; }

    // Display queries
    [[nodiscard]] i32 GetDisplayCount() const;
    [[nodiscard]] i32 QueryDisplays(std::span<DisplayInfo> outDisplays) const;
    [[nodiscard]] DisplayInfo GetPrimaryDisplayInfo() const;

    // Clipboard
    [[nodiscard]] bool HasClipboardText() const;
    [[nodiscard]] const char* GetClipboardText() const;
    bool SetClipboardText(const char* text);

    // Performance counters
    [[nodiscard]] u64 GetPerformanceCounter() const noexcept;
    [[nodiscard]] u64 GetPerformanceFrequency() const noexcept;

    // Power / Battery
    [[nodiscard]] PowerInfo GetPowerInfo() const;

    // System info
    [[nodiscard]] i32 GetSystemRAM() const noexcept;
    [[nodiscard]] i32 GetCPUCount() const noexcept;

private:
    bool m_initialized = false;
};

} // namespace EngineN
