#pragma once

#include "core/Result.h"
#include "core/Types.h"
#include "platform/PlatformTypes.h"
#include <span>
#include <vector>
#include <string>
#include <string_view>

namespace EngineN {

/// RAII-based SDL subsystem initialization and platform utility services.
/// Only ONE Platform instance may exist at any time.
/// Provides idempotent lifecycle management and non-allocating hardware diagnostics.
class Platform {
public:
    Platform() = default;
    ~Platform();

    Platform(const Platform&) = delete;
    Platform& operator=(const Platform&) = delete;
    Platform(Platform&&) = delete;
    Platform& operator=(Platform&&) = delete;

    /// Initialize SDL subsystems (VIDEO | GAMEPAD). Idempotent.
    [[nodiscard]] Result<void> Init();

    /// Shutdown SDL subsystems. Safe to call multiple times (idempotent).
    void Shutdown();

    [[nodiscard]] bool IsInitialized() const noexcept { return m_initialized; }

    // --- Display Queries ---
    [[nodiscard]] i32 GetDisplayCount() const;
    [[nodiscard]] i32 QueryDisplays(std::span<DisplayInfo> outDisplays) const;
    [[nodiscard]] std::vector<DisplayInfo> GetAllDisplays() const;
    [[nodiscard]] DisplayInfo GetPrimaryDisplayInfo() const;
    [[nodiscard]] DisplayInfo GetDisplayInfo(u32 displayId) const;
    [[nodiscard]] f32 GetDisplayRefreshRate(u32 displayId = 0) const;
    [[nodiscard]] f32 GetDisplayContentScale(u32 displayId = 0) const;

    // --- Clipboard (RAII Zero-Leak Abstraction) ---
    [[nodiscard]] bool HasClipboardText() const;
    [[nodiscard]] ClipboardText GetClipboardText() const;
    [[nodiscard]] std::string GetClipboardString() const;
    bool SetClipboardText(const char* text);
    bool SetClipboardText(std::string_view text);

    // --- Performance Counters ---
    [[nodiscard]] u64 GetPerformanceCounter() const noexcept;
    [[nodiscard]] u64 GetPerformanceFrequency() const noexcept;

    // --- Power / Battery Diagnostics ---
    [[nodiscard]] PowerInfo GetPowerInfo() const;

    // --- System Hardware Diagnostics (Non-allocating) ---
    [[nodiscard]] i32 GetSystemRAM() const noexcept;
    [[nodiscard]] i32 GetCPUCount() const noexcept;
    [[nodiscard]] i32 GetCPUCacheLineSize() const noexcept;

private:
    bool m_initialized = false;
};

} // namespace EngineN
