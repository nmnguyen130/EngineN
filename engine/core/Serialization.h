#pragma once

#include "core/Types.h"
#include <charconv>

namespace en {

// Bounds-checked binary stream writer over a pre-allocated byte span.
class BinaryWriter {
public:
    constexpr explicit BinaryWriter(std::span<u8> buffer) noexcept : m_buffer(buffer) {}

    template <typename T>
        requires std::is_trivially_copyable_v<T>
    bool Write(const T& value) noexcept {
        return WriteBytes(std::span<const u8>(reinterpret_cast<const u8*>(&value), sizeof(T)));
    }

    bool WriteBytes(std::span<const u8> bytes) noexcept {
        if (!m_ok || m_offset + bytes.size() > m_buffer.size()) {
            m_ok = false;
            return false;
        }
        if (!bytes.empty()) {
            std::memcpy(m_buffer.data() + m_offset, bytes.data(), bytes.size());
            m_crc = ~ComputeCrc32(bytes, ~m_crc);
            m_offset += bytes.size();
        }
        return true;
    }

    bool WriteString(std::string_view sv) noexcept {
        if (sv.size() > 0xFFFFU) {
            m_ok = false;
            return false;
        }
        const u16 len = static_cast<u16>(sv.size());
        if (!Write(len)) return false;
        return WriteBytes(std::span<const u8>(reinterpret_cast<const u8*>(sv.data()), sv.size()));
    }

    [[nodiscard]] constexpr std::span<const u8> WrittenSpan() const noexcept {
        return m_buffer.subspan(0, m_offset);
    }
    [[nodiscard]] constexpr usize Offset() const noexcept { return m_offset; }
    [[nodiscard]] constexpr u32 ChecksumCrc32() const noexcept { return ~m_crc; }
    [[nodiscard]] constexpr bool Ok() const noexcept { return m_ok; }

private:
    std::span<u8> m_buffer{};
    usize m_offset{0};
    u32 m_crc{0xFFFFFFFFU};
    bool m_ok{true};
};

// Bounds-checked binary stream reader over a read-only byte span.
class BinaryReader {
public:
    constexpr explicit BinaryReader(std::span<const u8> buffer) noexcept : m_buffer(buffer) {}

    template <typename T>
        requires std::is_trivially_copyable_v<T>
    [[nodiscard]] bool Read(T& outValue) noexcept {
        if (!m_ok || m_offset + sizeof(T) > m_buffer.size()) {
            m_ok = false;
            return false;
        }
        std::memcpy(&outValue, m_buffer.data() + m_offset, sizeof(T));
        m_offset += sizeof(T);
        return true;
    }

    [[nodiscard]] bool ReadBytes(std::span<u8> outBytes) noexcept {
        if (!m_ok || m_offset + outBytes.size() > m_buffer.size()) {
            m_ok = false;
            return false;
        }
        if (!outBytes.empty()) {
            std::memcpy(outBytes.data(), m_buffer.data() + m_offset, outBytes.size());
            m_offset += outBytes.size();
        }
        return true;
    }

    [[nodiscard]] bool ReadStringView(std::string_view& outSv) noexcept {
        u16 len = 0;
        if (!Read(len)) return false;
        if (m_offset + len > m_buffer.size()) {
            m_ok = false;
            return false;
        }
        outSv = std::string_view(reinterpret_cast<const char*>(m_buffer.data() + m_offset), len);
        m_offset += len;
        return true;
    }

    [[nodiscard]] constexpr usize Offset() const noexcept { return m_offset; }
    [[nodiscard]] constexpr usize Remaining() const noexcept { return m_buffer.size() - m_offset; }
    [[nodiscard]] constexpr bool Ok() const noexcept { return m_ok; }

private:
    std::span<const u8> m_buffer{};
    usize m_offset{0};
    bool m_ok{true};
};

// Zero-allocation locale-independent JSON builder using std::to_chars.
class JsonLiteWriter {
public:
    constexpr explicit JsonLiteWriter(std::span<char> outBuf) noexcept : m_buf(outBuf) {}

