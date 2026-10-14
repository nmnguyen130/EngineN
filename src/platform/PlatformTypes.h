#pragma once

#include "core/Result.h"
#include "core/Types.h"

#include <array>
#include <string_view>

namespace en::platform {

enum class PlatformErrc : u8 {
    SdlInitFailed,
    WindowCreateFailed,
    FullscreenFailed,
    PathUnavailable,
    PathRejected,
    ClipboardFailed,
    InvalidState,
};

[[nodiscard]] Error MakePlatformError(PlatformErrc code, std::string_view detail = {}) noexcept;

struct Extent2i {
    i32 w{0};
    i32 h{0};
    friend constexpr bool operator==(const Extent2i&, const Extent2i&) noexcept = default;
};

// Fixed-capacity NUL-terminated string buffer with zero heap allocation.
// Invariant: m_buf[m_len] == '\0' and m_len < N.
template <usize N>
class FixedString {
    static_assert(N >= 2);

public:
    constexpr FixedString() noexcept = default;
    constexpr explicit FixedString(std::string_view s) noexcept { (void)Assign(s); }

    constexpr bool Assign(std::string_view s) noexcept {
        const usize n = s.size() < (N - 1) ? s.size() : (N - 1);
        for (usize i = 0; i < n; ++i) {
            m_buf[i] = s[i];
        }
        m_buf[n] = '\0';
        m_len = n;
        return n == s.size();
    }

    [[nodiscard]] constexpr std::string_view View() const noexcept { return {m_buf.data(), m_len}; }
    [[nodiscard]] constexpr const char* CStr() const noexcept { return m_buf.data(); }
    [[nodiscard]] constexpr usize Size() const noexcept { return m_len; }
    [[nodiscard]] constexpr bool Empty() const noexcept { return m_len == 0; }

    constexpr operator std::string_view() const noexcept { return View(); }
    [[nodiscard]] constexpr bool operator==(std::string_view other) const noexcept { return View() == other; }

private:
    std::array<char, N> m_buf{};
    usize m_len{0};
};

inline constexpr usize kMaxPathBytes = 1024;
using FilePath = FixedString<kMaxPathBytes>;

} // namespace en::platform