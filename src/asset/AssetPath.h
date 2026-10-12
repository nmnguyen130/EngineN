#pragma once

#include "core/Types.h"
#include "core/Result.h"
#include "core/Error.h"
#include "asset/AssetTypes.h"
#include <string_view>
#include <array>

namespace EngineN {

/**
 * AssetPath: Canonical, normalized virtual asset path representation.
 * Enforces forward slashes, lowercase ASCII, collapsed separators, and rejects relative parent traversal.
 * Derives a deterministic 64-bit FNV-1a AssetId from the normalized path.
 */
class AssetPath {
public:
    [[nodiscard]] static Result<AssetPath, EngineError> Make(std::string_view raw) noexcept {
        AssetPath p;
        usize len = 0;
        bool lastSlash = true; // Strips leading slashes

        for (usize i = 0; i < raw.size(); ++i) {
            char c = raw[i];
            if (c == '\\') c = '/';
            if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a'); // Lowercase

            if (c == '/') {
                if (lastSlash) continue; // Collapse consecutive '//'
                lastSlash = true;
            } else if (c == '.' && lastSlash) {
                const bool dot = (i + 1 == raw.size()) || raw[i + 1] == '/' || raw[i + 1] == '\\';
                const bool dotdot = (i + 1 < raw.size()) && raw[i + 1] == '.' &&
                                    (i + 2 == raw.size() || raw[i + 2] == '/' || raw[i + 2] == '\\');
                if (dotdot) return Err(EngineError{"Asset path may not contain '..'"});
                if (dot) continue; // Drop "./"
                lastSlash = false;
            } else {
                lastSlash = false;
            }

            if (len + 1 >= p.m_buf.size()) return Err(EngineError{"Asset path exceeds buffer capacity"});
            p.m_buf[len++] = c;
        }

        if (len == 0) return Err(EngineError{"Asset path cannot be empty"});
        p.m_len = static_cast<u16>(len);

        u64 h = 14695981039346656037ULL;
        for (usize i = 0; i < len; ++i) {
            h ^= static_cast<u8>(p.m_buf[i]);
            h *= 1099511628211ULL;
        }
        p.m_id = AssetId{h};
        return p;
    }

    [[nodiscard]] AssetId          Id()   const noexcept { return m_id; }
    [[nodiscard]] std::string_view View() const noexcept { return {m_buf.data(), m_len}; }

private:
    std::array<char, 256> m_buf{};
    u16     m_len = 0;
    AssetId m_id{};
};

} // namespace EngineN
