#pragma once

#include "core/Types.h"

namespace en {

// Bounds-checked little-endian binary writer over a caller-owned byte span.
class BinaryWriter {
public:
    constexpr explicit BinaryWriter(std::span<u8> buf) noexcept : m_buf(buf) {}

    [[nodiscard]] constexpr usize Offset() const noexcept { return m_pos; }
    [[nodiscard]] constexpr usize Remaining() const noexcept { return m_buf.size() - m_pos; }
    [[nodiscard]] constexpr bool Ok() const noexcept { return !m_overflow; }
    [[nodiscard]] constexpr std::span<const u8> Written() const noexcept {
        return m_buf.subspan(0U, m_pos);
    }

    constexpr bool WriteBytes(const void* src, usize bytes) noexcept {
        if (m_overflow || bytes > Remaining()) {
            m_overflow = true;
            return false;
        }
        if (bytes > 0U) {
            std::memcpy(m_buf.data() + m_pos, src, bytes);
            m_pos += bytes;
        }
        return true;
    }

    constexpr bool WriteBytes(std::span<const u8> bytes) noexcept {
        return WriteBytes(bytes.data(), bytes.size());
    }

    template <typename T>
        requires std::is_trivially_copyable_v<T>
    constexpr bool Write(const T& v) noexcept {
        return WriteBytes(&v, sizeof(T));
    }

    constexpr bool WriteStr(std::string_view sv) noexcept {
        if (sv.size() > 0xFFFFU) {
            m_overflow = true;
            return false;
        }
        const u16 len = static_cast<u16>(sv.size());
        return Write(len) && WriteBytes(sv.data(), len);
    }

private:
    std::span<u8> m_buf{};
    usize         m_pos{0};
    bool          m_overflow{false};
};

// Bounds-checked little-endian binary reader over a caller-owned byte span.
class BinaryReader {
public:
    constexpr explicit BinaryReader(std::span<const u8> buf) noexcept : m_buf(buf) {}

    [[nodiscard]] constexpr usize Offset() const noexcept { return m_pos; }
    [[nodiscard]] constexpr usize Remaining() const noexcept { return m_buf.size() - m_pos; }
    [[nodiscard]] constexpr bool Ok() const noexcept { return !m_overflow; }

    constexpr bool ReadBytes(void* dst, usize bytes) noexcept {
        if (m_overflow || bytes > Remaining()) {
            m_overflow = true;
            return false;
        }
        if (bytes > 0U) {
            std::memcpy(dst, m_buf.data() + m_pos, bytes);
            m_pos += bytes;
        }
        return true;
    }

    constexpr bool ReadBytes(std::span<u8> dst) noexcept {
        return ReadBytes(dst.data(), dst.size());
    }

    template <typename T>
        requires std::is_trivially_copyable_v<T>
    constexpr bool Read(T& out) noexcept {
        return ReadBytes(&out, sizeof(T));
    }

    constexpr bool ReadStr(std::string_view& out) noexcept {
        u16 len = 0U;
        if (!Read(len) || len > Remaining()) {
            m_overflow = true;
            return false;
        }
        out = std::string_view{
            reinterpret_cast<const char*>(m_buf.data() + m_pos), len};
        m_pos += len;
        return true;
    }

    constexpr bool Skip(usize bytes) noexcept {
        if (m_overflow || bytes > Remaining()) {
            m_overflow = true;
            return false;
        }
        m_pos += bytes;
        return true;
    }

private:
    std::span<const u8> m_buf{};
    usize               m_pos{0};
    bool                m_overflow{false};
};

} // namespace en
