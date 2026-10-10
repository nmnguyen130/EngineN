#pragma once

#include "core/Result.h"
#include "core/Types.h"

#include <array>
#include <cstddef>
#include <span>
#include <string_view>
#include <type_traits>

namespace EngineN {

enum class PlatformErrc : u16 {
    SdlInitFailed,
    WindowCreateFailed,
    DisplayQueryFailed,
    FullscreenFailed,
    PathUnavailable,
    PathRejected,
    ClipboardFailed,
    DeviceOpenFailed,
    CapacityExceeded,
    InvalidState,
};

[[nodiscard]] EngineError MakePlatformError(PlatformErrc code, std::string_view detail = {}) noexcept;

template <typename Tag>
struct StrongId {
    u32 value{0};

    [[nodiscard]] constexpr explicit operator bool() const noexcept { return value != 0; }
    friend constexpr bool operator==(StrongId, StrongId) noexcept = default;
};

using DisplayId     = StrongId<struct DisplayIdTag>;
using WindowId      = StrongId<struct WindowIdTag>;
using GamepadId     = StrongId<struct GamepadIdTag>;
using AudioDeviceId = StrongId<struct AudioDeviceIdTag>;

struct IPoint {
    i32 x{0};
    i32 y{0};
    friend constexpr bool operator==(const IPoint&, const IPoint&) noexcept = default;
};

struct Extent2i {
    i32 w{0};
    i32 h{0};
    friend constexpr bool operator==(const Extent2i&, const Extent2i&) noexcept = default;
};

struct IRect {
    i32 x{0};
    i32 y{0};
    i32 w{0};
    i32 h{0};
    friend constexpr bool operator==(const IRect&, const IRect&) noexcept = default;
};


/**
 * FixedString: inline NUL-terminated static string buffer.
 * Non-allocating and safe for fixed stack and struct layout.
 */
template <usize N>
class FixedString {
    static_assert(N >= 2, "FixedString capacity must be at least 2 bytes");

public:
    constexpr FixedString() noexcept = default;

    constexpr explicit FixedString(std::string_view s) noexcept {
        (void)Assign(s);
    }

    constexpr bool Assign(std::string_view s) noexcept {
        const usize n = s.size() < (N - 1) ? s.size() : (N - 1);
        for (usize i = 0; i < n; ++i) {
            m_buf[i] = s[i];
        }
        m_buf[n] = '\0';
        m_len = n;
        return n == s.size();
    }

    constexpr void Clear() noexcept {
        m_buf[0] = '\0';
        m_len = 0;
    }

    [[nodiscard]] constexpr std::string_view View() const noexcept { return {m_buf.data(), m_len}; }
    [[nodiscard]] constexpr const char* CStr() const noexcept { return m_buf.data(); }
    [[nodiscard]] constexpr usize Size() const noexcept { return m_len; }
    [[nodiscard]] constexpr bool Empty() const noexcept { return m_len == 0; }
    static constexpr usize Capacity() noexcept { return N - 1; }

    constexpr bool operator==(std::string_view other) const noexcept { return View() == other; }

private:
    std::array<char, N> m_buf{};
    usize m_len{0};
};

/**
 * FixedVector: fixed-capacity non-allocating vector for trivially copyable items.
 */
template <typename T, usize N>
requires std::is_trivially_copyable_v<T>
class FixedVector {
public:
    [[nodiscard]] constexpr bool TryPush(const T& v) noexcept {
        if (m_size == N) return false;
        m_data[m_size++] = v;
        return true;
    }

    constexpr void Clear() noexcept {
        m_size = 0;
    }

    constexpr bool EraseAt(usize i) noexcept {
        if (i >= m_size) return false;
        for (usize j = i + 1; j < m_size; ++j) {
            m_data[j - 1] = m_data[j];
        }
        --m_size;
        return true;
    }

    [[nodiscard]] constexpr usize Size() const noexcept { return m_size; }
    [[nodiscard]] constexpr bool Empty() const noexcept { return m_size == 0; }
    [[nodiscard]] constexpr bool Full() const noexcept { return m_size == N; }
    static constexpr usize Capacity() noexcept { return N; }

    [[nodiscard]] constexpr T& operator[](usize i) noexcept { return m_data[i]; }
    [[nodiscard]] constexpr const T& operator[](usize i) const noexcept { return m_data[i]; }
    [[nodiscard]] constexpr std::span<T> View() noexcept { return {m_data.data(), m_size}; }
    [[nodiscard]] constexpr std::span<const T> View() const noexcept { return {m_data.data(), m_size}; }

private:
    std::array<T, N> m_data{};
    usize m_size{0};
};

inline constexpr usize kMaxPathBytes = 1024;

/**
 * Discrete app-facing platform notifications.
 */
enum class PlatformEventType : u8 {
    QuitRequested,
    Terminating,
    DisplayTopologyChanged,
    GamepadConnected,
    GamepadDisconnected,
    AudioDeviceAdded,
    AudioDeviceRemoved,
    ClipboardChanged
};

struct PlatformEvent {
    PlatformEventType type{PlatformEventType::QuitRequested};
    u8 slot{0};
    bool recording{false};
    u32 id{0};
    u64 timestampNs{0};
};
static_assert(std::is_trivially_copyable_v<PlatformEvent>, "PlatformEvent must be trivially copyable");

inline constexpr usize kMaxPlatformEventsPerFrame = 64;
using PlatformEventQueue = FixedVector<PlatformEvent, kMaxPlatformEventsPerFrame>;

} // namespace EngineN