    void AppendRaw(std::string_view sv) noexcept {
        if (m_pos + sv.size() >= m_buf.size()) return;
        std::memcpy(m_buf.data() + m_pos, sv.data(), sv.size());
        m_pos += sv.size();
        m_buf[m_pos] = '\0';
    }

    void AppendInt(i64 val) noexcept {
        if (m_pos + 24 >= m_buf.size()) return;
        auto [ptr, ec] = std::to_chars(m_buf.data() + m_pos, m_buf.data() + m_buf.size() - 1, val);
        if (ec == std::errc{}) {
            m_pos = static_cast<usize>(ptr - m_buf.data());
            m_buf[m_pos] = '\0';
        }
    }

    [[nodiscard]] constexpr std::string_view View() const noexcept { return {m_buf.data(), m_pos}; }
    [[nodiscard]] constexpr usize Size() const noexcept { return m_pos; }

private:
    std::span<char> m_buf{};
    usize m_pos{0};
};

// Fixed-capacity typed property store for data-driven reflection and entity parameters.
class alignas(64) PropertyBag {
public:
    static constexpr usize kMaxEntries = 64;
    enum class PropKind : u8 { None = 0, Int64 = 1, Float64 = 2, Bool = 3, StringId = 4 };

    struct alignas(16) Entry {
        StringId key{};
        union {
            i64 iVal{0};
            f64 fVal;
            u64 uVal;
        };
        PropKind kind{PropKind::None};
    };

    constexpr void Clear() noexcept { m_count = 0; }

    constexpr bool SetInt(StringId key, i64 v) noexcept { return Upsert(key, PropKind::Int64, std::bit_cast<u64>(v)); }
    constexpr bool SetFloat(StringId key, f64 v) noexcept { return Upsert(key, PropKind::Float64, std::bit_cast<u64>(v)); }
    constexpr bool SetBool(StringId key, bool v) noexcept { return Upsert(key, PropKind::Bool, v ? 1ULL : 0ULL); }
    constexpr bool SetStringId(StringId key, StringId v) noexcept { return Upsert(key, PropKind::StringId, v.value); }

    [[nodiscard]] constexpr i64 GetInt(StringId key, i64 fallback = 0) const noexcept {
        const Entry* e = Find(key, PropKind::Int64);
        return e ? e->iVal : fallback;
    }
    [[nodiscard]] constexpr f64 GetFloat(StringId key, f64 fallback = 0.0) const noexcept {
        const Entry* e = Find(key, PropKind::Float64);
        return e ? e->fVal : fallback;
    }
    [[nodiscard]] constexpr bool GetBool(StringId key, bool fallback = false) const noexcept {
        const Entry* e = Find(key, PropKind::Bool);
        return e ? (e->uVal != 0ULL) : fallback;
    }
    [[nodiscard]] constexpr StringId GetStringId(StringId key, StringId fallback = {}) const noexcept {
        const Entry* e = Find(key, PropKind::StringId);
        return e ? StringId{e->uVal} : fallback;
    }

    [[nodiscard]] constexpr usize Size() const noexcept { return m_count; }

private:
    constexpr bool Upsert(StringId key, PropKind kind, u64 raw) noexcept {
        if (!key.IsValid()) return false;
        for (usize i = 0; i < m_count; ++i) {
            if (m_entries[i].key == key) {
                m_entries[i].uVal = raw;
                m_entries[i].kind = kind;
                return true;
            }
        }
        if (m_count >= kMaxEntries) return false;
        m_entries[m_count].key = key;
        m_entries[m_count].uVal = raw;
        m_entries[m_count].kind = kind;
        ++m_count;
        return true;
    }

    [[nodiscard]] constexpr const Entry* Find(StringId key, PropKind kind) const noexcept {
        for (usize i = 0; i < m_count; ++i) {
            if (m_entries[i].key == key && m_entries[i].kind == kind) return &m_entries[i];
        }
        return nullptr;
    }

    Entry m_entries[kMaxEntries]{};
    usize m_count{0};
};

} // namespace en
